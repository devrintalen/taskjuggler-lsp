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

/** Serialise an LspRange to a mutable JSON object allocated in @p doc. */
yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r);

/** Map a KW_* keyword constant to the corresponding LSP SymbolKind (SK_*). */
int symbol_kind_for(int keyword);

/**
 * Serialise the documentSymbol tree to a heap-allocated, NUL-terminated JSON
 * array string.  Intended to be cached and later embedded via
 * yyjson_mut_rawncpy.
 *
 * @param syms     Top-level symbols of the document.
 * @param n        Length of @p syms.
 * @param out_len  Receives the byte length of the result, excluding the NUL.
 * @return Heap-allocated JSON; caller owns and must free().
 */
char *build_document_symbols_json(DocSymbol *const *syms, int n, size_t *out_len);
