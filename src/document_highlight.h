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
 * Build a DocumentHighlight[] JSON array for textDocument/documentHighlight.
 *
 * Returns NULL when the cursor is not on an identifier that can be resolved
 * to a symbol definition.  Otherwise returns an array of DocumentHighlight
 * objects: the definition site tagged as Write (kind 3) and all same-document
 * reference sites tagged as Read (kind 2).
 *
 * Works bidirectionally: triggers from both definition and reference sites.
 * For dotted dependency paths (e.g. "task1.subtask2"), each segment is
 * treated as an independent reference.
 *
 * Values are allocated in doc; caller owns doc.
 */
yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    const DocSymbol *symbols, int num_symbols,
    const TokenSpan *tokens, int num_tokens,
    LspPos cursor);
