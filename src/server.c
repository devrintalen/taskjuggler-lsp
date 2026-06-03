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

/* For the data-flow overview, document lifecycle, and query-dispatch
 * table, see doc/modules/server.rst.
 *
 * Concurrency model: the coordinator thread owns the live document store.
 * Notifications and the inline query methods (initialize / shutdown /
 * semanticTokens) mutate it under docs_mutex on the coordinator.  Every
 * other query is served by cloning a query_context (a frozen, fully-owned
 * copy of the documents and project tree the handler needs) under
 * docs_mutex, then running the handler on a worker thread with no lock
 * held.  See query_context.h.
 */

#include "server.h"
#include "parser.h"
#include "project_tree.h"
#include "query_context.h"
#include "grammar.tab.h"   /* KW_* keyword constants for per-kind routing */
#include "job_queue.h"
#include "threadpool.h"
#include "diagnostics.h"
#include "dependency.h"
#include "definition.h"
#include "references.h"
#include "document_highlight.h"
#include "document_symbol.h"
#include "folding_range.h"
#include "hover.h"
#include "signature.h"
#include "completion.h"
#include "semantic_tokens.h"
#include "semantic_tokens_delta.h"
#include "workspace_symbol.h"
#include "code_lens.h"
#include "compile_commands.h"
#include "pathutil.h"
#include "diag_worker.h"
#include "version.h"

#include <yyjson.h>
#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Document store
   ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_DOCS 64

/** Document store slot.  Each slot owns the parse-derived state directly
 *  (per-kind synthetic roots, tokens, include filenames); parse() returns
 *  a transient ParseOutput whose fields get moved here by doc_install_parse(). */
typedef struct Document {
    char        *uri;
    char        *text;              /**< mutable working copy; the snapshot holds its own parsed copy */
    _Atomic uint64_t doc_version;   /**< monotonic stamp counter; the next parse stamps the value, then ++ */
    int          in_use;
    int          disk_only;
    int          is_cc_root;        /**< 1 when this doc is named directly in compile_commands.json */

    /* Immutable parse output.  `snap` is the current parse's doc_snapshot
     * (NULL before the first parse — use it as the "has-parse" sentinel);
     * `prev_snap` is the one immediately before it, retained so a
     * semanticTokens/delta request can diff against the version the client
     * last held.  Both are refcounted: a published workspace_snapshot also
     * holds refs, so a snapshot outlives any in-flight query reading it.
     * Each is released and rotated by doc_install_parse(). */
    doc_snapshot *snap;
    doc_snapshot *prev_snap;

    /* Prefixes applied to this Document by the includer's `include` block,
     * one per kind.  Populated by follow_includes() from the includer's
     * captured IncludeRef when this file is pulled in; stay NULL on a
     * canonical .tjp or on orphan .tji files in a .tji-only workspace.
     * Captured into each workspace_snapshot's ws_doc at build time. */
    char        *task_prefix;
    char        *account_prefix;
    char        *report_prefix;
    char        *resource_prefix;

    /* Resolved file:// URIs of every `include` directive in this doc,
     * recorded by follow_includes() at parse time.  Owned by the
     * Document; cleared at the top of each follow_includes() run and
     * freed by doc_free().  Lets build_workspace_snapshot() walk the
     * include graph without re-parsing or threading state through the
     * load pipeline. */
    char       **included_uris;
    int          num_included_uris;
    int          included_uris_cap;
} Document;

static Document docs[MAX_DOCS];

/* The currently published immutable workspace snapshot.  Touched only by
 * the coordinator thread (notifications swap it; query coordination acquires
 * a ref from it), so the pointer itself needs no atomic; only the snapshot's
 * refcount is atomic, for the worker-side release.  NULL until the first
 * revalidate builds one. */
static workspace_snapshot *g_ws = NULL;

/* Serializes every read/write of docs[] — slots, their fields, and the
 * global trees built from them. */
static pthread_mutex_t docs_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *g_workspace_root = NULL;

/* compile_commands.json cache.  g_cc_path is set once at initialize so
 * the stat-poll in revalidate_all_docs has a stable target.  The
 * mtime/size pair is bumped each time the file is read; a difference
 * triggers reload.  g_cc_attempted is set after the first load attempt
 * so missing-file errors are only surfaced once per change. */
static char  *g_cc_path        = NULL;
static time_t g_cc_mtime_sec   = 0;
static long   g_cc_mtime_nsec  = 0;
static off_t  g_cc_size        = 0;
static int    g_cc_attempted   = 0;

/* Degradation status of the workspace's compile_commands.json, set by
 * reload_compile_commands() and stamped onto each workspace_snapshot.  Whenever
 * it is not CC_STATUS_OK — the file is missing (no workspace root, or absent) or
 * malformed (invalid JSON / wrong schema) — no project closures are loaded and
 * every editor file is parsed stand-alone, so the diagnostics workers emit the
 * per-file "Missing/Malformed compile_commands.json" warnings keyed off it. */
static cc_status g_cc_status   = CC_STATUS_OK;

/* Forward declarations. */
static char *normalize_uri(const char *raw_uri);
static void  load_file_from_disk(const char *path);
static void  follow_includes(const char *file_path, const ParseOutput *po);
static workspace_snapshot *build_workspace_snapshot(void);
static void  reload_compile_commands(void);
static void  maybe_reload_compile_commands(void);

/* ── Per-project ProjectNode tree ───────────────────────────────────────── *
 *
 * Each compile_commands.json entry becomes one project; its transitive
 * include closure (followed via Document.included_uris[]) is deep-copied
 * into a single ProjectNode tree with the includer's per-kind prefix
 * applied (see project_tree.h).  Built fresh by build_workspace_snapshot()
 * into the published workspace_snapshot on every notification.  Nodes of
 * every kind share one root: a node's `keyword` identifies its kind, so
 * walkers must filter on it to respect TaskJuggler's separate task /
 * account / resource / report id namespaces.
 *
 * This tree is the authoritative cross-file resolution surface: it is
 * prefix-applied (so dependency paths resolve against real qualified ids)
 * and each task node owns the dependency edges declared on it.
 * handle_definition / handle_references / handle_hover bridge the
 * per-document task under the cursor to its clone here (via
 * project_node_for_doc_task) and resolve against this tree.
 *
 * Each ws_doc records the index of the project that claimed it during the
 * snapshot's include BFS; handlers route cross-file lookups through that
 * membership.  Editor-only files outside every compile_commands closure
 * each form their own singleton project. */

/* ── Slot lookup / allocation / free ─────────────────────────────────────── */

static Document *doc_find(const char *uri) {
    if (!uri) return NULL;
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i].in_use && strcmp(docs[i].uri, uri) == 0)
            return &docs[i];

    char *canon = normalize_uri(uri);
    if (!canon) return NULL;
    Document *found = NULL;
    if (strcmp(canon, uri) != 0) {
        for (int i = 0; i < MAX_DOCS; i++)
            if (docs[i].in_use && strcmp(docs[i].uri, canon) == 0) {
                found = &docs[i];
                break;
            }
    }
    free(canon);
    return found;
}

static Document *doc_alloc(const char *uri) {
    char *canon = normalize_uri(uri);
    if (!canon) return NULL;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) {
            docs[i].in_use = 1;
            docs[i].uri    = canon;
            atomic_store(&docs[i].doc_version, 1);
            return &docs[i];
        }
    }
    free(canon);
    return NULL;
}

/** Release both doc_snapshots held by @p d (the live store's refs), nulling
 *  each so the slot is reusable.  A snapshot only frees once any
 *  workspace_snapshot and in-flight query also release their refs. */
static void doc_clear_parse_state(Document *d) {
    docsnap_release(d->snap);
    d->snap = NULL;
    docsnap_release(d->prev_snap);
    d->prev_snap = NULL;
}

