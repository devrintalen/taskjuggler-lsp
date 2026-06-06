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
#include "project_tree.h"
#include "grammar.tab.h"

/**
 * Return a copy of the TokenSpan that spans @p pos, or a token with kind
 * TK_EOF if none.  Caller must free result.text (if non-NULL).
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param pos         Cursor position.
 * @return Copy of the matching token, or one with `token_kind == TK_EOF`.
 */
TokenSpan tok_span_at(const TokenSpan *tokens, int num_tokens, LspPos pos);

/** One entry in the keyword stack produced by scan_kw_stack(). */
typedef struct {
    char     *kw;        /**< keyword text, heap-allocated */
    LspRange  range;     /**< source range of the keyword token */
    uint32_t  depth;     /**< brace depth at which this keyword was pushed */
    uint32_t  argc;      /**< number of argument tokens seen so far */
} KwStackEntry;

/**
 * Shared keyword-context scanner used by hover and signature help.
 *
 * Walks tokens up to cursor, tracking brace depth and a keyword stack.
 * For each token in the default case:
 *   - If tok->token_kind < @p kind_max, the token is pushed as a keyword entry,
 *     displacing any sibling entries at the same depth.
 *   - Otherwise, if @p track_argc is non-zero and the token ends before cursor,
 *     the arg_count of the innermost entry at the current depth is incremented.
 *
 * Pass KW_DOCS_END as kind_max to capture all hover-documented keywords.
 * Pass KW_SIG_END as kind_max to capture only signature-help keywords.
 *
 * On return, stack[0..return_value-1] are populated and all kw fields are
 * heap-allocated.  *out_depth holds the brace depth at cursor.
 * The caller is responsible for freeing all kw fields in the stack.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @param kind_max    Upper bound (exclusive) on KW_* values eligible to be
 *                    pushed as keyword entries (e.g. KW_DOCS_END,
 *                    KW_SIG_END).
 * @param track_argc  When non-zero, increment `arg_count` on the innermost
 *                    entry for every non-keyword token that ends before
 *                    @p cursor.
 * @param stack       Output buffer for the keyword stack.
 * @param stack_cap   Capacity of @p stack in entries.
 * @param out_depth   Receives the brace depth at @p cursor.
 * @return Number of entries written into @p stack.
 */
int scan_kw_stack(const TokenSpan *tokens, int num_tokens, LspPos cursor,
                  int kind_max, int track_argc,
                  KwStackEntry *stack, int stack_cap,
                  uint32_t *out_depth);

/**
 * Build the fully qualified, dot-separated id of @p sym by walking up the
 * parent chain and collecting ids of ancestors that share the same
 * `keyword` kind.  For example, a task `design` nested inside task `bar`
 * inside task `foo` yields `"foo.bar.design"`.  Ancestors of a different
 * kind (such as the enclosing `project`) are skipped, matching TJP
 * dotted-path semantics.
 *
 * @param sym  Target symbol.  May be NULL.
 * @return Heap-allocated dotted id (empty string if @p sym or its id is
 *         NULL).  Caller must free.
 */
char *sym_qualified_id(const tj_node *sym);

/** @brief Build the hover Markdown for a resolved task node in an assembled
 *  Project tree (full documentation at the definition in hover.c). */
char *project_node_hover_markdown(const ProjectNode *node);

/**
 * Return Markdown documentation for a TJP keyword, or NULL if unknown.
 *
 * @param kw  Keyword text (e.g. `"task"`, `"depends"`).
 * @return Markdown documentation string, or NULL when @p kw is not a
 *         documented keyword.  The returned pointer is to static storage
 *         and must not be freed.
 */
const char *keyword_docs(const char *kw);

/** Result of active_keyword_at() — keyword text and its source range. */
typedef struct {
    char    *keyword;    /**< keyword text, heap-allocated; NULL if none */
    LspRange range;      /**< source range of the keyword token */
} ActiveKeyword;

/**
 * Find the keyword that is "active" at cursor — the most recent documentable
 * keyword whose argument list encompasses the cursor position (i.e. it has
 * been started but not yet terminated by a deeper block or a sibling keyword).
 *
 * The returned `keyword` is heap-allocated; caller must free it.
 * The returned `range` is the span of the keyword token itself (not its
 * arguments).
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return The active keyword, or `{NULL, {0}}` when none exists.
 */
ActiveKeyword active_keyword_at(const TokenSpan *tokens, int num_tokens, LspPos cursor);
