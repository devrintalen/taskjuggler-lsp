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
 * Build the JSON array of CodeLens objects for textDocument/codeLens
 * responses.
 *
 * Each emitted lens hangs above a task's `length` or `duration` keyword
 * when the enclosing task carries an explicit endpoint:
 *
 *   - `start` is set  →  lens shows the computed end date
 *                        (title: `"→ ends YYYY-MM-DD"`)
 *   - `end` is set, `start` is not  →  lens shows the computed start
 *                        (title: `"← starts YYYY-MM-DD"`)
 *
 * Lenses are inert: the `command.command` field is the empty string, so
 * the editor renders the title and clicking does nothing. No
 * codeLens/resolve round-trip is required.
 *
 * Working-time math for `length` assumes the TaskJuggler defaults:
 * Mon-Fri weekdays, 8 working hours per day, no holidays.
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc          Destination mutable JSON document.
 * @param spans        Token spans of the current document.
 * @param owners       Per-token owner array, parallel to @p spans.
 * @param num_spans    Length of @p spans / @p owners.
 * @param symbols      Top-level symbols of the current document.
 * @param num_symbols  Length of @p symbols.
 * @return The newly created JSON array of CodeLens objects (may be empty).
 */
yyjson_mut_val *build_code_lens_json(yyjson_mut_doc *doc,
                                     const TokenSpan *spans,
                                     tj_node *const *owners,
                                     int num_spans,
                                     tj_node *const *symbols,
                                     int num_symbols);

/**
 * Handle "textDocument/codeLens": build code-lens annotations (e.g.
 * dependency counts) for the primary document.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params (unused).
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response containing the code-lens array.
 */
yyjson_mut_val *handle_code_lens(yyjson_mut_doc *doc, yyjson_val *id,
                                 yyjson_val *params, const query_doc *d);
