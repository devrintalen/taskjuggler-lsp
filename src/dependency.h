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
 * On-demand dependency resolution.
 *
 * Dependencies are captured on task tj_nodes at parse time (see
 * `Dependency` in parser.h) but left unresolved.  The textDocument/
 * definition and textDocument/references handlers resolve them lazily
 * at request time against the requester's project — they never write
 * back into `Dependency.resolved_target`, so the per-document tj_node
 * trees stay immutable and safe to share across query workers.
 */

/**
 * One document's contribution to a project's resolution scope: its
 * top-level tj_nodes (as produced by the server's flatten_top_nodes)
 * plus the URI used to build Locations that point into it.  All
 * pointers are borrowed for the duration of a single request.
 */
typedef struct {
    tj_node *const *top;   /**< top-level nodes of the document */
    int             n;     /**< length of @p top */
    const char     *uri;   /**< owning document URI (borrowed) */
} ProjectScope;

/**
 * Resolve one captured dependency to its target task.
 *
 * Absolute (zero-bang) references are looked up in the owner's own
 * scope first, then every other scope in the project; the resolved
 * task's owning URI is reported via @p out_uri.  Relative (bang)
 * references walk up @p owner_task's `parent_node` chain one level per
 * `!` and are resolved strictly in-file; a reference that climbs past
 * the document/project root does not resolve.
 *
 * @param dep          Dependency to resolve.
 * @param owner_task   Task tj_node that declared @p dep (the anchor for
 *                     the bang walk).
 * @param scopes       Every document in the requester's project.
 * @param num_scopes   Length of @p scopes.
 * @param owner_index  Index into @p scopes of the owner's document.
 * @param out_uri      When non-NULL, receives the URI of the resolved
 *                     task's document (a borrowed pointer into
 *                     @p scopes), or NULL on a miss.
 * @return The matched task tj_node, or NULL when unresolved.
 */
tj_node *resolve_dependency(const Dependency *dep, tj_node *owner_task,
                            const ProjectScope *scopes, int num_scopes,
                            int owner_index, const char **out_uri);

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
