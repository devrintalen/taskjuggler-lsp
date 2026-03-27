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

#include "server.h"
#include "parser.h"
#include "diagnostics.h"
#include "definition.h"
#include "references.h"
#include "document_symbol.h"
#include "folding_range.h"
#include "hover.h"
#include "signature.h"
#include "completion.h"
#include "semantic_tokens.h"
#include "workspace_symbol.h"

#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Data flow overview
   ═══════════════════════════════════════════════════════════════════════════
 *
 * INBOUND (editor → server)
 * ─────────────────────────
 * The editor sends JSON-RPC messages over stdin.  main() reads each message,
 * strips the Content-Length header, and calls server_process(json_text).
 * server_process() parses the JSON, dispatches to the matching handle_*
 * function, serialises the returned cJSON response with
 * cJSON_PrintUnformatted(), and returns the string to main() which calls
 * lsp_send_message() to write it to stdout.
 *
 * DOCUMENT LIFECYCLE
 * ──────────────────
 * Every open document is stored as a Document in the static docs[] array.
 * The Document holds the raw source text and a fully populated ParseResult.
 *
 * On didOpen / didChange / didChangeWatchedFiles (created or changed):
 *
 *   source text
 *       │
 *       ▼
 *   parse(text)                  ← parser.c entry point
 *       │  runs the flex lexer and bison parser together:
 *       │    lexer  →  tok_spans[]        (every token, in order)
 *       │    grammar→  doc_symbols[]      (task/resource/… tree)
 *       │    grammar→  raw_dep_refs[]     (unresolved dep expressions)
 *       │    grammar→  diagnostics[0..dep_diag_start-1]  (syntax errors)
 *       ▼
 *   ParseResult (stored in Document.parse)
 *       │
 *       ▼
 *   revalidate_all_docs()        ← called for every open document
 *       │  for each document, gathers doc_symbols[] from all *other* open
 *       │  documents as extra symbol pools, then calls:
 *       │
 *       ├─► revalidate_dep_refs(&parse, extra_pools, …)   ← diagnostics.c
 *       │       resolves each raw_dep_ref against the local + extra pools:
 *       │         success → appends a DefinitionLink to def_links[]
 *       │         failure → appends an error Diagnostic to diagnostics[]
 *       │                   (starting at dep_diag_start)
 *       │
 *       └─► publish_diagnostics(uri, &parse)              ← diagnostics.c
 *               serialises diagnostics[] and pushes a
 *               textDocument/publishDiagnostics notification to the editor
 *
 * On didClose / file deleted:
 *   parse_result_free() is called to release the ParseResult, the Document
 *   slot is cleared, and revalidate_all_docs() runs so that references from
 *   other files to symbols in the closed file are re-checked.
 *
 * QUERY DISPATCH — which ParseResult field feeds each feature
 * ───────────────────────────────────────────────────────────
 *
 *   ParseResult field    Feature handlers that consume it
 *   ─────────────────    ───────────────────────────────────────────────────
 *   tok_spans[]          hover            → active_keyword_at()
 *                        signature_help   → active_context()
 *                        completion       → build_completions_json()
 *                        folding_range    → build_folding_ranges_json()
 *                        semantic_tokens  → build_semantic_tokens_json()
 *
 *   doc_symbols[]        document_symbol  → build_document_symbols_json()
 *                        workspace_symbol → collect_workspace_symbols()
 *                        completion       → build_completions_json() (IDs)
 *                        references       → build_references_json()
 *
 *   def_links[]          definition       → build_definition_json()
 *                        references       → build_references_json()
 *
 *   diagnostics[]        (pushed proactively via publish_diagnostics;
 *                         never queried on demand)
 *
 *   raw_dep_refs[]       (consumed only by revalidate_dep_refs;
 *                         not read by any query handler)
 *
 * OUTBOUND (server → editor)
 * ──────────────────────────
 * Query handlers return a heap-allocated cJSON* response built by
 * make_response(id, result).  server_process() serialises it to a string and
 * returns it to main(), which writes:
 *
 *   Content-Length: <N>\r\n\r\n<json>
 *
 * to stdout via lsp_send_message().  Diagnostic push notifications follow
 * the same path but are sent directly from publish_diagnostics() without
 * going through the response/id machinery.
 *
   ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
   Document store
   ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_DOCS 64

