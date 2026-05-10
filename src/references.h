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
 * Build a Location[] JSON array for textDocument/references.
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc         Destination mutable JSON document.
 * @param cursor_uri  URI of the document the cursor is in (used for
 *                    same-document references where source_uri is NULL).
 * @param tokens      Token spans of the cursor document (carry `.owner`
 *                    links that let symbol_at() locate the declaration
 *                    under cursor).
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return An array of Location objects (one per dependency reference that
 *         resolves to the task under @p cursor, collected from the
 *         target's ref_links); an empty array when no dependency points
 *         to that task; or NULL when @p cursor is not on a task
 *         declaration identifier.
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor);
