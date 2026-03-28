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

/* Serialize an LspRange to a mutable JSON object. */
yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r);

/*
 * Serialize the documentSymbol tree to a heap-allocated, NUL-terminated JSON
 * array string.  Sets *out_len to the byte length (excluding NUL).
 * Caller owns the returned memory and must free() it.
 * Intended to be cached and embedded via yyjson_mut_rawncpy.
 */
char *build_document_symbols_json(const DocSymbol *syms, int n, size_t *out_len);