typedef struct {
    char       *uri;
    char       *text;
    ParseResult parse;
    int         in_use;
} Document;

static Document docs[MAX_DOCS];

/* Find the open document with the given URI, or return NULL if not found. */
static Document *doc_find(const char *uri) {
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i].in_use && strcmp(docs[i].uri, uri) == 0)
            return &docs[i];
    return NULL;
}

/* Claim a free Document slot for uri and return it, or NULL if the store
 * is full.  The returned slot has in_use=1 and uri set; all other fields
 * are zeroed.
 */
static Document *doc_alloc(const char *uri) {
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) {
            docs[i].in_use = 1;
            docs[i].uri    = strdup(uri);
            docs[i].text   = NULL;
            memset(&docs[i].parse, 0, sizeof(docs[i].parse));
            return &docs[i];
        }
    }
    return NULL; /* shouldn't happen in practice */
}

/* Release all heap memory owned by d and zero its fields, returning the
 * slot to the pool.
 */
static void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    parse_result_free(&d->parse);
    memset(d, 0, sizeof(*d));
}

/* ═══════════════════════════════════════════════════════════════════════════
   Server-to-client messaging
   ═══════════════════════════════════════════════════════════════════════════ */

/* Write msg to stdout with the required LSP Content-Length header.
 * Flushes stdout after writing so the editor receives the message promptly.
 */
void lsp_send_message(const char *msg) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
   File I/O helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Convert a file:// URI to a filesystem path.  Returns a heap-allocated
 * copy of the path portion (after "file://"), or NULL if the URI does not
 * use the file scheme.  Caller must free.
 */
static char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    return strdup(uri + 7);
}

/*
 * Read an entire file into a heap-allocated, NUL-terminated string.
 * Returns NULL on any error.  Caller must free.
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

/* ═══════════════════════════════════════════════════════════════════════════
   JSON helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Extract an LspPos from a JSON object with "line" and "character" fields.
 * Returns a zeroed LspPos if obj is NULL or the fields are absent.
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

/* Return the string value of obj[key], or NULL if missing or not a string. */
static const char *json_str(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *item = yyjson_obj_get(obj, key);
    return (item && yyjson_is_str(item)) ? yyjson_get_str(item) : NULL;
}

/* Copy an immutable id value into doc as a mutable value. */
static yyjson_mut_val *copy_id(yyjson_mut_doc *doc, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(doc);
    if (yyjson_is_str(id))  return yyjson_mut_str(doc, yyjson_get_str(id));
    if (yyjson_is_uint(id)) return yyjson_mut_uint(doc, yyjson_get_uint(id));
    if (yyjson_is_sint(id)) return yyjson_mut_int(doc, yyjson_get_int(id));
    if (yyjson_is_real(id)) return yyjson_mut_real(doc, yyjson_get_real(id));
    return yyjson_mut_null(doc);
}

/* Build a success response envelope around result. */
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

/*
 * Revalidate dep refs in all open documents using the combined symbol pools
 * from every other open document, then publish updated diagnostics.
 *
 * Called after any document open, change, or close so that cross-file
 * dependency references are resolved against the current workspace state.
 */
