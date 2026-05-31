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

#include "query_context.h"
#include "semantic_tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

/* ── DocSnapshot ─────────────────────────────────────────────────────────── */

DocSnapshot *docsnap_new(const char *uri, const char *text,
                         tj_node *root, TokenSpan *tok_spans,
                         int num_tok_spans, int num_sem_entries,
                         uint64_t doc_version) {
    DocSnapshot *s = calloc(1, sizeof(DocSnapshot));
    if (!s) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    atomic_init(&s->refcount, 1);
    s->doc_version     = doc_version;
    s->uri             = dup_or_null(uri);
    s->text            = dup_or_null(text);
    s->root            = root;        /* ownership moved in */
    s->tok_spans       = tok_spans;   /* ownership moved in */
    s->num_tok_spans   = num_tok_spans;
    s->num_sem_entries = num_sem_entries;
    atomic_init(&s->sem_memo, NULL);
    return s;
}

DocSnapshot *docsnap_acquire(DocSnapshot *s) {
    if (s) atomic_fetch_add_explicit(&s->refcount, 1, memory_order_relaxed);
    return s;
}

void docsnap_release(DocSnapshot *s) {
    if (!s) return;
    if (atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel) != 1)
        return;

    tj_node_free(s->root);
    for (int i = 0; i < s->num_tok_spans; i++)
        free(s->tok_spans[i].text);
    free(s->tok_spans);
    free(s->uri);
    free(s->text);

    SemTokenData *memo = atomic_load_explicit(&s->sem_memo, memory_order_acquire);
    if (memo) {
        free(memo->data);
        free(memo);
    }
    free(s);
}

void docsnap_sem_tokens(DocSnapshot *s, const uint32_t **out_data, size_t *out_count) {
    SemTokenData *memo = atomic_load_explicit(&s->sem_memo, memory_order_acquire);
    if (!memo) {
        /* Compute a fresh buffer and try to publish it.  The token data is a
         * pure function of the immutable token spans, so concurrent first
         * callers all compute identical results; compare-exchange lets one
         * win and the losers free their buffer and adopt the winner's. */
        SemTokenData *fresh = malloc(sizeof(SemTokenData));
        if (!fresh) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        fresh->data  = NULL;
        fresh->count = 0;
        compute_semantic_tokens_data(s->tok_spans, s->num_tok_spans,
                                     s->num_sem_entries,
                                     &fresh->data, &fresh->count);

        SemTokenData *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&s->sem_memo, &expected, fresh,
                                                     memory_order_acq_rel,
                                                     memory_order_acquire)) {
            memo = fresh;
        } else {
            free(fresh->data);
            free(fresh);
            memo = expected;
        }
    }
    *out_data  = memo->data;
    *out_count = memo->count;
}

/* ── WorkspaceSnapshot ───────────────────────────────────────────────────── */

WorkspaceSnapshot *ws_alloc(int num_docs) {
    WorkspaceSnapshot *ws = calloc(1, sizeof(WorkspaceSnapshot));
    if (!ws) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    atomic_init(&ws->refcount, 1);
    ws->num_docs = num_docs;
    if (num_docs > 0) {
        ws->docs = calloc((size_t)num_docs, sizeof(WsDoc));
        if (!ws->docs) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        for (int i = 0; i < num_docs; i++)
            ws->docs[i].project_index = -1;
    }
    return ws;
}

int ws_add_project(WorkspaceSnapshot *ws, const char *id) {
    if (ws->num_projects >= ws->projects_cap) {
        int nc = ws->projects_cap ? ws->projects_cap * 2 : 4;
        WsProject **tmp = realloc(ws->projects, (size_t)nc * sizeof(WsProject *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        ws->projects     = tmp;
        ws->projects_cap = nc;
    }
    WsProject *p = calloc(1, sizeof(WsProject));
    if (!p) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    p->id = dup_or_null(id);
    int index = ws->num_projects;
    ws->projects[ws->num_projects++] = p;
    return index;
}

WorkspaceSnapshot *ws_acquire(WorkspaceSnapshot *ws) {
    if (ws) atomic_fetch_add_explicit(&ws->refcount, 1, memory_order_relaxed);
    return ws;
}

void ws_release(WorkspaceSnapshot *ws) {
    if (!ws) return;
    if (atomic_fetch_sub_explicit(&ws->refcount, 1, memory_order_acq_rel) != 1)
        return;

    for (int i = 0; i < ws->num_docs; i++) {
        docsnap_release(ws->docs[i].snap);
        free(ws->docs[i].task_prefix);
        free(ws->docs[i].account_prefix);
        free(ws->docs[i].report_prefix);
        free(ws->docs[i].resource_prefix);
    }
    free(ws->docs);

    for (int i = 0; i < ws->num_projects; i++) {
        WsProject *p = ws->projects[i];
        if (!p) continue;
        project_node_free_children(&p->root);
        free(p->id);
        free(p);
    }
    free(ws->projects);

    free(ws);
}

/* ── query_context ───────────────────────────────────────────────────────── */

const query_doc *query_context_primary(const query_context *qc) {
    if (!qc || qc->primary_idx < 0 || qc->primary_idx >= qc->num_docs)
        return NULL;
    return &qc->docs[qc->primary_idx];
}

void query_context_free(query_context *qc) {
    if (!qc) return;
    free(qc->docs);
    docsnap_release(qc->prev_snap);
    ws_release(qc->ws);
    free(qc);
}
