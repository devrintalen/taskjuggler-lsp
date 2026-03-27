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

/* Return a copy of the TokenSpan that spans `pos`, or a token with kind TK_EOF
 * if none.  Caller must free result.text (if non-NULL). */
TokenSpan tok_span_at(const TokenSpan *tokens, int num_tokens, LspPos pos);

/*
 * Shared keyword-context scanner used by hover and signature help.
 *
 * Walks tokens up to cursor, tracking brace depth and a keyword stack.
 * For each token in the default case:
 *   - If tok->token_kind < kind_max, the token is pushed as a keyword entry,
 *     displacing any sibling entries at the same depth.
 *   - Otherwise, if track_argc is non-zero and the token ends before cursor,
 *     the arg_count of the innermost entry at the current depth is incremented.
 *
 * Pass KW_DOCS_END as kind_max to capture all hover-documented keywords.
 * Pass KW_SIG_END as kind_max to capture only signature-help keywords.
 *
 * On return, stack[0..return_value-1] are populated and all kw fields are
 * heap-allocated.  *out_depth holds the brace depth at cursor.
 * The caller is responsible for freeing all kw fields in the stack.
 */
typedef struct {
    char     *kw;
    LspRange  range;
    uint32_t  depth;
    uint32_t  argc;
} KwStackEntry;

int scan_kw_stack(const TokenSpan *tokens, int num_tokens, LspPos cursor,
                  int kind_max, int track_argc,
                  KwStackEntry *stack, int stack_cap,
                  uint32_t *out_depth);

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

ActiveKeyword active_keyword_at(const TokenSpan *tokens, int num_tokens, LspPos cursor);
