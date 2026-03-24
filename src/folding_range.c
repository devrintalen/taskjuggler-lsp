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

#include "folding_range.h"
#include "grammar.tab.h"

/* Maximum nesting depth for brace matching. */
#define MAX_BRACE_DEPTH 256

static void push_range(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                        uint32_t start_line, uint32_t end_line,
                        const char *kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, obj, "startLine", start_line);
    yyjson_mut_obj_add_uint(doc, obj, "endLine",   end_line);
    yyjson_mut_obj_add_str(doc,  obj, "kind",      kind);
    yyjson_mut_arr_add_val(arr, obj);
}

yyjson_mut_val *build_folding_ranges_json(yyjson_mut_doc *doc,
                                           const TokenSpan *spans, int num_spans) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    uint32_t brace_stack[MAX_BRACE_DEPTH];
    int brace_depth = 0;

    uint32_t bracket_stack[MAX_BRACE_DEPTH];
    int bracket_depth = 0;

    for (int i = 0; i < num_spans; i++) {
        const TokenSpan *s = &spans[i];

        switch (s->token_kind) {
        case TK_LBRACE:
            if (brace_depth < MAX_BRACE_DEPTH)
                brace_stack[brace_depth++] = s->start.line;
            break;

        case TK_RBRACE:
            if (brace_depth > 0) {
                uint32_t start_line = brace_stack[--brace_depth];
                uint32_t end_line   = s->start.line;
                if (end_line > start_line)
                    push_range(doc, arr, start_line, end_line, "region");
            }
            break;

        case TK_LBRACKET:
            if (bracket_depth < MAX_BRACE_DEPTH)
                bracket_stack[bracket_depth++] = s->start.line;
            break;

        case TK_RBRACKET:
            if (bracket_depth > 0) {
                uint32_t start_line = bracket_stack[--bracket_depth];
                uint32_t end_line   = s->start.line;
                if (end_line > start_line)
                    push_range(doc, arr, start_line, end_line, "region");
            }
            break;

        case TK_BLOCK_COMMENT:
            if (s->end.line > s->start.line)
                push_range(doc, arr, s->start.line, s->end.line, "comment");
            break;

        default:
            break;
        }
    }

    return arr;
}