static void revalidate_all_docs(void) {
    const DocSymbol *extra_pools[MAX_DOCS];
    int              extra_counts[MAX_DOCS];
    const char      *extra_uris[MAX_DOCS];

    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;

        int num_extra = 0;
        for (int j = 0; j < MAX_DOCS; j++) {
            if (!docs[j].in_use || j == i) continue;
            extra_pools[num_extra]  = docs[j].parse.doc_symbols;
            extra_counts[num_extra] = docs[j].parse.num_doc_symbols;
            extra_uris[num_extra]   = docs[j].uri;
            num_extra++;
        }

        revalidate_dep_refs(&docs[i].parse,
                            extra_pools, extra_counts, extra_uris,
                            num_extra);
        publish_diagnostics(docs[i].uri, &docs[i].parse);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Handlers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Handle the initialize request.
 * Responds with the server's capabilities and registers all supported
 * language features.  Client capabilities in params are not yet inspected.
 * id     — request id to echo back in the response
 * params — initialize parameters (currently unused)
 */
static yyjson_mut_val *handle_initialize(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params) {
    (void)params;

    // TODO none of the client capabilities are checked here.

    /* Server info */
    yyjson_mut_val *server_info = yyjson_mut_obj(doc);
    yyjson_mut_val *sn = yyjson_mut_arr(doc);
    // TODO add server version, with program version macro so it is always up to date
    yyjson_mut_arr_add_str(doc, sn, "taskjuggler-lsp");
    yyjson_mut_obj_add_val(doc, server_info, "name", sn);

    /* Completion options */
    yyjson_mut_val *comp_triggers = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, comp_triggers, ",");
    yyjson_mut_arr_add_str(doc, comp_triggers, " ");
    yyjson_mut_arr_add_str(doc, comp_triggers, "!");
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
    yyjson_mut_obj_add_uint(doc, tds, "change", 1); /* TextDocumentSyncKind.Full */
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
    yyjson_mut_obj_add_bool(doc, sem_opts, "full",   true);
    /* TODO: add "range": true when textDocument/semanticTokens/range is implemented */

    yyjson_mut_obj_add_val(doc,  caps, "textDocumentSync",          tds);
    yyjson_mut_obj_add_bool(doc, caps, "documentSymbolProvider",    true);
    yyjson_mut_obj_add_bool(doc, caps, "foldingRangeProvider",      true);
    yyjson_mut_obj_add_bool(doc, caps, "hoverProvider",             true);
    yyjson_mut_obj_add_bool(doc, caps, "definitionProvider",        true);
    yyjson_mut_obj_add_bool(doc, caps, "referencesProvider",        true);
    yyjson_mut_obj_add_val(doc,  caps, "signatureHelpProvider",     sig_opts);
    yyjson_mut_obj_add_val(doc,  caps, "completionProvider",        comp_opts);
    yyjson_mut_obj_add_val(doc,  caps, "semanticTokensProvider",    sem_opts);
    yyjson_mut_obj_add_bool(doc, caps, "workspaceSymbolProvider",   true);

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, result, "capabilities", caps);
    yyjson_mut_obj_add_val(doc, result, "serverInfo",   server_info);

    return make_response(doc, id, result);
}

/* Handle the shutdown request.
 * Returns a null result as required by the LSP spec; the server process
 * exits on the subsequent exit notification.
 */
static yyjson_mut_val *handle_shutdown(yyjson_mut_doc *doc, yyjson_val *id) {
    return make_response(doc, id, yyjson_mut_null(doc));
}

/*
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
}

/*
 * Handle workspace/didChangeWatchedFiles.
 *
 * The LSP spec defines three change types:
 *   1 = Created, 2 = Changed, 3 = Deleted
 *
 * For Created and Changed: read the file from disk and update (or add) it
 * in the document store, then re-parse.  For Deleted: remove the document
 * from the store.  In all cases, revalidate_all_docs() is called so that
 * cross-file dependency diagnostics are kept up to date.
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
            /* Deleted — remove from store and clear client-side diagnostics */
            Document *document = doc_find(uri);
            if (document) {
                ParseResult empty = {0};
                publish_diagnostics(uri, &empty);
                doc_free(document);
                changed = 1;
            }
        } else {
            /* Created (1) or Changed (2) — read from disk and (re-)parse */
            char *path = uri_to_path(uri);
            if (!path) continue;
            char *text = read_file(path);
            free(path);
            if (!text) continue;

            Document *document = doc_find(uri);
            if (!document) document = doc_alloc(uri);
            if (!document) { free(text); continue; }

            free(document->text);
            document->text = text;
            parse_result_free(&document->parse);
            document->parse = parse(text);
            changed = 1;
        }
    }

    if (changed) revalidate_all_docs();
}

