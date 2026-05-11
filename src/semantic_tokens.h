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

/* ── Semantic token type indices ─────────────────────────────────────────── *
 *
 * Positions in the legend.tokenTypes array advertised during initialize.
 * Any change here must be reflected in semantic_token_type_names[] in
 * semantic_tokens.c, keeping the order identical.
 */

/** Semantic token type for TJP keywords. */
#define SEMTOK_TYPE_KEYWORD  0
/** Semantic token type for line and block comments. */
#define SEMTOK_TYPE_COMMENT  1
/** Semantic token type for quoted strings and scissors blocks. */
#define SEMTOK_TYPE_STRING   2
/** Semantic token type for integer and date/duration literals. */
#define SEMTOK_TYPE_NUMBER   3
/** Semantic token type for declaration identifiers (task/resource/etc. names). */
#define SEMTOK_TYPE_VARIABLE 4
/** Semantic token type for dependency-reference identifiers. */
#define SEMTOK_TYPE_FUNCTION 5

/* ── Semantic token modifier bitmasks ───────────────────────────────────── *
 *
 * Bit positions correspond to positions in legend.tokenModifiers.  Any
 * change here must be reflected in semantic_token_modifier_names[] in
 * semantic_tokens.c.
 */

/**
 * Marker applied to the five structural keywords that introduce named
 * declarations: project, task, resource, account, shift.  This allows
 * editors to style them distinctly from property keywords.
 */
#define SEMTOK_MOD_DECLARATION (1 << 0)

/* ── Legend arrays ──────────────────────────────────────────────────────── *
 *
 * Exposed so that handle_initialize() can embed them directly into the
 * capabilities response without duplicating the string literals.
 */

/** Token-type names indexed by SEMTOK_TYPE_*. */
extern const char * const semantic_token_type_names[];
/** Length of #semantic_token_type_names. */
extern const int          num_semantic_token_types;
/** Token-modifier names indexed by bit position. */
extern const char * const semantic_token_modifier_names[];
/** Length of #semantic_token_modifier_names. */
extern const int          num_semantic_token_modifiers;

/**
 * Build an LSP SemanticTokens object `{ "data": [...] }` for the given
 * token span array.  The data array uses the standard five-integer delta
 * encoding per token: [deltaLine, deltaStartChar, length, tokenType,
 * tokenModifiers].
 *
 * Multi-line tokens (TK_BLOCK_COMMENT, TK_MULTI_LINE_STR) are split into
 * one entry per source line as required by the protocol.  The accumulated
 * text stored in TokenSpan.text is used to compute per-line lengths.
 *
 * Tokens recorded in tok_spans solely for cursor-position queries
 * (TK_LBRACE, TK_RBRACE, TK_BANG, TK_DOT, TK_COMMA) are silently skipped.
 *
 * Values are allocated in @p doc; caller owns @p doc.
 *
 * @param doc              Destination mutable JSON document.
 * @param spans            Token spans of the current document.
 * @param num_spans        Length of @p spans.
 * @param num_sem_entries  Upper bound on the number of semantic-token
 *                         entries that will be emitted (used to
 *                         pre-allocate the result array).
 * @return The SemanticTokens JSON object `{ "data": [...] }`.
 */
yyjson_mut_val *build_semantic_tokens_json(yyjson_mut_doc *doc,
                                            const TokenSpan *spans, int num_spans,
                                            int num_sem_entries);
