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
 * TODO(workspace-snapshot): query workers used to operate on a refcounted
 * workspace snapshot so they did not block notifications.  That model
 * relied on per-Document ParseResult refcounting and was retired during
 * the tj_node refactor.  Until a replacement lands, server_dispatch_query
 * acquires docs_mutex for the full handler so the live docs[] array stays
 * consistent under it.
 */

#include "server.h"
#include "parser.h"
#include "project_tree.h"
#include "workspace_snapshot.h"
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
#include <sys/mman.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Document store
   ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_DOCS 64

struct Project;

/** Document store slot.  Each slot owns the parse-derived state directly
 *  (per-kind synthetic roots, tokens, include filenames); parse() returns
 *  a transient ParseOutput whose fields get moved here by doc_install_parse(). */
typedef struct Document {
    char        *uri;
    char        *text;
    SemanticTokenResult sem_tokens; /**< cached semantic-tokens response */
    _Atomic uint64_t doc_version;   /**< bumped on every text/parse swap */
    int          in_use;
    int          disk_only;
    int          is_cc_root;        /**< 1 when this doc is named directly in compile_commands.json */

    /* Parse-derived state.  `slab` is non-NULL after a successful parse
     * and NULL before one has happened (use `slab` as the "has-parse"
     * sentinel).  Freed and replaced wholesale by doc_install_parse();
     * freed by doc_free() on slot release. */
    parse_slab  *slab;

    /* Prefixes applied to this Document by the includer's `include` block,
     * one per kind.  Populated by follow_includes() from the includer's
     * captured IncludeRef when this file is pulled in; stay NULL on a
     * canonical .tjp or on orphan .tji files in a .tji-only workspace. */
    char        *task_prefix;
    char        *account_prefix;
    char        *report_prefix;
    char        *resource_prefix;

    /* Resolved file:// URIs of every `include` directive in this doc,
     * recorded by follow_includes() at parse time.  Owned by the
     * Document; cleared at the top of each follow_includes() run and
     * freed by doc_free().  Lets rebuild_all_projects() walk the
     * include graph without re-parsing or threading state through the
     * load pipeline. */
    char       **included_uris;
    int          num_included_uris;
    int          included_uris_cap;

    /* Project this document belongs to, computed by
     * rebuild_all_projects() on every notification.  Borrowed pointer;
     * the projects[] registry owns the Project itself.  NULL between
     * parse and the next rebuild_all_projects() call. */
    struct Project *primary_project;
} Document;

static Document docs[MAX_DOCS];

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

/* Forward declarations. */
static char *normalize_uri(const char *raw_uri);
static void  load_file_from_disk(const char *path);
static void  follow_includes(const char *file_path, const parse_slab *slab);
static void  rebuild_all_projects(void);
static void  reload_compile_commands(void);
static void  maybe_reload_compile_commands(void);

/* ── Per-Project ProjectNode tree ───────────────────────────────────────── *
 *
 * Each compile_commands.json entry becomes one Project; its transitive
 * include closure (followed via Document.included_uris[]) is deep-copied
 * into the single ProjectNode tree below with the includer's per-kind
 * prefix applied (see project_tree.h).  Built fresh by
 * rebuild_all_projects() on every notification.  Nodes of every kind
 * share one root: a node's `keyword` identifies its kind, so walkers
 * must filter on it to respect TaskJuggler's separate task / account /
 * resource / report id namespaces.
 *
 * This tree is the authoritative cross-file resolution surface: it is
 * prefix-applied (so dependency paths resolve against real qualified
 * ids) and each task node owns the dependency edges declared on it.
 * handle_definition / handle_references / handle_hover bridge the
 * per-document task under the cursor to its clone here (via
 * project_node_for_doc_task) and resolve against this tree.
 *
 * Each Document has a primary_project pointer set during the rebuild;
 * handlers route cross-file lookups through that pointer.  Orphan
 * editor-only files (didOpen for a doc not reached by any
 * compile_commands entry) are handled by a follow-up commit; until then
 * they end up with primary_project == NULL. */
typedef struct Project {
    char        *id;             /**< canonical .tjp URI from compile_commands.json */
    int          is_orphan;      /**< reserved for singleton editor-only projects (commit 3) */
    ProjectNode  root;           /**< synthetic root over all kinds, owned outright */
} Project;

static Project **projects;
static int       num_projects;
static int       cap_projects;

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
            docs[i].sem_tokens.next_result_id = 1;
            atomic_store(&docs[i].doc_version, 1);
            return &docs[i];
        }
    }
    free(canon);
    return NULL;
}

