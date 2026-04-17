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
 * One document's contribution to a cross-file references search.
 * uri         — the URI of the document (used as the Location uri in results)
 * symbols     — root-level symbol array from the document's ParseResult
 * num_symbols — number of entries in symbols
 */
typedef struct {
    const char      *uri;
    DocSymbol *const *symbols;
    int               num_symbols;
} RefDocLinks;

/*
 * Build a Location[] JSON array for textDocument/references.
 *
 * Returns NULL when the cursor is not on a task declaration identifier.
 * Returns an empty array when the cursor is on a task declaration that
 * no dependency reference points to anywhere in the workspace.
 * Otherwise returns an array of Location objects, one per dependency
 * reference that resolves to the task under the cursor, across all supplied
 * documents.
 *
 * cursor_uri   — URI of the document the cursor is in
 * symbols/num_symbols — symbol tree of the cursor document (used to locate
 *                       the declaration under the cursor)
 * all_docs/num_docs   — one RefDocLinks entry per open document; all of these
 *                       are scanned for def_links that point to the target task
 * cursor       — the cursor position
 *
 * Values are allocated in doc; caller owns doc.
 */
yyjson_mut_val *build_references_json(yyjson_mut_doc *doc,
                                       const char *cursor_uri,
                                       DocSymbol *const *symbols, int num_symbols,
                                       const RefDocLinks *all_docs, int num_docs,
                                       LspPos cursor);
