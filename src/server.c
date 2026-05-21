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
#include "job_queue.h"
#include "threadpool.h"
#include "diagnostics.h"
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
#include "version.h"

#include <yyjson.h>
#include <ctype.h>
#include <dirent.h>
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
    char        *text;
    SemanticTokenResult sem_tokens; /**< cached semantic-tokens response */
    _Atomic uint64_t doc_version;   /**< bumped on every text/parse swap */
    int          in_use;
    int          disk_only;

    /* Parse-derived state.  All four synthetic roots are non-NULL after a
     * successful parse and NULL before one has happened (use `tasks` as
     * the "has-parse" sentinel).  Freed and replaced wholesale by
     * doc_install_parse(); freed by doc_free() on slot release. */
    tj_node     *tasks;
    tj_node     *accounts;
    tj_node     *reports;
    tj_node     *resources;
    tj_node     *project;           /**< project block; NULL for .tji or .tjp without one */
    TokenSpan   *tok_spans;
    int          num_tok_spans;
    int          tok_span_cap;
    int          num_sem_entries;
    IncludeRef  *includes;          /**< one entry per `include` directive (file + per-kind prefixes) */
    int          num_includes;
    int          includes_cap;

    /* Prefixes applied to this Document by the includer's `include` block,
     * one per kind.  Populated by follow_includes() from the includer's
     * captured IncludeRef when this file is pulled in; stay NULL on a
     * canonical .tjp or on orphan .tji files in a .tji-only workspace. */
    char        *task_prefix;
    char        *account_prefix;
    char        *report_prefix;
    char        *resource_prefix;

    /* Other Documents this one pulls in via `include`.  Populated by the
     * include resolver.  Borrowed pointers — the Document store owns
     * every slot. */
    struct Document **included_docs;
    int          num_included_docs;
    int          included_docs_cap;
} Document;

static Document docs[MAX_DOCS];

/* Serializes every read/write of docs[] — slots, their fields, and the
 * global trees built from them. */
static pthread_mutex_t docs_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *g_workspace_root = NULL;

/* Forward declarations. */
static char *normalize_uri(const char *raw_uri);
static void  load_file_from_disk(const char *path);
static void  follow_includes(const char *file_path, const Document *d);
static void  load_tj_files_recursive(const char *dir_path);
static void  rebuild_global_trees(void);

/* ── Global tj_node trees ───────────────────────────────────────────────── *
 *
 * Synthetic per-kind roots holding the merged view of every Document's
 * per-kind tree.  Built fresh by rebuild_global_trees() on every
 * document-state-changing notification: deep-copy the canonical
 * project's children in, then deep-copy every other Document's
 * top-level children at the prefix target inside the global tree.
 *
 * These trees own their children — completely separate memory from the
 * Document-owned trees, which stay immutable after parse. */
static tj_node g_task_tree;
static tj_node g_account_tree;
static tj_node g_report_tree;
static tj_node g_resource_tree;

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

/** Release the parse-derived fields on @p d (per-kind tj_node trees,
 *  tokens, include filenames), zeroing each so the slot is reusable. */
static void doc_clear_parse_state(Document *d) {
    tj_node_free(d->tasks);
    tj_node_free(d->accounts);
    tj_node_free(d->reports);
    tj_node_free(d->resources);
    tj_node_free(d->project);
    d->tasks = d->accounts = d->reports = d->resources = d->project = NULL;

    for (int i = 0; i < d->num_tok_spans; i++)
        free(d->tok_spans[i].text);
    free(d->tok_spans);
    d->tok_spans       = NULL;
    d->num_tok_spans   = 0;
    d->tok_span_cap    = 0;
    d->num_sem_entries = 0;

    for (int i = 0; i < d->num_includes; i++) {
        free(d->includes[i].filename);
        free(d->includes[i].task_prefix);
        free(d->includes[i].resource_prefix);
        free(d->includes[i].account_prefix);
        free(d->includes[i].report_prefix);
    }
    free(d->includes);
    d->includes      = NULL;
    d->num_includes  = 0;
    d->includes_cap  = 0;
}

