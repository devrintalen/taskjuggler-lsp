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

#pragma once

#include "project_tree.h"
#include <yyjson.h>

/**
 * Build the textDocument/references response for a task declaration.
 *
 * Walks @p project_root's task tree, resolving each dependency
 * (memoizing in-node) and collecting those that target @p wanted.
 * Returns an LSP `Location[]` of the matching dependency references
 * (empty when @p wanted has no incoming dependencies), or NULL when
 * @p wanted is NULL.
 *
 * @param doc           Destination mutable JSON document.
 * @param project_root  The requester's Project synthetic root.
 * @param wanted        The assembled-tree task the references point at.
 * @return A `Location[]` JSON array, or NULL.
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       ProjectNode *project_root,
                                       const ProjectNode *wanted);
