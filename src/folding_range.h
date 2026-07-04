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
#include "query_context.h"   /* query_doc */
#include <yyjson.h>

/**
 * Build the JSON array of FoldingRange objects for
 * textDocument/foldingRange responses.
 *
 * Two kinds of ranges are emitted:
 *   - `"region"`  — brace-delimited blocks and bracket pairs covering at
 *                   least two lines
 *   - `"comment"` — multi-line block comments
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc          Destination mutable JSON document.
 * @param spans        Token spans of the current document.
 * @param num_spans    Length of @p spans.
 * @param symbols      Top-level symbols of the current document.
 * @param num_symbols  Length of @p symbols.
 * @return The newly created JSON array of FoldingRange objects.
 */
yyjson_mut_val *build_folding_ranges_json(yyjson_mut_doc *doc,
                                           const TokenSpan *spans,
                                           int num_spans,
                                           tj_node *const *symbols,
                                           int num_symbols);

/**
 * Handle "textDocument/foldingRange": compute folding ranges for all block
 * constructs in the primary document.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params (unused).
 * @param qc      Query context (unused; present for dispatch symmetry).
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response containing the folding-range array.
 */
yyjson_mut_val *handle_folding_range(yyjson_mut_doc *doc, yyjson_val *id,
                                     yyjson_val *params, const query_context *qc,
                                     const query_doc *d);