/** Move every parse-derived field from @p po into @p d, releasing whatever
 *  @p d held previously.  @p po is freed (its fields have been moved out,
 *  so the shell is empty afterwards). */
static void doc_install_parse(Document *d, ParseOutput *po) {
    doc_clear_parse_state(d);
    if (po) {
        d->tasks               = po->tasks;
        d->accounts            = po->accounts;
        d->reports             = po->reports;
        d->resources           = po->resources;
        d->project             = po->project;
        d->tok_spans           = po->tok_spans;
        d->num_tok_spans       = po->num_tok_spans;
        d->tok_span_cap        = po->tok_span_cap;
        d->num_sem_entries     = po->num_sem_entries;
        d->includes            = po->includes;
        d->num_includes        = po->num_includes;
        d->includes_cap        = po->includes_cap;
        /* Zero the source so parse_output_free does not double-free. */
        memset(po, 0, sizeof(*po));
        free(po);
    }
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
    free(d->included_docs);
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
    doc_install_parse(document, parse(text));
    follow_includes(path, document);
}

/** Replace @p *slot with a fresh strdup of @p value (NULL when @p value
 *  is NULL).  Used to copy IncludeRef prefix strings onto the includee. */
static void replace_string(char **slot, const char *value) {
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static void follow_includes(const char *file_path, const Document *d) {
    if (!d || !d->num_includes) return;

    size_t path_len = strlen(file_path);
    const char *last_slash = NULL;
    for (size_t i = path_len; i-- > 0; ) {
        if (file_path[i] == '/') { last_slash = file_path + i; break; }
    }
    size_t dir_len = last_slash ? (size_t)(last_slash - file_path) : 0;

    for (int i = 0; i < d->num_includes; i++) {
        const IncludeRef *inc = &d->includes[i];
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
        free(target_uri);
        if (target) {
            replace_string(&target->task_prefix,     inc->task_prefix);
            replace_string(&target->resource_prefix, inc->resource_prefix);
            replace_string(&target->account_prefix,  inc->account_prefix);
            replace_string(&target->report_prefix,   inc->report_prefix);
        }

        free(full_path);
    }
}

static void load_tj_files_recursive(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    char  **names     = NULL;
    int     num_names = 0;
    int     cap_names = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (num_names >= cap_names) {
            int nc = cap_names ? cap_names * 2 : 16;
            char **tmp = realloc(names, (size_t)nc * sizeof(char *));
            if (!tmp) continue;
            names = tmp;
            cap_names = nc;
        }
        names[num_names] = strdup(entry->d_name);
        if (names[num_names]) num_names++;
    }
    closedir(dir);

    if (num_names > 1) {
        for (int i = 1; i < num_names; i++) {
            char *key = names[i];
            int j = i - 1;
            while (j >= 0 && strcmp(names[j], key) > 0) {
                names[j + 1] = names[j];
                j--;
            }
            names[j + 1] = key;
        }
    }

    size_t dir_len = strlen(dir_path);
    for (int i = 0; i < num_names; i++) {
        size_t name_len = strlen(names[i]);
        char *full_path = malloc(dir_len + 1 + name_len + 1);
        if (!full_path) { free(names[i]); continue; }
        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, names[i], name_len + 1);
        free(names[i]);

        struct stat st;
        if (stat(full_path, &st) != 0) { free(full_path); continue; }

        if (S_ISDIR(st.st_mode)) {
            load_tj_files_recursive(full_path);
        } else if (S_ISREG(st.st_mode) && name_len >= 4) {
            const char *ext = full_path + dir_len + 1 + name_len - 4;
            if (strcmp(ext, ".tji") == 0 || strcmp(ext, ".tjp") == 0)
                load_file_from_disk(full_path);
        }

        free(full_path);
    }
    free(names);
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
   Global tj_node tree rebuild
   ═══════════════════════════════════════════════════════════════════════════ */