/** Build a fresh doc_snapshot from @p po (moving its tree and token spans in)
 *  and rotate it onto @p d: the outgoing current snapshot becomes prev_snap
 *  (replacing the one before it), so a semanticTokens/delta request can still
 *  diff against the version the client last held.  Includes are not part of
 *  the snapshot — callers consume them via follow_includes() before calling
 *  here; parse_output_free() releases the leftover include array and shell. */
static void doc_install_parse(Document *d, ParseOutput *po) {
    doc_snapshot *fresh = NULL;
    if (po) {
        uint64_t version = atomic_fetch_add(&d->doc_version, 1);
        fresh = docsnap_new(d->uri, d->text,
                            po->root, po->tok_spans,
                            po->num_tok_spans, po->num_sem_entries,
                            version);
        /* Ownership of the tree and token spans moved into the snapshot;
         * null them out so parse_output_free only releases what po still
         * owns (the includes array and the struct shell). */
        po->root            = NULL;
        po->tok_spans       = NULL;
        po->num_tok_spans   = 0;
        po->tok_span_cap    = 0;
        po->num_sem_entries = 0;
        parse_output_free(po);
    }

    docsnap_release(d->prev_snap);
    d->prev_snap = d->snap;   /* retains its existing ref, reassigned */
    d->snap      = fresh;     /* fresh holds ref 1, or NULL on a no-parse */
}

static void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    doc_clear_parse_state(d);
    free(d->task_prefix);
    free(d->account_prefix);
    free(d->report_prefix);
    free(d->resource_prefix);
    for (int i = 0; i < d->num_included_uris; i++)
        free(d->included_uris[i]);
    free(d->included_uris);
    memset(d, 0, sizeof(*d));
}

/* ═══════════════════════════════════════════════════════════════════════════
   Server-to-client messaging
   ═══════════════════════════════════════════════════════════════════════════ */

static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

void lsp_send_message(const char *msg) {
    pthread_mutex_lock(&stdout_mutex);
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

/** Send a window/showMessage notification to the client.  Used to
 *  surface non-fatal load/configuration errors (e.g. missing
 *  compile_commands.json) without crashing the session.  @p type
 *  follows the LSP MessageType enum: 1=Error, 2=Warning, 3=Info,
 *  4=Log. */
static void show_message(int type, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, params, "type", type);
    yyjson_mut_obj_add_str(doc, params, "message", message);
    yyjson_mut_val *note = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, note, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, note, "method",  "window/showMessage");
    yyjson_mut_obj_add_val(doc, note, "params",  params);
    yyjson_mut_doc_set_root(doc, note);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (text) {
        lsp_send_message(text);
        free(text);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   URI / path helpers (unchanged from pre-refactor)
   ═══════════════════════════════════════════════════════════════════════════ */

static char *percent_decode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    size_t wi = 0;
    for (size_t ri = 0; ri < len; ri++) {
        if (src[ri] == '%' && ri + 2 < len
                && isxdigit((unsigned char)src[ri + 1])
                && isxdigit((unsigned char)src[ri + 2])) {
            char hex[3] = { src[ri + 1], src[ri + 2], '\0' };
            dst[wi++] = (char)strtol(hex, NULL, 16);
            ri += 2;
        } else {
            dst[wi++] = src[ri];
        }
    }
    dst[wi] = '\0';
    return dst;
}

static char *percent_encode_path(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len * 3 + 1);
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    size_t wi = 0;
    for (size_t ri = 0; ri < len; ri++) {
        unsigned char c = (unsigned char)src[ri];
        if (c == '/'
                || (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[wi++] = (char)c;
        } else {
            dst[wi++] = '%';
            dst[wi++] = "0123456789ABCDEF"[c >> 4];
            dst[wi++] = "0123456789ABCDEF"[c & 0xf];
        }
    }
    dst[wi] = '\0';
    return dst;
}

char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    return percent_decode(uri + 7);
}

char *path_to_uri(const char *path) {
    char *encoded = percent_encode_path(path);
    size_t enc_len = strlen(encoded);
    char *uri = malloc(7 + enc_len + 1);
    if (!uri) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(uri, "file://", 7);
    memcpy(uri + 7, encoded, enc_len + 1);
    free(encoded);
    return uri;
}

static char *lexical_normalize_path(const char *path) {
    if (!path) return NULL;
    size_t len = strlen(path);
    char *out = malloc(len + 2);
    if (!out) return NULL;

    int absolute = (len > 0 && path[0] == '/');
    size_t wi = 0;
    if (absolute) out[wi++] = '/';

    size_t i = 0;
    int wrote_segment = 0;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) break;
        size_t seg_start = i;
        while (i < len && path[i] != '/') i++;
        size_t seg_len = i - seg_start;
        if (seg_len == 1 && path[seg_start] == '.') continue;
        if (wrote_segment) out[wi++] = '/';
        memcpy(out + wi, path + seg_start, seg_len);
        wi += seg_len;
        wrote_segment = 1;
    }
    out[wi] = '\0';
    return out;
}

static char *normalize_uri(const char *raw_uri) {
    if (!raw_uri) return NULL;
    if (strncmp(raw_uri, "file://", 7) != 0) return strdup(raw_uri);

    char *path = uri_to_path(raw_uri);
    if (!path) return strdup(raw_uri);

    char *canon = realpath(path, NULL);
    if (!canon) canon = lexical_normalize_path(path);
    free(path);
    if (!canon) return NULL;

    char *uri = path_to_uri(canon);
    free(canon);
    return uri;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return NULL; }
    rewind(file);
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return NULL; }
    size_t read_count = fread(buffer, 1, (size_t)size, file);
    buffer[read_count] = '\0';
    fclose(file);
    return buffer;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Workspace loading
   ═══════════════════════════════════════════════════════════════════════════ */

static void load_file_from_disk(const char *path) {
    char *uri = path_to_uri(path);
    if (doc_find(uri)) { free(uri); return; }

    char *text = read_file(path);
    if (!text) { free(uri); return; }

    Document *document = doc_alloc(uri);
    free(uri);
    if (!document) { free(text); return; }

    document->text      = text;
    document->disk_only = 1;
    ParseOutput *po = parse(text);
    follow_includes(path, po);
    doc_install_parse(document, po);
}

/** Replace @p *slot with a fresh strdup of @p value (NULL when @p value
 *  is NULL).  Used to copy IncludeRef prefix strings onto the includee. */
