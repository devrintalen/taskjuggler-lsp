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

#include "parser.h"
#include "diagnostics.h"
#include "grammar.tab.h"  /* yyparse() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── flex scanner interface ──────────────────────────────────────────────── *
 *
 * These are declared in the flex-generated lexer.yy.c.  We use void * for
 * YY_BUFFER_STATE to avoid pulling in the full flex header.
 */
typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
extern int             yycolumn; /* column tracker defined in lexer.l */
extern int             yylineno; /* line counter managed by flex %option yylineno */

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

ParseResult *g_result          = NULL;
TokenSpan   *g_tok_spans       = NULL;
int          g_num_tok_spans   = 0;
int          g_tok_span_cap    = 0;
int          g_num_sem_entries = 0;

/* Returns 1 if a token of the given kind will be emitted as a semantic token.
 * Matches the skip set in classify() in semantic_tokens.c. */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/* Called from lexer.l for every token that callers may need to inspect. */
void g_push_tok_span(int kind,
                     uint32_t sl, uint32_t sc,
                     uint32_t el, uint32_t ec,
                     const char *text) {
    if (g_num_tok_spans >= g_tok_span_cap) {
        g_tok_span_cap = g_tok_span_cap ? g_tok_span_cap * 2 : 64;
        TokenSpan *tmp = realloc(g_tok_spans,
                                 (size_t)g_tok_span_cap * sizeof(TokenSpan));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_tok_spans = tmp;
    }
    g_tok_spans[g_num_tok_spans++] = (TokenSpan){
        .token_kind = kind,
        .start      = { sl, sc },
        .end        = { el, ec },
        .text       = text ? strdup(text) : NULL,
    };
    if (is_sem_highlighted(kind))
        g_num_sem_entries += (int)(el - sl + 1);
}

/* ── Token helpers ───────────────────────────────────────────────────────── */

/* Frees the heap-allocated text field of t and sets it to NULL. */
void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

/* ── DocSymbol helpers ───────────────────────────────────────────────────── */

/* Recursively frees all heap memory owned by s (name, detail, children array)
 * but does not free s itself, as it is typically stored inline in an array.
 */
void doc_symbol_free(DocSymbol *s) {
    free(s->name);
    free(s->detail);
    for (int i = 0; i < s->num_children; i++)
        doc_symbol_free(&s->children[i]);
    free(s->children);
}

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

/* Releases all heap memory owned by r (diagnostics, symbols, token spans,
 * definition links, raw dep refs), then zeroes the struct.
 */
void parse_result_free(ParseResult *r) {
    for (int i = 0; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    free(r->diagnostics);
    for (int i = 0; i < r->num_doc_symbols; i++)
        doc_symbol_free(&r->doc_symbols[i]);
    free(r->doc_symbols);

    for (int i = 0; i < r->num_tok_spans; i++)
        free(r->tok_spans[i].text);
    free(r->tok_spans);

    for (int i = 0; i < r->num_def_links; i++)
        free(r->def_links[i].target_uri);
    free(r->def_links);

    for (int i = 0; i < r->num_raw_dep_refs; i++) {
        DepRef *dr = &r->raw_dep_refs[i];
        for (int j = 0; j < dr->nseg; j++) free(dr->segs[j]);
        free(dr->segs);
        for (int j = 0; j < dr->scope_n; j++) free(dr->scope[j]);
        free(dr->scope);
    }
    free(r->raw_dep_refs);

    memset(r, 0, sizeof(*r));
}

/* Appends s to r's doc_symbols array, growing it if needed.
 * Ownership of all heap memory inside s transfers to r.
 */
void push_doc_symbol(ParseResult *r, DocSymbol s) {
    if (r->num_doc_symbols >= r->doc_sym_cap) {
        int nc = r->doc_sym_cap ? r->doc_sym_cap * 2 : 4;
        DocSymbol *tmp = realloc(r->doc_symbols, (size_t)nc * sizeof(DocSymbol));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->doc_symbols = tmp;
        r->doc_sym_cap = nc;
    }
    r->doc_symbols[r->num_doc_symbols++] = s;
}

/* ── DocSymbol tree navigation ───────────────────────────────────────────── */

/* Navigate the symbol tree following the path segments path[0..plen-1] and
 * return the children array at that node.  Transparently descends into
 * SK_MODULE (project) nodes when matching path segments against tasks.
 *
 * syms  — root-level symbols to start from
 * n     — number of entries in syms
 * path  — array of identifier strings to follow (task IDs)
 * plen  — number of segments in path; 0 returns (syms, n) immediately
 * out_n — set to the number of children at the matched node on success, 0 on failure
 *
 * Returns the children array at the matched node, or NULL if not found.
 */
const DocSymbol *doc_symbol_find_path(const DocSymbol *syms, int n,
                                      const char **path, int plen,
                                      int *out_n) {
    if (plen == 0) { *out_n = n; return syms; }
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION && strcmp(syms[i].detail, path[0]) == 0)
            return doc_symbol_find_path(syms[i].children, syms[i].num_children,
                                        path + 1, plen - 1, out_n);
        /* Transparently traverse project containers so that task scope paths
         * rooted inside a project body resolve correctly. */
        if (syms[i].kind == SK_MODULE) {
            const DocSymbol *found = doc_symbol_find_path(
                syms[i].children, syms[i].num_children, path, plen, out_n);
            if (found) return found;
        }
    }
    *out_n = 0;
    return NULL;
}

/* ── Keyword classification ──────────────────────────────────────────────── */

/* Returns the LSP SymbolKind constant for a top-level declaration keyword.
 * Defaults to SK_FUNCTION (task) for any keyword not explicitly listed.
 *
 * kw — keyword string (e.g. "project", "resource", "task")
 */
int symbol_kind_for(const char *kw) {
    if (strcmp(kw, "project")  == 0) return SK_MODULE;
    if (strcmp(kw, "resource") == 0) return SK_OBJECT;
    if (strcmp(kw, "account")  == 0) return SK_VARIABLE;
    if (strcmp(kw, "shift")    == 0) return SK_EVENT;
    return SK_FUNCTION;
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseResult parse(const char *src) {
    /* Set up global state for lexer.l and grammar.y */
    ParseResult result = {0};
    g_result          = &result;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;
    dep_refs_reset();
    yycolumn        = 0;
    yylineno        = 1;

    /* Feed source to flex and run the bison parser */
    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    /* Record where dep-validation diagnostics will start (after syntax errors).
     * Actual validation is deferred to revalidate_dep_refs() in the server so
     * that cross-file symbols are available. */
    result.dep_diag_start = result.num_diagnostics;

    /* Transfer dep_refs ownership from globals to ParseResult. */
    dep_refs_transfer(&result);

    /* Transfer tok_spans array ownership to the ParseResult */
    result.tok_spans       = g_tok_spans;
    result.num_tok_spans   = g_num_tok_spans;
    result.num_sem_entries = g_num_sem_entries;

    /* Clear globals */
    g_result          = NULL;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;

    return result;
}