/** Free every owned child of @p root, leaving @p root itself as an empty
 *  synthetic-root shell.  Used to tear down the global trees before each
 *  rebuild. */
static void free_global_root_children(tj_node *root) {
    if (!root) return;
    for (int i = 0; i < root->num_children; i++)
        tj_node_free(root->children[i]);
    free(root->children);
    root->children     = NULL;
    root->num_children = 0;
    root->children_cap = 0;
}

/** Return 1 when @p uri's path component ends in ".tjp" (case-sensitive). */
static int uri_is_tjp(const char *uri) {
    if (!uri) return 0;
    size_t len = strlen(uri);
    return len >= 4 && strcmp(uri + len - 4, ".tjp") == 0;
}

/** Pick the canonical project Document for hoisting.
 *
 *  Preference order:
 *    1. The single editor-open .tjp, if exactly one exists.
 *    2. Any editor-open .tjp (warn that the choice is ambiguous).
 *    3. The single loaded .tjp on disk.
 *    4. Any loaded .tjp (warn).
 *    5. NULL — no .tjp loaded; the caller falls back to .tji-only mode.
 *
 *  TODO(canonical-selection): the user-facing spec says the canonical .tjp
 *  is the one that transitively includes the currently-edited file, with
 *  ambiguity raising an error.  This stub uses a simpler heuristic until
 *  diagnostics are reintroduced and we can route a real error to the
 *  client.
 */
static Document *find_canonical_tjp(void) {
    Document *editor_pick = NULL;
    int       editor_count = 0;
    Document *any_pick = NULL;
    int       any_count = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use || !docs[i].tasks) continue;
        if (!uri_is_tjp(docs[i].uri)) continue;
        any_count++;
        if (!any_pick) any_pick = &docs[i];
        if (!docs[i].disk_only) {
            editor_count++;
            if (!editor_pick) editor_pick = &docs[i];
        }
    }
    if (editor_count == 1) return editor_pick;
    if (editor_count > 1) {
        fprintf(stderr,
                "taskjuggler-lsp: %d .tjp files open in editor; picking %s\n",
                editor_count, editor_pick->uri);
        return editor_pick;
    }
    if (any_count == 1) return any_pick;
    if (any_count > 1) {
        fprintf(stderr,
                "taskjuggler-lsp: %d .tjp files in workspace and none open in editor; picking %s\n",
                any_count, any_pick->uri);
        return any_pick;
    }
    return NULL;
}

/** Walk @p start's children along the dot-separated @p path and return
 *  the matched node.  Returns @p start when @p path is NULL or empty.
 *  Operates on the global tree (which owns its children outright), so
 *  there is no separate "included" array to consult. */
static tj_node *find_node_by_dotted_path(tj_node *start, const char *path) {
    if (!start) return NULL;
    if (!path || !path[0]) return start;

    char *copy = strdup(path);
    if (!copy) return NULL;
    tj_node *cur = start;
    for (char *seg = strtok(copy, "."); seg && cur; seg = strtok(NULL, ".")) {
        tj_node *next = NULL;
        for (int i = 0; i < cur->num_children && !next; i++)
            if (cur->children[i]->id && strcmp(cur->children[i]->id, seg) == 0)
                next = cur->children[i];
        cur = next;
    }
    free(copy);
    return cur;
}

/** Deep-copy each top-level child of @p from_root and attach the copies
 *  to @p target. */
static void copy_top_level(tj_node *target, tj_node *from_root) {
    if (!target || !from_root) return;
    for (int i = 0; i < from_root->num_children; i++) {
        tj_node *copy = tj_node_clone(from_root->children[i]);
        tj_node_append_child(target, copy);
    }
}

