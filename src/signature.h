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
#include "grammar.tab.h"
#include <yyjson.h>

/* Result of scanning context up to a cursor position. */
typedef struct {
    char    *keyword;   /* heap-allocated; NULL if no active context */
    uint32_t arg_count;
} ActiveContext;

/* Return 1 if `kw` has a known signature-help entry, 0 otherwise. */
int has_signature_help(const char *kw);

/*
 * Scan tokens up to `cursor` and return the innermost active statement
 * keyword together with the number of completed argument tokens.
 * Caller must free result.keyword (if non-NULL).
 */
ActiveContext active_context(const TokenSpan *tokens, int num_tokens, LspPos cursor);

/*
 * Build a LSP SignatureHelp JSON object for `kw` with `active_param`
 * highlighted, or NULL if `kw` has no known signature.
 * Values are allocated in doc; caller owns doc.
 */
yyjson_mut_val *build_signature_help_json(yyjson_mut_doc *doc,
                                           const char *kw,
                                           uint32_t active_param);
