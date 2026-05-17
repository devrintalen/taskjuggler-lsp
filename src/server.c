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
 * table, see doc/modules/server.rst. */

#include "server.h"
#include "parser.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Document store
   ═══════════════════════════════════════════════════════════════════════════ */

/** Maximum number of documents the server holds open at once. */
#define MAX_DOCS 64

/** Document store slot — URI plus authoritative text and its parse result. */
typedef struct {
    char        *uri;                 /**< document URI, heap-allocated */
    char        *text;                /**< full source text, heap-allocated */
    ParseResult *parse;               /**< parse output for the current text; NULL when slot has no parse */
    char       *doc_symbols_json;     /**< cached documentSymbol JSON array; NULL = invalid */
    size_t      doc_symbols_json_len; /**< byte length of doc_symbols_json (excluding NUL) */
    /* Last semantic-tokens response sent to the client.  Retained across
     * revalidations so semanticTokens/full/delta requests can diff against
     * exactly what the client is holding.  NULL until the client makes its
     * first semanticTokens request. */
    uint32_t   *sem_tokens_data;
    size_t      sem_tokens_count;     /**< entries in sem_tokens_data (multiple of 5) */
    char       *sem_tokens_result_id; /**< resultId returned alongside the data */
    int         in_use;               /**< 1 if this slot holds a live document */
    int         disk_only;            /**< 1 = loaded from disk/watcher, not opened by editor */
} Document;

/** Fixed-size document store; entries with `in_use == 0` are free. */
static Document docs[MAX_DOCS];

/** Maximum number of workspace root URIs the server tracks. */
#define MAX_WORKSPACE_ROOTS 16
/**
 * Workspace root filesystem paths extracted from the `initialize` params.
 * Populated from `rootUri` and/or `workspaceFolders`.  Heap-allocated
 * entries; never freed (server lifetime).
 */
static char *g_workspace_roots[MAX_WORKSPACE_ROOTS];
/** Number of entries currently used in #g_workspace_roots. */
static int   g_num_workspace_roots = 0;

/** Forward declaration — doc_find / doc_alloc normalize URIs internally so that
 * different spellings of the same file (trailing slashes, "./", symlinks) map
 * to the same document slot. */
static char *normalize_uri(const char *raw_uri);

/**
 * Find the open document with the given URI.
 *
 * Fast-path exact match first (clients overwhelmingly send the canonical
 * URI we already stored); falls back to normalising and retrying so that
 * non-canonical URI spellings still hit the stored canonical key.
 *
 * @param uri  Document URI to look up.
 * @return The matching Document, or NULL when none is in use.
 */
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

/**
 * Claim a free Document slot for @p uri and return it.
 *
 * The stored URI is always canonical (see normalize_uri) so the document
 * store is deduplicated regardless of how the URI was spelled.  The
 * returned slot has `in_use=1` and `uri` set; all other fields are zeroed.
 *
 * @param uri  Document URI to associate with the new slot.
 * @return The newly allocated Document, or NULL when the store is full.
 */
static Document *doc_alloc(const char *uri) {
    char *canon = normalize_uri(uri);
    if (!canon) return NULL;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) {
            docs[i].in_use = 1;
            docs[i].uri    = canon; /* ownership transferred */
            docs[i].text   = NULL;
            docs[i].parse  = NULL;
            return &docs[i];
        }
    }
    free(canon);
    return NULL; /* shouldn't happen in practice */
}

/**
 * Release all heap memory owned by @p d and zero its fields, returning the
 * slot to the pool.
 *
 * @param d  Document slot to free.
 */
static void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    free(d->doc_symbols_json);
    free(d->sem_tokens_data);
    free(d->sem_tokens_result_id);
    parse_result_release(d->parse);
    memset(d, 0, sizeof(*d));
}

/* ═══════════════════════════════════════════════════════════════════════════
   Server-to-client messaging
   ═══════════════════════════════════════════════════════════════════════════ */

void lsp_send_message(const char *msg) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
   File I/O helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Decode a percent-encoded string (e.g. a URI path component).
 *
 * @param src  Percent-encoded source string.
 * @return Heap-allocated, NUL-terminated decoded string.  Caller must free().
 */
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

/**
 * Percent-encode a filesystem path for use in a `file://` URI.
 *
 * `/` is preserved as-is; all other characters outside the RFC 3986
 * unreserved set (`A-Z a-z 0-9 - _ . ~`) are encoded as `%XX`.
 *
 * @param src  Filesystem path to encode.
 * @return Heap-allocated, NUL-terminated encoded string.  Caller must free().
 */
static char *percent_encode_path(const char *src) {
    size_t len = strlen(src);
    /* Worst case: every byte becomes %XX (3 chars) */
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

/**
 * Convert a `file://` URI to a filesystem path.
 *
 * @param uri  Source URI.
 * @return Heap-allocated, percent-decoded copy of the path portion (after
 *         `file://`), or NULL when @p uri does not use the file scheme.
 *         Caller must free().
 */
static char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    return percent_decode(uri + 7);
}

/**
 * Convert a filesystem path to a `file://` URI.
 *
 * @param path  Filesystem path.
 * @return Heap-allocated URI.  Caller must free().
 */
static char *path_to_uri(const char *path) {
    char *encoded = percent_encode_path(path);
    size_t enc_len = strlen(encoded);
    /* "file://" prefix (7) + encoded path + NUL */
    char *uri = malloc(7 + enc_len + 1);
    if (!uri) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(uri, "file://", 7);
    memcpy(uri + 7, encoded, enc_len + 1);
    free(encoded);
    return uri;
}