/** Release the parse-derived fields on @p d, zeroing each so the slot
 *  is reusable. */
static void doc_clear_parse_state(Document *d) {
    parse_slab_free(d->slab);
    d->slab = NULL;
}

/** Install @p slab as @p d's current parse state, releasing whatever
 *  @p d held previously.  Ownership of @p slab transfers to @p d. */
static void doc_install_parse(Document *d, parse_slab *slab) {
    doc_clear_parse_state(d);
    d->slab = slab;
    atomic_fetch_add(&d->doc_version, 1);
}

static void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    semantic_token_result_release(&d->sem_tokens);
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

static char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    return percent_decode(uri + 7);
}

static char *path_to_uri(const char *path) {
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
    parse_slab *slab = parse(text);
    follow_includes(path, slab);
    doc_install_parse(document, slab);
}

/** Replace @p *slot with a fresh strdup of @p value (NULL when @p value
 *  is NULL).  Used to copy IncludeRef prefix strings onto the includee. */
static void replace_string(char **slot, const char *value) {
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static void follow_includes(const char *file_path, const parse_slab *slab) {
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

    if (!slab || !slab->num_includes) return;

    size_t path_len = strlen(file_path);
    const char *last_slash = NULL;
    for (size_t i = path_len; i-- > 0; ) {
        if (file_path[i] == '/') { last_slash = file_path + i; break; }
    }
    size_t dir_len = last_slash ? (size_t)(last_slash - file_path) : 0;

    for (int i = 0; i < slab->num_includes; i++) {
        const IncludeRef *inc = &slab->includes[i];
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
         * rebuild_all_projects() can BFS the include graph.  Ownership
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
    for (char *seg = strtok_r(copy, ".", &save); seg && cur; seg = strtok_r(NULL, ".", &save)) {
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

/* ── Per-Project tree rebuild ──────────────────────────────────────────── */

static void project_free(Project *p) {
    if (!p) return;
    free(p->id);
    project_node_free_children(&p->root);
    free(p);
}

static void projects_clear(void) {
    for (int i = 0; i < num_projects; i++)
        project_free(projects[i]);
    num_projects = 0;
}

/** Copy each top-level declaration of @p d into @p p's tree, applying @p d's
 *  matching per-kind prefix.  Routes by the node's own `keyword` (the
 *  per-kind grouping the Document no longer keeps) to pick both the prefix
 *  and the namespace the prefix path is resolved within; the project block
 *  is document-local metadata and is skipped. */
static void copy_document_into_project(Project *p, Document *d) {
    if (!d->slab) return;
    tj_node *root = slab_node(d->slab, d->slab->root_idx);
    if (!root) return;
    tj_idx *kids = slab_children(d->slab, root);
    for (int i = 0; i < root->num_children; i++) {
        tj_idx      child_idx = kids[i];
        tj_node    *child = slab_node(d->slab, child_idx);
        if (!child) continue;
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
        ProjectNode *target = find_node_by_dotted_path(&p->root, prefix, kind);
        if (!target) continue;
        project_node_append_child(target,
                                  project_node_from_tj(d->slab, child_idx, d->uri));
    }
}

/** Look up @p uri in docs[] (with normalization fallback).  Returns
 *  NULL if no in-use slot matches. */
static Document *doc_find_by_uri(const char *uri) {
    return doc_find(uri);
}

/** BFS from @p root along included_uris[], deep-copying every reachable
 *  Document's top-level into @p p with prefixes applied.  Marks each
 *  visited doc's primary_project to @p p if not already claimed by a
 *  prior project. */
static void project_populate_from_root(Project *p, Document *root) {
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
    copy_document_into_project(p, root);
    if (!root->primary_project) root->primary_project = p;

    while (q_head < q_len) {
        Document *cur = queue[q_head++];
        for (int i = 0; i < cur->num_included_uris; i++) {
            Document *child = doc_find_by_uri(cur->included_uris[i]);
            if (!child || !child->slab) continue;
            int seen = 0;
            for (int v = 0; v < v_len && !seen; v++)
                if (visited[v] == child) seen = 1;
            if (seen) continue;
            PUSH(visited, v_len, v_cap, child);
            PUSH(queue,   q_len, q_cap, child);
            copy_document_into_project(p, child);
            if (!child->primary_project) child->primary_project = p;
        }
    }

cleanup:
    free(queue);
    free(visited);
    #undef PUSH
}

/** Append @p p to projects[], growing the array if needed.  Returns
 *  1 on success, 0 on OOM (caller must project_free(p) on failure). */
static int projects_append(Project *p) {
    if (num_projects >= cap_projects) {
        int new_cap = cap_projects ? cap_projects * 2 : 4;
        Project **tmp = realloc(projects, (size_t)new_cap * sizeof(Project *));
        if (!tmp) return 0;
        projects   = tmp;
        cap_projects = new_cap;
    }
    projects[num_projects++] = p;
    return 1;
}

/** Build one Project per is_cc_root Document, then one singleton
 *  "orphan" Project for every remaining in-use Document not reached by
 *  any cc_root's include closure.  Orphans exist so editor-opened
 *  files outside the compile_commands.json closure still get in-file
 *  LSP behavior (completion, hover, etc.) without bleeding into other
 *  projects' cross-file pools. */
static void rebuild_all_projects(void) {
    projects_clear();
    for (int i = 0; i < MAX_DOCS; i++)
        docs[i].primary_project = NULL;

    /* Pass 1: compile_commands roots + their include closures. */
    for (int i = 0; i < MAX_DOCS; i++) {
        Document *root = &docs[i];
        if (!root->in_use || !root->is_cc_root || !root->slab) continue;

        Project *p = calloc(1, sizeof(*p));
        if (!p) continue;
        p->id = root->uri ? strdup(root->uri) : NULL;
        if (!projects_append(p)) { project_free(p); continue; }

        project_populate_from_root(p, root);
    }

    /* Pass 2: unclaimed in-use docs each become their own singleton
     * orphan project.  Anchored on the doc's own top-level with no
     * prefix; the doc is its sole member. */
    for (int i = 0; i < MAX_DOCS; i++) {
        Document *d = &docs[i];
        if (!d->in_use || !d->slab || d->primary_project) continue;

        Project *p = calloc(1, sizeof(*p));
        if (!p) continue;
        p->id        = d->uri ? strdup(d->uri) : NULL;
        p->is_orphan = 1;
        if (!projects_append(p)) { project_free(p); continue; }

        copy_document_into_project(p, d);
        d->primary_project = p;
    }
}

/* Republish (now-empty) diagnostics for editor-managed documents after a
 * notification so the client clears stale markers from the pre-refactor
 * diagnostic stream. */
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
        show_message(1,
            "taskjuggler-lsp: no workspace root; cannot locate "
            "compile_commands.json.  No documents will be loaded; "
            "cross-file LSP features are disabled.");
        return;
    }

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
             * project root.  rebuild_all_projects() seeds one Project
             * per is_cc_root doc and BFS-walks its include closure. */
            char *uri = path_to_uri(entries[i].file_abs);
            Document *root = uri ? doc_find(uri) : NULL;
            free(uri);
            if (root) root->is_cc_root = 1;
        }
        break;
    case CC_NOT_FOUND:
        show_message(1,
            "taskjuggler-lsp: compile_commands.json not found at workspace "
            "root.  No documents will be loaded; create the file (a JSON "
            "array of { \"file\": \"<path>\" } entries) to enable "
            "cross-file LSP features.");
        break;
    case CC_PARSE_ERROR:
        show_message(1,
            "taskjuggler-lsp: compile_commands.json is not valid JSON; "
            "no documents loaded.  See server stderr for the parse error.");
        break;
    case CC_SCHEMA_ERROR:
        show_message(1,
            "taskjuggler-lsp: compile_commands.json does not match the "
            "expected schema (top-level JSON array of objects with a "
            "`file` field).  No documents loaded.");
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

static int dependency_count_subtree(const parse_slab *slab, const tj_node *n) {
    if (!n) return 0;
    int total = n->num_dependencies;
    tj_idx *kids = slab_children(slab, n);
    for (int i = 0; i < n->num_children; i++)
        total += dependency_count_subtree(slab, slab_node(slab, kids[i]));
    return total;
}

static int doc_has_project_block(const Document *d) {
    if (!d->slab) return 0;
    tj_node *root = slab_node(d->slab, d->slab->root_idx);
    if (!root) return 0;
    tj_idx *kids = slab_children(d->slab, root);
    for (int i = 0; i < root->num_children; i++) {
        tj_node *child = slab_node(d->slab, kids[i]);
        if (child && child->keyword == KW_PROJECT) return 1;
    }
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
 *  references across every task in the document.
 *  Caller must hold docs_mutex. */
static void dump_docs_to_stderr(const char *trigger) {
    int total = 0, editor = 0, disk = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        total++;
        if (docs[i].disk_only) disk++; else editor++;
    }
    fprintf(stderr,
            "taskjuggler-lsp: docs[] after %s — %d total (%d editor, %d disk), "
            "%d projects\n",
            trigger, total, editor, disk, num_projects);
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        const char *pid = docs[i].primary_project
                          ? (docs[i].primary_project->id
                              ? docs[i].primary_project->id : "(no-id)")
                          : "(none)";
        tj_node *dbg_root = docs[i].slab ? slab_node(docs[i].slab, docs[i].slab->root_idx) : NULL;
        int deps = dependency_count_subtree(docs[i].slab, dbg_root);
        fprintf(stderr, "  [%2d] %c%c%c%c  proj=%s  deps=%d  %s\n",
                i,
                docs[i].disk_only          ? 'D' : 'd',
                docs[i].slab               ? 'P' : '-',
                doc_has_project_block(&docs[i]) ? 'R' : '-',
                docs[i].is_cc_root         ? 'C' : '-',
                pid,
                deps,
                docs[i].uri ? docs[i].uri : "(null)");
    }
    fflush(stderr);
}

static void revalidate_all_docs(void) {
    maybe_reload_compile_commands();
    rebuild_all_projects();
    republish_all_diagnostics();
    dump_docs_to_stderr("revalidate_all_docs");
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
            parse_slab *slab = parse(text);
            follow_includes(path, slab);
            doc_install_parse(document, slab);
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
        parse_slab *slab = parse(text);
        follow_includes(path, slab);
        doc_install_parse(new_doc, slab);
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
    parse_slab *slab = parse(text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, slab);
        free(path);
    }
    doc_install_parse(d, slab);

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
    parse_slab *slab = parse(d->text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, slab);
        free(path);
    }
    doc_install_parse(d, slab);

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
        parse_slab *slab = parse(text);
        follow_includes(path, slab);
        doc_install_parse(d, slab);
    } else {
        publish_diagnostics(uri);
        doc_free(d);
    }
    free(path);

    revalidate_all_docs();
}

