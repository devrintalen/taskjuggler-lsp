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

#include "definition.h"
#include "document_symbol.h"  /* range_json */
#include "dependency.h"       /* dependency_at_cursor */
#include "rpc.h"

yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       ProjectNode *owner, int dep_index,
                                       ProjectNode *project_root) {
    if (!owner || !project_root ||
        dep_index < 0 || dep_index >= owner->num_dependencies)
        return NULL;

    ProjectNode *target = project_dep_resolve(owner, dep_index, project_root);
    if (!target) return NULL;

    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri", target->source_uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, target->selection_range));
    return location;
}

yyjson_mut_val *handle_definition(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params, const query_context *qc,
                                  const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    LspPos pos = json_to_pos(pos_obj);
    if (!qc->project_root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    yyjson_mut_val   *result = NULL;
    if (dependency_at_cursor(d->tok_spans, d->tok_owners, d->num_tok_spans, pos,
                             &owner, &dep)) {
        ProjectNode *merged_owner =
            project_node_for_doc_task(qc->project_root, d->task_prefix, owner);
        if (merged_owner) {
            int ordinal = (int)(dep - owner->dependencies);
            if (ordinal >= 0 && ordinal < merged_owner->num_dependencies)
                result = build_definition_json(doc, merged_owner, ordinal,
                                               qc->project_root);
        }
    }
    if (!result) return make_response(doc, id, yyjson_mut_null(doc));
    return make_response(doc, id, result);
}