/** For each of the four kinds, find @p d's prefix target inside the
 *  matching global root and deep-copy @p d's top-level entries under
 *  that target.  When a prefix doesn't resolve to a node in the global
 *  tree, this kind's copy is skipped — TODO(global-tree): emit a
 *  diagnostic instead once the diagnostic channel returns.
 *
 *  TODO(nested-includes): when @p d itself includes another .tji whose
 *  prefix target lives only in @p d's tree, the second .tji needs @p d's
 *  copy to have happened first.  Document slot order today follows load
 *  order, which is includer-before-includee for the workspace scan, so
 *  the simple in-order pass below is usually right — but it's not
 *  guaranteed.  A topological pass is the proper fix. */
static void copy_document_into_globals(Document *d) {
    struct {
        tj_node    *global_root;
        tj_node    *doc_root;
        const char *prefix;
    } kinds[] = {
        { &g_task_tree,     d->tasks,     d->task_prefix     },
        { &g_account_tree,  d->accounts,  d->account_prefix  },
        { &g_report_tree,   d->reports,   d->report_prefix   },
        { &g_resource_tree, d->resources, d->resource_prefix },
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        tj_node *target = find_node_by_dotted_path(kinds[k].global_root,
                                                    kinds[k].prefix);
        if (!target) continue;
        copy_top_level(target, kinds[k].doc_root);
    }
}

/** Build the global per-kind trees from the current Document store.
 *
 *  Rebuilds from scratch on every notification: free the prior copy,
 *  deep-copy the canonical project's children in, then deep-copy every
 *  other Document's top-level children at its prefix target. */
static void rebuild_global_trees(void) {
    /* 1. Free everything from the previous cycle. */
    free_global_root_children(&g_task_tree);
    free_global_root_children(&g_account_tree);
    free_global_root_children(&g_report_tree);
    free_global_root_children(&g_resource_tree);

    /* 2. Pick the canonical project. */
    Document *canon = find_canonical_tjp();

    if (canon) {
        /* 3a. Anchor the globals on copies of canon's top-level entries. */
        copy_top_level(&g_task_tree,     canon->tasks);
        copy_top_level(&g_account_tree,  canon->accounts);
        copy_top_level(&g_report_tree,   canon->reports);
        copy_top_level(&g_resource_tree, canon->resources);

        /* 3b. Copy every other loaded Document under its prefix target. */
        for (int i = 0; i < MAX_DOCS; i++) {
            if (!docs[i].in_use || !docs[i].tasks) continue;
            if (&docs[i] == canon) continue;
            copy_document_into_globals(&docs[i]);
        }
    } else {
        /* 4. .tji-only fallback: copy every loaded document's top-levels
         *    onto the synthetic root with no prefix. */
        for (int i = 0; i < MAX_DOCS; i++) {
            if (!docs[i].in_use || !docs[i].tasks) continue;
            copy_top_level(&g_task_tree,     docs[i].tasks);
            copy_top_level(&g_account_tree,  docs[i].accounts);
            copy_top_level(&g_report_tree,   docs[i].reports);
            copy_top_level(&g_resource_tree, docs[i].resources);
        }
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

static void revalidate_all_docs(void) {
    rebuild_global_trees();
    republish_all_diagnostics();
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

    if (g_workspace_root) {
        load_tj_files_recursive(g_workspace_root);
        revalidate_all_docs();
    }
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
            doc_install_parse(document, parse(text));
            follow_includes(path, document);
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
        doc_install_parse(new_doc, parse(text));
        follow_includes(path, new_doc);
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
    doc_install_parse(d, parse(text));

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, d);
        free(path);
    }

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
    doc_install_parse(d, parse(d->text));

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, d);
        free(path);
    }

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
        doc_install_parse(d, parse(text));
        follow_includes(path, d);
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

