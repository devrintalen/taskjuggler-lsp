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
#include <yyjson.h>

/**
 * Build a Location JSON object for textDocument/definition.
 *
 * Locates a DefinitionLink whose source range contains @p cursor via
 * symbol_at() + parent-chain walk.  When found, returns a JSON object
 * of the form `{ "uri": "<uri>", "range": <target selection_range> }`.
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc         Destination mutable JSON document.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @param uri         URI placed into the response's `uri` field when the
 *                    target lives in the same document as the source.
 * @return The Location JSON object, or NULL when no definition link
 *         covers @p cursor.
 */
yyjson_mut_val *build_definition_json(yyjson_mut_doc *doc,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor, const char *uri);

/**
 * Find a DefinitionLink whose source range contains the cursor.
 *
 * Uses symbol_at() to locate the innermost enclosing DocSymbol and scans its
 * def_links, walking parents on miss.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return Pointer to the matching link, or NULL when none covers the cursor.
 */
const DefinitionLink *find_def_link_at(const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor);
