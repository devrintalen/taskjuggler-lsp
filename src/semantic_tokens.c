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

#include "semantic_tokens.h"
#include "grammar.tab.h"

#include <stdint.h>
#include <string.h>

/* ── Legend ──────────────────────────────────────────────────────────────── */

const char * const semantic_token_type_names[] = {
    "keyword",   /* SEMTOK_TYPE_KEYWORD  = 0 */
    "comment",   /* SEMTOK_TYPE_COMMENT  = 1 */
    "string",    /* SEMTOK_TYPE_STRING   = 2 */
    "number",    /* SEMTOK_TYPE_NUMBER   = 3 */
    "variable",  /* SEMTOK_TYPE_VARIABLE = 4 */
};
const int num_semantic_token_types =
    (int)(sizeof(semantic_token_type_names) / sizeof(semantic_token_type_names[0]));

const char * const semantic_token_modifier_names[] = {
    "declaration",  /* SEMTOK_MOD_DECLARATION = bit 0 */
};
const int num_semantic_token_modifiers =
    (int)(sizeof(semantic_token_modifier_names) / sizeof(semantic_token_modifier_names[0]));

/* ── Token classification ─────────────────────────────────────────────────── *
 *
 * Maps a token kind to a (type, modifiers) pair.
 * Returns 1 if the token should be highlighted, 0 if it should be skipped.
 */
static int classify(int kind, int *out_type, int *out_modifiers) {
    *out_modifiers = 0;

    switch (kind) {
    /* Punctuation tokens recorded in tok_spans only for cursor-position
     * queries; they carry no semantic highlighting meaning. */
    case TK_LBRACE:
    case TK_RBRACE:
    case TK_BANG:
    case TK_DOT:
    case TK_COMMA:
         return 0;
    /* Comments. */
    case TK_LINE_COMMENT:
    case TK_BLOCK_COMMENT:
        *out_type = SEMTOK_TYPE_COMMENT;
        return 1;
    /* String literals (single- and multi-line). */
    case TK_STR:
    case TK_MULTI_LINE_STR:
        *out_type = SEMTOK_TYPE_STRING;
        return 1;
    /* Date literals: structured literal values, styled as strings. */
    case TK_DATE:
        *out_type = SEMTOK_TYPE_STRING;
        return 1;
    /* Duration literals: numeric/quantitative values. */
    case TK_DURATION:
        *out_type = SEMTOK_TYPE_NUMBER;
        return 1;
    /* User-defined identifiers. */
    case TK_IDENT:
        *out_type = SEMTOK_TYPE_VARIABLE;
        return 1;
    /* Structural declaration keywords: introduce a named entity.
     * The declaration modifier allows editors to style them distinctly
     * from plain property keywords. */
    case KW_PROJECT:
    case KW_TASK:
    case KW_RESOURCE:
    case KW_ACCOUNT:
    case KW_SHIFT:
        *out_type      = SEMTOK_TYPE_KEYWORD;
        *out_modifiers = SEMTOK_MOD_DECLARATION;
        return 1;
    default:
        break;
    }

    /* All remaining KW_* property/attribute keywords. */
    if (kind >= KW_PROJECT && kind <= KW_YEARLYWORKINGDAYS) {
        *out_type = SEMTOK_TYPE_KEYWORD;
        return 1;
    }

    return 0;
}

/* ── Delta-encoded data emission ─────────────────────────────────────────── */

/* Append one five-integer entry to the data array.  Zero-length entries are
 * silently dropped; they arise at the end of a multi-line token when the
 * closing delimiter sits at the very start of a line. */
static void push_entry(yyjson_mut_doc *doc, yyjson_mut_val *data,
                        uint32_t delta_line, uint32_t delta_start,
                        uint32_t length,
                        int token_type, int modifiers) {
    if (length == 0) return;
    yyjson_mut_arr_add_uint(doc, data, delta_line);
    yyjson_mut_arr_add_uint(doc, data, delta_start);
    yyjson_mut_arr_add_uint(doc, data, length);
    yyjson_mut_arr_add_uint(doc, data, (uint64_t)token_type);
    yyjson_mut_arr_add_uint(doc, data, (uint64_t)modifiers);
}

/* Emit one or more data entries for a single token, splitting across source
 * lines when necessary.  prev_line and prev_char are updated after each
 * entry so that subsequent tokens encode their deltas correctly.
 *
 * For multi-line tokens the text field is walked character by character to
 * find per-line lengths.  The text begins at the token's start column in
 * the source, so the character count up to each '\n' is the highlight length
 * for that line without any further adjustment.
 */
static void emit_token(yyjson_mut_doc *doc, yyjson_mut_val *data,
                        uint32_t start_line, uint32_t start_char,
                        uint32_t end_line,   uint32_t end_char,
                        int token_type, int modifiers,
                        const char *text,
                        uint32_t *prev_line, uint32_t *prev_char) {
    if (start_line == end_line) {
        /* Single-line token. */
        uint32_t delta_line  = start_line - *prev_line;
        uint32_t delta_start = (delta_line == 0)
                               ? start_char - *prev_char
                               : start_char;
        push_entry(doc, data, delta_line, delta_start,
                   end_char - start_char, token_type, modifiers);
        *prev_line = start_line;
        *prev_char = start_char;
        return;
    }

    /* Multi-line token: walk text, emitting one entry per source line. */
    const char *p = text ? text : "";
    uint32_t current_line = start_line;
    uint32_t current_char = start_char;

    while (current_line < end_line) {
        uint32_t delta_line  = current_line - *prev_line;
        uint32_t delta_start = (delta_line == 0)
                               ? current_char - *prev_char
                               : current_char;

        /* Count characters on this source line within the token text. */
        const char *nl = strchr(p, '\n');
        uint32_t seg_len = nl ? (uint32_t)(nl - p) : (uint32_t)strlen(p);

        if (seg_len > 0) {
            push_entry(doc, data, delta_line, delta_start, seg_len, token_type, modifiers);
            *prev_line = current_line;
            *prev_char = current_char;
        }

        if (nl) p = nl + 1;
        current_line++;
        current_char = 0; /* subsequent lines start at column 0 */
    }

    /* Last line: ends at end_char; current_char is 0 for all but the
     * degenerate single-line case (already handled above). */
    {
        uint32_t delta_line  = current_line - *prev_line;
        uint32_t delta_start = (delta_line == 0)
                               ? current_char - *prev_char
                               : current_char;
        push_entry(doc, data, delta_line, delta_start, end_char, token_type, modifiers);
        *prev_line = current_line;
        *prev_char = current_char;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

yyjson_mut_val *build_semantic_tokens_json(yyjson_mut_doc *doc,
                                            const TokenSpan *spans, int num_spans) {
    yyjson_mut_val *data = yyjson_mut_arr(doc);
    uint32_t prev_line = 0, prev_char = 0;

    for (int i = 0; i < num_spans; i++) {
        const TokenSpan *s = &spans[i];
        int token_type, modifiers;
        if (!classify(s->token_kind, &token_type, &modifiers)) continue;

        emit_token(doc, data,
                   s->start.line, s->start.character,
                   s->end.line,   s->end.character,
                   token_type, modifiers,
                   s->text,
                   &prev_line, &prev_char);
    }

    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, result, "data", data);
    return result;
}
