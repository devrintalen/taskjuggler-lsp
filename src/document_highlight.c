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

/*
 * document_highlight.c — textDocument/documentHighlight response builder
 *
 * ── Overview ─────────────────────────────────────────────────────────────
 *
 * Document-highlight is answered from three data structures in ParseResult:
 *
 *   doc_symbols — the symbol tree; each node has a selection_range covering
 *                 its declaration identifier, an id field with the
 *                 identifier text, and def_links[] with resolved references
 *   tok_spans   — flat ordered token array used to identify the token at the
 *                 cursor and to find per-segment ranges within dotted paths
 *
 * At query time, build_document_highlight_json():
 *
 *   1. Finds the TK_IDENT token at the cursor position.
 *
 *   2. Resolves the target symbol — either the cursor is on a definition
 *      site (matched via doc_symbols) or on a reference site (matched via
 *      def_links on the enclosing symbol).  For dotted paths, each segment
 *      is resolved independently.
 *
 *   3. Collects highlights: the definition as Write (kind 3) and all
 *      same-document references as Read (kind 2).
 *
 * ── Bidirectional triggering ─────────────────────────────────────────────
 *
 * Unlike textDocument/references which only triggers from definition sites,
 * documentHighlight works from both directions: cursor on a definition
 * highlights its references, and cursor on a reference highlights the
 * definition and all sibling references.
 */

#include "document_highlight.h"
#include "document_symbol.h"
#include "hover.h"
#include "grammar.tab.h"
#include <string.h>

/* Returns 1 if position p falls within range r (both endpoints inclusive). */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

/* Returns 1 if ranges a and b have identical start and end positions. */
static int range_eq(LspRange a, LspRange b) {
    return pos_cmp(a.start, b.start) == 0 && pos_cmp(a.end, b.end) == 0;
}

/* Walk the symbol tree depth-first to find any node whose selection_range
 * contains pos.  Uses each symbol's range to skip subtrees that cannot
 * contain pos.  Not filtered by kind — matches all symbol types. */
static const DocSymbol *find_symbol_at(DocSymbol *const *syms, int n,
                                       LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (!pos_in_range(pos, syms[i]->range))
            continue;
        if (pos_in_range(pos, syms[i]->selection_range))
            return syms[i];
        return find_symbol_at(syms[i]->children, syms[i]->num_children, pos);
    }
    return NULL;
}

/* Walk the symbol tree depth-first to find the first node whose detail
 * matches the given string.  Used for intermediate segments in dotted
 * dependency paths where no def_link directly targets the segment. */
static const DocSymbol *find_symbol_by_id(DocSymbol *const *syms, int n,
                                          const char *id) {
    for (int i = 0; i < n; i++) {
        if (syms[i]->id && strcmp(syms[i]->id, id) == 0)
            return syms[i];
        const DocSymbol *found =
            find_symbol_by_id(syms[i]->children, syms[i]->num_children, id);
        if (found) return found;
    }
    return NULL;
}

/* Walk the symbol tree looking for a same-document DefinitionLink whose
 * source range contains pos.  Uses each symbol's range to skip subtrees
 * that cannot contain pos.  Returns the link's target, or NULL. */
static const DocSymbol *find_link_target_at(DocSymbol *const *syms, int n,
                                            LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (!pos_in_range(pos, syms[i]->range))
            continue;

        for (int j = 0; j < syms[i]->num_def_links; j++) {
            const DefinitionLink *link = &syms[i]->def_links[j];
            if (link->target_uri) continue;
            if (pos_in_range(pos, link->source))
                return link->target;
        }
        return find_link_target_at(syms[i]->children, syms[i]->num_children,
                                   pos);
    }
    return NULL;
}

/* Append a DocumentHighlight object to arr.
 * kind: 2 = Read, 3 = Write. */
static void push_highlight(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                           LspRange range, int kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "range", range_json(doc, range));
    yyjson_mut_obj_add_uint(doc, obj, "kind", (uint64_t)kind);
    yyjson_mut_arr_add_val(arr, obj);
}

/* Collect Read highlights from the target symbol's ref_links.
 * For each same-document reference, find the specific token within the
 * ref source range that matches the target's id. */
static void collect_ref_highlights(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                   const DocSymbol *target,
                                   const TokenSpan *tokens, int num_tokens) {
    for (int i = 0; i < target->num_ref_links; i++) {
        const ReferenceLink *ref = &target->ref_links[i];
        if (ref->source_uri) continue;

        /* Scan only tokens within the ref source range */
        for (int t = 0; t < num_tokens; t++) {
            if (pos_cmp(tokens[t].start, ref->source.end) > 0) break;
            if (pos_cmp(tokens[t].end, ref->source.start) < 0) continue;
            if (tokens[t].token_kind != TK_IDENT) continue;
            if (!tokens[t].text) continue;
            if (strcmp(tokens[t].text, target->id) != 0) continue;

            LspRange token_range = { tokens[t].start, tokens[t].end };
            if (range_eq(token_range, target->selection_range)) continue;

            push_highlight(doc, arr, token_range, 2);
        }
    }
}

yyjson_mut_val *build_document_highlight_json(
    yyjson_mut_doc *doc,
    DocSymbol *const *symbols, int num_symbols,
    const TokenSpan *tokens, int num_tokens,
    LspPos cursor) {

    /* Step 1: find the token at cursor. */
    TokenSpan tok = tok_span_at(tokens, num_tokens, cursor);
    if (tok.token_kind != TK_IDENT) {
        free(tok.text);
        return NULL;
    }

    const DocSymbol *target = NULL;

    /* Step 2a: check if cursor is on a definition site. */
    target = find_symbol_at(symbols, num_symbols, cursor);

    /* Step 2b: check if cursor is on a reference site. */
    if (!target) {
        const DocSymbol *link_target =
            find_link_target_at(symbols, num_symbols, cursor);
        if (link_target) {
            if (link_target->id && tok.text
                    && strcmp(link_target->id, tok.text) == 0) {
                target = link_target;
            } else {
                target = find_symbol_by_id(symbols, num_symbols, tok.text);
            }
        }
    }

    free(tok.text);

    if (!target || !target->id) return NULL;

    /* Step 3: collect highlights. */
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    /* 3a: definition site — Write. */
    push_highlight(doc, arr, target->selection_range, 3);

    /* 3b: reference sites — Read. */
    collect_ref_highlights(doc, arr, target, tokens, num_tokens);

    return arr;
}
