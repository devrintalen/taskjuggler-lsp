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
 * @param slab         Parse slab owning the node and token arrays.
 * @param spans        Token spans of the current document.
 * @param num_spans    Length of @p spans.
 * @param root_idx     Index of the synthetic root node in @p slab.
 * @return The newly created JSON array of FoldingRange objects.
 */
yyjson_mut_val *build_folding_ranges_json(yyjson_mut_doc *doc,
                                           const parse_slab *slab,
                                           const TokenSpan *spans,
                                           int num_spans,
                                           tj_idx root_idx);