/**
 * Lexically normalise a filesystem path: collapse repeated slashes, drop
 * `"."` segments, strip trailing slashes.  Does not touch `".."` or
 * resolve symlinks — the caller uses this as a fallback when realpath(3)
 * fails (file doesn't exist yet).
 *
 * @param path  Filesystem path to normalise.
 * @return Heap-allocated normalised path.  Caller must free().
 */
static char *lexical_normalize_path(const char *path) {
    if (!path) return NULL;
    size_t len = strlen(path);
    char *out = malloc(len + 2); /* worst case: original + leading '/' + NUL */
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
        if (seg_len == 1 && path[seg_start] == '.') continue; /* drop "." */
        if (wrote_segment) out[wi++] = '/';
        memcpy(out + wi, path + seg_start, seg_len);
        wi += seg_len;
        wrote_segment = 1;
    }
    out[wi] = '\0';
    return out;
}

/**
 * Normalise a URI into the canonical key used by the document store.
 *
 * For `file://` URIs, runs realpath(3) to collapse `.`, `..`, `//`, and
 * symlinks when the file exists; falls back to lexical_normalize_path()
 * for URIs whose backing file does not exist yet.  Non-file URIs are
 * passed through unchanged.
 *
 * @param raw_uri  URI to canonicalise.
 * @return Heap-allocated canonical URI.  Caller must free().
 */
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

/**
 * Read an entire file into memory.
 *
 * @param path  Filesystem path to the file.
 * @return Heap-allocated, NUL-terminated contents on success, or NULL on
 *         any error.  Caller must free().
 */
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

/*
 * Load a single file from disk into the document store.
 * If the URI is already present, it is skipped (does not overwrite
 * editor-managed documents with stale disk content).  URI dedup —
 * covering "./"-prefixed, trailing-slash, and symlinked spellings — is
 * handled inside doc_find / doc_alloc via normalize_uri().
 */
/** Forward declaration — follow_includes() and load_file_from_disk() are
 * mutually recursive: loading a file may trigger following its includes. */
static void follow_includes(const char *file_path, const ParseResult *parse);

/**
 * Load a `.tjp` / `.tji` file from @p path into the document store as a
 * disk-only entry, parse it, and follow any `include` directives it
 * contains.  Silently skips paths whose URI is already in the store
 * (cycle-safe).
 *
 * @param path  Filesystem path of the file to load.
 */
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
    document->parse     = parse(text);
    follow_includes(path, document->parse);
}

/**
 * For each filename listed in @p result->included_files, resolve it
 * relative to the directory containing @p file_path and load it into the
 * document store if not already present.  Cycle-safe: load_file_from_disk()
 * skips URIs that are already in the store.
 *
 * @param file_path  Filesystem path of the file whose includes are being
 *                   followed.
 * @param result     ParseResult of @p file_path.
 */
static void follow_includes(const char *file_path, const ParseResult *result) {
    if (!result->num_included_files) return;

    /* Compute the directory portion of file_path */
    size_t path_len = strlen(file_path);
    const char *last_slash = NULL;
    for (size_t i = path_len; i-- > 0; ) {
        if (file_path[i] == '/') { last_slash = file_path + i; break; }
    }
    size_t dir_len = last_slash ? (size_t)(last_slash - file_path) : 0;

    for (int i = 0; i < result->num_included_files; i++) {
        const char *filename = result->included_files[i];
        size_t fname_len = strlen(filename);

        char *full_path;
        if (filename[0] == '/') {
            /* Absolute path — use as-is */
            full_path = malloc(fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, filename, fname_len + 1);
        } else if (last_slash) {
            /* Relative path with a parent directory.  dir_len == 0 means the
             * parent is the filesystem root "/", so we always write one slash
             * between the prefix and the filename. */
            full_path = malloc(dir_len + 1 + fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, file_path, dir_len);
            full_path[dir_len] = '/';
            memcpy(full_path + dir_len + 1, filename, fname_len + 1);
        } else {
            /* file_path had no slash — resolve relative to CWD. */
            full_path = malloc(fname_len + 1);
            if (!full_path) continue;
            memcpy(full_path, filename, fname_len + 1);
        }

        load_file_from_disk(full_path);
        free(full_path);
    }
}

/**
 * Recursively walk @p dir_path and call load_file_from_disk() for every
 * regular `.tjp` or `.tji` file found.  Silently skips unreadable
 * directories.  Entries are processed in sorted order so the document
 * store ordering is deterministic across filesystems (readdir order is
 * filesystem-dependent).
 *
 * @param dir_path  Filesystem path to the directory to walk.
 */
