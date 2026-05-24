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

yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor,
                                       const ProjectScope *scopes,
                                       int num_scopes, int self_index) {
    tj_node          *owner = NULL;
    const Dependency *dep   = NULL;
    if (!dependency_at_cursor(tokens, num_tokens, cursor, &owner, &dep))
        return NULL;

    const char *target_uri = NULL;
    tj_node    *target = resolve_dependency(dep, owner, scopes, num_scopes,
                                            self_index, &target_uri);
    if (!target) return NULL;
    if (!target_uri && self_index >= 0 && self_index < num_scopes)
        target_uri = scopes[self_index].uri;

    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri", target_uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, target->selection_range));
    return location;
}
