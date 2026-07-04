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

/* JSON-RPC dispatch: the routing layer between the framed transport in
 * main.c and the handlers.  For the data-flow overview, document lifecycle,
 * and query-dispatch table, see doc/modules/server.rst.
 *
 * Concurrency model: the coordinator thread owns the live document store
 * (document_store.h).  Notifications and the inline lifecycle methods
 * (initialize / shutdown) mutate it under docs_mutex on the coordinator —
 * those handlers live in lifecycle.c, and anything document-changing funnels
 * through workspace.c's revalidate_all_docs().  Every other request is a
 * read-only query: the coordinator pins the current immutable workspace
 * snapshot into the Job's query_context under docs_mutex, then a worker runs
 * the handler lock-free against the pinned snapshot.  See query_context.h. */

#include "server.h"
#include "document_store.h"
#include "workspace.h"
#include "lifecycle.h"
#include "query_context.h"
#include "job_queue.h"
#include "threadpool.h"
#include "diag_worker.h"
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
#include "rpc.h"
#include "debug.h"

#include <yyjson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Notification dispatch
   ═══════════════════════════════════════════════════════════════════════════ */

/** Return non-zero when @p method is a JSON-RPC notification (i.e. a
 *  state-mutating lifecycle method that carries no id).
 *  @param method  JSON-RPC method string from the incoming message.
 *  @return        Non-zero if @p method is a notification, zero otherwise. */
static int is_notification_method(const char *method) {
    if (!method) return 0;
    return strcmp(method, "initialized") == 0
        || strcmp(method, "textDocument/didOpen") == 0
        || strcmp(method, "textDocument/didChange") == 0
        || strcmp(method, "textDocument/didClose") == 0
        || strcmp(method, "workspace/didChangeWatchedFiles") == 0
        || strcmp(method, "workspace/didRenameFiles") == 0;
}

/** Serialize @p resp as the root of @p out_doc and send it to the client
 *  via lsp_send_message().
 *  @param out_doc  Mutable document whose root will be set to @p resp.
 *  @param resp     JSON-RPC response or error object to serialize and send. */
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

/* ═══════════════════════════════════════════════════════════════════════════
   Query coordination
   ═══════════════════════════════════════════════════════════════════════════ */

/** Extract the primary document URI from @p params, accepting both the flat
 *  "textDocument" form and the nested "textDocumentPosition" form.
 *  @param params  JSON params object of the incoming request; may be NULL.
 *  @return        Borrowed URI string, or NULL when not found. */
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

/** Build a query_doc view over @p w's current snapshot for a pinned query
 *  context. All pointers are borrowed from the snapshot (the context holds a
 *  ref on the workspace snapshot for the query's lifetime); @p is_primary
 *  marks the single document the request targets.
 *  @param w           Workspace doc whose snapshot is non-NULL.
 *  @param is_primary  Non-zero when this is the request's primary document.
 *  @return            A query_doc view borrowing from @p w's snapshot. */
static query_doc make_query_doc(ws_doc *w, int is_primary) {
    doc_snapshot *s = w->snap;
    return (query_doc){
        .uri             = s->uri,
        .text            = s->text,
        .task_prefix     = w->task_prefix,
        .account_prefix  = w->account_prefix,
        .report_prefix   = w->report_prefix,
        .resource_prefix = w->resource_prefix,
        .root            = s->root,
        .tok_spans       = s->tok_spans,
        .tok_owners      = s->tok_owners,
        .num_tok_spans   = s->num_tok_spans,
        .num_sem_entries = s->num_sem_entries,
        .is_primary      = is_primary,
        .snap            = s,
    };
}

/** Build the query_context for one query by pinning the currently published
 *  workspace snapshot and populating borrowed query_doc views from its
 *  ws_docs.  When @p want_all_docs is set every document is included (no
 *  primary); otherwise the context holds the primary document plus its
 *  same-project siblings.  The primary's previous snapshot is also retained
 *  for semanticTokens/delta.  Caller must hold docs_mutex.
 *  @param primary_uri  Canonical URI of the document the request targets;
 *                      may be NULL for workspace-global requests.
 *  @param want_all_docs  Non-zero to include all documents (workspace/symbol).
 *  @return             Heap-allocated query_context; caller takes ownership. */
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

    workspace_snapshot *ws = ws_acquire(workspace_current());
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
        qc->docs[n] = make_query_doc(w, is_primary);
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

/** Return non-zero when @p m is a method that must be handled inline on the
 *  coordinator thread rather than dispatched to a worker.  Currently only
 *  "initialize" and "shutdown" qualify.
 *  @param m  JSON-RPC method string.
 *  @return   Non-zero for inline methods, zero for worker-dispatched ones. */
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

/* ═══════════════════════════════════════════════════════════════════════════
   Inbound message entry point
   ═══════════════════════════════════════════════════════════════════════════ */

void server_process(const char *json_text) {
    yyjson_doc *in_doc = yyjson_read(json_text, strlen(json_text), 0);
    if (!in_doc) return;

    yyjson_val *root   = yyjson_doc_get_root(in_doc);
    yyjson_val *method = yyjson_obj_get(root, "method");
    const char *m = (method && yyjson_is_str(method)) ? yyjson_get_str(method) : "";

    DLOG(DEBUG_RPC, LOG_VERBOSE, "recv method=%s", m[0] ? m : "(response)");

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
    docstore_init();
}