/* ── Read-only query handlers ─────────────────────────────────────────────
 *
 * Each handler takes `d` (the primary Document, already located by the
 * dispatcher under docs_mutex) and returns the response JSON.  Caller
 * holds docs_mutex; no further locking is needed because the previous
 * snapshot machinery was retired.
 */

/** Get the root node's children index array and count for @p d's slab.
 *  Returns NULL/0 when @p d has no parse. */
static tj_idx *doc_kids(const Document *d, int *out_n) {
    if (!d || !d->slab) { *out_n = 0; return NULL; }
    tj_node *root = slab_node(d->slab, d->slab->root_idx);
    if (!root) { *out_n = 0; return NULL; }
    *out_n = root->num_children;
    return slab_children(d->slab, root);
}

/** Get the root node's children index array and count directly from a slab.
 *  Used by snapshot-based paths where no Document wrapper is available. */
static tj_idx *slab_root_kids(const parse_slab *slab, int *out_n) {
    if (!slab) { *out_n = 0; return NULL; }
    tj_node *root = slab_node(slab, slab->root_idx);
    if (!root) { *out_n = 0; return NULL; }
    *out_n = root->num_children;
    return slab_children(slab, root);
}

static yyjson_mut_val *handle_document_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                               yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    size_t  json_len = 0;
    char   *json     = build_document_symbols_json(d->slab, d->slab->root_idx, &json_len);
    if (!json) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_mut_val *raw = yyjson_mut_rawncpy(doc, json, json_len);
    free(json);
    return make_response(doc, id, raw);
}

