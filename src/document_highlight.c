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

#include "document_highlight.h"
#include "document_symbol.h"
#include "hover.h"
#include "grammar.tab.h"
#include <string.h>

/* TODO(document-highlight): The previous implementation used
 * DefinitionLink/ReferenceLink edges populated during dep-ref
 * resolution to cross-link reference sites with their definition.
 * Those edges are not built during the tj_node refactor; until they
 * return, fall back to a simple identifier-text match: highlight the
 * declaration whose selection_range covers the cursor (Write, kind 3)
 * and every other TK_IDENT token in the document whose text equals
 * the declaration's id (Read, kind 2).  This is coarser than the
 * original behaviour (it cannot distinguish a dependency reference
 * from an unrelated identifier of the same name), but it's still a
 * useful UX while dep tracking is offline. */

static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

static int range_eq(LspRange a, LspRange b) {
    return pos_cmp(a.start, b.start) == 0 && pos_cmp(a.end, b.end) == 0;
}

static const tj_node *find_decl_at(const TokenSpan *tokens, int num_tokens,
                                    LspPos pos) {
    for (tj_node *sym = tj_node_at(tokens, num_tokens, pos);
         sym != NULL; sym = sym->parent_node) {
        if (pos_in_range(pos, sym->selection_range))
            return sym;
    }
    return NULL;
}

static void push_highlight(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                           LspRange range, int kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "range", range_json(doc, range));
    yyjson_mut_obj_add_uint(doc, obj, "kind", (uint64_t)kind);
    yyjson_mut_arr_add_val(arr, obj);
}

yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    tj_node *const *symbols, int num_symbols,
    const TokenSpan *tokens, int num_tokens,
    LspPos cursor) {

    (void)symbols; (void)num_symbols;

    TokenSpan tok = tok_span_at(tokens, num_tokens, cursor);
    if (tok.token_kind != TK_IDENT) {
        free(tok.text);
        return NULL;
    }

    const tj_node *target = find_decl_at(tokens, num_tokens, cursor);
    /* Cursor not on a declaration name — fall back to "any identifier
     * token whose text matches the cursor token's text".  No Write
     * highlight in that case since we have no declaration site to
     * point at. */
    const char *match = (target && target->id) ? target->id : tok.text;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    if (target)
        push_highlight(doc, arr, target->selection_range, 3);

    for (int t = 0; t < num_tokens; t++) {
        if (tokens[t].token_kind != TK_IDENT) continue;
        if (!tokens[t].text) continue;
        if (strcmp(tokens[t].text, match) != 0) continue;
        LspRange tr = { tokens[t].start, tokens[t].end };
        if (target && range_eq(tr, target->selection_range)) continue;
        push_highlight(doc, arr, tr, 2);
    }

    free(tok.text);
    return arr;
}