static void load_tj_files_recursive(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    /* Collect entry names first so we can sort them */
    char  **names     = NULL;
    int     num_names = 0;
    int     cap_names = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; /* skip hidden and . / .. */
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

    /* Sort alphabetically for deterministic load order */
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

/**
 * Extract an LspPos from a JSON object with `"line"` and `"character"` fields.
 *
 * @param obj  JSON object to read.
 * @return The extracted position, or a zeroed LspPos when @p obj is NULL or
 *         the fields are absent.
 */
static LspPos json_to_pos(yyjson_val *obj) {
    LspPos p = {0};
    if (!obj) return p;
    yyjson_val *ln = yyjson_obj_get(obj, "line");
    yyjson_val *ch = yyjson_obj_get(obj, "character");
    if (ln && yyjson_is_num(ln)) p.line      = (uint32_t)yyjson_get_num(ln);
    if (ch && yyjson_is_num(ch)) p.character = (uint32_t)yyjson_get_num(ch);
    return p;
}

/**
 * Apply a single incremental text edit described by @p range / @p new_text
 * to @p src.  `range.end` is exclusive — `src[start_byte..end_byte)` is
 * replaced by @p new_text.  Character offsets are treated as byte offsets
 * within a line, consistent with how the lexer tracks columns.
 *
 * @param src       Original document text (may be NULL, treated as `""`).
 * @param range     Range to replace.
 * @param new_text  Replacement text.
 * @return Heap-allocated NUL-terminated result string.  Caller must free().
 */
static char *apply_incremental_change(const char *src,
                                      LspRange range,
                                      const char *new_text)
{
    if (!src) src = "";
    size_t src_len     = strlen(src);
    size_t new_len     = strlen(new_text);

    /* Find byte offset of range.start */
    size_t start_byte = 0;
    uint32_t cur_line = 0, cur_char = 0;
    while (start_byte < src_len) {
        if (cur_line == range.start.line && cur_char == range.start.character)
            break;
        if (src[start_byte] == '\n') { cur_line++; cur_char = 0; }
        else                         { cur_char++; }
        start_byte++;
    }

    /* Find byte offset of range.end, continuing from start */
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
    memcpy(result,                       src,      start_byte);
    memcpy(result + start_byte,          new_text, new_len);
    memcpy(result + start_byte + new_len, src + end_byte, suffix_len);
    result[start_byte + new_len + suffix_len] = '\0';
    return result;
}

/**
 * Look up a string field on a JSON object.
 *
 * @param obj  JSON object (may be NULL).
 * @param key  Field name.
 * @return The string value at @p key, or NULL when @p obj is NULL or the
 *         field is missing or not a string.
 */
static const char *json_str(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *item = yyjson_obj_get(obj, key);
    return (item && yyjson_is_str(item)) ? yyjson_get_str(item) : NULL;
}

/**
 * Copy an immutable JSON id value into @p doc as a mutable value.
 *
 * @param doc  Destination mutable JSON document.
 * @param id   Source id from the incoming JSON-RPC message (may be NULL or
 *             null).
 * @return Mutable copy of @p id; a JSON null when @p id is NULL or null.
 */
static yyjson_mut_val *copy_id(yyjson_mut_doc *doc, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(doc);
    if (yyjson_is_str(id))  return yyjson_mut_strcpy(doc, yyjson_get_str(id));
    if (yyjson_is_uint(id)) return yyjson_mut_uint(doc, yyjson_get_uint(id));
    if (yyjson_is_sint(id)) return yyjson_mut_int(doc, yyjson_get_int(id));
    if (yyjson_is_real(id)) return yyjson_mut_real(doc, yyjson_get_real(id));
    return yyjson_mut_null(doc);
}

/**
 * Build a success JSON-RPC response envelope around @p result.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param result  Response payload.
 * @return The JSON-RPC response object.
 */
static yyjson_mut_val *make_response(yyjson_mut_doc *doc, yyjson_val *id,
                                      yyjson_mut_val *result) {
    yyjson_mut_val *resp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, resp, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, resp, "id", copy_id(doc, id));
    yyjson_mut_obj_add_val(doc, resp, "result", result);
    return resp;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Cross-file revalidation
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Revalidate cross-file dep refs in all open documents, then publish updated
 * diagnostics.  Called after any document open, change, or close.
 *
 * Three passes, in order:
 *   A. Clear any cross-file state accumulated by the previous cycle.  This
 *      must happen for ALL documents before any resolution — pass B adds
 *      ref_links to symbols in other documents, so mixing clear and resolve
 *      would wipe links we just added.
 *   B. For each document, collect the other open documents' top-level
 *      symbols and re-resolve cross_file_deps[] against them.
 *   C. Publish diagnostics.
 */
static void revalidate_all_docs(void) {
    /* Pass A: clear cross-file state in all open documents. */
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        clear_cross_file_state(docs[i].parse);
    }

    /* Pass B: resolve cross-file deps in each document against all others. */
    DocSymbol *const *extra_roots[MAX_DOCS];
    int               extra_counts[MAX_DOCS];
    const char       *extra_uris[MAX_DOCS];
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;

        int num_extra = 0;
        for (int j = 0; j < MAX_DOCS; j++) {
            if (!docs[j].in_use || j == i) continue;
            extra_roots[num_extra]  = docs[j].parse->doc_symbols;
            extra_counts[num_extra] = docs[j].parse->num_doc_symbols;
            extra_uris[num_extra]   = docs[j].uri;
            num_extra++;
        }

        resolve_cross_file_deps(docs[i].parse,
                                extra_roots, extra_counts, extra_uris,
                                num_extra, docs[i].uri);
    }

    /* Pass C: publish diagnostics for editor-managed documents only.
     * disk_only documents are loaded by the workspace scan and by include
     * resolution; the editor hasn't asked about them, so publishing
     * diagnostics for them would spam the client with noise (and potentially
     * huge payloads from unrelated files in the workspace). */
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        if (docs[i].disk_only) continue;
        publish_diagnostics(docs[i].uri, docs[i].parse);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Handlers
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Handle the `initialize` request.
 *
 * Responds with the server's capabilities and registers all supported
 * language features.  Client capabilities in @p params are not yet
 * inspected.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Initialize parameters (currently unused).
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_initialize(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    // TODO none of the client capabilities are checked here.

    /* Extract workspace root paths for use in handle_initialized().
     * Collect rootUri first, then all workspaceFolders entries, deduplicating
     * by URI string so a client that sends both doesn't double-scan. */
    if (params) {
        yyjson_val *root_uri_val = yyjson_obj_get(params, "rootUri");
        if (root_uri_val && yyjson_is_str(root_uri_val)
                && g_num_workspace_roots < MAX_WORKSPACE_ROOTS) {
            char *path = uri_to_path(yyjson_get_str(root_uri_val));
            if (path) g_workspace_roots[g_num_workspace_roots++] = path;
        }

        yyjson_val *folders = yyjson_obj_get(params, "workspaceFolders");
        if (folders && yyjson_is_arr(folders)) {
            size_t idx, max;
            yyjson_val *folder;
            yyjson_arr_foreach(folders, idx, max, folder) {
                if (g_num_workspace_roots >= MAX_WORKSPACE_ROOTS) break;
                const char *uri = json_str(folder, "uri");
                if (!uri) continue;
                char *path = uri_to_path(uri);
                if (!path) continue;
                /* Skip if already present (rootUri == workspaceFolders[0]) */
                int duplicate = 0;
                for (int k = 0; k < g_num_workspace_roots; k++) {
                    if (strcmp(g_workspace_roots[k], path) == 0) {
                        duplicate = 1; break;
                    }
                }
                if (duplicate) { free(path); continue; }
                g_workspace_roots[g_num_workspace_roots++] = path;
            }
        }
    }

    /* Server info — LSP InitializeResult.serverInfo.
     * Per LSP spec, both fields are strings, not arrays. */
    yyjson_mut_val *server_info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, server_info, "name",    "taskjuggler-lsp");
    yyjson_mut_obj_add_str(doc, server_info, "version", VERSION_STRING);

    /* Completion options */
    yyjson_mut_val *comp_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, comp_triggers, "!");
    yyjson_mut_arr_add_str(doc, comp_triggers, ".");
    yyjson_mut_val *comp_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc,  comp_opts, "triggerCharacters", comp_triggers);
    yyjson_mut_obj_add_bool(doc, comp_opts, "resolveProvider", false);

    /* Signature help */
    yyjson_mut_val *sig_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, sig_triggers, " ");
    yyjson_mut_val *sig_opts = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sig_opts, "triggerCharacters", sig_triggers);

    /* Capabilities */
    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tds = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, tds, "change", 2); /* TextDocumentSyncKind.Incremental */
    yyjson_mut_obj_add_bool(doc, tds, "openClose", true);
    /* Semantic tokens */
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
    /* TODO: add "range": true when textDocument/semanticTokens/range is implemented */

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

    /* workspace.fileOperations.didRename — server wants rename notifications
     * for .tjp and .tji files so it can update the document store. */
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
    yyjson_mut_val *workspace_caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, workspace_caps, "fileOperations", file_ops);
    yyjson_mut_obj_add_val(doc, caps, "workspace", workspace_caps);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, result, "capabilities", caps);
    yyjson_mut_obj_add_val(doc, result, "serverInfo",   server_info);

    return make_response(doc, id, result);
}