/* Map a per-document task tj_node to its clone in @p d's assembled
 * Project tree.  Cursor lookups (dependency_at_cursor / task_decl_at_cursor)
 * return per-document nodes, but cross-file resolution lives in the
 * ProjectNode tree, so callers bridge through here first.
 *
 * The per-document task's unprefixed dotted id (its in-file ancestry) is
 * appended to @p d's task prefix target inside the Project tree.  Returns
 * NULL when @p d has no project, the prefix target is missing, or the path
 * does not resolve. */
static ProjectNode *project_node_for_doc_task(const Document *d,
                                              const tj_node *per_doc_task) {
    if (!d || !d->primary_project || !per_doc_task || !d->slab) return NULL;
    Project *p = d->primary_project;

    char *qid = sym_qualified_id(d->slab, per_doc_task);   /* unprefixed in-file path */
    if (!qid || !qid[0]) { free(qid); return NULL; }

    ProjectNode *cur = find_node_by_dotted_path(&p->root, d->task_prefix,
                                                NODE_KIND_TASK);
    char *save = NULL;
    for (char *seg = strtok_r(qid, ".", &save); seg && cur; seg = strtok_r(NULL, ".", &save)) {
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
                                             yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *arr = build_folding_ranges_json(doc, d->slab,
                                                     d->slab->tok_spans,
                                                     d->slab->num_tok_spans,
                                                     d->slab->root_idx);
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_code_lens(yyjson_mut_doc *doc, yyjson_val *id,
                                         yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    int n; tj_idx *kids = doc_kids(d, &n);
    yyjson_mut_val *arr = build_code_lens_json(doc, d->slab,
                                                d->slab->tok_spans,
                                                d->slab->num_tok_spans,
                                                NULL, n);
    (void)kids;
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_hover(yyjson_mut_doc *doc, yyjson_val *id,
                                     yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);

    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    if (d->primary_project &&
        dependency_at_cursor(d->slab,
                             d->slab->tok_spans, d->slab->num_tok_spans, pos,
                             &owner, &dep)) {
        ProjectNode *merged_owner = project_node_for_doc_task(d, owner);
        ProjectNode *target = NULL;
        if (merged_owner) {
            int ordinal = (int)(dep - slab_deps(d->slab, owner));
            if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                target = project_dep_resolve(merged_owner, ordinal,
                                             &d->primary_project->root);
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

    ActiveKeyword ak = active_keyword_at(d->slab, d->slab->tok_spans,
                                          d->slab->num_tok_spans, pos);
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
                                              yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    ActiveContext ac = active_context(d->slab, d->slab->tok_spans,
                                       d->slab->num_tok_spans, pos);
    if (!ac.keyword) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *sig = build_signature_help_json(doc, ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, sig);
}

static char *mint_sem_tokens_result_id(Document *d) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRIu64,
                     d->sem_tokens.next_result_id++);
    if (n < 0) return NULL;
    return strdup(buf);
}

static yyjson_mut_val *handle_semantic_tokens_full(yyjson_mut_doc *doc, yyjson_val *id,
                                                    yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *buf = NULL;
    size_t    count = 0;
    compute_semantic_tokens_data(d->slab,
                                  d->slab->tok_spans,
                                  d->slab->num_tok_spans,
                                  d->slab->num_sem_entries,
                                  &buf, &count);

    char *result_id = mint_sem_tokens_result_id(d);
    yyjson_mut_val *result = build_semantic_tokens_json_from_buf(doc, buf, count, result_id);
    semantic_token_result_replace(&d->sem_tokens, buf, count, result_id);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                          yyjson_val *params, Document *d) {
    const char *previous_result_id = params ? json_str(params, "previousResultId") : NULL;
    if (!d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *new_buf = NULL;
    size_t    new_count = 0;
    compute_semantic_tokens_data(d->slab,
                                  d->slab->tok_spans,
                                  d->slab->num_tok_spans,
                                  d->slab->num_sem_entries,
                                  &new_buf, &new_count);

    char *result_id = mint_sem_tokens_result_id(d);
    yyjson_mut_val *result;
    if (d->sem_tokens.data && d->sem_tokens.result_id && previous_result_id &&
        strcmp(d->sem_tokens.result_id, previous_result_id) == 0) {
        result = build_semantic_tokens_delta_json(doc,
                                                   d->sem_tokens.data, d->sem_tokens.count,
                                                   new_buf, new_count,
                                                   result_id);
    } else {
        result = build_semantic_tokens_json_from_buf(doc, new_buf, new_count, result_id);
    }

    semantic_token_result_replace(&d->sem_tokens, new_buf, new_count, result_id);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_references(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!d->primary_project) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *task = task_decl_at_cursor(d->slab, d->slab->tok_spans,
                                         d->slab->num_tok_spans, pos);
    ProjectNode *wanted = project_node_for_doc_task(d, task);
    yyjson_mut_val *result = build_references_json(doc,
                                                    &d->primary_project->root,
                                                    wanted);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc,
                                                  yyjson_val *id,
                                                  yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    int n; tj_idx *kids = doc_kids(d, &n);
    (void)kids;

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_document_highlight_json(doc, d->slab,
                                                            NULL, n,
                                                            d->slab->tok_spans,
                                                            d->slab->num_tok_spans,
                                                            pos);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!d->primary_project) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    yyjson_mut_val   *result = NULL;
    if (dependency_at_cursor(d->slab,
                             d->slab->tok_spans, d->slab->num_tok_spans, pos,
                             &owner, &dep)) {
        ProjectNode *merged_owner = project_node_for_doc_task(d, owner);
        if (merged_owner) {
            int ordinal = (int)(dep - slab_deps(d->slab, owner));
            if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                result = build_definition_json(doc, merged_owner, ordinal,
                                               &d->primary_project->root);
        }
    }
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_completion(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, Document *d,
                                          workspace_snapshot *snap) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->slab) return make_response(doc, id, yyjson_mut_null(doc));

    /* Gather top-level children arrays of every other doc_snapshot in the
     * same project.  The snapshot already filters to the primary's project,
     * so every slot other than primary_idx is a cross-file pool candidate. */
    const tj_idx *extra_pools[MAX_DOCS];
    int           extra_counts[MAX_DOCS];
    int           num_extra = 0;
    if (snap) {
        for (int i = 0; i < snap->num_docs && num_extra < MAX_DOCS; i++) {
            if (i == snap->primary_idx) continue;
            doc_snapshot *eds = snap->docs[i];
            if (!eds->page) continue;
            int n; tj_idx *kids = slab_root_kids(&eds->slab, &n);
            if (!kids) continue;
            extra_pools[num_extra]  = kids;
            extra_counts[num_extra] = n;
            num_extra++;
        }
    }

    int self_n; tj_idx *self_kids = doc_kids(d, &self_n);

    LspPos pos             = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_completions_json(doc,
                                                     d->slab,
                                                     d->slab->tok_spans,
                                                     d->slab->num_tok_spans,
                                                     pos,
                                                     self_kids, self_n,
                                                     extra_pools,
                                                     extra_counts,
                                                     num_extra,
                                                     d->text);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                                yyjson_val *params,
                                                workspace_snapshot *snap) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (snap) {
        for (int i = 0; i < snap->num_docs; i++) {
            doc_snapshot *ds = snap->docs[i];
            if (!ds->page) continue;
            collect_workspace_symbols(doc, query, &ds->slab, ds->uri, arr);
        }
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

/* ── Workspace snapshot helpers ───────────────────────────────────────────
 *
 * Build a self-contained per-query copy of all relevant Document state.
 * Caller must hold docs_mutex.  The returned snapshot is owned by the
 * caller and is freed after the handler completes.
 */

/** Copy one Document's parse slab page and metadata into a doc_snapshot.
 *  The mmap page is copied via memcpy (one syscall + bulk copy) so the
 *  snapshot's slab pointers reference the copy, not the live page.
 *  Caller holds docs_mutex. */
static doc_snapshot *doc_snapshot_create(const Document *d) {
    doc_snapshot *ds = calloc(1, sizeof(doc_snapshot));
    if (!ds) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    ds->uri             = d->uri             ? strdup(d->uri)             : NULL;
    ds->text            = d->text            ? strdup(d->text)            : NULL;
    ds->task_prefix     = d->task_prefix     ? strdup(d->task_prefix)     : NULL;
    ds->account_prefix  = d->account_prefix  ? strdup(d->account_prefix)  : NULL;
    ds->report_prefix   = d->report_prefix   ? strdup(d->report_prefix)   : NULL;
    ds->resource_prefix = d->resource_prefix ? strdup(d->resource_prefix) : NULL;

    if (d->slab && d->slab->page) {
        size_t sz = d->slab->page->total_mmap_size;
        void *pg = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pg == MAP_FAILED) {
            fprintf(stderr, "taskjuggler-lsp: mmap failed\n"); exit(1);
        }
        memcpy(pg, d->slab->page, sz);

        /* Relocate slab pointer fields from the live page to the copy.
         * Each field is an absolute pointer into the original page; adding
         * the delta (copy_base − orig_base) yields the corresponding address
         * in the copied page. */
        ptrdiff_t delta = (char *)pg - (char *)d->slab->page;
        ds->page           = (parse_page_header *)pg;
        ds->slab           = *d->slab;  /* copy counts and sentinel fields */
        ds->slab.page      = ds->page;
        ds->slab.nodes     = d->slab->nodes
            ? (tj_node    *)((char *)d->slab->nodes     + delta) : NULL;
        ds->slab.children  = d->slab->children
            ? (tj_idx     *)((char *)d->slab->children  + delta) : NULL;
        ds->slab.deps      = d->slab->deps
            ? (Dependency *)((char *)d->slab->deps      + delta) : NULL;
        ds->slab.tok_spans = d->slab->tok_spans
            ? (TokenSpan  *)((char *)d->slab->tok_spans + delta) : NULL;
        ds->slab.strings   = d->slab->strings
            ? (char *)d->slab->strings + delta : NULL;
        /* includes are not used by query handlers; clear to avoid dangling refs */
        ds->slab.includes     = NULL;
        ds->slab.num_includes = 0;
    }

    /* Deep-copy sem_tokens for delta comparison. */
    if (d->sem_tokens.data && d->sem_tokens.count > 0) {
        ds->sem_tokens.data = malloc(d->sem_tokens.count * sizeof(uint32_t));
        if (!ds->sem_tokens.data) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        memcpy(ds->sem_tokens.data, d->sem_tokens.data,
               d->sem_tokens.count * sizeof(uint32_t));
    }
    ds->sem_tokens.count          = d->sem_tokens.count;
    ds->sem_tokens.result_id      = d->sem_tokens.result_id
        ? strdup(d->sem_tokens.result_id) : NULL;
    ds->sem_tokens.next_result_id = d->sem_tokens.next_result_id;

    return ds;
}

/** Build a workspace_snapshot for the given primary document and its
 *  project.  Every Document in the same project gets a doc_snapshot so
 *  completion's extra-pool builder can work without touching docs[].
 *  Caller must hold docs_mutex. */
static workspace_snapshot *workspace_snapshot_create(const Document *primary) {
    workspace_snapshot *snap = calloc(1, sizeof(workspace_snapshot));
    if (!snap) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    /* Capacity: at most MAX_DOCS entries. */
    snap->docs = malloc((size_t)MAX_DOCS * sizeof(doc_snapshot *));
    if (!snap->docs) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    snap->primary_idx = -1;

    for (int i = 0; i < MAX_DOCS; i++) {
        Document *d = &docs[i];
        if (!d->in_use || !d->slab) continue;
        /* When primary is set, include only docs in the same project.
         * When primary is NULL (workspace/symbol), include all docs. */
        if (primary && d->primary_project &&
            primary->primary_project &&
            d->primary_project != primary->primary_project &&
            d != primary) continue;

        doc_snapshot *ds = doc_snapshot_create(d);
        if (d == primary) snap->primary_idx = snap->num_docs;
        snap->docs[snap->num_docs++] = ds;
    }

    /* Deep-copy the primary project's root for cross-file resolution. */
    if (primary && primary->primary_project) {
        snap->project_root =
            project_node_deep_copy(&primary->primary_project->root);
    }

    return snap;
}

/** Extract the textDocument URI (if any) from a query job's params. */
static const char *primary_uri_from_job(const Job *job) {
    yyjson_val *root   = yyjson_doc_get_root(job->request_doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) return json_str(td, "uri");
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (tdp) {
        td = yyjson_obj_get(tdp, "textDocument");
        if (td) return json_str(td, "uri");
    }
    return NULL;
}

workspace_snapshot *server_snapshot_for_job(Job *job) {
    const char *uri = primary_uri_from_job(job);
    pthread_mutex_lock(&docs_mutex);
    Document *primary = uri ? doc_find(uri) : NULL;
    workspace_snapshot *snap = workspace_snapshot_create(primary);
    pthread_mutex_unlock(&docs_mutex);
    return snap;
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

void server_dispatch_query(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *resp = NULL;

    /* initialize and shutdown mutate global server state; run under full mutex. */
    if (strcmp(m, "initialize") == 0 || strcmp(m, "shutdown") == 0) {
        pthread_mutex_lock(&docs_mutex);
        if (strcmp(m, "initialize") == 0)
            resp = handle_initialize(out_doc, id_item, params);
        else
            resp = handle_shutdown(out_doc, id_item);
        pthread_mutex_unlock(&docs_mutex);
        if (resp) send_response(out_doc, resp);
        yyjson_mut_doc_free(out_doc);
        return;
    }

    /* All other queries operate on a workspace_snapshot.  The coordinator
     * pre-computes the snapshot at dispatch time (before any subsequent
     * notification runs) and attaches it to the job; use that if present.
     * Fall back to taking a fresh snapshot here for cases where the job
     * arrives without a pre-attached snapshot (e.g. direct test callers). */
    const char *primary_uri = primary_uri_from_job(job);
    workspace_snapshot *snap = job->snapshot;
    job->snapshot = NULL;  /* transfer ownership; job_free must not free it */
    if (!snap) {
        pthread_mutex_lock(&docs_mutex);
        Document *live_primary = primary_uri ? doc_find(primary_uri) : NULL;
        snap = workspace_snapshot_create(live_primary);
        pthread_mutex_unlock(&docs_mutex);
    }

    /* Build a proxy Document from the primary snapshot so handler signatures
     * remain unchanged.  proxy_proj wraps snap->project_root so handlers that
     * read d->primary_project->root work without holding the live mutex. */
    Document proxy = {0};
    Project  proxy_proj = {0};
    doc_snapshot *pds = (snap->primary_idx >= 0) ? snap->docs[snap->primary_idx] : NULL;
    if (pds) {
        proxy.uri             = pds->uri;
        proxy.text            = pds->text;
        proxy.task_prefix     = pds->task_prefix;
        proxy.account_prefix  = pds->account_prefix;
        proxy.report_prefix   = pds->report_prefix;
        proxy.resource_prefix = pds->resource_prefix;
        proxy.slab            = &pds->slab;
        proxy.sem_tokens      = pds->sem_tokens;
        proxy.in_use          = 1;
    }
    if (snap->project_root) {
        proxy_proj.root       = *snap->project_root;  /* borrow struct value; children owned by snap */
        proxy.primary_project = &proxy_proj;
    }
    Document *d = pds ? &proxy : NULL;

    if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/foldingRange") == 0) {
        resp = handle_folding_range(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/codeLens") == 0) {
        resp = handle_code_lens(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/documentHighlight") == 0) {
        resp = handle_document_highlight(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(out_doc, id_item, params, d, snap);
    } else if (strcmp(m, "workspace/symbol") == 0) {
        resp = handle_workspace_symbol(out_doc, id_item, params, snap);
    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(out_doc, id_item, params, d);
    } else if (strcmp(m, "textDocument/semanticTokens/full/delta") == 0) {
        resp = handle_semantic_tokens_full_delta(out_doc, id_item, params, d);
    } else if (id_item) {
        resp = make_response(out_doc, id_item, yyjson_mut_null(out_doc));
    }

    /* Semantic-token handlers update d->sem_tokens in-place (via
     * semantic_token_result_replace which frees old data).  Write the new
     * state back to the live Document under a brief re-lock, then zero out
     * pds->sem_tokens so workspace_snapshot_free does not double-free the
     * transferred heap data. */
    if (pds && (strcmp(m, "textDocument/semanticTokens/full") == 0 ||
                strcmp(m, "textDocument/semanticTokens/full/delta") == 0)) {
        pthread_mutex_lock(&docs_mutex);
        Document *cur = primary_uri ? doc_find(primary_uri) : NULL;
        if (cur) {
            semantic_token_result_release(&cur->sem_tokens);
            cur->sem_tokens = proxy.sem_tokens;
        } else {
            semantic_token_result_release(&proxy.sem_tokens);
        }
        pds->sem_tokens.data      = NULL;
        pds->sem_tokens.result_id = NULL;
        pds->sem_tokens.count     = 0;
        pthread_mutex_unlock(&docs_mutex);
    }

    /* Send before freeing the snapshot: handler-built JSON may contain string
     * pointers into the snapshot's mmap page, which munmap() invalidates. */
    if (resp) send_response(out_doc, resp);
    yyjson_mut_doc_free(out_doc);

    workspace_snapshot_free(snap);
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
    /* Lifecycle and semantic-token methods must run inline in the coordinator:
     * initialize/shutdown mutate global state that subsequent notifications
     * depend on; semanticTokens methods accumulate result_id state that the
     * next semanticTokens/delta call must read back. */
    job->is_lifecycle    = (strcmp(m, "initialize") == 0
                         || strcmp(m, "shutdown") == 0
                         || strcmp(m, "textDocument/semanticTokens/full") == 0
                         || strcmp(m, "textDocument/semanticTokens/full/delta") == 0);
    threadpool_enqueue_job(job);
}

void server_init() {
    for (int i = 0; i < MAX_DOCS; i++)
        docs[i].in_use = 0;
}
