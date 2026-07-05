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
#include "query_context.h"   /* query_context */
#include <yyjson.h>

/**
 * Append SymbolInformation entries whose name contains @p query
 * (case-insensitive substring; empty query matches all) from the given
 * symbol tree into @p arr.
 *
 * @param doc    Destination mutable JSON document (owns allocations).
 * @param query  Case-insensitive substring filter; empty string matches all.
 * @param syms   Top-level symbols to walk.
 * @param n      Length of @p syms.
 * @param uri    URI placed into each entry's `location.uri` field.
 * @param arr    JSON array to append SymbolInformation entries to.
 */
void collect_workspace_symbols(yyjson_mut_doc *doc, const char *query,
                                tj_node *const *syms, int n,
                                const char *uri, yyjson_mut_val *arr);

/**
 * Handle "workspace/symbol": collect all named symbols across every document
 * in the query context that match the query string.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params with a "query" string (empty matches all).
 * @param qc      Query context containing all workspace documents.
 * @param d       Primary query document (unused; present for dispatch symmetry).
 * @return JSON-RPC response containing the SymbolInformation array.
 */
yyjson_mut_val *handle_workspace_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                        yyjson_val *params,
                                        const query_context *qc,
                                        const query_doc *d);