/**
 * Handle the `shutdown` request.
 *
 * Returns a null result as required by the LSP spec; the server process
 * exits on the subsequent `exit` notification.
 *
 * @param doc  Destination mutable JSON document.
 * @param id   Request id to echo back.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_shutdown(yyjson_mut_doc *doc, yyjson_val *id) {
    return make_response(doc, id, yyjson_mut_null(doc));
}

/**
 * Send client/registerCapability to ask the client to watch all .tjp and
 * .tji files in the workspace.  The server will then receive
 * workspace/didChangeWatchedFiles whenever those files change on disk.
 */
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

    /* Perform an initial scan of the workspace for existing .tji files.
     * File watchers only fire on future changes, so without this scan the
     * server would never learn about .tji files that already exist on disk
     * when the session starts. */
    for (int i = 0; i < g_num_workspace_roots; i++)
        load_tj_files_recursive(g_workspace_roots[i]);
    if (g_num_workspace_roots > 0)
        revalidate_all_docs();
}

/**
 * Handle `workspace/didChangeWatchedFiles`.
 *
 * The LSP spec defines three change types:
 *   1 = Created, 2 = Changed, 3 = Deleted
 *
 * For Created and Changed: read the file from disk and update (or add) it
 * in the document store, then re-parse.  For Deleted: remove the document
 * from the store.  In all cases, revalidate_all_docs() is called so that
 * cross-file dependency diagnostics are kept up to date.
 *
 * @param params  Notification `params` object.
 */
static void handle_did_change_watched_files(yyjson_val *params) {
    if (!params) return;
    yyjson_val *changes = yyjson_obj_get(params, "changes");
    if (!changes || !yyjson_is_arr(changes)) return;

    int changed = 0;

    size_t idx, max;
    yyjson_val *event;
    yyjson_arr_foreach(changes, idx, max, event) {
        const char *uri   = json_str(event, "uri");
        yyjson_val *type_item = yyjson_obj_get(event, "type");
        if (!uri || !type_item || !yyjson_is_num(type_item)) continue;
        int type = (int)yyjson_get_num(type_item);

        if (type == 3) {
            /* Deleted — remove from store.  Skip if the editor has the file
             * open: the editor's in-memory version stays authoritative until
             * it sends didClose.  No publish needed — disk_only documents
             * never had diagnostics published to the client. */
            Document *document = doc_find(uri);
            if (document && document->disk_only) {
                doc_free(document);
                changed = 1;
            }
        } else {
            /* Created (1) or Changed (2) — read from disk and (re-)parse.
             * Skip if the editor has the file open: the editor's in-memory
             * version is authoritative and it will send didChange itself. */
            Document *document = doc_find(uri);
            if (document && !document->disk_only) continue;

            char *path = uri_to_path(uri);
            if (!path) continue;
            char *text = read_file(path);
            if (!text) { free(path); continue; }

            if (!document) document = doc_alloc(uri);
            if (!document) { free(text); free(path); continue; }

            free(document->text);
            free(document->doc_symbols_json);
            document->doc_symbols_json     = NULL;
            document->doc_symbols_json_len = 0;
            document->text      = text;
            document->disk_only = 1;
            parse_result_release(document->parse);
            document->parse = parse(text);
            follow_includes(path, document->parse);
            free(path);
            changed = 1;
        }
    }

    if (changed) revalidate_all_docs();
}

