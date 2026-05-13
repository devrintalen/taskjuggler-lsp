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

/* See doc/modules/document_highlight.rst for the module overview. */

#include "document_highlight.h"
#include "document_symbol.h"
#include "hover.h"
#include "grammar.tab.h"
#include <string.h>

/**
 * Test whether @p p falls within range @p r (endpoints inclusive).
 *
 * @param p  Position to test.
 * @param r  Range.
 * @return 1 when @p p is inside @p r, 0 otherwise.
 */
static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

/**
 * Test whether ranges @p a and @p b have identical start and end positions.
 *
 * @param a  First range.
 * @param b  Second range.
 * @return 1 when the ranges are exactly equal, 0 otherwise.
 */
static int range_eq(LspRange a, LspRange b) {
    return pos_cmp(a.start, b.start) == 0 && pos_cmp(a.end, b.end) == 0;
}

/**
 * Find the innermost DocSymbol whose selection_range contains @p pos.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param pos         Position to look up.
 * @return The matching symbol, or NULL when @p pos sits outside every
 *         selection range.  Runs in O(log T + D) where T is @p num_tokens
 *         and D is the symbol nesting depth.
 */
static const DocSymbol *find_symbol_at(const TokenSpan *tokens, int num_tokens,
                                       LspPos pos) {
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, pos);
         sym != NULL; sym = sym->parent) {
        if (pos_in_range(pos, sym->selection_range))
            return sym;
    }
    return NULL;
}

/**
 * Walk the symbol tree depth-first to find the first node whose id
 * matches @p id.  Used for intermediate segments in dotted dependency
 * paths where no def_link directly targets the segment.
 *
 * @param syms  Sibling symbols to search.
 * @param n     Length of @p syms.
 * @param id    Identifier to match.
 * @return The matching symbol, or NULL when no symbol carries @p id.
 */
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

/**
 * Find a same-document DefinitionLink whose source range contains @p pos
 * and return its target.  Uses symbol_at() to locate the innermost
 * enclosing symbol, then scans def_links walking up parents on miss.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param pos         Position to look up.
 * @return The target symbol, or NULL when @p pos is not on a same-document
 *         dependency reference.
 */
static const DocSymbol *find_link_target_at(const TokenSpan *tokens,
                                            int num_tokens, LspPos pos) {
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, pos);
         sym != NULL; sym = sym->parent) {
        for (int j = 0; j < sym->num_def_links; j++) {
            const DefinitionLink *link = &sym->def_links[j];
            if (link->target_uri) continue;
            if (pos_in_range(pos, link->source))
                return link->target;
        }
    }
    return NULL;
}

/**
 * Append a DocumentHighlight object to @p arr.
 *
 * @param doc    Destination mutable JSON document.
 * @param arr    DocumentHighlight[] array.
 * @param range  Source range of the highlight.
 * @param kind   LSP DocumentHighlightKind: 2 = Read, 3 = Write.
 */
static void push_highlight(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                           LspRange range, int kind) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "range", range_json(doc, range));
    yyjson_mut_obj_add_uint(doc, obj, "kind", (uint64_t)kind);
    yyjson_mut_arr_add_val(arr, obj);
}

/**
 * Collect Read highlights from @p target's ref_links into @p arr.
 *
 * For each same-document reference, find the specific token within the
 * ref source range that matches the target's id.
 *
 * @param doc         Destination mutable JSON document.
 * @param arr         DocumentHighlight[] array to append to.
 * @param target      Symbol whose incoming references are being collected.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 */
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
    target = find_symbol_at(tokens, num_tokens, cursor);

    /* Step 2b: check if cursor is on a reference site. */
    if (!target) {
        const DocSymbol *link_target =
            find_link_target_at(tokens, num_tokens, cursor);
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
