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
#include <stddef.h>
#include <stdint.h>
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
 * editors to style them distinctly from plain property keywords.
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
 * Last semantic-tokens response sent to the client for a single
 * document, plus the counter used to mint future resultIds.  Retained
 * across revalidations so semanticTokens/full/delta requests can diff
 * against exactly what the client is holding.  `data` / `result_id` are
 * NULL until the client makes its first semanticTokens request;
 * `next_result_id` starts at 1.  Synchronization is the caller's
 * responsibility (the document store holds these under its mutex).
 */
typedef struct SemanticTokenResult {
    uint32_t *data;            /**< flat uint32 buffer last sent (NULL until first response) */
    size_t    count;           /**< entries in data (multiple of 5) */
    char     *result_id;       /**< resultId returned alongside data */
    uint64_t  next_result_id;  /**< monotonic counter for minting fresh resultIds */
} SemanticTokenResult;

/**
 * Free `data` and `result_id` and zero them out.  `next_result_id` is
 * left intact so a subsequent response continues the id sequence.
 * NULL-safe.
 */
void semantic_token_result_release(SemanticTokenResult *r);

/**
 * Replace the cached payload in @p r with @p new_data / @p new_count /
 * @p new_result_id, freeing whatever was there before.  Ownership of
 * both pointers transfers to @p r.  Leaves `next_result_id` untouched.
 */
void semantic_token_result_replace(SemanticTokenResult *r,
                                    uint32_t *new_data, size_t new_count,
                                    char *new_result_id);

/**
 * Compute the LSP-encoded semantic-tokens data array for the given token
 * spans.  The output uses the standard five-integer delta encoding per
 * token: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers].
 *
 * Multi-line tokens (TK_BLOCK_COMMENT, TK_MULTI_LINE_STR) are split into
 * one entry per source line as required by the protocol.  The accumulated
 * text stored in the string pool at TokenSpan.text_off is used to compute
 * per-line lengths.
 *
 * Tokens recorded in tok_spans solely for cursor-position queries
 * (TK_LBRACE, TK_RBRACE, TK_BANG, TK_DOT, TK_COMMA) are silently skipped.
 *
 * @param slab             Parse slab owning the token string pool.
 * @param spans            Token spans of the current document.
 * @param num_spans        Length of @p spans.
 * @param num_sem_entries  Upper bound on the number of semantic-token
 *                         entries that will be emitted (used to
 *                         pre-allocate the result array).
 * @param out_buf          Receives a malloc'd flat uint32 buffer; caller
 *                         owns and must free().
 * @param out_count        Receives the number of uint32 entries written.
 *                         Always a multiple of 5.
 */
void compute_semantic_tokens_data(const parse_slab *slab,
                                   const TokenSpan *spans, int num_spans,
                                   int num_sem_entries,
                                   uint32_t **out_buf, size_t *out_count);

/**
 * Build an LSP SemanticTokens object `{ "resultId": "...", "data": [...] }`
 * from a precomputed flat uint32 buffer.  The integer array is serialized
 * via a fast text path and embedded as a raw JSON value.
 *
 * @param doc        Destination mutable JSON document.
 * @param buf        Flat uint32 buffer (length is @p count).
 * @param count      Number of uint32 entries in @p buf (multiple of 5).
 * @param result_id  Result id string to include.  When NULL, the resultId
 *                   field is omitted from the response.
 * @return The SemanticTokens JSON object.
 */
yyjson_mut_val *build_semantic_tokens_json_from_buf(yyjson_mut_doc *doc,
                                                     const uint32_t *buf, size_t count,
                                                     const char *result_id);

/**
 * Serialize a uint32 buffer as a JSON array using the same fast text path
 * used by the SemanticTokens response.  Exposed so that the delta builder
 * can embed insert arrays without duplicating the code.
 *
 * @param doc    Destination mutable JSON document.
 * @param buf    Flat uint32 buffer.
 * @param count  Number of uint32 entries in @p buf.
 * @return A mutable JSON value holding the serialized array.
 */
yyjson_mut_val *build_uint32_array_json(yyjson_mut_doc *doc,
                                         const uint32_t *buf, size_t count);