/**
 * Handle `workspace/didRenameFiles`.
 *
 * For each renamed file: remove the old URI from the document store
 * (clearing its diagnostics), then read the new URI from disk and add it.
 * Revalidates all documents once at the end so that cross-file references
 * are updated.
 *
 * @param params  Notification `params` object.
 */
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

        /* Remove old URI.  Only publish empty if the editor had this file
         * open — disk_only documents never had diagnostics on the client. */
        Document *old_doc = doc_find(old_uri);
        if (old_doc) {
            if (!old_doc->disk_only) {
                ParseResult empty = {0};
                publish_diagnostics(old_uri, &empty);
            }
            doc_free(old_doc);
            changed = 1;
        }

        /* Load new URI from disk */
        char *path = uri_to_path(new_uri);
        if (!path) continue;
        char *text = read_file(path);
        if (!text) { free(path); continue; }

        Document *new_doc = doc_find(new_uri);
        if (!new_doc) new_doc = doc_alloc(new_uri);
        if (!new_doc) { free(text); free(path); continue; }

        free(new_doc->text);
        free(new_doc->doc_symbols_json);
        new_doc->doc_symbols_json     = NULL;
        new_doc->doc_symbols_json_len = 0;
        parse_result_release(new_doc->parse);
        new_doc->text      = text;
        new_doc->disk_only = 1;
        new_doc->parse     = parse(text);
        follow_includes(path, new_doc->parse);
        free(path);
        changed = 1;
    }

    if (changed) revalidate_all_docs();
}

/**
 * Handle `textDocument/didOpen`.
 *
 * Stores the document text, parses it, and triggers cross-file
 * revalidation so that references to symbols in this file resolve.
 *
 * @param params  Notification params containing `textDocument.uri` and
 *                `textDocument.text`.
 */
static void handle_didopen(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri  = json_str(tdi, "uri");
    const char *text = json_str(tdi, "text");
    if (!uri || !text) return;

    Document *d = doc_find(uri);
    if (d) {
        if (!d->disk_only) return; /* duplicate open: LSP spec client error */
        /* File was pre-loaded from disk.
         * If the editor text matches the disk content, just promote the
         * document to editor-managed and publish — we skipped the publish
         * during the workspace scan so the editor has never seen diagnostics
         * for this file. */
        if (d->text && strcmp(d->text, text) == 0) {
            d->disk_only = 0;
            revalidate_all_docs();
            return;
        }
        /* Text differs from disk — replace with authoritative editor content */
        free(d->text);
        free(d->doc_symbols_json);
        d->doc_symbols_json     = NULL;
        d->doc_symbols_json_len = 0;
        parse_result_release(d->parse);
        d->parse = NULL;
    } else {
        d = doc_alloc(uri);
        if (!d) return; /* document store full */
    }
    d->disk_only = 0;
    d->text  = strdup(text);
    d->parse = parse(text);

    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, d->parse);
        free(path);
    }

    revalidate_all_docs();
}

/**
 * Handle `textDocument/didChange`.
 *
 * Applies each content change in order to the stored document text,
 * re-parses, and revalidates all open documents.  Each change may be a
 * full replacement (no `range` field) or an incremental edit (has `range`
 * and `text` fields).  Changes are applied sequentially; each change's
 * range is relative to the text produced by the previous one.
 *
 * @param params  Notification params containing `textDocument.uri` and
 *                `contentChanges[]`.
 */
static void handle_didchange(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi     = yyjson_obj_get(params, "textDocument");
    yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
    if (!tdi || !changes || !yyjson_is_arr(changes)) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    if (yyjson_arr_size(changes) == 0) return;

    /* LSP requires didOpen before any didChange for a given URI.  If we have
     * no record of this document, the client is violating the protocol —
     * ignore the notification rather than silently synthesising an open. */
    Document *d = doc_find(uri);
    if (!d) return;

    /* Start with the current document text (may be NULL for a fresh slot). */
    char *current = d->text ? strdup(d->text) : strdup("");
    if (!current) return;

    size_t idx, max;
    yyjson_val *change;
    yyjson_arr_foreach(changes, idx, max, change) {
        yyjson_val *range_obj = yyjson_obj_get(change, "range");
        const char *new_text  = yyjson_get_str(yyjson_obj_get(change, "text"));
        if (!new_text) { free(current); return; }

        if (range_obj) {
            /* Incremental edit: apply the range-based replacement. */
            LspRange range;
            range.start = json_to_pos(yyjson_obj_get(range_obj, "start"));
            range.end   = json_to_pos(yyjson_obj_get(range_obj, "end"));
            char *next = apply_incremental_change(current, range, new_text);
            free(current);
            if (!next) return;
            current = next;
        } else {
            /* Full replacement. */
            free(current);
            current = strdup(new_text);
            if (!current) return;
        }
    }

    free(d->text);
    free(d->doc_symbols_json);
    d->doc_symbols_json     = NULL;
    d->doc_symbols_json_len = 0;
    d->text = current;
    parse_result_release(d->parse);
    d->parse = parse(d->text);

    /* Pick up any new `include` directives the edit introduced. */
    char *path = uri_to_path(uri);
    if (path) {
        follow_includes(path, d->parse);
        free(path);
    }

    revalidate_all_docs();
}

