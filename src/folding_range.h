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
#include <cjson/cJSON.h>

/*
 * Build the JSON array of FoldingRange objects for
 * textDocument/foldingRange responses.
 *
 * Two kinds of ranges are emitted:
 *   "region"  — brace-delimited blocks ({ ... }), covering at least two lines
 *   "comment" — multi-line block comments
 *
 * Caller owns the returned cJSON array.
 */
cJSON *build_folding_ranges_json(const TokenSpan *spans, int num_spans);
