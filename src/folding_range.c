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

#include "folding_range.h"
#include "document_symbol.h"
#include "grammar.tab.h"

/** Maximum nesting depth for bracket matching. */
#define MAX_BRACKET_DEPTH 256

/**
 * Append one FoldingRange entry to @p arr.
 *
 * @param doc         Destination mutable JSON document.
 * @param arr         FoldingRange[] array.
 * @param start_line  Start line of the foldable region.
 * @param end_line    End line of the foldable region.
 * @param kind        LSP FoldingRangeKind string (`"region"` or
 *                    `"comment"`).
 */
static void push_range(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                        uint32_t start_line, uint32_t end_line,
                        const char *kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, obj, "startLine", start_line);
    yyjson_mut_obj_add_uint(doc, obj, "endLine",   end_line);
    yyjson_mut_obj_add_str(doc,  obj, "kind",      kind);
    yyjson_mut_arr_add_val(arr, obj);
}

/**
 * Recursively emit `"region"` folding ranges from the tj_node tree.
 *
 * @param doc   Destination mutable JSON document.
 * @param arr   FoldingRange[] array.
 * @param slab  Parse slab owning the node and children arrays.
 * @param kids  Children index array to walk.
 * @param n     Length of @p kids.
 */
static void emit_symbol_ranges(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                const parse_slab *slab,
                                const tj_idx *kids, int n) {
    for (int i = 0; i < n; i++) {
        tj_node *sym = slab_node(slab, kids[i]);
        if (!sym) continue;
        if (sym->range.end.line > sym->range.start.line)
            push_range(doc, arr,
                       sym->range.start.line,
                       sym->range.end.line, "region");
        tj_idx *child_kids = slab_children(slab, sym);
        emit_symbol_ranges(doc, arr, slab, child_kids, sym->num_children);
    }
}

/**
 * Scan tokens for bracket pairs and block comments that are not
 * represented in the tj_node tree, emitting one entry per pair.
 *
 * @param doc        Destination mutable JSON document.
 * @param arr        FoldingRange[] array.
 * @param spans      Token spans of the current document.
 * @param num_spans  Length of @p spans.
 */
static void emit_token_ranges(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                               const TokenSpan *spans, int num_spans) {
    uint32_t bracket_stack[MAX_BRACKET_DEPTH];
    int bracket_depth = 0;

    for (int i = 0; i < num_spans; i++) {
        const TokenSpan *s = &spans[i];

        switch (s->token_kind) {
        case TK_LBRACKET:
            if (bracket_depth < MAX_BRACKET_DEPTH)
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
}

/* Build the JSON array for a textDocument/foldingRange response.
 * Emits folding ranges from two sources:
 *   — tj_node tree ranges for brace-delimited blocks ({ ... })
 *   — Token scan for bracket pairs ([ ... ]) and multi-line block comments
 */
yyjson_mut_val *build_folding_ranges_json(yyjson_mut_doc *doc,
                                           const parse_slab *slab,
                                           const TokenSpan *spans, int num_spans,
                                           tj_idx root_idx) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    tj_node *root = slab_node(slab, root_idx);
    if (root) {
        tj_idx *kids = slab_children(slab, root);
        emit_symbol_ranges(doc, arr, slab, kids, root->num_children);
    }
    emit_token_ranges(doc, arr, spans, num_spans);
    return arr;
}
