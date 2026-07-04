/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file */

#include "workspace.h"
#include "parser.h"
#include "project_tree.h"
#include "grammar.tab.h"   /* KW_* keyword constants for per-kind routing */
#include "compile_commands.h"
#include "pathutil.h"
#include "diagnostics.h"
#include "diag_worker.h"
#include "rpc.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* copy_document_into_project() indexes a prefix_set directly with a
 * NodeKind, which is only sound while the two enums list the four id
 * namespaces in the same order. */
_Static_assert((int)PREFIX_TASK     == (int)NODE_KIND_TASK &&
               (int)PREFIX_ACCOUNT  == (int)NODE_KIND_ACCOUNT &&
               (int)PREFIX_RESOURCE == (int)NODE_KIND_RESOURCE &&
               (int)PREFIX_REPORT   == (int)NODE_KIND_REPORT,
               "prefix_kind must mirror NodeKind order");

/* ═══════════════════════════════════════════════════════════════════════════
   Workspace root & compile_commands.json cache
   ═══════════════════════════════════════════════════════════════════════════ */

/** Filesystem path of the opened workspace root (decoded from rootUri); NULL when no folder is open. */
static char *g_workspace_root = NULL;

/* compile_commands.json cache.  g_cc_path is set once at initialize so
 * the stat-poll in revalidate_all_docs has a stable target.  The
 * mtime/size pair is bumped each time the file is read; a difference
 * triggers reload.  g_cc_attempted is set after the first load attempt
 * so missing-file errors are only surfaced once per change. */
/** Absolute path to compile_commands.json; set once at initialize, NULL until then. */
static char  *g_cc_path        = NULL;
/** Seconds component of the last-seen mtime of compile_commands.json. */
static time_t g_cc_mtime_sec   = 0;
/** Nanoseconds component of the last-seen mtime of compile_commands.json. */
static long   g_cc_mtime_nsec  = 0;
/** File size of compile_commands.json at the last successful stat. */
static off_t  g_cc_size        = 0;
/** Non-zero after the first load attempt; suppresses repeated missing-file errors. */
static int    g_cc_attempted   = 0;

/** Degradation status of compile_commands.json; stamped onto each workspace_snapshot
 *  so diagnostics workers can emit per-file warnings when the file is absent or malformed. */
static cc_status g_cc_status   = CC_STATUS_OK;

/* The currently published immutable workspace snapshot.  Touched only by
 * the coordinator thread (notifications swap it; query coordination acquires
 * a ref from it), so the pointer itself needs no atomic; only the snapshot's
 * refcount is atomic, for the worker-side release.  NULL until the first
 * revalidate builds one. */
static workspace_snapshot *g_ws = NULL;

void workspace_set_root_from_uri(const char *root_uri) {
    if (root_uri && !g_workspace_root)
        g_workspace_root = uri_to_path(root_uri);

    if (g_workspace_root && !g_cc_path) {
        size_t root_len = strlen(g_workspace_root);
        int need_sep = (root_len > 0 && g_workspace_root[root_len - 1] != '/');
        const char *fname = "compile_commands.json";
        size_t fname_len = strlen(fname);
        g_cc_path = malloc(root_len + (need_sep ? 1 : 0) + fname_len + 1);
        if (g_cc_path) {
            memcpy(g_cc_path, g_workspace_root, root_len);
            size_t off = root_len;
            if (need_sep) g_cc_path[off++] = '/';
            memcpy(g_cc_path + off, fname, fname_len + 1);
        }
    }
}

const char *workspace_root(void) {
    return g_workspace_root;
}

const char *workspace_cc_path(void) {
    return g_cc_path;
}

