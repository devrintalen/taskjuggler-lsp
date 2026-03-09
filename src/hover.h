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

/* Return a copy of the token that spans `pos`, or a token with kind TK_EOF if none.
 * Caller must free the returned token. */
Token token_at(const Token *tokens, int num_tokens, LspPos pos);

/* Return Markdown documentation for a TJP keyword, or NULL if unknown. */
const char *keyword_docs(const char *kw);

/*
 * Find the keyword that is "active" at cursor — the most recent documentable
 * keyword whose argument list encompasses the cursor position (i.e. it has
 * been started but not yet terminated by a deeper block or a sibling keyword).
 *
 * result.keyword is heap-allocated; caller must free it.
 * result.range is the span of the keyword token itself (not its arguments).
 * Returns {NULL, {0}} when no active keyword exists.
 */
typedef struct {
    char    *keyword;
    LspRange range;
} ActiveKeyword;

ActiveKeyword active_keyword_at(const Token *tokens, int num_tokens, LspPos cursor);