/**
 * Handle `textDocument/didClose`.
 *
 * When the editor closes a file, attempt to reload it from disk so that
 * cross-file features (completions, definition, references) keep working
 * even when the file is not open in the editor.  If the file cannot be
 * read, remove it from the store and clear client-side diagnostics.
 *
 * @param params  Notification params containing `textDocument.uri`.
 */
static void handle_didclose(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    Document *d = doc_find(uri);
    if (!d) return;

    char *path = uri_to_path(uri);
    char *text  = path ? read_file(path) : NULL;

    if (text) {
        /* Reload from disk — keep the document as a background (disk-only) entry */
        free(d->text);
        free(d->doc_symbols_json);
        d->doc_symbols_json     = NULL;
        d->doc_symbols_json_len = 0;
        d->text      = text;
        d->disk_only = 1;
        parse_result_release(d->parse);
        d->parse = parse(text);
        follow_includes(path, d->parse);
    } else {
        /* File gone from disk — remove and clear client-side diagnostics */
        ParseResult empty = {0};
        publish_diagnostics(uri, &empty);
        doc_free(d);
    }
    free(path);

    /* Revalidate remaining docs — the closed file's symbols are no longer available */
    revalidate_all_docs();
}

/**
 * Handle `textDocument/documentSymbol`.
 *
 * Returns the doc_symbols[] tree for the requested document as a
 * hierarchical symbol list, or null if the document is not open.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_document_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                               yyjson_val *params) {
    const char *uri = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }
    if (!uri) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    if (!d->doc_symbols_json)
        d->doc_symbols_json = build_document_symbols_json(d->parse->doc_symbols,
                                                           d->parse->num_doc_symbols,
                                                           &d->doc_symbols_json_len);
    yyjson_mut_val *raw = yyjson_mut_rawncpy(doc,
                                              d->doc_symbols_json,
                                              d->doc_symbols_json_len);
    return make_response(doc, id, raw);
}

/**
 * Handle `textDocument/foldingRange`.
 *
 * Returns folding ranges derived from the brace token spans of the
 * document.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_folding_range(yyjson_mut_doc *doc, yyjson_val *id,
                                             yyjson_val *params) {
    const char *uri = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }
    if (!uri) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *arr = build_folding_ranges_json(doc,
                                                     d->parse->tok_spans,
                                                     d->parse->num_tok_spans,
                                                     d->parse->doc_symbols,
                                                     d->parse->num_doc_symbols);
    return make_response(doc, id, arr);
}

/**
 * Handle `textDocument/codeLens`.
 *
 * Returns code lenses derived from token-level analysis: a hint above
 * each `length` / `duration` keyword whose enclosing task carries an
 * explicit `start` or `end` date.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_code_lens(yyjson_mut_doc *doc, yyjson_val *id,
                                         yyjson_val *params) {
    const char *uri = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }
    if (!uri) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *arr = build_code_lens_json(doc,
                                                d->parse->tok_spans,
                                                d->parse->num_tok_spans,
                                                d->parse->doc_symbols,
                                                d->parse->num_doc_symbols);
    return make_response(doc, id, arr);
}

/**
 * Return a human-readable label for a DocSymbol keyword.
 *
 * @param keyword  KW_* constant from grammar.tab.h.
 * @return Static string label (e.g. `"Task"`, `"Resource"`).  Returns
 *         `"Symbol"` for keywords with no specific label.
 */
static const char *sym_kind_label(int keyword) {
    switch (keyword) {
    case KW_TASK:     return "Task";
    case KW_RESOURCE: return "Resource";
    case KW_ACCOUNT:  return "Account";
    case KW_SHIFT:    return "Shift";
    case KW_PROJECT:  return "Project";
    default:          return "Symbol";
    }
}

/**
 * Handle `textDocument/hover`.
 *
 * First checks whether the cursor lands on a resolved dependency
 * reference; if so, returns the target symbol's name and kind as a hover
 * card.  Falls back to keyword documentation if no definition link
 * matches.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_hover(yyjson_mut_doc *doc, yyjson_val *id,
                                     yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params; /* some clients pass position at top level */

    const char *uri = NULL;
    yyjson_val *td = yyjson_obj_get(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);

    /* Check whether the cursor is on a resolved dependency/allocation reference.
     * If so, show the target symbol's kind, id, and name instead of keyword docs. */
    const DefinitionLink *hover_link = find_def_link_at(
        d->parse->tok_spans, d->parse->num_tok_spans, pos);
    if (hover_link) {
        const DocSymbol *sym = hover_link->target;
        if (!sym) goto keyword_hover;

        /* Build: "**<Kind> `<dotted.id>`** — <name>" */
        const char *label    = sym_kind_label(sym->keyword);
        char       *sym_id   = sym_qualified_id(sym);
        const char *sym_name = sym->name   ? sym->name   : "";
        char hover_text[512];
        snprintf(hover_text, sizeof(hover_text),
                 "**%s `%s`** \xe2\x80\x94 %s", label, sym_id, sym_name);
        free(sym_id);

        yyjson_mut_val *contents = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, contents, "kind",  "markdown");
        yyjson_mut_obj_add_strcpy(doc, contents, "value", hover_text);

        yyjson_mut_val *hover = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, hover, "contents", contents);
        yyjson_mut_obj_add_val(doc, hover, "range",    range_json(doc, hover_link->source));
        return make_response(doc, id, hover);
    }

