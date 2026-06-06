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
 * Build the textDocument/definition response for a dependency reference.
 *
 * Resolves @p owner's dependency at @p dep_index against @p project_root
 * (memoizing in-node) and returns a single LSP `Location` pointing at the
 * target task's identifier.  Returns NULL — serialised as JSON `null` by
 * the caller — when the reference does not resolve.
 *
 * @param doc           Destination mutable JSON document.
 * @param owner         The ProjectNode task whose dependency is under the cursor.
 * @param dep_index     Index of the dependency within @p owner.
 * @param project_root  The requester's Project synthetic root.
 * @return A `Location` JSON object, or NULL.
 */
yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       ProjectNode *owner, int dep_index,
                                       ProjectNode *project_root);
