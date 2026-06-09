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

#include "references.h"
#include "document_symbol.h"  /* range_json */
#include "dependency.h"       /* task_decl_at_cursor */
#include "rpc.h"

/**
 * Recursively collect incoming references to @p wanted within a Project
 * tree, appending one LSP `Location` per matching dependency to @p arr.
 *
 * @param doc           Output yyjson_mut document used to mint location values.
 * @param arr           Output array on @p doc that the matching locations are
 *                      appended to.
 * @param node          The current node in the depth-first walk.  This is
 *                      the traversal cursor — it starts at the project root
 *                      and descends through `children` on each recursive
 *                      call, so it differs from @p project_root on every
 *                      call but the first.  Each task `node` may declare
 *                      dependencies; those resolving to @p wanted produce a
 *                      reference Location anchored at `node`'s source URI.
 * @param wanted        The target task whose incoming references we want.
 *                      A dependency is a match when it resolves to exactly
 *                      this node.
 * @param project_root  The Project's synthetic root, held constant across
 *                      the recursion and forwarded to project_dep_resolve()
 *                      as the resolution context: absolute paths resolve in
 *                      the project's single prefix-applied namespace, and
 *                      bang-relative climbs are bounded by it.
 */
static void collect_refs_in_subtree(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                     ProjectNode *node, const ProjectNode *wanted,
                                     ProjectNode *project_root) {
    if (!node) return;
    for (int i = 0; i < node->num_dependencies; i++) {
        ProjectNode *target = project_dep_resolve(node, i, project_root);
        if (target != wanted) continue;
        yyjson_mut_val *location = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, location, "uri", node->source_uri);
        yyjson_mut_obj_add_val(doc, location, "range",
                               range_json(doc, node->dependencies[i].source_range));
        yyjson_mut_arr_add_val(arr, location);
    }
    for (int i = 0; i < node->num_children; i++)
        collect_refs_in_subtree(doc, arr, node->children[i], wanted,
                                project_root);
}

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       ProjectNode *project_root,
                                       const ProjectNode *wanted) {
    if (!project_root || !wanted) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    collect_refs_in_subtree(doc, arr, project_root, wanted, project_root);
    return arr;
}

yyjson_mut_val *handle_references(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params, const query_context *qc,
                                  const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!qc->project_root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *task = task_decl_at_cursor(d->tok_spans, d->tok_owners, d->num_tok_spans, pos);
    ProjectNode *wanted =
        project_node_for_doc_task(qc->project_root, d->task_prefix, task);
    yyjson_mut_val *result = build_references_json(doc,
                                                   qc->project_root,
                                                   wanted);
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}
