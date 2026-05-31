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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

/* Clone one document's read state into @p dst, deep-copying the tj_node tree
 * and relocating each cloned TokenSpan.owner into that copy via the pointer
 * map produced by tj_node_deep_copy(). */
static void query_doc_clone(query_doc *dst, const query_doc_src *src) {
    dst->uri             = dup_or_null(src->uri);
    dst->text            = dup_or_null(src->text);
    dst->task_prefix     = dup_or_null(src->task_prefix);
    dst->account_prefix  = dup_or_null(src->account_prefix);
    dst->report_prefix   = dup_or_null(src->report_prefix);
    dst->resource_prefix = dup_or_null(src->resource_prefix);
    dst->num_sem_entries = src->num_sem_entries;
    dst->is_primary      = src->is_primary;

    tj_node_map map = {0};
    dst->root = tj_node_deep_copy(src->root, &map);

    dst->num_tok_spans = src->num_tok_spans;
    if (src->num_tok_spans > 0 && src->tok_spans) {
        dst->tok_spans = malloc((size_t)src->num_tok_spans * sizeof(TokenSpan));
        if (!dst->tok_spans) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        for (int i = 0; i < src->num_tok_spans; i++) {
            const TokenSpan *s = &src->tok_spans[i];
            TokenSpan       *d = &dst->tok_spans[i];
            d->token_kind = s->token_kind;
            d->start      = s->start;
            d->end        = s->end;
            d->text       = s->text ? strdup(s->text) : NULL;
            d->owner      = tj_node_map_lookup(&map, s->owner);
        }
    }

    tj_node_map_free(&map);
}

static void query_doc_free_fields(query_doc *d) {
    free(d->uri);
    free(d->text);
    free(d->task_prefix);
    free(d->account_prefix);
    free(d->report_prefix);
    free(d->resource_prefix);
    tj_node_free(d->root);
    for (int i = 0; i < d->num_tok_spans; i++)
        free(d->tok_spans[i].text);
    free(d->tok_spans);
}

query_context *query_context_build(const query_doc_src *srcs, int num_docs,
                                   int primary_idx,
                                   const ProjectNode *project_root,
                                   const char *project_id) {
    query_context *qc = calloc(1, sizeof(query_context));
    if (!qc) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    qc->primary_idx  = primary_idx;
    qc->project_root = project_node_deep_copy(project_root);
    qc->project_id   = dup_or_null(project_id);

    if (num_docs > 0) {
        qc->docs = calloc((size_t)num_docs, sizeof(query_doc));
        if (!qc->docs) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        for (int i = 0; i < num_docs; i++)
            query_doc_clone(&qc->docs[i], &srcs[i]);
        qc->num_docs = num_docs;
    }

    return qc;
}

const query_doc *query_context_primary(const query_context *qc) {
    if (!qc || qc->primary_idx < 0 || qc->primary_idx >= qc->num_docs)
        return NULL;
    return &qc->docs[qc->primary_idx];
}

void query_context_free(query_context *qc) {
    if (!qc) return;
    for (int i = 0; i < qc->num_docs; i++)
        query_doc_free_fields(&qc->docs[i]);
    free(qc->docs);
    project_node_free(qc->project_root);
    free(qc->project_id);
    free(qc);
}