keyword_hover:;
    /* Fall back to keyword documentation */
    ActiveKeyword ak = active_keyword_at(d->parse->tok_spans, d->parse->num_tok_spans, pos);
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

/**
 * Handle `textDocument/signatureHelp`.
 *
 * Determines the active keyword context at the cursor and returns a
 * SignatureHelp object with the keyword's argument signature.  Returns
 * null when the cursor is not in a recognised keyword context.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_signature_help(yyjson_mut_doc *doc, yyjson_val *id,
                                              yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    const char *uri = NULL;
    yyjson_val *td = yyjson_obj_get(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    ActiveContext ac = active_context(d->parse->tok_spans, d->parse->num_tok_spans, pos);
    if (!ac.keyword) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *sig = build_signature_help_json(doc, ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, sig);
}

/**
 * Monotonic counter used to mint resultIds for semanticTokens responses.
 * One process-wide counter is sufficient: stale ids from prior sessions
 * cannot match because the server restarts the counter at 1.
 */
static uint64_t next_sem_tokens_result_id = 1;

/** Format the next resultId as a heap-allocated decimal string. */
static char *mint_sem_tokens_result_id(void) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRIu64, next_sem_tokens_result_id++);
    if (n < 0) return NULL;
    return strdup(buf);
}

/**
 * Replace the cached semantic-tokens data stored on @p d with @p new_buf
 * and @p new_result_id, freeing whatever was there before.  Ownership of
 * both pointers transfers to @p d.
 */
static void doc_set_sem_tokens(Document *d,
                                uint32_t *new_buf, size_t new_count,
                                char *new_result_id) {
    free(d->sem_tokens_data);
    free(d->sem_tokens_result_id);
    d->sem_tokens_data      = new_buf;
    d->sem_tokens_count     = new_count;
    d->sem_tokens_result_id = new_result_id;
}

/**
 * Handle `textDocument/semanticTokens/full`.
 *
 * Returns the full delta-encoded semantic token list for the document
 * together with a fresh resultId.  The data is cached on the Document
 * so a subsequent `semanticTokens/full/delta` request can diff against
 * exactly what was sent.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_semantic_tokens_full(yyjson_mut_doc *doc, yyjson_val *id,
                                                    yyjson_val *params) {
    const char *uri = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }
    if (!uri) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *buf = NULL;
    size_t    count = 0;
    compute_semantic_tokens_data(d->parse->tok_spans,
                                  d->parse->num_tok_spans,
                                  d->parse->num_sem_entries,
                                  &buf, &count);
    char *result_id = mint_sem_tokens_result_id();
    yyjson_mut_val *result = build_semantic_tokens_json_from_buf(doc, buf, count, result_id);
    /* Transfer ownership of buf and result_id to the document cache. */
    doc_set_sem_tokens(d, buf, count, result_id);
    return make_response(doc, id, result);
}

/**
 * Handle `textDocument/semanticTokens/full/delta`.
 *
 * If the document has a cached previous response and its resultId
 * matches the request's @c previousResultId, the response is a
 * `SemanticTokensDelta` describing the minimal patch from the cached
 * data to the current data.  Otherwise the response is a full
 * `SemanticTokens` (the spec-mandated fallback when the server cannot
 * compute a diff).  In either case the cache is replaced with the
 * newly computed data and a fresh resultId.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `previousResultId`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_semantic_tokens_full_delta(yyjson_mut_doc *doc, yyjson_val *id,
                                                          yyjson_val *params) {
    const char *uri = NULL;
    const char *previous_result_id = NULL;
    if (params) {
        yyjson_val *td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
        previous_result_id = json_str(params, "previousResultId");
    }
    if (!uri) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    uint32_t *new_buf = NULL;
    size_t    new_count = 0;
    compute_semantic_tokens_data(d->parse->tok_spans,
                                  d->parse->num_tok_spans,
                                  d->parse->num_sem_entries,
                                  &new_buf, &new_count);
    char *result_id = mint_sem_tokens_result_id();

    yyjson_mut_val *result;
    if (d->sem_tokens_data && d->sem_tokens_result_id && previous_result_id &&
        strcmp(d->sem_tokens_result_id, previous_result_id) == 0) {
        result = build_semantic_tokens_delta_json(doc,
                                                   d->sem_tokens_data, d->sem_tokens_count,
                                                   new_buf, new_count,
                                                   result_id);
    } else {
        /* Fallback: client and server are out of sync — send a full set. */
        result = build_semantic_tokens_json_from_buf(doc, new_buf, new_count, result_id);
    }

    doc_set_sem_tokens(d, new_buf, new_count, result_id);
    return make_response(doc, id, result);
}