static void replace_string(char **slot, const char *value) {
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static void follow_includes(const char *file_path, const ParseOutput *po) {
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

    size_t path_len = strlen(file_path);
    const char *last_slash = NULL;
    for (size_t i = path_len; i-- > 0; ) {
        if (file_path[i] == '/') { last_slash = file_path + i; break; }
    }
    size_t dir_len = last_slash ? (size_t)(last_slash - file_path) : 0;

    for (int i = 0; i < po->num_includes; i++) {
        const IncludeRef *inc = &po->includes[i];
        const char *filename = inc->filename;
        if (!filename) continue;
        size_t fname_len = strlen(filename);

        char *full_path;
        if (filename[0] == '/') {
            full_path = malloc(fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, filename, fname_len + 1);
        } else if (last_slash) {
            full_path = malloc(dir_len + 1 + fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, file_path, dir_len);
            full_path[dir_len] = '/';
            memcpy(full_path + dir_len + 1, filename, fname_len + 1);
        } else {
            full_path = malloc(fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, filename, fname_len + 1);
        }

        load_file_from_disk(full_path);

        /* Locate the included Document and propagate this include's
         * prefixes onto it.  load_file_from_disk normalises and
         * inserts under a file:// URI, so look it up the same way. */
        char *target_uri = path_to_uri(full_path);
        Document *target = target_uri ? doc_find(target_uri) : NULL;
        if (target) {
            replace_string(&target->task_prefix,     inc->task_prefix);
            replace_string(&target->resource_prefix, inc->resource_prefix);
            replace_string(&target->account_prefix,  inc->account_prefix);
            replace_string(&target->report_prefix,   inc->report_prefix);
        }

        /* Record this resolved URI on the includer so
         * build_workspace_snapshot() can BFS the include graph.  Ownership
         * of target_uri transfers to includer->included_uris[]. */
        if (includer && target_uri) {
            if (includer->num_included_uris >= includer->included_uris_cap) {
                int new_cap = includer->included_uris_cap
                              ? includer->included_uris_cap * 2 : 4;
                char **tmp = realloc(includer->included_uris,
                                     (size_t)new_cap * sizeof(char *));
                if (tmp) {
                    includer->included_uris     = tmp;
                    includer->included_uris_cap = new_cap;
                }
            }
            if (includer->num_included_uris < includer->included_uris_cap) {
                includer->included_uris[includer->num_included_uris++] = target_uri;
                target_uri = NULL;
            }
        }
        free(target_uri);

        free(full_path);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   JSON helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static LspPos json_to_pos(yyjson_val *obj) {
    LspPos p = {0};
    if (!obj) return p;
    yyjson_val *ln = yyjson_obj_get(obj, "line");
    yyjson_val *ch = yyjson_obj_get(obj, "character");
    if (ln && yyjson_is_num(ln)) p.line      = (uint32_t)yyjson_get_num(ln);
    if (ch && yyjson_is_num(ch)) p.character = (uint32_t)yyjson_get_num(ch);
    return p;
}

static char *apply_incremental_change(const char *src,
                                      LspRange range,
                                      const char *new_text) {
    if (!src) src = "";
    size_t src_len = strlen(src);
    size_t new_len = strlen(new_text);

    size_t start_byte = 0;
    uint32_t cur_line = 0, cur_char = 0;
    while (start_byte < src_len) {
        if (cur_line == range.start.line && cur_char == range.start.character)
            break;
        if (src[start_byte] == '\n') { cur_line++; cur_char = 0; }
        else                         { cur_char++; }
        start_byte++;
    }

    size_t end_byte = start_byte;
    cur_line = range.start.line;
    cur_char = range.start.character;
    while (end_byte < src_len) {
        if (cur_line == range.end.line && cur_char == range.end.character)
            break;
        if (src[end_byte] == '\n') { cur_line++; cur_char = 0; }
        else                       { cur_char++; }
        end_byte++;
    }

    size_t suffix_len = src_len - end_byte;
    char *result = malloc(start_byte + new_len + suffix_len + 1);
    if (!result) return NULL;
    memcpy(result,                        src,      start_byte);
    memcpy(result + start_byte,           new_text, new_len);
    memcpy(result + start_byte + new_len, src + end_byte, suffix_len);
    result[start_byte + new_len + suffix_len] = '\0';
    return result;
}

static const char *json_str(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *item = yyjson_obj_get(obj, key);
    return (item && yyjson_is_str(item)) ? yyjson_get_str(item) : NULL;
}

static yyjson_mut_val *copy_id(yyjson_mut_doc *doc, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(doc);
    if (yyjson_is_str(id))  return yyjson_mut_strcpy(doc, yyjson_get_str(id));
    if (yyjson_is_uint(id)) return yyjson_mut_uint(doc, yyjson_get_uint(id));
    if (yyjson_is_sint(id)) return yyjson_mut_int(doc, yyjson_get_int(id));
    if (yyjson_is_real(id)) return yyjson_mut_real(doc, yyjson_get_real(id));
    return yyjson_mut_null(doc);
}

static yyjson_mut_val *make_response(yyjson_mut_doc *doc, yyjson_val *id,
                                      yyjson_mut_val *result) {
    yyjson_mut_val *resp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, resp, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, resp, "id", copy_id(doc, id));
    yyjson_mut_obj_add_val(doc, resp, "result", result);
    return resp;
}

static yyjson_mut_val *make_error_response(yyjson_mut_doc *doc, yyjson_val *id,
                                            int code, const char *message) {
    yyjson_mut_val *resp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, resp, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, resp, "id", copy_id(doc, id));
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_str(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, resp, "error", err);
    return resp;
}

/* ═══════════════════════════════════════════════════════════════════════════
   tj_node tree-building helpers (shared by per-Project rebuild)
   ═══════════════════════════════════════════════════════════════════════════ */

/* Coarse kind bucket for a node, collapsing the keyword set into the four
 * id namespaces TaskJuggler keeps separate.  KW_PROJECT maps to NODE_KIND_OTHER
 * but is never inserted into a Project tree (it stays document-local). */
typedef enum {
    NODE_KIND_TASK,
    NODE_KIND_ACCOUNT,
    NODE_KIND_RESOURCE,
    NODE_KIND_REPORT,
    NODE_KIND_OTHER
} NodeKind;

static NodeKind node_kind_of(int keyword) {
    switch (keyword) {
    case KW_TASK:     return NODE_KIND_TASK;
    case KW_ACCOUNT:  return NODE_KIND_ACCOUNT;
    case KW_RESOURCE:
    case KW_SHIFT:    return NODE_KIND_RESOURCE;
    case KW_PROJECT:  return NODE_KIND_OTHER;
    default:          return NODE_KIND_REPORT;
    }
}

/** Walk @p start's children along the dot-separated @p path and return
 *  the matched node, considering only children whose kind matches @p kind.
 *  Returns @p start when @p path is NULL or empty.  Used to locate an
 *  includer's prefix target inside a Project tree; the kind filter keeps
 *  same-named declarations in different namespaces from colliding now that
 *  all kinds share one root. */
static ProjectNode *find_node_by_dotted_path(ProjectNode *start, const char *path,
                                             NodeKind kind) {
    if (!start) return NULL;
    if (!path || !path[0]) return start;

    char *copy = strdup(path);
    if (!copy) return NULL;
    ProjectNode *cur = start;
    char *save = NULL;
    for (char *seg = strtok_r(copy, ".", &save); seg && cur;
         seg = strtok_r(NULL, ".", &save)) {
        ProjectNode *next = NULL;
        for (int i = 0; i < cur->num_children && !next; i++) {
            ProjectNode *child = cur->children[i];
            if (node_kind_of(child->keyword) == kind &&
                child->id && strcmp(child->id, seg) == 0)
                next = child;
        }
        cur = next;
    }
    free(copy);
    return cur;
}

/* ── Workspace snapshot build ──────────────────────────────────────────── */

/* docs[] slot index -> ws_doc index in the snapshot being built, or -1 when
 * the slot is unused / unparsed.  Set up once per build_workspace_snapshot()
 * call and consulted by the include BFS to stamp project membership. */
static int ws_doc_index_of(const int *slot_to_wsdoc, const Document *d) {
    return slot_to_wsdoc[(int)(d - docs)];
}

/** Copy each top-level declaration of @p d (from its current snapshot) into
 *  project @p pidx's tree, applying @p d's matching per-kind prefix.  Routes
 *  by the node's own `keyword` to pick both the prefix and the namespace the
 *  prefix path is resolved within; the project block is document-local
 *  metadata and is skipped. */
static void copy_document_into_project(workspace_snapshot *ws, int pidx, Document *d) {
    if (!d->snap || !d->snap->root) return;
    ProjectNode *proot = &ws->projects[pidx]->root;
    tj_node *root = d->snap->root;
    for (int i = 0; i < root->num_children; i++) {
        tj_node    *child = root->children[i];
        const char *prefix;
        NodeKind    kind = node_kind_of(child->keyword);
        switch (child->keyword) {
        case KW_TASK:
            prefix = d->task_prefix;     break;
        case KW_ACCOUNT:
            prefix = d->account_prefix;  break;
        case KW_RESOURCE:
        case KW_SHIFT:
            prefix = d->resource_prefix; break;
        case KW_PROJECT:
            continue;   /* project block stays document-local */
        default:
            prefix = d->report_prefix;   break;
        }
        ProjectNode *target = find_node_by_dotted_path(proot, prefix, kind);
        if (!target) continue;
        project_node_append_child(target,
                                  project_node_from_tj(child, d->uri));
    }
}

/** BFS from @p root along included_uris[], copying every reachable Document's
 *  top-level into project @p pidx with prefixes applied, and stamping each
 *  visited doc's ws_doc.project_index to @p pidx unless a prior project
 *  already claimed it. */
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
    copy_document_into_project(ws, pidx, root);
    int root_wsd = ws_doc_index_of(slot_to_wsdoc, root);
    if (root_wsd >= 0 && ws->docs[root_wsd].project_index < 0)
        ws->docs[root_wsd].project_index = pidx;

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
            copy_document_into_project(ws, pidx, child);
            int child_wsd = ws_doc_index_of(slot_to_wsdoc, child);
            if (child_wsd >= 0 && ws->docs[child_wsd].project_index < 0)
                ws->docs[child_wsd].project_index = pidx;
        }
    }

