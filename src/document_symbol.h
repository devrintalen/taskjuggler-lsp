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
 * Return the innermost tj_node whose range contains @p pos (inclusive
 * start, exclusive end), using the precomputed `.owner_idx` field on @p tokens.
 *
 * @param slab        Parse slab owning the token and node arrays.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param pos         Position to look up.
 * @return The matching node, or NULL when @p pos is outside every tj_node.
 */
tj_node *tj_node_at(const parse_slab *slab,
                    const TokenSpan *tokens, int num_tokens, LspPos pos);

/**
 * Navigate a tj_node tree by following @p path (an array of @p plen
 * identifier strings).  Used to resolve dotted task paths against the
 * task tree.
 *
 * @param slab          Parse slab owning the node and string arrays.
 * @param child_indices Flat children index array to start from.
 * @param n             Length of @p child_indices.
 * @param path          Identifiers to follow, outermost first.
 * @param plen          Length of @p path.
 * @return The matched node, or NULL when the path does not resolve.
 */
tj_node *tj_node_find_path(const parse_slab *slab,
                            const tj_idx *child_indices, int n,
                            const char **path, int plen);

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
 * Renders all children of the node at @p root_idx.
 *
 * @param slab      Parse slab owning the nodes and string pool.
 * @param root_idx  Index of the synthetic root node whose children to render.
 * @param out_len   Receives the byte length of the result, excluding the NUL.
 * @return Heap-allocated JSON; caller owns and must free().
 */
char *build_document_symbols_json(const parse_slab *slab, tj_idx root_idx,
                                   size_t *out_len);
