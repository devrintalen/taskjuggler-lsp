/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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
#include "hover.h"
#include "signature.h"
#include "completion.h"
#include "semantic_tokens.h"

#include <stdio.h>
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
   Publish diagnostics notification
   ═══════════════════════════════════════════════════════════════════════════ */

static void write_message(const char *msg) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
}

static void publish_diagnostics(const char *uri, const ParseResult *r) {
    cJSON *diag_arr = cJSON_CreateArray();
    for (int i = 0; i < r->num_diagnostics; i++) {
        const Diagnostic *d = &r->diagnostics[i];
        cJSON *dj = cJSON_CreateObject();

        cJSON *range = cJSON_CreateObject();
        cJSON *start = cJSON_CreateObject();
        cJSON_AddNumberToObject(start, "line",      d->range.start.line);
        cJSON_AddNumberToObject(start, "character", d->range.start.character);
        cJSON *end = cJSON_CreateObject();
        cJSON_AddNumberToObject(end, "line",      d->range.end.line);
        cJSON_AddNumberToObject(end, "character", d->range.end.character);
        cJSON_AddItemToObject(range, "start", start);
        cJSON_AddItemToObject(range, "end",   end);
        cJSON_AddItemToObject(dj, "range", range);
        cJSON_AddNumberToObject(dj, "severity", d->severity);
        cJSON_AddStringToObject(dj, "message",  d->message);
        cJSON_AddItemToArray(diag_arr, dj);
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    cJSON_AddItemToObject(params, "diagnostics", diag_arr);

    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "textDocument/publishDiagnostics");
    cJSON_AddItemToObject(notif, "params", params);

    char *text = cJSON_PrintUnformatted(notif);
    write_message(text);
    cJSON_free(text);
    cJSON_Delete(notif);
}

/* ═══════════════════════════════════════════════════════════════════════════
   on_change: parse + store + publish
   ═══════════════════════════════════════════════════════════════════════════ */

static void on_change(const char *uri, const char *text) {
    Document *d = doc_find(uri);
    if (!d) d = doc_alloc(uri);
    if (!d) return;

    free(d->text);
    d->text = strdup(text);

    parse_result_free(&d->parse);
    d->parse = parse(text);

    publish_diagnostics(uri, &d->parse);
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
   Symbol → DocumentSymbol JSON
   ═══════════════════════════════════════════════════════════════════════════ */

static cJSON *range_json(LspRange r) {
    cJSON *s = cJSON_CreateObject();
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "line",      r.start.line);
    cJSON_AddNumberToObject(st, "character", r.start.character);
    cJSON *en = cJSON_CreateObject();
    cJSON_AddNumberToObject(en, "line",      r.end.line);
    cJSON_AddNumberToObject(en, "character", r.end.character);
    cJSON_AddItemToObject(s, "start", st);
    cJSON_AddItemToObject(s, "end",   en);
    return s;
}

static cJSON *symbol_to_json(const Symbol *sym) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name",   sym->name   ? sym->name   : "");
    cJSON_AddStringToObject(obj, "detail", sym->detail ? sym->detail : "");
    cJSON_AddNumberToObject(obj, "kind",   sym->kind);
    cJSON_AddItemToObject(obj, "range",          range_json(sym->range));
    cJSON_AddItemToObject(obj, "selectionRange", range_json(sym->selection_range));
    if (sym->num_children > 0) {
        cJSON *ch = cJSON_CreateArray();
        for (int i = 0; i < sym->num_children; i++)
            cJSON_AddItemToArray(ch, symbol_to_json(&sym->children[i]));
        cJSON_AddItemToObject(obj, "children", ch);
    }
    return obj;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Handlers
   ═══════════════════════════════════════════════════════════════════════════ */

static cJSON *handle_initialize(cJSON *id, cJSON *params) {
    (void)params;

    /* SemanticTokensLegend */
    cJSON *legend = cJSON_CreateObject();
    cJSON *tt = cJSON_CreateArray();
    cJSON_AddItemToArray(tt, cJSON_CreateString("string"));
    cJSON *tm = cJSON_CreateArray();
    cJSON_AddItemToArray(tm, cJSON_CreateString("declaration"));
    cJSON_AddItemToObject(legend, "tokenTypes",     tt);
    cJSON_AddItemToObject(legend, "tokenModifiers", tm);

    cJSON *sem_opts = cJSON_CreateObject();
    cJSON_AddItemToObject(sem_opts, "legend", legend);
    cJSON_AddBoolToObject(sem_opts, "full", 1);

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
    cJSON_AddItemToObject(caps, "textDocumentSync",       tds);
    cJSON_AddBoolToObject(caps, "documentSymbolProvider", 1);
    cJSON_AddBoolToObject(caps, "hoverProvider",          1);
    cJSON_AddItemToObject(caps, "signatureHelpProvider",  sig_opts);
    cJSON_AddItemToObject(caps, "completionProvider",     comp_opts);
    cJSON_AddItemToObject(caps, "semanticTokensProvider", sem_opts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "capabilities", caps);

    return make_response(id, result);
}

