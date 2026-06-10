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

#include "document_highlight.h"
#include "document_symbol.h"
#include "dependency.h"
#include "grammar.tab.h"
#include "rpc.h"
#include <string.h>

/* Document highlight resolves the cursor to a single target task and reports
 * every same-document occurrence of that task: its declaration as Write
 * (kind 3) and each dependency that resolves to it as Read (kind 2).  The
 * cursor → target resolution (which triggers from both declaration and
 * reference sites) runs in the handler against the pinned snapshot's
 * ProjectNode tree; this file walks that same tree to collect the
 * occurrences, mirroring references.c but emitting highlight kinds instead
 * of cross-file Locations. */

/**
 * Append a single document highlight object to @p arr.
 *
 * @param doc    yyjson mutable document used for all allocations.
 * @param arr    JSON array to append the highlight object to.
 * @param range  Source range covered by this highlight.
 * @param kind   LSP DocumentHighlightKind value (2 = Read, 3 = Write).
 */
static void push_highlight(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                           LspRange range, int kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "range", range_json(doc, range));
    yyjson_mut_obj_add_uint(doc, obj, "kind", (uint64_t)kind);
    yyjson_mut_arr_add_val(arr, obj);
}

/**
 * Find the last TK_IDENT token whose start position falls within @p range
 * and write its range into @p out.
 *
 * A dependency's source_range spans the whole reference (leading bangs and
 * every dotted segment); the highlight covers only the identifier token that
 * names the resolved target, i.e. the path's final segment.
 *
 * @param tokens      Array of token spans for the current document.
 * @param num_tokens  Number of entries in @p tokens.
 * @param range       Source range to search within.
 * @param out         Output parameter set to the range of the last matching
 *                    TK_IDENT token when one is found.
 * @return            1 if a matching token was found, 0 otherwise.
 */
static int last_ident_in_range(const TokenSpan *tokens, int num_tokens,
                               LspRange range, LspRange *out) {
    int found = 0;
    for (int t = 0; t < num_tokens; t++) {
        if (tokens[t].token_kind != TK_IDENT) continue;
        if (pos_cmp(tokens[t].start, range.start) < 0) continue;
        if (pos_cmp(tokens[t].start, range.end) > 0) continue;
        out->start = tokens[t].start;
        out->end   = tokens[t].end;
        found = 1;
    }
    return found;
}

/**
 * Recursively walk the project tree depth-first, appending a Read highlight
 * (kind 2) for each dependency that is declared in @p doc_uri and resolves to
 * @p wanted.  Cross-file dependencies are skipped because highlight ranges
 * are document-local.
 *
 * @param doc           yyjson mutable document used for all allocations.
 * @param arr           JSON array to append highlight objects to.
 * @param node          Current node in the depth-first traversal.
 * @param wanted        Target node that dependencies must resolve to.
 * @param project_root  Root of the project tree, passed to
 *                      project_dep_resolve() for resolution.
 * @param doc_uri       URI of the document being highlighted; only
 *                      dependencies whose source_uri matches are emitted.
 * @param tokens        Array of token spans for the current document.
 * @param num_tokens    Number of entries in @p tokens.
 */
static void collect_read_highlights(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                    ProjectNode *node, const ProjectNode *wanted,
                                    ProjectNode *project_root,
                                    const char *doc_uri,
                                    const TokenSpan *tokens, int num_tokens) {
    if (!node) return;
    if (node->source_uri && strcmp(node->source_uri, doc_uri) == 0) {
        for (int i = 0; i < node->num_dependencies; i++) {
            if (project_dep_resolve(node, i, project_root) != wanted) continue;
            LspRange segment;
            if (last_ident_in_range(tokens, num_tokens,
                                    node->dependencies[i].source_range,
                                    &segment))
                push_highlight(doc, arr, segment, 2);
        }
    }
    for (int i = 0; i < node->num_children; i++)
        collect_read_highlights(doc, arr, node->children[i], wanted,
                                project_root, doc_uri, tokens, num_tokens);
}

yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    ProjectNode *project_root,
    const ProjectNode *wanted,
    const char *doc_uri,
    const TokenSpan *tokens, int num_tokens) {

    if (!project_root || !wanted || !doc_uri) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    /* Write: the declaration itself, only when it lives in this document. */
    if (wanted->source_uri && strcmp(wanted->source_uri, doc_uri) == 0)
        push_highlight(doc, arr, wanted->selection_range, 3);

    /* Read: every in-document dependency resolving to the target. */
    collect_read_highlights(doc, arr, project_root, wanted, project_root,
                            doc_uri, tokens, num_tokens);

    return arr;
}

/** Resolve the cursor to the single task it highlights: directly from a task
 *  declaration, or from a dependency reference to its target task (the
 *  highlight target is the same in both directions). Resolution runs against
 *  the pinned snapshot's ProjectNode tree, exactly as definition/references
 *  do.
 *  @param qc   Pinned query context holding the workspace snapshot.
 *  @param d    Query document the cursor is in.
 *  @param pos  Cursor position to resolve.
 *  @return The target ProjectNode, or NULL when the cursor is on neither a
 *          task declaration nor a resolvable dependency. */
static ProjectNode *resolve_highlight_target(const query_context *qc,
                                             const query_doc *d, LspPos pos) {
    tj_node *decl = task_decl_at_cursor(d->tok_spans, d->tok_owners,
                                        d->num_tok_spans, pos);
    if (decl)
        return project_node_for_doc_task(qc->project_root, d->task_prefix, decl);

    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    if (!dependency_at_cursor(d->tok_spans, d->tok_owners, d->num_tok_spans, pos,
                              &owner, &dep))
        return NULL;

    return project_resolve_dep_ref(qc->project_root, d->task_prefix, owner, dep);
}

yyjson_mut_val *handle_document_highlight(yyjson_mut_doc *doc, yyjson_val *id,
                                          yyjson_val *params,
                                          const query_context *qc,
                                          const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root || !qc->project_root)
        return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    ProjectNode *wanted = resolve_highlight_target(qc, d, pos);

    yyjson_mut_val *result =
        build_document_highlight_json(doc, qc->project_root, wanted,
                                      d->uri, d->tok_spans, d->num_tok_spans);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}
