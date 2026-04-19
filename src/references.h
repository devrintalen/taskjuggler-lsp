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

#pragma once

#include "parser.h"
#include <yyjson.h>

/*
 * Build a Location[] JSON array for textDocument/references.
 *
 * Returns NULL when the cursor is not on a task declaration identifier.
 * Returns an empty array when the cursor is on a task declaration that
 * no dependency reference points to anywhere in the workspace.
 * Otherwise returns an array of Location objects, one per dependency
 * reference that resolves to the task under the cursor, collected from
 * the target's ref_links.
 *
 * cursor_uri   — URI of the document the cursor is in (used for
 *                same-document references where source_uri is NULL)
 * tokens/num_tokens — token spans of the cursor document (carry .owner links
 *                     that let symbol_at() locate the declaration under cursor)
 * cursor       — the cursor position
 *
 * Values are allocated in doc; caller owns doc.
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       const TokenSpan *tokens, int num_tokens,
                                       LspPos cursor);
