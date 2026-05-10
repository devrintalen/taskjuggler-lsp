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
#include "grammar.tab.h"
#include <yyjson.h>

/**
 * Build the textDocument/completion response for the given cursor position.
 *
 * Returns a JSON CompletionList object, or a JSON null when there are no
 * applicable completions.  Values are allocated in @p doc; the caller owns
 * the document.
 *
 * @param doc          Destination mutable JSON document.
 * @param tokens       Token spans for the current document.
 * @param num_tokens   Length of @p tokens.
 * @param cursor       Cursor position.
 * @param symbols      Top-level symbols of the current document.
 * @param num_symbols  Length of @p symbols.
 * @param extra_pools  Top-level symbols of every other open/background doc.
 * @param extra_counts Per-pool lengths matching @p extra_pools.
 * @param num_extra    Length of @p extra_pools.
 * @param text         Raw source text of the current document.
 */
yyjson_mut_val *build_completions_json(yyjson_mut_doc *doc,
                                        const TokenSpan *tokens, int num_tokens,
                                        LspPos cursor,
                                        DocSymbol *const *symbols, int num_symbols,
                                        DocSymbol *const **extra_pools,
                                        const int *extra_counts,
                                        int num_extra,
                                        const char *text);
