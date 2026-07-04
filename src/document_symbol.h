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
 * Return the innermost tj_node whose range contains @p pos (inclusive
 * start, exclusive end), using the precomputed per-token @p owners array
 * (parallel to @p tokens).
 *
 * @param tokens      Token spans of the current document.
 * @param owners      Per-token innermost-enclosing node, parallel to @p tokens.
 * @param num_tokens  Length of @p tokens / @p owners.
 * @param pos         Position to look up.
 * @return The matching node, or NULL when @p pos is outside every tj_node.
 */
tj_node *tj_node_at(const TokenSpan *tokens, tj_node *const *owners,
                    int num_tokens, LspPos pos);

/**
 * Navigate a tj_node tree by following @p path (an array of @p plen
 * identifier strings).  Used to resolve dotted task paths against the
 * task tree.
 *
 * @param syms   Top-level nodes to start from.
 * @param n      Length of @p syms.
 * @param path   Identifiers to follow, outermost first.
 * @param plen   Length of @p path.
 * @param out_n  Receives the length of the returned children array.
 * @return Children array of the matched node, or NULL with `*out_n = 0`
 *         when the path does not resolve.
 */
tj_node *const *tj_node_find_path(tj_node *const *syms, int n,
                                  const char **path, int plen,
                                  int *out_n);

/**
 * Serialise an LspRange to a mutable JSON object allocated in @p doc.
 *
 * @param doc  Destination mutable JSON document.
 * @param r    Range to serialise.
 * @return The newly created JSON object.
 */
yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r);

/**
 * Map a KW_* keyword constant to the corresponding LSP SymbolKind (SK_*).
 *
 * @param keyword  KW_* constant from grammar.tab.h.
 * @return The matching LSP SymbolKind value.
 */
int symbol_kind_for(int keyword);

/**
 * Serialise the document's tj_node forest to a heap-allocated,
 * NUL-terminated JSON array string.  Intended to be embedded via
 * yyjson_mut_rawncpy.
 *
 * @param syms     Top-level nodes to render.
 * @param n        Length of @p syms.
 * @param out_len  Receives the byte length of the result, excluding the NUL.
 * @return Heap-allocated JSON; caller owns and must free().
 */
char *build_document_symbols_json(tj_node *const *syms, int n, size_t *out_len);

/**
 * Handle "textDocument/documentSymbol": return a flat list of all top-level
 * declarations in the primary document.
 *
 * @param doc     Mutable document for building the response.
 * @param id      Request id from the incoming JSON-RPC message.
 * @param params  Request params (unused; present for dispatch symmetry).
 * @param qc      Query context (unused; present for dispatch symmetry).
 * @param d       Primary query document; may be NULL.
 * @return JSON-RPC response containing the symbol array.
 */
yyjson_mut_val *handle_document_symbol(yyjson_mut_doc *doc, yyjson_val *id,
                                       yyjson_val *params, const query_context *qc,
                                       const query_doc *d);