static cJSON *handle_shutdown(cJSON *id) {
    return make_response(id, cJSON_CreateNull());
}

static cJSON *handle_document_symbol(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < d->parse.num_symbols; i++)
        cJSON_AddItemToArray(arr, symbol_to_json(&d->parse.symbols[i]));
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
    Token  tok = token_at(d->text, pos);
    if (tok.kind == TK_EOF || !tok.text || !tok.text[0]) {
        token_free(&tok);
        return make_response(id, cJSON_CreateNull());
    }

    const char *docs = keyword_docs(tok.text);
    if (!docs) {
        token_free(&tok);
        return make_response(id, cJSON_CreateNull());
    }

    cJSON *contents = cJSON_CreateObject();
    cJSON_AddStringToObject(contents, "kind",  "markdown");
    cJSON_AddStringToObject(contents, "value", docs);

    cJSON *hover = cJSON_CreateObject();
    cJSON_AddItemToObject(hover, "contents", contents);

    cJSON *range = cJSON_CreateObject();
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "line",      tok.start.line);
    cJSON_AddNumberToObject(st, "character", tok.start.character);
    cJSON *en = cJSON_CreateObject();
    cJSON_AddNumberToObject(en, "line",      tok.end.line);
    cJSON_AddNumberToObject(en, "character", tok.end.character);
    cJSON_AddItemToObject(range, "start", st);
    cJSON_AddItemToObject(range, "end",   en);
    cJSON_AddItemToObject(hover, "range", range);

    token_free(&tok);
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
    ActiveContext ac = active_context(d->text, pos);
    if (!ac.keyword) return make_response(id, cJSON_CreateNull());

    cJSON *sig = build_signature_help_json(ac.keyword, ac.arg_count);
    free(ac.keyword);
    if (!sig) return make_response(id, cJSON_CreateNull());
    return make_response(id, sig);
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
    cJSON *result = completions_json(d->text, pos,
                                     d->parse.symbols, d->parse.num_symbols);
    return make_response(id, result);
}

static cJSON *handle_semantic_tokens(cJSON *id, cJSON *params) {
    const char *uri = NULL;
    cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (td) uri = json_str(td, "uri");
    if (!uri) return make_response(id, cJSON_CreateNull());

    Document *d = doc_find(uri);
    if (!d) return make_response(id, cJSON_CreateNull());

    return make_response(id, build_semantic_tokens_json(d->text));
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
        cJSON *tdi = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        if (tdi) {
            const char *uri  = json_str(tdi, "uri");
            const char *text = json_str(tdi, "text");
            if (uri && text) on_change(uri, text);
        }

    } else if (strcmp(m, "textDocument/didChange") == 0) {
        cJSON *tdi = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        cJSON *changes = cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
        if (tdi && changes && cJSON_IsArray(changes)) {
            const char *uri = json_str(tdi, "uri");
            /* Use the last change (full sync) */
            int n = cJSON_GetArraySize(changes);
            if (n > 0 && uri) {
                cJSON *last = cJSON_GetArrayItem(changes, n - 1);
                const char *text = json_str(last, "text");
                if (text) on_change(uri, text);
            }
        }

    } else if (strcmp(m, "textDocument/didClose") == 0) {
        cJSON *tdi = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
        if (tdi) {
            const char *uri = json_str(tdi, "uri");
            if (uri) {
                Document *d = doc_find(uri);
                if (d) {
                    /* Publish empty diagnostics */
                    ParseResult empty = {0};
                    publish_diagnostics(uri, &empty);
                    doc_free(d);
                }
            }
        }

    } else if (strcmp(m, "textDocument/documentSymbol") == 0) {
        resp = handle_document_symbol(id_item, params);

    } else if (strcmp(m, "textDocument/hover") == 0) {
        resp = handle_hover(id_item, params);

    } else if (strcmp(m, "textDocument/signatureHelp") == 0) {
        resp = handle_signature_help(id_item, params);

    } else if (strcmp(m, "textDocument/completion") == 0) {
        resp = handle_completion(id_item, params);

    } else if (strcmp(m, "textDocument/semanticTokens/full") == 0) {
        resp = handle_semantic_tokens(id_item, params);

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