static yyjson_mut_val *handle_document_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                               yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    /* For now, render the four per-kind trees concatenated at the top
     * level (project node first when present).  TODO(document-symbol):
     * once the project block reliably contains its children in the
     * hierarchical sense, fold the per-kind entries inside it. */
    int top_n = 0;
    if (d->project) top_n++;
    top_n += d->tasks->num_children;
    top_n += d->accounts->num_children;
    top_n += d->reports->num_children;
    top_n += d->resources->num_children;

    tj_node **top = top_n
        ? malloc((size_t)top_n * sizeof(tj_node *))
        : NULL;
    int w = 0;
    if (d->project)
        top[w++] = d->project;
    for (int i = 0; i < d->tasks->num_children;     i++) top[w++] = d->tasks->children[i];
    for (int i = 0; i < d->accounts->num_children;  i++) top[w++] = d->accounts->children[i];
    for (int i = 0; i < d->reports->num_children;   i++) top[w++] = d->reports->children[i];
    for (int i = 0; i < d->resources->num_children; i++) top[w++] = d->resources->children[i];

    size_t  json_len = 0;
    char   *json     = build_document_symbols_json(top, w, &json_len);
    free(top);
    if (!json) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_mut_val *raw = yyjson_mut_rawncpy(doc, json, json_len);
    free(json);
    return make_response(doc, id, raw);
}

/* Reusable: gather every doc's top-level nodes (project + four trees) as
 * a flat array, for handlers that take a tj_node *const * symbols pool. */
static tj_node **flatten_top_nodes(Document *d, int *out_n) {
    *out_n = 0;
    if (!d || !d->tasks) return NULL;
    int n = (d->project ? 1 : 0)
          + d->tasks->num_children
          + d->accounts->num_children
          + d->reports->num_children
          + d->resources->num_children;
    if (!n) return NULL;
    tj_node **arr = malloc((size_t)n * sizeof(tj_node *));
    int w = 0;
    if (d->project) arr[w++] = d->project;
    for (int i = 0; i < d->tasks->num_children;     i++) arr[w++] = d->tasks->children[i];
    for (int i = 0; i < d->accounts->num_children;  i++) arr[w++] = d->accounts->children[i];
    for (int i = 0; i < d->reports->num_children;   i++) arr[w++] = d->reports->children[i];
    for (int i = 0; i < d->resources->num_children; i++) arr[w++] = d->resources->children[i];
    *out_n = w;
    return arr;
}

