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

#include <stdlib.h>
#include <string.h>

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

static Document *doc_find(const char *uri) {
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i].in_use && strcmp(docs[i].uri, uri) == 0)
            return &docs[i];
    return NULL;
}

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

static void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    parse_result_free(&d->parse);
    memset(d, 0, sizeof(*d));
}

/* ═══════════════════════════════════════════════════════════════════════════
   JSON helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static LspPos json_to_pos(const cJSON *obj) {
    LspPos p = {0};
    if (!obj) return p;
    cJSON *ln = cJSON_GetObjectItemCaseSensitive(obj, "line");
    cJSON *ch = cJSON_GetObjectItemCaseSensitive(obj, "character");
    if (cJSON_IsNumber(ln)) p.line      = (uint32_t)ln->valuedouble;
    if (cJSON_IsNumber(ch)) p.character = (uint32_t)ch->valuedouble;
    return p;
}

static const char *json_str(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

/* Build a success response. Takes ownership of result. */
static cJSON *make_response(cJSON *id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else    cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Handlers
   ═══════════════════════════════════════════════════════════════════════════ */

static cJSON *handle_initialize(cJSON *id, cJSON *params) {
    (void)params;

    // TODO none of the client capabilities are checked here.

    /* Server info */
    cJSON *server_info = cJSON_CreateObject();
    cJSON *sn = cJSON_CreateArray();
    // TODO add server version, with program version macro so it is always up to date
    cJSON_AddItemToArray(sn, cJSON_CreateString("taskjuggler-lsp"));
    cJSON_AddItemToObject(server_info, "name", sn);

    /* Completion options */
    cJSON *comp_triggers = cJSON_CreateArray();
    cJSON_AddItemToArray(comp_triggers, cJSON_CreateString(","));
    cJSON_AddItemToArray(comp_triggers, cJSON_CreateString(" "));
    cJSON_AddItemToArray(comp_triggers, cJSON_CreateString("!"));
    cJSON *comp_opts = cJSON_CreateObject();
    cJSON_AddItemToObject(comp_opts, "triggerCharacters", comp_triggers);
    cJSON_AddBoolToObject(comp_opts,  "resolveProvider", 0);

    /* Signature help */
    cJSON *sig_triggers = cJSON_CreateArray();
    cJSON_AddItemToArray(sig_triggers, cJSON_CreateString(" "));
    cJSON *sig_opts = cJSON_CreateObject();
    cJSON_AddItemToObject(sig_opts, "triggerCharacters", sig_triggers);

    /* Capabilities */
    cJSON *caps = cJSON_CreateObject();
    cJSON *tds = cJSON_CreateObject();
    cJSON_AddNumberToObject(tds, "change", 1); /* TextDocumentSyncKind.Full */
    cJSON *open_close = cJSON_CreateBool(1);
    cJSON_AddItemToObject(tds, "openClose", open_close);
    /* Semantic tokens */
    cJSON *sem_types = cJSON_CreateArray();
    for (int i = 0; i < num_semantic_token_types; i++)
        cJSON_AddItemToArray(sem_types,
                             cJSON_CreateString(semantic_token_type_names[i]));
    cJSON *sem_mods = cJSON_CreateArray();
    for (int i = 0; i < num_semantic_token_modifiers; i++)
        cJSON_AddItemToArray(sem_mods,
                             cJSON_CreateString(semantic_token_modifier_names[i]));
    cJSON *sem_legend = cJSON_CreateObject();
    cJSON_AddItemToObject(sem_legend, "tokenTypes",     sem_types);
    cJSON_AddItemToObject(sem_legend, "tokenModifiers", sem_mods);
    cJSON *sem_opts = cJSON_CreateObject();
    cJSON_AddItemToObject(sem_opts, "legend", sem_legend);
    cJSON_AddBoolToObject(sem_opts,  "full",  1);
    /* TODO: add "range": true when textDocument/semanticTokens/range is implemented */

    cJSON_AddItemToObject(caps, "textDocumentSync",          tds);
    cJSON_AddBoolToObject(caps, "documentSymbolProvider",    1);
    cJSON_AddBoolToObject(caps, "foldingRangeProvider",      1);
    cJSON_AddBoolToObject(caps, "hoverProvider",             1);
    cJSON_AddBoolToObject(caps, "definitionProvider",        1);
    cJSON_AddBoolToObject(caps, "referencesProvider",        1);
    cJSON_AddItemToObject(caps, "signatureHelpProvider",     sig_opts);
    cJSON_AddItemToObject(caps, "completionProvider",        comp_opts);
    cJSON_AddItemToObject(caps, "semanticTokensProvider",    sem_opts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON_AddItemToObject(result, "serverInfo",   server_info);

    return make_response(id, result);
}

static cJSON *handle_shutdown(cJSON *id) {
    return make_response(id, cJSON_CreateNull());
}

static void handle_didopen(cJSON *id, cJSON *params) {
    (void)id;
    cJSON *tdi = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
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

    publish_diagnostics(uri, &docs[i].parse);
}

static void handle_didchange(cJSON *params) {
    cJSON *tdi     = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    cJSON *changes = cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
    if (!tdi || !changes || !cJSON_IsArray(changes)) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    int n = cJSON_GetArraySize(changes);
    if (n <= 0) return;

    /* Use the last change (full sync) */
    cJSON      *last = cJSON_GetArrayItem(changes, n - 1);
    const char *text = json_str(last, "text");
    if (!text) return;

    Document *d = doc_find(uri);
    if (!d) d = doc_alloc(uri);
    if (!d) return;

    free(d->text);
    d->text = strdup(text);
    parse_result_free(&d->parse);
    d->parse = parse(text);
    publish_diagnostics(uri, &d->parse);
}

static void handle_didclose(cJSON *params) {
    cJSON *tdi = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (!tdi) return;

    const char *uri = json_str(tdi, "uri");
    if (!uri) return;

    Document *d = doc_find(uri);
    if (!d) return;

    /* Publish empty diagnostics to clear client-side errors */
    ParseResult empty = {0};
    publish_diagnostics(uri, &empty);
    doc_free(d);
}

static cJSON *handle_document_symbol(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    cJSON *arr = build_document_symbols_json(d->parse.doc_symbols,
                                             d->parse.num_doc_symbols);
    return make_response(id, arr);
}

static cJSON *handle_folding_range(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    cJSON *arr = build_folding_ranges_json(d->parse.tok_spans,
                                           d->parse.num_tok_spans);
    return make_response(id, arr);
}

static cJSON *handle_hover(cJSON *id, cJSON *params) {
    cJSON *tdp = cJSON_GetObjectItemCaseSensitive(params, "textDocumentPosition");
    if (!tdp) tdp = params; /* some clients pass position at top level */

    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    cJSON *pos_obj = cJSON_GetObjectItemCaseSensitive(tdp, "position");
    if (!pos_obj) pos_obj = cJSON_GetObjectItemCaseSensitive(params, "position");

    if (!uri || !pos_obj) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    LspPos pos = json_to_pos(pos_obj);

    ActiveKeyword ak = active_keyword_at(d->parse.tok_spans, d->parse.num_tok_spans, pos);
    if (!ak.keyword) return make_response(id, cJSON_CreateNull());

    const char *doc_text = keyword_docs(ak.keyword);
    free(ak.keyword);
    if (!doc_text) return make_response(id, cJSON_CreateNull());

    cJSON *contents = cJSON_CreateObject();
    cJSON_AddStringToObject(contents, "kind",  "markdown");
    cJSON_AddStringToObject(contents, "value", doc_text);

    cJSON *hover = cJSON_CreateObject();
    cJSON_AddItemToObject(hover, "contents", contents);
    cJSON_AddItemToObject(hover, "range",    range_json(ak.range));

    return make_response(id, hover);
}

static cJSON *handle_signature_help(cJSON *id, cJSON *params) {
    cJSON *tdp = cJSON_GetObjectItemCaseSensitive(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    cJSON *pos_obj = cJSON_GetObjectItemCaseSensitive(tdp, "position");
    if (!pos_obj) pos_obj = cJSON_GetObjectItemCaseSensitive(params, "position");

    if (!uri || !pos_obj) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    LspPos pos = json_to_pos(pos_obj);
    ActiveContext ac = active_context(d->parse.tok_spans, d->parse.num_tok_spans, pos);
    if (!ac.keyword) return make_response(id, cJSON_CreateNull());

    cJSON *sig = build_signature_help_json(ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(id, cJSON_CreateNull());
    return make_response(id, sig);
}

static cJSON *handle_semantic_tokens_full(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    cJSON *result = build_semantic_tokens_json(d->parse.tok_spans,
                                               d->parse.num_tok_spans);
    return make_response(id, result);
}

static cJSON *handle_references(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");

    cJSON *pos_obj = cJSON_GetObjectItemCaseSensitive(params, "position");

    if (!uri || !pos_obj) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    LspPos pos    = json_to_pos(pos_obj);
    cJSON *result = build_references_json(d->parse.def_links,
                                          d->parse.num_def_links,
                                          d->parse.doc_symbols,
                                          d->parse.num_doc_symbols,
                                          pos, uri);
    if (!result) return make_response(id, cJSON_CreateNull());
    return make_response(id, result);
}

static cJSON *handle_definition(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");

    cJSON *pos_obj = cJSON_GetObjectItemCaseSensitive(params, "position");

    if (!uri || !pos_obj) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    LspPos pos    = json_to_pos(pos_obj);
    cJSON *result = build_definition_json(d->parse.def_links,
                                          d->parse.num_def_links,
                                          pos, uri);
    if (!result) return make_response(id, cJSON_CreateNull());
    return make_response(id, result);
}

static cJSON *handle_completion(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *tdp = cJSON_GetObjectItemCaseSensitive(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    cJSON *td = cJSON_GetObjectItemCaseSensitive(tdp, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) {
        td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        if (td) uri = json_str(td, "uri");
    }

    cJSON *pos_obj = cJSON_GetObjectItemCaseSensitive(tdp, "position");
    if (!pos_obj) pos_obj = cJSON_GetObjectItemCaseSensitive(params, "position");

    if (!uri || !pos_obj) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    LspPos pos    = json_to_pos(pos_obj);
    cJSON *result = build_completions_json(d->parse.tok_spans, d->parse.num_tok_spans, pos,
                                           d->parse.doc_symbols, d->parse.num_doc_symbols);
    return make_response(id, result);
}


/* ═══════════════════════════════════════════════════════════════════════════
   Main dispatch
   ═══════════════════════════════════════════════════════════════════════════ */

char *server_process(const char *json_text) {
    cJSON *msg = cJSON_Parse(json_text);
    if (!msg) return NULL;

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *method  = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *params  = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (!params) params = cJSON_CreateObject(); /* avoid NULL checks everywhere */

    const char *m = (method && cJSON_IsString(method)) ? method->valuestring : "";

    cJSON *resp = NULL;

    if (strcmp(m, "initialize") == 0) {
        resp = handle_initialize(id_item, params);

    } else if (strcmp(m, "initialized") == 0) {
        /* notification — no response */

    } else if (strcmp(m, "shutdown") == 0) {
        resp = handle_shutdown(id_item);

    } else if (strcmp(m, "exit") == 0) {
        cJSON_Delete(msg);
        exit(0);

    } else if (strcmp(m, "textDocument/didOpen") == 0) {
	 handle_didopen(id_item, params);
	 /* no response needed */

    } else if (strcmp(m, "textDocument/didChange") == 0) {
        handle_didchange(params);

    } else if (strcmp(m, "textDocument/didClose") == 0) {
        handle_didclose(params);

    } else if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(id_item, params);

    } else if (strcmp(m, "textDocument/foldingRange") == 0) {
        resp = handle_folding_range(id_item, params);

    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(id_item, params);

    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(id_item, params);

    } else if (strcmp(m, "textDocument/references") == 0) {
        resp = handle_references(id_item, params);

    } else if (strcmp(m, "textDocument/definition") == 0) {
        resp = handle_definition(id_item, params);

    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(id_item, params);

    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens_full(id_item, params);

    /* TODO: textDocument/semanticTokens/full/delta — requires storing a
     * resultId per document and computing a token diff against the previously
     * returned set.  Advertise "full": { "delta": true } in capabilities once
     * implemented. */

    /* TODO: textDocument/semanticTokens/range — requires filtering tok_spans
     * to the requested range before encoding.  Advertise "range": true in
     * capabilities once implemented. */

    } else if (id_item) {
        /* Unknown request — return null result */
        resp = make_response(id_item, cJSON_CreateNull());
    }

    cJSON_Delete(msg);

    if (!resp) return NULL;

    char *text = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return text;
}

void server_init() {
     // Initialize array of Document objects
     for (int i=0; i<MAX_DOCS; i++) {
	  docs[i].in_use = 0;
     }
}