workspace_snapshot *workspace_current(void) {
    return g_ws;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Workspace loading
   ═══════════════════════════════════════════════════════════════════════════ */

void load_file_from_disk(const char *path) {
    char *uri = path_to_uri(path);
    if (doc_find(uri)) { free(uri); return; }

    char *text = read_file(path);
    if (!text) {
        DLOG(DEBUG_DOCSTORE, LOG_INFO, "load from disk failed (unreadable): %s", path);
        free(uri);
        return;
    }

    Document *document = doc_alloc(uri);
    free(uri);
    if (!document) { free(text); return; }

    DLOG(DEBUG_DOCSTORE, LOG_VERBOSE, "loaded from disk: %s (%zu bytes)",
         path, strlen(text));

    document->text      = text;
    document->disk_only = 1;
    ParseOutput *po = parse(text);
    follow_includes(path, po);
    doc_install_parse(document, po);
}

void install_disk_text(Document *document, char *text, const char *path) {
    free(document->text);
    document->text      = text;
    document->disk_only = 1;
    ParseOutput *po = parse(text);
    follow_includes(path, po);
    doc_install_parse(document, po);
}

/** Resolve an include directive's filename to an absolute filesystem path.
 *  Absolute filenames are copied as-is; relative ones are joined against the
 *  directory containing @p includer_path (or copied as-is when the includer
 *  has no directory component).
 *  @param includer_path  Path of the file containing the include directive.
 *  @param filename       The include directive's (possibly relative) filename.
 *  @return malloc'd path the caller must free, or NULL on allocation failure. */
static char *resolve_include_path(const char *includer_path, const char *filename) {
    size_t fname_len = strlen(filename);

    size_t path_len = strlen(includer_path);
    const char *last_slash = NULL;
    for (size_t i = path_len; i-- > 0; ) {
        if (includer_path[i] == '/') { last_slash = includer_path + i; break; }
    }

    if (filename[0] == '/' || !last_slash) {
        char *full_path = malloc(fname_len + 1);
        if (full_path) memcpy(full_path, filename, fname_len + 1);
        return full_path;
    }

    size_t dir_len = (size_t)(last_slash - includer_path);
    char *full_path = malloc(dir_len + 1 + fname_len + 1);
    if (!full_path) return NULL;
    memcpy(full_path, includer_path, dir_len);
    full_path[dir_len] = '/';
    memcpy(full_path + dir_len + 1, filename, fname_len + 1);
    return full_path;
}

/** Append @p uri to @p includer's included_uris[], growing the array as
 *  needed. On success the array takes ownership of @p uri and 1 is returned;
 *  on allocation failure the list is left unchanged and 0 is returned (the
 *  caller retains ownership of @p uri).
 *  @param includer  Document whose included_uris[] is appended to.
 *  @param uri       URI to append (ownership transfers on success).
 *  @return 1 on success, 0 on allocation failure. */
static int includer_append_included_uri(Document *includer, char *uri) {
    if (includer->num_included_uris >= includer->included_uris_cap) {
        int new_cap = includer->included_uris_cap
                      ? includer->included_uris_cap * 2 : 4;
        char **grown = realloc(includer->included_uris,
                               (size_t)new_cap * sizeof(char *));
        if (!grown) return 0;
        includer->included_uris     = grown;
        includer->included_uris_cap = new_cap;
    }
    includer->included_uris[includer->num_included_uris++] = uri;
    return 1;
}

void follow_includes(const char *file_path, const ParseOutput *po) {
    /* Look up the includer Document so we can repopulate its
     * included_uris[] as we resolve each include below.  follow_includes
     * runs exactly once per parse, so clear any prior list before the
     * early-return: a parse that newly removed all includes still needs
     * to drop the stale URIs. */
    char *includer_uri = path_to_uri(file_path);
    Document *includer = includer_uri ? doc_find(includer_uri) : NULL;
    free(includer_uri);
    if (includer) {
        for (int i = 0; i < includer->num_included_uris; i++)
            free(includer->included_uris[i]);
        includer->num_included_uris = 0;
    }

    if (!po || !po->num_includes) return;

    for (int i = 0; i < po->num_includes; i++) {
        const IncludeRef *inc = &po->includes[i];
        const char *filename = inc->filename;
        if (!filename) continue;

        char *full_path = resolve_include_path(file_path, filename);
        if (!full_path) continue;

        DLOG(DEBUG_INCLUDES, LOG_VERBOSE, "include '%s' -> %s", filename, full_path);
        load_file_from_disk(full_path);

        /* Locate the included Document and propagate this include's
         * prefixes onto it.  load_file_from_disk normalises and
         * inserts under a file:// URI, so look it up the same way. */
        char *target_uri = path_to_uri(full_path);
        Document *target = target_uri ? doc_find(target_uri) : NULL;
        if (!target)
            DLOG(DEBUG_INCLUDES, LOG_INFO, "include unresolved: %s (from %s)",
                 full_path, file_path);
        if (target)
            prefix_set_copy(&target->prefixes, &inc->prefixes);

        /* Record this resolved URI on the includer so
         * build_workspace_snapshot() can BFS the include graph.  Ownership
         * of target_uri transfers to includer->included_uris[]. */
        if (includer && target_uri && includer_append_included_uri(includer, target_uri))
            target_uri = NULL;
        free(target_uri);

        free(full_path);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   compile_commands.json loading
   ═══════════════════════════════════════════════════════════════════════════ */

/** Load every .tjp listed in a parsed compile_commands.json into docs[] and
 *  tag each as a project root. Each load_file_from_disk() cascades through
 *  follow_includes() to pull in the file's transitive .tji closure; the
 *  is_cc_root tag seeds build_workspace_snapshot()'s per-project BFS.
 *
 *  compile_commands.json is the only docs[] populator at startup; if
 *  it is missing or malformed the server has zero docs[] entries and
 *  every cross-file feature (workspace symbol, definition, references,
 *  cross-file diagnostics) is inert until the file is fixed.
 *
 *  @param entries  Parsed compile_commands entries.
 *  @param n        Number of entries. */
static void load_compile_entries(const CompileEntry *entries, int n) {
    for (int i = 0; i < n; i++) {
        if (!entries[i].file_abs) continue;
        DLOG(DEBUG_COMPILE_COMMANDS, LOG_VERBOSE, "  entry[%d] %s",
             i, entries[i].file_abs);
        load_file_from_disk(entries[i].file_abs);
        char *uri = path_to_uri(entries[i].file_abs);
        Document *root = uri ? doc_find(uri) : NULL;
        free(uri);
        if (root) root->is_cc_root = 1;
    }
}

void reload_compile_commands(void) {
    g_cc_attempted = 1;
    DLOG(DEBUG_COMPILE_COMMANDS, LOG_INFO, "reload: cc_path=%s",
         g_cc_path ? g_cc_path : "(none)");

    if (!g_cc_path) {
        /* No workspace root (rootUri null / no folder open), so we cannot
         * locate compile_commands.json.  This is a legitimate single-file
         * scenario, not an error: we no longer surface a window/showMessage
         * here.  The degradation is instead reported per-file as warning
         * diagnostics by the diagnostics workers, keyed off g_cc_status. */
        g_cc_status = CC_STATUS_MISSING;
        return;
    }

    /* Cleared up front; re-set below to MISSING (absent) or MALFORMED
     * (invalid JSON / wrong schema) as the load result dictates. */
    g_cc_status = CC_STATUS_OK;

    struct stat st;
    if (stat(g_cc_path, &st) == 0) {
        g_cc_mtime_sec  = st.st_mtim.tv_sec;
        g_cc_mtime_nsec = st.st_mtim.tv_nsec;
        g_cc_size       = st.st_size;
    } else {
        g_cc_mtime_sec = g_cc_mtime_nsec = 0;
        g_cc_size      = 0;
    }

    CompileEntry *entries = NULL;
    int           n       = 0;
    CompileCommandsResult res =
        compile_commands_load(g_workspace_root, &entries, &n);

    switch (res) {
    case CC_OK:
        load_compile_entries(entries, n);
        break;
    case CC_NOT_FOUND:
        /* compile_commands.json is absent at the workspace root.  As with the
         * no-root case above, this is reported per-file as warning diagnostics
         * by the diagnostics workers rather than a window/showMessage, so no
         * notification is emitted here. */
        g_cc_status = CC_STATUS_MISSING;
        break;
    case CC_PARSE_ERROR:
        /* Present but not valid JSON.  Like the missing case, no documents are
         * loaded; the degradation is reported per-file as warning diagnostics
         * (keyed off g_cc_status) rather than a window/showMessage.  The parse
         * error detail is logged to stderr by compile_commands_load(). */
        g_cc_status = CC_STATUS_MALFORMED;
        break;
    case CC_SCHEMA_ERROR:
        /* Present but does not match the expected schema (top-level JSON array
         * of objects with a `file` field).  Reported per-file as warning
         * diagnostics, as above. */
        fprintf(stderr,
            "taskjuggler-lsp: compile_commands.json does not match the "
            "expected schema (top-level JSON array of objects with a "
            "`file` field); no documents loaded.\n");
        g_cc_status = CC_STATUS_MALFORMED;
        break;
    case CC_NO_ROOT:
        /* Already handled above by the g_cc_path NULL check. */
        break;
    }

    DLOG(DEBUG_COMPILE_COMMANDS, LOG_INFO, "reload result: status=%d, %d entries",
         g_cc_status, res == CC_OK ? n : 0);
    compile_commands_free(entries, n);
}

/** Stat g_cc_path; if its mtime or size has changed since the last
 *  load (or the file is now present after a missing-first-attempt),
 *  trigger reload_compile_commands.  Called at the top of every
 *  revalidate_all_docs(), so every user-driven parse event picks up
 *  on-disk edits to compile_commands.json. */
static void maybe_reload_compile_commands(void) {
    if (!g_cc_path) return;
    struct stat st;
    if (stat(g_cc_path, &st) != 0) {
        /* File disappeared since the last successful load.  Only nag
         * the user once per transition by clearing the cache. */
        if (g_cc_mtime_sec || g_cc_mtime_nsec || g_cc_size) {
            g_cc_mtime_sec = g_cc_mtime_nsec = 0;
            g_cc_size      = 0;
            show_message(1,
                "taskjuggler-lsp: compile_commands.json has been removed.");
        }
        return;
    }
    if (st.st_mtim.tv_sec  != g_cc_mtime_sec ||
        st.st_mtim.tv_nsec != g_cc_mtime_nsec ||
        st.st_size         != g_cc_size      ||
        !g_cc_attempted) {
        DLOG(DEBUG_COMPILE_COMMANDS, LOG_VERBOSE,
             "compile_commands.json changed on disk; reloading");
        reload_compile_commands();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Workspace snapshot build

   The id-namespace classification and dotted-path navigation helpers used
   below (node_kind_of, find_node_by_dotted_path) live in project_tree.{h,c}.

   Each compile_commands.json entry becomes one project; its transitive
   include closure (followed via Document.included_uris[]) is deep-copied
   into a single ProjectNode tree with the includer's per-kind prefix
   applied (see project_tree.h).  Built fresh into the published
   workspace_snapshot on every notification.  Nodes of every kind share one
   root: a node's `keyword` identifies its kind, so walkers must filter on
   it to respect TaskJuggler's separate task / account / resource / report
   id namespaces.

   This tree is the authoritative cross-file resolution surface: it is
   prefix-applied (so dependency paths resolve against real qualified ids)
   and each task node owns the dependency edges declared on it.
   handle_definition / handle_references / handle_hover bridge the
   per-document task under the cursor to its clone here (via
   project_node_for_doc_task) and resolve against this tree.

   Each ws_doc records the index of the project that claimed it during the
   snapshot's include BFS; handlers route cross-file lookups through that
   membership.  Editor-only files outside every compile_commands closure
   each form their own singleton project.
   ═══════════════════════════════════════════════════════════════════════════ */

/** Return the ws_doc index for the Document @p d using the @p slot_to_wsdoc
 *  mapping built during build_workspace_snapshot(), or -1 when @p d's slot
 *  has no corresponding ws_doc entry.
 *  @param slot_to_wsdoc  Array mapping each docs[] slot index to its ws_doc
 *                        index, or -1 when the slot is unused or unparsed.
 *  @param d              Document pointer within docs[].
 *  @return               ws_doc index, or -1. */
static int ws_doc_index_of(const int *slot_to_wsdoc, const Document *d) {
    return slot_to_wsdoc[(int)(d - docs)];
}

/** Copy each top-level declaration of @p d (from its current snapshot) into
 *  project @p pidx's tree, applying @p d's matching per-kind prefix.  Routes
 *  by the node's own keyword to pick both the prefix and the namespace the
 *  prefix path is resolved within; the project block is document-local
 *  metadata and is skipped.
 *  @param ws    Workspace snapshot being built; owns the project array.
 *  @param pidx  Index of the target project within @p ws->projects.
 *  @param d     Source document whose top-level nodes are to be copied. */
static void copy_document_into_project(workspace_snapshot *ws, int pidx, Document *d) {
    if (!d->snap || !d->snap->root) return;
    ProjectNode *proot = &ws->projects[pidx]->root;
    tj_node *root = d->snap->root;
    /* Intern this document's URI once: every node copied below shares it, so a
     * 10k-node document interns one string instead of strdup'ing per node. */
    char *source_uri = d->uri
        ? arena_strndup(ws->node_strings, d->uri, strlen(d->uri))
        : NULL;
    for (int i = 0; i < root->num_children; i++) {
        tj_node    *child = root->children[i];
        NodeKind    kind = node_kind_of(child->keyword);
        if (kind == NODE_KIND_OTHER)
            continue;   /* project block stays document-local */
        const char *prefix = prefix_get(&d->prefixes, (prefix_kind)kind);
        ProjectNode *target = find_node_by_dotted_path(proot, prefix, kind);
        if (!target) continue;
        project_node_append_child(
            target, project_node_from_tj(child, source_uri, ws->node_strings));
    }
}

/** Copy @p d's top-level declarations into project @p pidx and stamp the
 *  document's ws_doc with that project index, unless it was already claimed
 *  by an earlier project (first include-BFS to reach a doc wins).
 *  @param ws            Snapshot under construction.
 *  @param pidx          Index of the project being assembled.
 *  @param d             Member document to fold in.
 *  @param slot_to_wsdoc docs[] slot -> ws_doc index map for the stamp. */
static void assign_doc_to_project(workspace_snapshot *ws, int pidx, Document *d,
                                  const int *slot_to_wsdoc) {
    copy_document_into_project(ws, pidx, d);
    int wsd = ws_doc_index_of(slot_to_wsdoc, d);
    if (wsd >= 0 && ws->docs[wsd].project_index < 0)
        ws->docs[wsd].project_index = pidx;
}

/** BFS from @p root along included_uris[], copying every reachable Document's
 *  top-level into project @p pidx with prefixes applied, and stamping each
 *  visited doc's ws_doc.project_index to @p pidx unless a prior project
 *  already claimed it.
 *  @param ws            Workspace snapshot being built.
 *  @param pidx          Index of the target project within @p ws->projects.
 *  @param root          Root document that seeds the BFS (the compile_commands
 *                       entry point).
 *  @param slot_to_wsdoc Mapping from docs[] slot index to ws_doc index. */
static void project_populate_from_root(workspace_snapshot *ws, int pidx,
                                       Document *root, const int *slot_to_wsdoc) {
    /* Queue holds borrowed Document pointers; visited[] dedupes within
     * this BFS so a diamond include doesn't double-copy. */
    Document **queue   = NULL;
    int        q_len   = 0;
    int        q_head  = 0;
    int        q_cap   = 0;

    Document **visited = NULL;
    int        v_len   = 0;
    int        v_cap   = 0;

    /* Append `val` to a growable array (`arr` / `len` / `cap`), doubling
     * the capacity when full and jumping to `cleanup` on allocation
     * failure.  Function-body-local macro; #undef'd at the end of the
     * helper. */
    #define PUSH(arr, len, cap, val) do {                       \
        if ((len) >= (cap)) {                                   \
            int _nc = (cap) ? (cap) * 2 : 8;                    \
            void *_t = realloc((arr), (size_t)_nc * sizeof(*(arr))); \
            if (!_t) goto cleanup;                              \
            (arr) = _t;                                         \
            (cap) = _nc;                                        \
        }                                                       \
        (arr)[(len)++] = (val);                                 \
    } while (0)

    PUSH(queue,   q_len, q_cap, root);
    PUSH(visited, v_len, v_cap, root);

    /* Anchor the project on the root document's own top-level.  Its
     * prefixes are NULL, so every declaration lands unprefixed in the
     * matching per-kind tree. */
    assign_doc_to_project(ws, pidx, root, slot_to_wsdoc);

    while (q_head < q_len) {
        Document *cur = queue[q_head++];
        for (int i = 0; i < cur->num_included_uris; i++) {
            Document *child = doc_find(cur->included_uris[i]);
            if (!child || !child->snap || !child->snap->root) continue;
            int seen = 0;
            for (int v = 0; v < v_len && !seen; v++)
                if (visited[v] == child) seen = 1;
            if (seen) continue;
            PUSH(visited, v_len, v_cap, child);
            PUSH(queue,   q_len, q_cap, child);
            assign_doc_to_project(ws, pidx, child, slot_to_wsdoc);
        }
    }

cleanup:
    free(queue);
    free(visited);
    #undef PUSH
}

/** Populate one ws_doc from its live Document: take a ref on the current
 *  parse snapshot and copy the per-kind include prefixes into the immutable
 *  workspace snapshot.
 *  @param w  Destination ws_doc in the snapshot under construction.
 *  @param d  Source document whose snapshot and prefixes are captured. */
static void populate_ws_doc(ws_doc *w, Document *d) {
    w->snap      = docsnap_acquire(d->snap);
    prefix_set_copy(&w->prefixes, &d->prefixes);
    w->disk_only = d->disk_only;
}

/** Build an immutable workspace_snapshot from the current docs[]: one ws_doc
 *  per parsed in-use document (referencing its doc_snapshot and capturing the
 *  include-prefixes in force now), one project per is_cc_root document with
 *  its include closure assembled, and one singleton "orphan" project for
 *  every remaining document not reached by any closure.  Orphans exist so
 *  editor-opened files outside the compile_commands.json closure still get
 *  in-file LSP behavior (completion, hover, etc.) without bleeding into other
 *  projects' cross-file pools.
 *  @return  Newly allocated workspace_snapshot with refcount 1. */
static workspace_snapshot *build_workspace_snapshot(void) {
    int slot_to_wsdoc[MAX_DOCS];
    int ndoc = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (docs[i].in_use && docs[i].snap)
            slot_to_wsdoc[i] = ndoc++;
        else
            slot_to_wsdoc[i] = -1;
    }

    workspace_snapshot *ws = ws_alloc(ndoc);

    /* Populate each ws_doc: ref the live snapshot, copy the prefixes. */
    for (int i = 0; i < MAX_DOCS; i++) {
        int wsd = slot_to_wsdoc[i];
        if (wsd < 0) continue;
        populate_ws_doc(&ws->docs[wsd], &docs[i]);
    }

    /* Stamp the cc-status so the diagnostics workers can emit the per-file
     * "Missing/Malformed compile_commands.json" warnings off the immutable
     * snapshot. */
    ws->cc_status = g_cc_status;

    /* Pass 1: compile_commands roots + their include closures. */
    for (int i = 0; i < MAX_DOCS; i++) {
        Document *root = &docs[i];
        if (!root->in_use || !root->is_cc_root || !root->snap) continue;
        int pidx = ws_add_project(ws, root->uri);
        ws->projects[pidx]->from_compile_commands = 1;
        project_populate_from_root(ws, pidx, root, slot_to_wsdoc);
    }

    /* Pass 2: unclaimed parsed docs each become their own singleton orphan
     * project, anchored on the doc's own top-level with no prefix. */
    for (int i = 0; i < MAX_DOCS; i++) {
        int wsd = slot_to_wsdoc[i];
        if (wsd < 0 || ws->docs[wsd].project_index >= 0) continue;
        Document *d = &docs[i];
        int pidx = ws_add_project(ws, d->uri);
        copy_document_into_project(ws, pidx, d);
        ws->docs[wsd].project_index = pidx;
    }

    return ws;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Revalidation
   ═══════════════════════════════════════════════════════════════════════════ */

/** Publish an empty diagnostics baseline for every editor-managed document
 *  after a notification so the client clears stale markers from the previous
 *  revision.  The diagnostics workers (diag_registry_update) then layer the
 *  real markers on top asynchronously: tj3 results plus, when no usable
 *  compile_commands.json is present, the per-file "Missing
 *  compile_commands.json" warnings (see diag_collect_cc_missing). */
static void republish_all_diagnostics(void) {
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        if (docs[i].disk_only) continue;
        publish_diagnostics(docs[i].uri);
    }
}

/* The docs[] table dump and its private helpers are compiled only when the
 * revalidation category is at LOG_VERBOSE or above.  This keeps its per-slot
 * tree walks out of the default build entirely (rather than merely silencing
 * the output), and avoids unused-function warnings when the category is off. */
#if DEBUG_REVALIDATE >= LOG_VERBOSE

/** Recursively sum the @c num_dependencies across @p n and its subtree.
 *  @param n  Root node to sum; NULL is treated as zero.
 *  @return   Total dependency count for @p n and all descendants. */
static int dependency_count_subtree(const tj_node *n) {
    if (!n) return 0;
    int total = n->num_dependencies;
    for (int i = 0; i < n->num_children; i++)
        total += dependency_count_subtree(n->children[i]);
    return total;
}

/** True when @p d declares a project block (scans root's top-level
 *  children for a KW_PROJECT node).
 *  @param d  Document to inspect; safe to call with an unparsed slot.
 *  @return   Non-zero when a KW_PROJECT top-level child is found, zero otherwise. */
static int doc_has_project_block(const Document *d) {
    if (!d->snap || !d->snap->root) return 0;
    tj_node *root = d->snap->root;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i]->keyword == KW_PROJECT) return 1;
    return 0;
}

/** Dump the live docs[] slot table to stderr.  One header line followed
 *  by one line per occupied slot: index, flags, project id, dep count,
 *  URI.  Flags are a fixed-width string so columns line up:
 *    D = disk_only (lowercase d = editor-owned)
 *    P = has parse output (root tree present)
 *    R = has a project block (canonical root candidate)
 *    C = compile_commands.json root
 *  @c deps= shows the total number of captured @c depends + @c precedes
 *  references across every task in the document.  Caller must hold docs_mutex.
 *  @param trigger  Short label for the event that caused the dump (used in
 *                  the header line).
 *  @param ws       Freshly published snapshot, consulted for project
 *                  membership and count; may be NULL. */
static void dump_docs_to_stderr(const char *trigger, const workspace_snapshot *ws) {
    int total = 0, editor = 0, disk = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        total++;
        if (docs[i].disk_only) disk++; else editor++;
    }
    fprintf(stderr,
            "taskjuggler-lsp: docs[] after %s — %d total (%d editor, %d disk), "
            "%d projects\n",
            trigger, total, editor, disk, ws ? ws->num_projects : 0);
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        /* Find this doc's ws_doc (by snapshot identity) to report its project. */
        const char *pid = "(none)";
        if (ws && docs[i].snap) {
            for (int w = 0; w < ws->num_docs; w++) {
                if (ws->docs[w].snap != docs[i].snap) continue;
                int pidx = ws->docs[w].project_index;
                if (pidx >= 0 && pidx < ws->num_projects)
                    pid = ws->projects[pidx]->id ? ws->projects[pidx]->id : "(no-id)";
                break;
            }
        }
        int deps = docs[i].snap ? dependency_count_subtree(docs[i].snap->root) : 0;
        fprintf(stderr, "  [%2d] %c%c%c%c  proj=%s  deps=%d  %s\n",
                i,
                docs[i].disk_only          ? 'D' : 'd',
                docs[i].snap               ? 'P' : '-',
                doc_has_project_block(&docs[i]) ? 'R' : '-',
                docs[i].is_cc_root         ? 'C' : '-',
                pid,
                deps,
                docs[i].uri ? docs[i].uri : "(null)");
    }
    fflush(stderr);
}