/* Handle textDocument/didOpen.
 * Stores the document text, parses it, and triggers cross-file
 * revalidation so that references to symbols in this file resolve.
 * params — notification params containing textDocument.uri and textDocument.text
 */
static void handle_didopen(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri  = json_str(tdi, "uri");
    const char *text = json_str(tdi, "text");
    if (!uri || !text) return;

    int i = 0;

    /* Check each open document */
    for (i = 0; i < MAX_DOCS; i++) {
        if (docs[i].in_use && strcmp(docs[i].uri, uri) == 0) {
            /* The document is already opened, signal an error */
            return;
        } else if (!docs[i].in_use) {
            /* Found a free document slot, use it */
            break;
        }
    }

    if (i == MAX_DOCS && docs[i - 1].in_use) {
        /* No free document slots */
        return;
    }

    docs[i].in_use = 1;
    docs[i].uri    = strdup(uri);
    docs[i].text   = strdup(text);
    docs[i].parse  = parse(text);

    revalidate_all_docs();
}

/* Handle textDocument/didChange.
 * Replaces the stored document text with the last full-sync content change,
 * re-parses, and revalidates all open documents.
 * params — notification params containing textDocument.uri and contentChanges[]
 */
static void handle_didchange(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi     = yyjson_obj_get(params, "textDocument");
    yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
    if (!tdi || !changes || !yyjson_is_arr(changes)) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    if (yyjson_arr_size(changes) == 0) return;

    /* Use the last change (full sync) */
    yyjson_val *last = yyjson_arr_get_last(changes);
    const char *text = json_str(last, "text");
    if (!text) return;

    Document *d = doc_find(uri);
    if (!d) d = doc_alloc(uri);
    if (!d) return;

    free(d->text);
    d->text = strdup(text);
    parse_result_free(&d->parse);
    d->parse = parse(text);
    revalidate_all_docs();
}

/* Handle textDocument/didClose.
 * Clears the document from the store, publishes empty diagnostics to remove
 * any editor-side errors, and revalidates remaining documents so that
 * cross-file references to this file's symbols are re-checked.
 * params — notification params containing textDocument.uri
 */
static void handle_didclose(yyjson_val *params) {
    if (!params) return;
    yyjson_val *tdi = yyjson_obj_get(params, "textDocument");
    if (!tdi) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    Document *d = doc_find(uri);
    if (!d) return;

    /* Publish empty diagnostics to clear client-side errors for the closed file */
    ParseResult empty = {0};
    publish_diagnostics(uri, &empty);
    doc_free(d);

    /* Revalidate remaining docs — the closed file's symbols are no longer available */
    revalidate_all_docs();
}