static yyjson_mut_val *handle_folding_range(yyjson_mut_doc *doc, yyjson_val *id,
                                             yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    int n = 0;
    tj_node **top = flatten_top_nodes(d, &n);
    yyjson_mut_val *arr = build_folding_ranges_json(doc,
                                                     d->tok_spans,
                                                     d->num_tok_spans,
                                                     top, n);
    free(top);
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_code_lens(yyjson_mut_doc *doc, yyjson_val *id,
                                         yyjson_val *params, Document *d) {
    (void)params;
    if (!d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    int n = 0;
    tj_node **top = flatten_top_nodes(d, &n);
    yyjson_mut_val *arr = build_code_lens_json(doc,
                                                d->tok_spans,
                                                d->num_tok_spans,
                                                top, n);
    free(top);
    return make_response(doc, id, arr);
}

static yyjson_mut_val *handle_hover(yyjson_mut_doc *doc, yyjson_val *id,
                                     yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);

    /* TODO(hover): the previous design checked dep-link hover targets
     * first.  With link resolution offline, fall through directly to
     * keyword documentation. */
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
                                              yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    ActiveContext ac = active_context(d->tok_spans,
                                       d->num_tok_spans, pos);
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
    if (!d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *buf = NULL;
    size_t    count = 0;
    compute_semantic_tokens_data(d->tok_spans,
                                  d->num_tok_spans,
                                  d->num_sem_entries,
                                  &buf, &count);

    char *result_id = mint_sem_tokens_result_id(d);
    yyjson_mut_val *result = build_semantic_tokens_json_from_buf(doc, buf, count, result_id);
    semantic_token_result_replace(&d->sem_tokens, buf, count, result_id);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                          yyjson_val *params, Document *d) {
    const char *previous_result_id = params ? json_str(params, "previousResultId") : NULL;
    if (!d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *new_buf = NULL;
    size_t    new_count = 0;
    compute_semantic_tokens_data(d->tok_spans,
                                  d->num_tok_spans,
                                  d->num_sem_entries,
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
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_references_json(doc,
                                                    d->uri,
                                                    d->tok_spans,
                                                    d->num_tok_spans,
                                                    pos);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc,
                                                  yyjson_val *id,
                                                  yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    int n = 0;
    tj_node **top = flatten_top_nodes(d, &n);

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_document_highlight_json(doc,
                                                            top, n,
                                                            d->tok_spans,
                                                            d->num_tok_spans,
                                                            pos);
    free(top);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_definition_json(doc,
                                                    d->tok_spans,
                                                    d->num_tok_spans,
                                                    pos, d->uri);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_completion(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params, Document *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->tasks) return make_response(doc, id, yyjson_mut_null(doc));

    /* Gather top-level nodes of other editor-managed Documents as extra
     * pools.  TODO(completion): once the global tj_node tree is built we
     * can drop the per-doc walk here in favour of the global view. */
    tj_node *const *extra_pools[MAX_DOCS];
    int             extra_counts[MAX_DOCS];
    tj_node       **extra_alloc[MAX_DOCS] = {0};
    int             num_extra = 0;
    for (int i = 0; i < MAX_DOCS && num_extra < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        if (&docs[i] == d) continue;
        if (docs[i].disk_only) continue;
        int n = 0;
        tj_node **top = flatten_top_nodes(&docs[i], &n);
        if (!top) continue;
        extra_alloc[num_extra]  = top;
        extra_pools[num_extra]  = top;
        extra_counts[num_extra] = n;
        num_extra++;
    }

    int self_n = 0;
    tj_node **self_top = flatten_top_nodes(d, &self_n);

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
    free(self_top);
    for (int i = 0; i < num_extra; i++) free(extra_alloc[i]);
    return make_response(doc, id, result);
}

static yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                                yyjson_val *params) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use || !docs[i].tasks) continue;
        int n = 0;
        tj_node **top = flatten_top_nodes(&docs[i], &n);
        if (!top) continue;
        collect_workspace_symbols(doc, query, top, n, docs[i].uri, arr);
        free(top);
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

void server_dispatch_query(Job *job) {
    yyjson_val *root    = yyjson_doc_get_root(job->request_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *resp = NULL;

    /* Locate the primary Document (if any) so handlers receive a stable
     * pointer.  Held docs_mutex covers both lookup and handler execution. */
    pthread_mutex_lock(&docs_mutex);

    const char *primary_uri = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) primary_uri = json_str(td, "uri");
        if (!primary_uri) {
            yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
            if (tdp) {
                td = yyjson_obj_get(tdp, "textDocument");
                if (td) primary_uri = json_str(td, "uri");
            }
        }
    }
    Document *primary = primary_uri ? doc_find(primary_uri) : NULL;

    if (strcmp(m, "initialize") == 0) {
        resp = handle_initialize(out_doc, id_item, params);
    } else if (strcmp(m, "shutdown") == 0) {
        resp = handle_shutdown(out_doc, id_item);
    } else if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/foldingRange") == 0) {
        resp = handle_folding_range(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/codeLens") == 0) {
        resp = handle_code_lens(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/documentHighlight") == 0) {
        resp = handle_document_highlight(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(out_doc, id_item, params, primary);
    } else if (strcmp(m, "workspace/symbol") == 0) {
        resp = handle_workspace_symbol(out_doc, id_item, params);
    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(out_doc, id_item, params, primary);
    } else if (strcmp(m, "textDocument/semanticTokens/full/delta") == 0) {
        resp = handle_semantic_tokens_full_delta(out_doc, id_item, params, primary);
    } else if (id_item) {
        resp = make_response(out_doc, id_item, yyjson_mut_null(out_doc));
    }

    pthread_mutex_unlock(&docs_mutex);

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
    memset(&g_task_tree,     0, sizeof(g_task_tree));
    memset(&g_account_tree,  0, sizeof(g_account_tree));
    memset(&g_report_tree,   0, sizeof(g_report_tree));
    memset(&g_resource_tree, 0, sizeof(g_resource_tree));
}