#endif /* DEBUG_REVALIDATE >= LOG_VERBOSE */

/** Build a fresh workspace snapshot from the current docs[] and swap it in
 *  as the published @c g_ws, releasing the previous one (which an in-flight
 *  query may still be reading — it survives until that query releases its
 *  ref).  Must be called on the coordinator thread. */
static void publish_workspace_snapshot(void) {
    workspace_snapshot *fresh = build_workspace_snapshot();
    workspace_snapshot *old   = g_ws;
    g_ws = fresh;
    ws_release(old);
}

void revalidate_all_docs(void) {
#if DEBUG_REVALIDATE
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    maybe_reload_compile_commands();
    publish_workspace_snapshot();
    republish_all_diagnostics();
    /* Hand the freshly published snapshot to the per-project diagnostics
     * workers (spawning/retiring them as projects appear/disappear).  Every
     * snapshot-updating notification funnels through here, so this single
     * call covers didOpen/didChange/didClose/watched-files/rename/cc-reload. */
    diag_registry_update(g_ws);
#if DEBUG_REVALIDATE
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0
              + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    DLOG(DEBUG_REVALIDATE, LOG_INFO, "revalidate complete: %d projects in %.2f ms",
         g_ws ? g_ws->num_projects : 0, ms);
#endif
#if DEBUG_REVALIDATE >= LOG_VERBOSE
    dump_docs_to_stderr("revalidate_all_docs", g_ws);
#endif
}