/**
 * Handle `textDocument/references`.
 *
 * Returns all locations that reference the symbol under the cursor, using
 * the precomputed def_links[] and doc_symbols[] from the parsed document.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_references(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    const char *uri = NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) uri = json_str(td, "uri");

    yyjson_val *pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_references_json(doc,
                                                    uri,
                                                    d->parse->tok_spans,
                                                    d->parse->num_tok_spans,
                                                    pos);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

/**
 * Handle `textDocument/documentHighlight`.
 *
 * Returns all occurrences of the symbol under the cursor within the same
 * document, using def_links[], doc_symbols[], and tok_spans[].
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc,
                                                  yyjson_val *id,
                                                  yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    const char *uri = NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) uri = json_str(td, "uri");

    yyjson_val *pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_document_highlight_json(doc,
                                                            d->parse->doc_symbols,
                                                            d->parse->num_doc_symbols,
                                                            d->parse->tok_spans,
                                                            d->parse->num_tok_spans,
                                                            pos);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

/**
 * Handle `textDocument/definition`.
 *
 * Returns the definition location for the dep-ref expression under the
 * cursor, looked up from the precomputed def_links[] in the parsed
 * document.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    const char *uri = NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) uri = json_str(td, "uri");

    yyjson_val *pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos         = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_definition_json(doc,
                                                    d->parse->tok_spans,
                                                    d->parse->num_tok_spans,
                                                    pos, uri);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

/**
 * Handle `textDocument/completion`.
 *
 * Returns a completion list appropriate for the cursor context, including
 * keyword suggestions and task-identifier completions from the symbol
 * tree.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `textDocument.uri` and
 *                `position`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_completion(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    const char *uri = NULL;
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *td = yyjson_obj_get(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = yyjson_obj_get(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");

    if (!uri || !pos_obj) return make_response(doc, id, yyjson_mut_null(doc));

    Document *d = doc_find(uri);
    if (!d) return make_response(doc, id, yyjson_mut_null(doc));

    /* Gather symbol pools from other editor-managed documents.  disk_only
     * documents (workspace scan + include follows) are excluded: the
     * workspace may contain large unrelated .tjp files, and recursing
     * their symbol trees would produce enormous completion responses. */
    DocSymbol *const *extra_pools[MAX_DOCS];
    int               extra_counts[MAX_DOCS];
    int               num_extra = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use || &docs[i] == d) continue;
        if (docs[i].disk_only) continue;
        extra_pools[num_extra]  = docs[i].parse->doc_symbols;
        extra_counts[num_extra] = docs[i].parse->num_doc_symbols;
        num_extra++;
    }

    LspPos pos             = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_completions_json(doc,
                                                     d->parse->tok_spans,
                                                     d->parse->num_tok_spans,
                                                     pos,
                                                     d->parse->doc_symbols,
                                                     d->parse->num_doc_symbols,
                                                     extra_pools,
                                                     extra_counts,
                                                     num_extra,
                                                     d->text);
    return make_response(doc, id, result);
}

/**
 * Handle `workspace/symbol`.
 *
 * Returns all task symbols across all open documents whose names contain
 * the request's query string.
 *
 * @param doc     Destination mutable JSON document.
 * @param id      Request id to echo back.
 * @param params  Request params containing `query`.
 * @return The JSON-RPC response.
 */
static yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                                yyjson_val *params) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        collect_workspace_symbols(doc, query,
                                  docs[i].parse->doc_symbols,
                                  docs[i].parse->num_doc_symbols,
                                  docs[i].uri, arr);
    }
    return make_response(doc, id, arr);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main dispatch
   ═══════════════════════════════════════════════════════════════════════════ */

char *server_process(const char *json_text) {
    yyjson_doc *in_doc = yyjson_read(json_text, strlen(json_text), 0);
    if (!in_doc) return NULL;

    yyjson_val *root    = yyjson_doc_get_root(in_doc);
    yyjson_val *id_item = yyjson_obj_get(root, "id");
    yyjson_val *method  = yyjson_obj_get(root, "method");
    yyjson_val *params  = yyjson_obj_get(root, "params");

    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    yyjson_mut_doc *out_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *resp = NULL;

    if (strcmp(m, "initialize") == 0) {
        resp = handle_initialize(out_doc, id_item, params);

    } else if (strcmp(m, "initialized") == 0) {
        handle_initialized();
        /* notification — no response to client */

    } else if (strcmp(m, "shutdown") == 0) {
        resp = handle_shutdown(out_doc, id_item);

    } else if (strcmp(m, "exit") == 0) {
        yyjson_doc_free(in_doc);
        yyjson_mut_doc_free(out_doc);
        exit(0);

    } else if (strcmp(m, "textDocument/didOpen") == 0) {
        handle_didopen(params);
        /* no response needed */

    } else if (strcmp(m, "textDocument/didChange") == 0) {
        handle_didchange(params);

    } else if (strcmp(m, "textDocument/didClose") == 0) {
        handle_didclose(params);

    } else if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/foldingRange") == 0) {
        resp = handle_folding_range(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/codeLens") == 0) {
        resp = handle_code_lens(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/documentHighlight") == 0) {
        resp = handle_document_highlight(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(out_doc, id_item, params);

    } else if (strcmp(m, "workspace/didChangeWatchedFiles") == 0) {
        handle_did_change_watched_files(params);

    } else if (strcmp(m, "workspace/didRenameFiles") == 0) {
        handle_did_rename_files(params);

    } else if (strcmp(m, "workspace/symbol") == 0) {
        resp = handle_workspace_symbol(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/semanticTokens/full/delta") == 0) {
        resp = handle_semantic_tokens_full_delta(out_doc, id_item, params);

    /* TODO: textDocument/semanticTokens/range — requires filtering tok_spans
     * to the requested range before encoding.  Advertise "range": true in
     * capabilities once implemented. */

    } else if (id_item) {
        /* Unknown request — return null result */
        resp = make_response(out_doc, id_item, yyjson_mut_null(out_doc));
    }

    yyjson_doc_free(in_doc);

    if (!resp) {
        yyjson_mut_doc_free(out_doc);
        return NULL;
    }

    yyjson_mut_doc_set_root(out_doc, resp);
    char *text = yyjson_mut_write(out_doc, 0, NULL);
    yyjson_mut_doc_free(out_doc);
    return text;
}

void server_init() {
     // Initialize array of Document objects
     for (int i=0; i<MAX_DOCS; i++) {
	  docs[i].in_use = 0;
     }
}