cleanup:
    free(queue);
    free(visited);
    #undef PUSH
}

/** Build an immutable workspace_snapshot from the current docs[]: one ws_doc
 *  per parsed in-use document (referencing its doc_snapshot and capturing the
 *  include-prefixes in force now), one project per is_cc_root document with
 *  its include closure assembled, and one singleton "orphan" project for
 *  every remaining document not reached by any closure.  Orphans exist so
 *  editor-opened files outside the compile_commands.json closure still get
 *  in-file LSP behavior (completion, hover, etc.) without bleeding into other
 *  projects' cross-file pools.  Returned with refcount 1. */
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
        Document *d = &docs[i];
        ws_doc    *w = &ws->docs[wsd];
        w->snap            = docsnap_acquire(d->snap);
        w->task_prefix     = d->task_prefix     ? strdup(d->task_prefix)     : NULL;
        w->account_prefix  = d->account_prefix  ? strdup(d->account_prefix)  : NULL;
        w->report_prefix   = d->report_prefix   ? strdup(d->report_prefix)   : NULL;
        w->resource_prefix = d->resource_prefix ? strdup(d->resource_prefix) : NULL;
        w->disk_only       = d->disk_only;
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

/* Publish an empty diagnostics baseline for every editor-managed document
 * after a notification so the client clears stale markers from the previous
 * revision.  The diagnostics workers (diag_registry_update) then layer the
 * real markers on top asynchronously: tj3 results plus, when no usable
 * compile_commands.json is present, the per-file "Missing
 * compile_commands.json" warnings (see diag_collect_cc_missing). */
static void republish_all_diagnostics(void) {
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        if (docs[i].disk_only) continue;
        publish_diagnostics(docs[i].uri);
    }
}

/** Read compile_commands.json from g_cc_path and call
 *  load_file_from_disk() on every listed .tjp.  Each load cascades
 *  into follow_includes() so the .tjp's transitive .tji closure ends
 *  up in docs[] as disk_only.  Already-loaded files short-circuit
 *  inside load_file_from_disk(), so reloads are idempotent.
 *
 *  compile_commands.json is the only docs[] populator at startup; if
 *  it is missing or malformed the server has zero docs[] entries and
 *  every cross-file feature (workspace symbol, definition, references,
 *  cross-file diagnostics) is inert until the file is fixed.  Errors
 *  surface via window/showMessage as Error severity to make the
 *  degradation visible.
 *
 *  Updates the mtime/size cache on every call (even on failure) so
 *  the stat-poll does not re-trigger until the file changes again. */
static void reload_compile_commands(void) {
    g_cc_attempted = 1;

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
        for (int i = 0; i < n; i++) {
            if (!entries[i].file_abs) continue;
            load_file_from_disk(entries[i].file_abs);
            /* Tag the doc that holds this compile_commands entry as a
             * project root.  build_workspace_snapshot() seeds one project
             * per is_cc_root doc and BFS-walks its include closure. */
            char *uri = path_to_uri(entries[i].file_abs);
            Document *root = uri ? doc_find(uri) : NULL;
            free(uri);
            if (root) root->is_cc_root = 1;
        }
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
        reload_compile_commands();
    }
}

/** Recursively sum the `num_dependencies` across @p n and its subtree. */
static int dependency_count_subtree(const tj_node *n) {
    if (!n) return 0;
    int total = n->num_dependencies;
    for (int i = 0; i < n->num_children; i++)
        total += dependency_count_subtree(n->children[i]);
    return total;
}

/** True when @p d declares a project block (scans root's top-level
 *  children for a KW_PROJECT node). */
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
 *  `deps=` shows the total number of captured `depends` + `precedes`
 *  references across every task in the document.  @p ws is the freshly
 *  published snapshot, consulted for project membership and count.
 *  Caller must hold docs_mutex. */
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

/* Build a fresh workspace snapshot from the current docs[] and atomically
 * swap it in as the published g_ws, releasing the previous one (which an
 * in-flight query may still be reading — it survives until that query
 * releases its ref).  Coordinator-only. */
static void publish_workspace_snapshot(void) {
    workspace_snapshot *fresh = build_workspace_snapshot();
    workspace_snapshot *old   = g_ws;
    g_ws = fresh;
    ws_release(old);
}

