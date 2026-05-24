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

#include "dependency.h"
#include <yyjson.h>

/**
 * Build the textDocument/references response for a cursor on a task
 * declaration.
 *
 * Locates the task identifier under @p cursor, then scans every task in
 * the requester's project (@p scopes), resolving each dependency on
 * demand and collecting those that target the cursor's task.  Returns an
 * LSP `Location[]` of the matching dependency references (empty when the
 * task has no incoming dependencies), or NULL when the cursor is not on a
 * task identifier.
 *
 * @param doc          Destination mutable JSON document.
 * @param tokens       Token spans of the document under the cursor.
 * @param num_tokens   Length of @p tokens.
 * @param cursor       Cursor position.
 * @param scopes       Every document in the requester's project.
 * @param num_scopes   Length of @p scopes.
 * @return A `Location[]` JSON array, or NULL.
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor,
                                       const ProjectScope *scopes,
                                       int num_scopes);