/* Handle textDocument/documentSymbol.
 * Returns the doc_symbols[] tree for the requested document as a hierarchical
 * symbol list, or null if the document is not open.
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

    yyjson_mut_val *arr = build_document_symbols_json(doc,
                                                       d->parse.doc_symbols,
                                                       d->parse.num_doc_symbols);
    return make_response(doc, id, arr);
}

/* Handle textDocument/foldingRange.
 * Returns folding ranges derived from the brace token spans of the document.
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
                                                     d->parse.tok_spans,
                                                     d->parse.num_tok_spans);
    return make_response(doc, id, arr);
}

/* Handle textDocument/hover.
 * Determines the active keyword at the cursor position and returns its
 * documentation as a Markdown hover card.  Returns null if the cursor is
 * not over a keyword that has documentation.
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

    ActiveKeyword ak = active_keyword_at(d->parse.tok_spans, d->parse.num_tok_spans, pos);
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

/* Handle textDocument/signatureHelp.
 * Determines the active keyword context at the cursor and returns a
 * SignatureHelp object with the keyword's argument signature.
 * Returns null if the cursor is not in a recognised keyword context.
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
    ActiveContext ac = active_context(d->parse.tok_spans, d->parse.num_tok_spans, pos);
    if (!ac.keyword) return make_response(doc, id, yyjson_mut_null(doc));

    yyjson_mut_val *sig = build_signature_help_json(doc, ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, sig);
}

/* Handle textDocument/semanticTokens/full.
 * Returns the full delta-encoded semantic token list for the document.
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

    yyjson_mut_val *result = build_semantic_tokens_json(doc,
                                                         d->parse.tok_spans,
                                                         d->parse.num_tok_spans,
                                                         d->parse.num_sem_entries);
    return make_response(doc, id, result);
}

/* Handle textDocument/references.
 * Returns all locations that reference the symbol under the cursor, using
 * the precomputed def_links[] and doc_symbols[] from the parsed document.
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

    LspPos pos         = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_references_json(doc,
                                                    d->parse.def_links,
                                                    d->parse.num_def_links,
                                                    d->parse.doc_symbols,
                                                    d->parse.num_doc_symbols,
                                                    pos, uri);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

/* Handle textDocument/definition.
 * Returns the definition location for the dep-ref expression under the cursor,
 * looked up from the precomputed def_links[] in the parsed document.
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
                                                    d->parse.def_links,
                                                    d->parse.num_def_links,
                                                    pos, uri);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}

/* Handle textDocument/completion.
 * Returns a completion list appropriate for the cursor context, including
 * keyword suggestions and task-identifier completions from the symbol tree.
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

    LspPos pos             = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_completions_json(doc,
                                                     d->parse.tok_spans,
                                                     d->parse.num_tok_spans,
                                                     pos,
                                                     d->parse.doc_symbols,
                                                     d->parse.num_doc_symbols);
    return make_response(doc, id, result);
}

/* Handle workspace/symbol.
 * Returns all task symbols across all open documents whose names contain query.
 */
static yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                                yyjson_val *params) {
    const char *query = params ? json_str(params, "query") : NULL;
    if (!query) query = "";

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) continue;
        collect_workspace_symbols(doc, query,
                                  docs[i].parse.doc_symbols,
                                  docs[i].parse.num_doc_symbols,
                                  docs[i].uri, arr);
    }
    return make_response(doc, id, arr);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main dispatch
   ═══════════════════════════════════════════════════════════════════════════ */

/* Parse and dispatch one JSON-RPC message.
 * json_text — NUL-terminated JSON-RPC message body (no Content-Length header)
 * Returns a heap-allocated JSON response string for request messages, or
 * NULL for notifications (which have no response).  Caller must free.
 */
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

    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(out_doc, id_item, params);

    } else if (strcmp(m, "workspace/didChangeWatchedFiles") == 0) {
        handle_did_change_watched_files(params);

    } else if (strcmp(m, "workspace/symbol") == 0) {
        resp = handle_workspace_symbol(out_doc, id_item, params);

    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(out_doc, id_item, params);

    /* TODO: textDocument/semanticTokens/full/delta — requires storing a
     * resultId per document and computing a token diff against the previously
     * returned set.  Advertise "full": { "delta": true } in capabilities once
     * implemented. */

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

/* Initialise the document store.
 * Must be called once before server_process() is invoked for the first time.
 */
void server_init() {
     // Initialize array of Document objects
     for (int i=0; i<MAX_DOCS; i++) {
	  docs[i].in_use = 0;
     }
}
