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

#include "parser.h"

/*
 * Cursor → dependency / task-declaration lookup.
 *
 * Dependencies are captured on task tj_nodes at parse time (see
 * `Dependency` in parser.h).  These helpers locate the per-document node
 * under a cursor; cross-file resolution itself runs against the
 * assembled per-Project ProjectNode tree (see project_tree.h), not here.
 */

/**
 * Locate the dependency reference under @p cursor, if any.
 *
 * Walks outward from the innermost tj_node at @p cursor and returns the
 * first enclosing task carrying a dependency whose `source_range`
 * contains @p cursor.
 *
 * @param tokens      Token spans of the document under the cursor.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @param out_owner   Receives the task that declared the dependency.
 * @param out_dep     Receives the matched dependency.
 * @return 1 when a dependency reference is found, 0 otherwise.
 */
int dependency_at_cursor(const TokenSpan *tokens, int num_tokens,
                         LspPos cursor,
                         tj_node **out_owner, const Dependency **out_dep);

/**
 * Return the task declaration whose identifier (`selection_range`)
 * contains @p cursor, walking outward from the innermost enclosing node.
 *
 * @param tokens      Token spans of the document under the cursor.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return The matched task tj_node, or NULL when the cursor is not on a
 *         task identifier.
 */
tj_node *task_decl_at_cursor(const TokenSpan *tokens, int num_tokens,
                             LspPos cursor);