static void revalidate_all_docs(void) {
    maybe_reload_compile_commands();
    publish_workspace_snapshot();
    republish_all_diagnostics();
    /* Hand the freshly published snapshot to the per-project diagnostics
     * workers (spawning/retiring them as projects appear/disappear).  Every
     * snapshot-updating notification funnels through here, so this single
     * call covers didOpen/didChange/didClose/watched-files/rename/cc-reload. */
    diag_registry_update(g_ws);
    dump_docs_to_stderr("revalidate_all_docs", g_ws);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Handlers
   ═══════════════════════════════════════════════════════════════════════════ */

static yyjson_mut_val *handle_initialize(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    if (params && !g_workspace_root) {
        yyjson_val *root_uri_val = yyjson_obj_get(params, "rootUri");
        if (root_uri_val && yyjson_is_str(root_uri_val)) {
            g_workspace_root = uri_to_path(yyjson_get_str(root_uri_val));
        }
    }
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

    yyjson_mut_val *server_info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, server_info, "name",    "taskjuggler-lsp");
    yyjson_mut_obj_add_str(doc, server_info, "version", VERSION_STRING);

    yyjson_mut_val *comp_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, comp_triggers, "!");
    yyjson_mut_arr_add_str(doc, comp_triggers, ".");
    yyjson_mut_val *comp_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc,  comp_opts, "triggerCharacters", comp_triggers);
    yyjson_mut_obj_add_bool(doc, comp_opts, "resolveProvider", false);

    yyjson_mut_val *sig_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, sig_triggers, " ");
    yyjson_mut_val *sig_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sig_opts, "triggerCharacters", sig_triggers);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tds = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, tds, "change", 2);
    yyjson_mut_obj_add_bool(doc, tds, "openClose", true);
    yyjson_mut_val *sem_types = yyjson_mut_arr(doc);
    for (int i = 0; i < num_semantic_token_types; i++)
        yyjson_mut_arr_add_str(doc, sem_types, semantic_token_type_names[i]);
    yyjson_mut_val *sem_mods = yyjson_mut_arr(doc);
    for (int i = 0; i < num_semantic_token_modifiers; i++)
        yyjson_mut_arr_add_str(doc, sem_mods, semantic_token_modifier_names[i]);
    yyjson_mut_val *sem_legend = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sem_legend, "tokenTypes",     sem_types);
    yyjson_mut_obj_add_val(doc, sem_legend, "tokenModifiers", sem_mods);
    yyjson_mut_val *sem_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc,  sem_opts, "legend", sem_legend);
    yyjson_mut_val *sem_full = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, sem_full, "delta", true);
    yyjson_mut_obj_add_val(doc, sem_opts, "full", sem_full);

    yyjson_mut_obj_add_val(doc,  caps, "textDocumentSync",          tds);
    yyjson_mut_obj_add_bool(doc, caps, "documentSymbolProvider",    true);
    yyjson_mut_obj_add_bool(doc, caps, "foldingRangeProvider",      true);
    yyjson_mut_val *code_lens_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, code_lens_opts, "resolveProvider", false);
    yyjson_mut_obj_add_val(doc,  caps, "codeLensProvider",          code_lens_opts);
    yyjson_mut_obj_add_bool(doc, caps, "hoverProvider",             true);
    yyjson_mut_obj_add_bool(doc, caps, "definitionProvider",        true);
    yyjson_mut_obj_add_bool(doc, caps, "referencesProvider",        true);
    yyjson_mut_obj_add_bool(doc, caps, "documentHighlightProvider", true);
    yyjson_mut_obj_add_val(doc,  caps, "signatureHelpProvider",     sig_opts);
    yyjson_mut_obj_add_val(doc,  caps, "completionProvider",        comp_opts);
    yyjson_mut_obj_add_val(doc,  caps, "semanticTokensProvider",    sem_opts);
    yyjson_mut_obj_add_bool(doc, caps, "workspaceSymbolProvider",   true);

    yyjson_mut_val *rename_filter_tjp = yyjson_mut_obj(doc);
    yyjson_mut_val *pattern_tjp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, pattern_tjp, "glob", "**/*.tjp");
    yyjson_mut_obj_add_val(doc, rename_filter_tjp, "pattern", pattern_tjp);
    yyjson_mut_val *rename_filter_tji = yyjson_mut_obj(doc);
    yyjson_mut_val *pattern_tji = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, pattern_tji, "glob", "**/*.tji");
    yyjson_mut_obj_add_val(doc, rename_filter_tji, "pattern", pattern_tji);
    yyjson_mut_val *rename_filters = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(rename_filters, rename_filter_tjp);
    yyjson_mut_arr_add_val(rename_filters, rename_filter_tji);
    yyjson_mut_val *did_rename_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, did_rename_opts, "filters", rename_filters);
    yyjson_mut_val *file_ops = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, file_ops, "didRename", did_rename_opts);
    yyjson_mut_val *workspace_folders_caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, workspace_folders_caps, "supported", false);
    yyjson_mut_val *workspace_caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, workspace_caps, "workspaceFolders", workspace_folders_caps);
    yyjson_mut_obj_add_val(doc, workspace_caps, "fileOperations",   file_ops);
    yyjson_mut_obj_add_val(doc, caps, "workspace", workspace_caps);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, result, "capabilities", caps);
    yyjson_mut_obj_add_val(doc, result, "serverInfo",   server_info);

    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_shutdown(yyjson_mut_doc *doc, yyjson_val *id) {
    return make_response(doc, id, yyjson_mut_null(doc));
}

static void handle_initialized(void) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *watcher_tjp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, watcher_tjp, "globPattern", "**/*.tjp");
    yyjson_mut_val *watcher_tji = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, watcher_tji, "globPattern", "**/*.tji");
    yyjson_mut_val *watchers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(watchers, watcher_tjp);
    yyjson_mut_arr_add_val(watchers, watcher_tji);

    yyjson_mut_val *register_options = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, register_options, "watchers", watchers);

    yyjson_mut_val *registration = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, registration, "id",     "file-watcher");
    yyjson_mut_obj_add_str(doc, registration, "method", "workspace/didChangeWatchedFiles");
    yyjson_mut_obj_add_val(doc, registration, "registerOptions", register_options);

    yyjson_mut_val *registrations = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(registrations, registration);

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, params, "registrations", registrations);

    yyjson_mut_val *request = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, request, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, request, "id",      "watcher-reg");
    yyjson_mut_obj_add_str(doc, request, "method",  "client/registerCapability");
    yyjson_mut_obj_add_val(doc, request, "params",  params);

    yyjson_mut_doc_set_root(doc, request);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    lsp_send_message(text);
    free(text);

    /* compile_commands.json is now the only docs[] populator at startup
     * (besides editor didOpen events).  follow_includes() inside
     * load_file_from_disk() still cascades into the .tji closure of each
     * listed .tjp; nothing else is pulled in from the workspace tree. */
    reload_compile_commands();
    if (g_workspace_root) revalidate_all_docs();
}

static void handle_did_change_watched_files(yyjson_val *params) {
    if (!params) return;
    yyjson_val *changes = yyjson_obj_get(params, "changes");
    if (!changes || !yyjson_is_arr(changes)) return;

    int changed = 0;

    size_t idx, max;
    yyjson_val *event;
    yyjson_arr_foreach(changes, idx, max, event) {
        const char *uri = json_str(event, "uri");
        yyjson_val *type_item = yyjson_obj_get(event, "type");
        if (!uri || !type_item || !yyjson_is_num(type_item)) continue;
        int type = (int)yyjson_get_num(type_item);

        if (type == 3) {
            Document *document = doc_find(uri);
            if (document && document->disk_only) {
                doc_free(document);
                changed = 1;
            }
        } else {
            Document *document = doc_find(uri);
            if (document && !document->disk_only) continue;

            char *path = uri_to_path(uri);
            if (!path) continue;
            char *text = read_file(path);
            if (!text) { free(path); continue; }

            if (!document) document = doc_alloc(uri);
            if (!document) { free(text); free(path); continue; }

            free(document->text);
            document->text      = text;
            document->disk_only = 1;
            ParseOutput *po = parse(text);
            follow_includes(path, po);
            doc_install_parse(document, po);
            free(path);
            changed = 1;
        }
    }

    if (changed) revalidate_all_docs();
}

static void handle_did_rename_files(yyjson_val *params) {
    if (!params) return;
    yyjson_val *files = yyjson_obj_get(params, "files");
    if (!files || !yyjson_is_arr(files)) return;

    int changed = 0;

    size_t idx, max;
    yyjson_val *file_item;
    yyjson_arr_foreach(files, idx, max, file_item) {
        const char *old_uri = json_str(file_item, "oldUri");
        const char *new_uri = json_str(file_item, "newUri");
        if (!old_uri || !new_uri) continue;

        Document *old_doc = doc_find(old_uri);
        if (old_doc) {
            if (!old_doc->disk_only)
                publish_diagnostics(old_uri);
            doc_free(old_doc);
            changed = 1;
        }

        char *path = uri_to_path(new_uri);
        if (!path) continue;
        char *text = read_file(path);
        if (!text) { free(path); continue; }

        Document *new_doc = doc_find(new_uri);
        if (!new_doc) new_doc = doc_alloc(new_uri);
        if (!new_doc) { free(text); free(path); continue; }

        free(new_doc->text);
        new_doc->text      = text;
        new_doc->disk_only = 1;
        ParseOutput *po = parse(text);
        follow_includes(path, po);
        doc_install_parse(new_doc, po);
        free(path);
        changed = 1;
    }

    if (changed) revalidate_all_docs();
}

static void handle_didopen(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri  = json_str(tdi, "uri");
    const char *text = json_str(tdi, "text");
    if (!uri || !text) return;

    Document *d = doc_find(uri);
    if (d) {
        if (!d->disk_only) return; /* duplicate open: client error */
        if (d->text && strcmp(d->text, text) == 0) {
            d->disk_only = 0;
            revalidate_all_docs();
            return;
        }
        free(d->text);
        d->text = NULL;
        doc_clear_parse_state(d);
    } else {
        d = doc_alloc(uri);
        if (!d) return;
    }
    d->disk_only = 0;
    d->text  = strdup(text);
    ParseOutput *po = parse(text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, po);
        free(path);
    }
    doc_install_parse(d, po);

    revalidate_all_docs();
}

