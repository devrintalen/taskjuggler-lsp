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

/* Resolve every dependency declared in @p node's subtree against the
 * project and append a Location for each one that targets @p wanted. */
static void collect_refs_in_subtree(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                     tj_node *node, const tj_node *wanted,
                                     const ProjectScope *scopes,
                                     int num_scopes, int owner_index,
                                     const char *owner_uri) {
    if (!node) return;
    for (int i = 0; i < node->num_dependencies; i++) {
        const Dependency *dep = &node->dependencies[i];
        tj_node *target = resolve_dependency(dep, node, scopes, num_scopes,
                                             owner_index, NULL);
        if (target != wanted) continue;
        yyjson_mut_val *location = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, location, "uri", owner_uri);
        yyjson_mut_obj_add_val(doc, location, "range",
                               range_json(doc, dep->source_range));
        yyjson_mut_arr_add_val(arr, location);
    }
    for (int i = 0; i < node->num_children; i++)
        collect_refs_in_subtree(doc, arr, node->children[i], wanted,
                                scopes, num_scopes, owner_index, owner_uri);
}

yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor,
                                       const ProjectScope *scopes,
                                       int num_scopes) {
    tj_node *task = task_decl_at_cursor(tokens, num_tokens, cursor);
    if (!task) return NULL;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int p = 0; p < num_scopes; p++) {
        for (int i = 0; i < scopes[p].n; i++)
            collect_refs_in_subtree(doc, arr, scopes[p].top[i], task,
                                    scopes, num_scopes, p, scopes[p].uri);
    }
    return arr;
}
