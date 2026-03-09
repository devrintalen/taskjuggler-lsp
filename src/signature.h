/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

/* Result of scanning context up to a cursor position. */
typedef struct {
    char    *keyword;   /* heap-allocated; NULL if no active context */
    uint32_t arg_count;
} ActiveContext;

/*
 * Scan tokens up to `cursor` and return the innermost active statement
 * keyword together with the number of completed argument tokens.
 * Caller must free result.keyword (if non-NULL).
 */
ActiveContext active_context(const Token *tokens, int num_tokens, LspPos cursor);

/*
 * Build a LSP SignatureHelp JSON object for `kw` with `active_param`
 * highlighted, or NULL if `kw` has no known signature.
 * Caller owns the returned cJSON object.
 */
cJSON *build_signature_help_json(const char *kw, uint32_t active_param);