static void handle_didchange(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi     = yyjson_obj_get(params, "textDocument");
    yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
    if (!tdi || !changes || !yyjson_is_arr(changes)) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    if (yyjson_arr_size(changes) == 0) return;

    Document *d = doc_find(uri);
    if (!d) return;

    char *current = d->text ? strdup(d->text) : strdup("");
    if (!current) return;

    size_t idx, max;
    yyjson_val *change;
    yyjson_arr_foreach(changes, idx, max, change) {
        yyjson_val *range_obj = yyjson_obj_get(change, "range");
        const char *new_text  = yyjson_get_str(yyjson_obj_get(change, "text"));
        if (!new_text) { free(current); return; }

        if (range_obj) {
            LspRange range;
            range.start = json_to_pos(yyjson_obj_get(range_obj, "start"));
            range.end   = json_to_pos(yyjson_obj_get(range_obj, "end"));
            char *next = apply_incremental_change(current, range, new_text);
            free(current);
            if (!next) return;
            current = next;
        } else {
            free(current);
            current = strdup(new_text);
            if (!current) return;
        }
    }

    free(d->text);
    d->text = current;
    ParseOutput *po = parse(d->text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, po);
        free(path);
    }
    doc_install_parse(d, po);

    revalidate_all_docs();
}

static void handle_didclose(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    Document *d = doc_find(uri);
    if (!d) return;

    char *path = uri_to_path(uri);
    char *text = path ? read_file(path) : NULL;

    if (text) {
        free(d->text);
        d->text      = text;
        d->disk_only = 1;
        ParseOutput *po = parse(text);
        follow_includes(path, po);
        doc_install_parse(d, po);
    } else {
        publish_diagnostics(uri);
        doc_free(d);
    }
    free(path);

    revalidate_all_docs();
}

/* ── Read-only query handlers ─────────────────────────────────────────────
 *
 * Each handler takes `d` (the primary query_doc, cloned from the live store
 * by server_coordinate_query() under docs_mutex) and returns the response
 * JSON.  Handlers run on a query worker with no lock held: every pointer
 * they touch lives inside the Job's private query_context, so a concurrent
 * notification mutating the live store cannot disturb them.  Cross-document
 * handlers also receive `qc` for its sibling-document pools and project
 * tree.
 */

/** A document's top-level symbol pool: the children of its synthetic
 *  `root`, in source order, or an empty pool when @p d has no parse.
 *  The pointers are borrowed from @p d — never freed by the caller. */
static void doc_symbol_pool(const query_doc *d, tj_node *const **out_top, int *out_n) {
    if (d && d->root) {
        *out_top = (tj_node *const *)d->root->children;
        *out_n   = d->root->num_children;
    } else {
        *out_top = NULL;
        *out_n   = 0;
    }
}

static yyjson_mut_val *handle_document_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                               yyjson_val *params, const query_doc *d) {
    (void)params;
    if (!d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    /* root->children are already every top-level declaration in source
     * order (project block included).  TODO(document-symbol): once the
     * project block reliably contains its children in the hierarchical
     * sense, nest the in-project entries under it. */
    tj_node *const *top = (tj_node *const *)d->root->children;
    int             w   = d->root->num_children;

    size_t  json_len = 0;
    char   *json     = build_document_symbols_json(top, w, &json_len);
    if (!json) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_mut_val *raw = yyjson_mut_rawncpy(doc, json, json_len);
    free(json);
    return make_response(doc, id, raw);
}

/* Map a per-document task tj_node to its clone in @p qc's assembled
 * Project tree.  Cursor lookups (dependency_at_cursor / task_decl_at_cursor)
 * return per-document nodes, but cross-file resolution lives in the
 * ProjectNode tree, so callers bridge through here first.
 *
 * The per-document task's unprefixed dotted id (its in-file ancestry) is
 * appended to @p d's task prefix target inside the Project tree.  Returns
 * NULL when @p qc has no project tree, the prefix target is missing, or the
 * path does not resolve. */
static ProjectNode *project_node_for_doc_task(const query_context *qc,
                                              const query_doc *d,
                                              const tj_node *per_doc_task) {
    if (!qc || !qc->project_root || !d || !per_doc_task) return NULL;

    char *qid = sym_qualified_id(per_doc_task);   /* unprefixed in-file path */
    if (!qid || !qid[0]) { free(qid); return NULL; }

    ProjectNode *cur = find_node_by_dotted_path(qc->project_root, d->task_prefix,
                                                NODE_KIND_TASK);
    char *save = NULL;
    for (char *seg = strtok_r(qid, ".", &save); seg && cur;
         seg = strtok_r(NULL, ".", &save)) {
        ProjectNode *next = NULL;
        for (int i = 0; i < cur->num_children && !next; i++) {
            ProjectNode *child = cur->children[i];
            if (child->keyword == KW_TASK && child->id &&
                strcmp(child->id, seg) == 0)
                next = child;
        }
        cur = next;
    }
    free(qid);
    return cur;
}

static yyjson_mut_val *handle_folding_range(yyjson_mut_doc *doc, yyjson_val *id,
                                             yyjson_val *params, const query_doc *d) {
    (void)params;
    if (!d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *const *top; int n;
    doc_symbol_pool(d, &top, &n);
    yyjson_mut_val *arr = build_folding_ranges_json(doc,
                                                     d->tok_spans,
                                                     d->num_tok_spans,
                                                     top, n);
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_code_lens(yyjson_mut_doc *doc, yyjson_val *id,
                                         yyjson_val *params, const query_doc *d) {
    (void)params;
    if (!d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *const *top; int n;
    doc_symbol_pool(d, &top, &n);
    yyjson_mut_val *arr = build_code_lens_json(doc,
                                                d->tok_spans,
                                                d->num_tok_spans,
                                                top, n);
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_hover(yyjson_mut_doc *doc, yyjson_val *id,
                                     yyjson_val *params, const query_context *qc,
                                     const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);

    /* A cursor on a dependency reference resolves to its target task; show
     * the target's qualified id and name.  Falls through to keyword docs
     * when the cursor is not on a dependency or the reference is unresolved. */
    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    if (qc->project_root &&
        dependency_at_cursor(d->tok_spans, d->num_tok_spans, pos,
                             &owner, &dep)) {
        ProjectNode *merged_owner = project_node_for_doc_task(qc, d, owner);
        ProjectNode *target = NULL;
        if (merged_owner) {
            int ordinal = (int)(dep - owner->dependencies);
            if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                target = project_dep_resolve(merged_owner, ordinal,
                                             qc->project_root);
        }
        if (target) {
            char *value = project_node_hover_markdown(target);
            yyjson_mut_val *contents = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, contents, "kind", "markdown");
            yyjson_mut_obj_add_strcpy(doc, contents, "value", value);
            free(value);

            yyjson_mut_val *hover = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, hover, "contents", contents);
            yyjson_mut_obj_add_val(doc, hover, "range",
                                   range_json(doc, dep->source_range));
            return make_response(doc, id, hover);
        }
    }

    ActiveKeyword ak = active_keyword_at(d->tok_spans,
                                          d->num_tok_spans, pos);
    if (!ak.keyword) return make_response(doc, id, yyjson_mut_null(doc));

    const char *doc_text = keyword_docs(ak.keyword);
    free(ak.keyword);
    if (!doc_text) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *contents = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, contents, "kind",  "markdown");
    yyjson_mut_obj_add_str(doc, contents, "value", doc_text);

    yyjson_mut_val *hover = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, hover, "contents", contents);
    yyjson_mut_obj_add_val(doc, hover, "range",    range_json(doc, ak.range));

    return make_response(doc, id, hover);
}

