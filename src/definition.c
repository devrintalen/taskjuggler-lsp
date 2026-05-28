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
                                       ProjectNode *owner, int dep_index,
                                       ProjectNode *project_root) {
    if (!owner || !project_root ||
        dep_index < 0 || dep_index >= owner->num_dependencies)
        return NULL;

    ProjectNode *target = project_dep_resolve(owner, dep_index, project_root);
    if (!target) return NULL;

    yyjson_mut_val *location = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, location, "uri",
                           owner->dependencies[dep_index].target_uri);
    yyjson_mut_obj_add_val(doc, location, "range",
                           range_json(doc, target->selection_range));
    return location;
}