static yyjson_mut_val *handle_signature_help(yyjson_mut_doc *doc, yyjson_val *id,
                                              yyjson_val *params, const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    ActiveContext ac = active_context(d->tok_spans,
                                       d->num_tok_spans, pos);
    if (!ac.keyword) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *sig = build_signature_help_json(doc, ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, sig);
}

/* The semanticTokens resultId is the document's parse version: it is stable
 * per snapshot (two requests against the same parse share one id) and lets a
 * delta diff by version with no write-back, so these run lock-free against
 * the pinned snapshot like any other query.  The token data itself is
 * memoized inside the doc_snapshot (docsnap_sem_tokens). */
static void sem_tokens_result_id(doc_snapshot *s, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%" PRIu64, s->doc_version);
}

static yyjson_mut_val *handle_semantic_tokens_full(yyjson_mut_doc *doc, yyjson_val *id,
                                                    yyjson_val *params, const query_doc *d) {
    (void)params;
    if (!d || !d->snap || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    const uint32_t *buf = NULL;
    size_t          count = 0;
    docsnap_sem_tokens(d->snap, &buf, &count);

    char result_id[32];
    sem_tokens_result_id(d->snap, result_id, sizeof(result_id));
    yyjson_mut_val *result =
        build_semantic_tokens_json_from_buf(doc, buf, count, result_id);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                          yyjson_val *params,
                                                          const query_context *qc,
                                                          const query_doc *d) {
    const char *previous_result_id = params ? json_str(params, "previousResultId") : NULL;
    if (!d || !d->snap || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    const uint32_t *new_buf = NULL;
    size_t          new_count = 0;
    docsnap_sem_tokens(d->snap, &new_buf, &new_count);

    char result_id[32];
    sem_tokens_result_id(d->snap, result_id, sizeof(result_id));

    /* Identify the snapshot the client is holding by its resultId (== that
     * snapshot's parse version): either the current parse (no change since)
     * or the immediately previous one retained on the query_context.  Diff
     * against that base; otherwise fall back to a full response. */
    doc_snapshot *base = NULL;
    if (previous_result_id) {
        uint64_t prev_version = strtoull(previous_result_id, NULL, 10);
        if (prev_version == d->snap->doc_version)
            base = d->snap;
        else if (qc->prev_snap && prev_version == qc->prev_snap->doc_version)
            base = qc->prev_snap;
    }

    yyjson_mut_val *result;
    if (base) {
        const uint32_t *old_buf = NULL;
        size_t          old_count = 0;
        docsnap_sem_tokens(base, &old_buf, &old_count);
        result = build_semantic_tokens_delta_json(doc, old_buf, old_count,
                                                   new_buf, new_count, result_id);
    } else {
        result = build_semantic_tokens_json_from_buf(doc, new_buf, new_count, result_id);
    }
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_references(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, const query_context *qc,
                                          const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!qc->project_root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *task = task_decl_at_cursor(d->tok_spans, d->num_tok_spans, pos);
    ProjectNode *wanted = project_node_for_doc_task(qc, d, task);
    yyjson_mut_val *result = build_references_json(doc,
                                                    qc->project_root,
                                                    wanted);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc,
                                                  yyjson_val *id,
                                                  yyjson_val *params,
                                                  const query_context *qc,
                                                  const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root || !qc->project_root)
        return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);

    /* Resolve the cursor to a single target task, triggering from either a
     * task declaration or a dependency reference (the target is the same in
     * both directions).  Resolution runs against the pinned snapshot's
     * ProjectNode tree, exactly as definition/references do. */
    ProjectNode *wanted = NULL;
    tj_node *decl = task_decl_at_cursor(d->tok_spans, d->num_tok_spans, pos);
    if (decl) {
        wanted = project_node_for_doc_task(qc, d, decl);
    } else {
        tj_node          *owner = NULL;
        const Dependency *dep   = NULL;
        if (dependency_at_cursor(d->tok_spans, d->num_tok_spans, pos,
                                 &owner, &dep)) {
            ProjectNode *merged_owner = project_node_for_doc_task(qc, d, owner);
            if (merged_owner) {
                int ordinal = (int)(dep - owner->dependencies);
                if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                    wanted = project_dep_resolve(merged_owner, ordinal,
                                                 qc->project_root);
            }
        }
    }

    yyjson_mut_val *result =
        build_document_highlight_json(doc, qc->project_root, wanted,
                                      d->uri, d->tok_spans, d->num_tok_spans);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, const query_context *qc,
                                          const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!qc->project_root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    yyjson_mut_val   *result = NULL;
    if (dependency_at_cursor(d->tok_spans, d->num_tok_spans, pos,
                             &owner, &dep)) {
        ProjectNode *merged_owner = project_node_for_doc_task(qc, d, owner);
        if (merged_owner) {
            int ordinal = (int)(dep - owner->dependencies);
            if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                result = build_definition_json(doc, merged_owner, ordinal,
                                               qc->project_root);
        }
    }
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_completion(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, const query_context *qc,
                                          const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    /* Every non-primary doc in the context is a sibling in the requester's
     * project (the clone step already restricted membership), so each one's
     * top-level pool is an extra cross-file completion source.  Orphans
     * have no siblings, so the loop naturally yields no extras for them. */
    tj_node *const *extra_pools[MAX_DOCS];
    int             extra_counts[MAX_DOCS];
    int             num_extra = 0;
    for (int i = 0; i < qc->num_docs && num_extra < MAX_DOCS; i++) {
        if (qc->docs[i].is_primary) continue;
        tj_node *const *top; int n;
        doc_symbol_pool(&qc->docs[i], &top, &n);
        if (!top) continue;
        extra_pools[num_extra]  = top;
        extra_counts[num_extra] = n;
        num_extra++;
    }

    tj_node *const *self_top; int self_n;
    doc_symbol_pool(d, &self_top, &self_n);

    LspPos pos             = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_completions_json(doc,
                                                     d->tok_spans,
                                                     d->num_tok_spans,
                                                     pos,
                                                     self_top, self_n,
                                                     extra_pools,
                                                     extra_counts,
                                                     num_extra,
                                                     d->text);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                                yyjson_val *params,
                                                const query_context *qc) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < qc->num_docs; i++) {
        if (!qc->docs[i].root) continue;
        tj_node *const *top; int n;
        doc_symbol_pool(&qc->docs[i], &top, &n);
        if (!top) continue;
        collect_workspace_symbols(doc, query, top, n, qc->docs[i].uri, arr);
    }
    return make_response(doc, id, arr);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main dispatch
   ═══════════════════════════════════════════════════════════════════════════ */

static int is_notification_method(const char *method) {
    if (!method) return 0;
    return strcmp(method, "initialized") == 0
        || strcmp(method, "textDocument/didOpen") == 0
        || strcmp(method, "textDocument/didChange") == 0
        || strcmp(method, "textDocument/didClose") == 0
        || strcmp(method, "workspace/didChangeWatchedFiles") == 0
        || strcmp(method, "workspace/didRenameFiles") == 0;
}

static void send_response(yyjson_mut_doc *out_doc, yyjson_mut_val *resp) {
    yyjson_mut_doc_set_root(out_doc, resp);
    char *text = yyjson_mut_write(out_doc, 0, NULL);
    if (text) {
        lsp_send_message(text);
        free(text);
    }
}

void server_dispatch_notification(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    pthread_mutex_lock(&docs_mutex);

    if (strcmp(m, "initialized") == 0) {
        handle_initialized();
    } else if (strcmp(m, "textDocument/didOpen") == 0) {
        handle_didopen(params);
    } else if (strcmp(m, "textDocument/didChange") == 0) {
        handle_didchange(params);
    } else if (strcmp(m, "textDocument/didClose") == 0) {
        handle_didclose(params);
    } else if (strcmp(m, "workspace/didChangeWatchedFiles") == 0) {
        handle_did_change_watched_files(params);
    } else if (strcmp(m, "workspace/didRenameFiles") == 0) {
        handle_did_rename_files(params);
    }

    pthread_mutex_unlock(&docs_mutex);
}

void server_dispatch_cancelled(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    if (!id_item) return;

    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *resp = make_error_response(out_doc, id_item, -32800,
                                                "Request cancelled");
    send_response(out_doc, resp);
    yyjson_mut_doc_free(out_doc);
}

/* Pull the request's primary document URI out of `params`, accepting both
 * the flat `textDocument` form and the nested `textDocumentPosition` form. */
static const char *request_primary_uri(yyjson_val *params) {
    if (!params) return NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) {
        const char *uri = json_str(td, "uri");
        if (uri) return uri;
    }
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (tdp) {
        td = yyjson_obj_get(tdp, "textDocument");
        if (td) return json_str(td, "uri");
    }
    return NULL;
}

/* Build the query_context for one query under docs_mutex by pinning the
 * currently published workspace snapshot (an O(1) refcount bump) and pointing
 * borrowed query_doc views at its ws_docs.  When @p want_all_docs is set
 * (workspace/symbol) every document is included and there is no primary;
 * otherwise the context holds the primary document plus its same-project
 * siblings.  The primary's previous doc_snapshot is also ref'd for
 * semanticTokens/delta.  Caller must hold docs_mutex. */
static query_context *build_query_context_locked(const char *primary_uri,
                                                 int want_all_docs) {
    query_context *qc = calloc(1, sizeof(query_context));
    if (!qc) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    qc->primary_idx = -1;

    /* Resolve the primary document and retain its previous snapshot for
     * delta, even if it is not represented in the snapshot yet. */
    Document *primary_doc = primary_uri ? doc_find(primary_uri) : NULL;
    if (primary_doc)
        qc->prev_snap = docsnap_acquire(primary_doc->prev_snap);
    const char *primary_canon = primary_doc ? primary_doc->uri : NULL;

    workspace_snapshot *ws = ws_acquire(g_ws);
    qc->ws = ws;
    if (!ws) return qc;   /* no snapshot yet: empty context, handlers return null */

    /* Locate the primary ws_doc and its project. */
    int primary_proj = -1;
    if (primary_canon) {
        for (int i = 0; i < ws->num_docs; i++) {
            doc_snapshot *s = ws->docs[i].snap;
            if (s && s->uri && strcmp(s->uri, primary_canon) == 0) {
                primary_proj = ws->docs[i].project_index;
                break;
            }
        }
    }

    if (ws->num_docs > 0) {
        qc->docs = calloc((size_t)ws->num_docs, sizeof(query_doc));
        if (!qc->docs) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    }

    int n = 0;
    for (int i = 0; i < ws->num_docs; i++) {
        ws_doc       *w = &ws->docs[i];
        doc_snapshot *s = w->snap;
        if (!s) continue;
        int is_primary = (primary_canon && s->uri && strcmp(s->uri, primary_canon) == 0);
        if (!is_primary && !want_all_docs) {
            /* Same-project siblings only; an orphan's singleton project has
             * no siblings, so the primary stands alone. */
            if (primary_proj < 0 || w->project_index != primary_proj) continue;
        }
        qc->docs[n] = (query_doc){
            .uri             = s->uri,
            .text            = s->text,
            .task_prefix     = w->task_prefix,
            .account_prefix  = w->account_prefix,
            .report_prefix   = w->report_prefix,
            .resource_prefix = w->resource_prefix,
            .root            = s->root,
            .tok_spans       = s->tok_spans,
            .num_tok_spans   = s->num_tok_spans,
            .num_sem_entries = s->num_sem_entries,
            .is_primary      = is_primary,
            .snap            = s,
        };
        if (is_primary) qc->primary_idx = n;
        n++;
    }
    qc->num_docs = n;

    if (primary_proj >= 0 && primary_proj < ws->num_projects) {
        qc->project_root = &ws->projects[primary_proj]->root;
        qc->project_id   = ws->projects[primary_proj]->id;
    }
    return qc;
}

/* Only the state-mutating lifecycle methods run inline on the coordinator;
 * every other query (semanticTokens included, now that its data and resultId
 * live in the snapshot) is served lock-free from a pinned snapshot. */
static int is_inline_query_method(const char *m) {
    return strcmp(m, "initialize") == 0
        || strcmp(m, "shutdown") == 0;
}

int server_coordinate_query(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    if (is_inline_query_method(m)) {
        yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *resp = NULL;

        pthread_mutex_lock(&docs_mutex);
        if (strcmp(m, "initialize") == 0) {
            resp = handle_initialize(out_doc, id_item, params);
        } else {
            resp = handle_shutdown(out_doc, id_item);
        }
        pthread_mutex_unlock(&docs_mutex);

        if (resp) send_response(out_doc, resp);
        yyjson_mut_doc_free(out_doc);
        return 1;   /* handled inline; coordinator frees the Job */
    }

    /* Every other query is served from a pinned snapshot.  Build the context
     * under the lock and let the coordinator hand the Job to the worker pool. */
    int want_all_docs = (strcmp(m, "workspace/symbol") == 0);

    pthread_mutex_lock(&docs_mutex);
    const char *primary_uri = request_primary_uri(params);
    job->context = build_query_context_locked(primary_uri, want_all_docs);
    pthread_mutex_unlock(&docs_mutex);

    return 0;   /* ownership transferred to the worker pool */
}

void server_run_query(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    const query_context *qc = job->context;
    const query_doc     *d  = query_context_primary(qc);

    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *resp = NULL;

    if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/foldingRange") == 0) {
        resp = handle_folding_range(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/codeLens") == 0) {
        resp = handle_code_lens(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "textDocument/documentHighlight") == 0) {
        resp = handle_document_highlight(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/semanticTokens/full/delta") == 0) {
        resp = handle_semantic_tokens_full_delta(out_doc, id_item, params, qc, d);
    } else if (strcmp(m, "workspace/symbol") == 0) {
        resp = handle_workspace_symbol(out_doc, id_item, params, qc);
    } else if (id_item) {
        resp = make_response(out_doc, id_item, yyjson_mut_null(out_doc));
    }

    if (resp) send_response(out_doc, resp);
    yyjson_mut_doc_free(out_doc);
}

void server_process(const char *json_text) {
    yyjson_doc *in_doc = yyjson_read(json_text, strlen(json_text), 0);
    if (!in_doc) return;

    yyjson_val *root   = yyjson_doc_get_root(in_doc);
    yyjson_val *method = yyjson_obj_get(root, "method");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    if (strcmp(m, "exit") == 0) {
        yyjson_doc_free(in_doc);
        threadpool_stop();
        diag_registry_shutdown();
        exit(0);
    }

    if (strcmp(m, "$/cancelRequest") == 0) {
        yyjson_val *p      = yyjson_obj_get(root, "params");
        yyjson_val *id_val = p ? yyjson_obj_get(p, "id") : NULL;
        if (id_val && yyjson_is_int(id_val)) {
            threadpool_cancel_by_id(yyjson_get_sint(id_val));
        }
        yyjson_doc_free(in_doc);
        return;
    }

    Job *job = calloc(1, sizeof(Job));
    if (!job) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    job->request_doc = in_doc;

    yyjson_val *id_item = yyjson_obj_get(root, "id");
    if (id_item && yyjson_is_int(id_item)) {
        job->has_id = 1;
        job->id     = yyjson_get_sint(id_item);
    }

    job->is_notification = is_notification_method(m);
    threadpool_enqueue_job(job);
}

void server_init() {
    for (int i = 0; i < MAX_DOCS; i++)
        docs[i].in_use = 0;
}
