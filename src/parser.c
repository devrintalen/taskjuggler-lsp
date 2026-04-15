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

    for (int i = 0; i < r->num_dep_edges; i++) {
        DepEdge *e = &r->dep_edges[i];
        for (int j = 0; j < e->nseg; j++) free(e->segs[j]);
        free(e->segs);
    }
    free(r->dep_edges);

    /* dep_edge_scopes is normally consumed by assign_dep_edges(), but
     * free any leftovers in case parse was interrupted. */
    if (r->dep_edge_scopes) {
        for (int i = 0; i < r->num_dep_edges; i++) {
            DepEdgeScope *sc = &r->dep_edge_scopes[i];
            for (int j = 0; j < sc->scope_n; j++) free(sc->scope[j]);
            free(sc->scope);
        }
        free(r->dep_edge_scopes);
    }

    for (int i = 0; i < r->num_included_files; i++)
        free(r->included_files[i]);
    free(r->included_files);

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

/* Append a heap-allocated copy of the unquoted filename from an include
 * statement.  quoted_text is the raw TK_STR token text (e.g. "\"foo.tji\"");
 * the surrounding quotes are stripped before storing.
 */
void push_included_file(ParseResult *r, const char *quoted_text) {
    if (!quoted_text) return;
    size_t len = strlen(quoted_text);
    /* Strip surrounding quotes if present */
    const char *inner = quoted_text;
    size_t inner_len  = len;
    if (len >= 2 && (quoted_text[0] == '"' || quoted_text[0] == '\'')) {
        inner     = quoted_text + 1;
        inner_len = len - 2;
    }
    char *copy = malloc(inner_len + 1);
    if (!copy) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(copy, inner, inner_len);
    copy[inner_len] = '\0';

    if (r->num_included_files >= r->included_files_cap) {
        int nc = r->included_files_cap ? r->included_files_cap * 2 : 4;
        char **tmp = realloc(r->included_files, (size_t)nc * sizeof(char *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->included_files     = tmp;
        r->included_files_cap = nc;
    }
    r->included_files[r->num_included_files++] = copy;
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

/* ── DocSymbol tree linkage ─────────────────────────────────────────────── */

/* Recursively set parent pointers for all children in the symbol tree. */
static void assign_parents(DocSymbol *syms, int n, DocSymbol *parent) {
    for (int i = 0; i < n; i++) {
        syms[i].parent = parent;
        assign_parents(syms[i].children, syms[i].num_children, &syms[i]);
    }
}

/* ── Token-to-symbol cross-referencing ──────────────────────────────────── */

/* Returns 1 if pos is contained within range (inclusive of start, exclusive
 * of end), using standard LSP half-open interval semantics. */
static int range_contains(LspRange range, LspPos pos) {
    if (pos_cmp(pos, range.start) < 0) return 0;
    if (pos_cmp(pos, range.end) >= 0) return 0;
    return 1;
}

/* Find the deepest DocSymbol whose range contains pos.
 * Returns NULL if no symbol contains the position. */
static DocSymbol *find_deepest_symbol(DocSymbol *syms, int n, LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (!range_contains(syms[i].range, pos))
            continue;
        /* This symbol contains pos; check if a child is more specific. */
        DocSymbol *child = find_deepest_symbol(syms[i].children,
                                               syms[i].num_children, pos);
        return child ? child : &syms[i];
    }
    return NULL;
}

/* Assign each token span's owner pointer to the deepest enclosing DocSymbol.
 * Called after parsing when both tok_spans[] and doc_symbols[] are finalized. */
static void assign_token_owners(ParseResult *r) {
    for (int i = 0; i < r->num_tok_spans; i++) {
        r->tok_spans[i].owner = find_deepest_symbol(
            r->doc_symbols, r->num_doc_symbols, r->tok_spans[i].start);
    }
}

/* ── Dependency edge owner resolution ──────────────────────────────────── */

/* Find a mutable DocSymbol by following the scope path (task IDs from root).
 * Like doc_symbol_find_path but returns the node itself rather than its
 * children, and returns a mutable pointer.  Returns NULL if not found. */
static DocSymbol *find_symbol_by_scope(DocSymbol *syms, int n,
                                       char **scope, int scope_n) {
    if (scope_n == 0) return NULL;
    for (int i = 0; i < n; i++) {
        /* Transparently descend into project containers */
        if (syms[i].kind == SK_MODULE) {
            DocSymbol *found = find_symbol_by_scope(
                syms[i].children, syms[i].num_children, scope, scope_n);
            if (found) return found;
        }
        if (syms[i].kind == SK_FUNCTION && syms[i].detail &&
                strcmp(syms[i].detail, scope[0]) == 0) {
            if (scope_n == 1) return &syms[i];
            return find_symbol_by_scope(syms[i].children, syms[i].num_children,
                                        scope + 1, scope_n - 1);
        }
    }
    return NULL;
}

/* Resolve the temporary scope snapshots in dep_edge_scopes[] to owner
 * pointers on each DepEdge, then free the scopes parallel array. */
static void assign_dep_edges(ParseResult *r) {
    if (!r->dep_edge_scopes) return;
    for (int i = 0; i < r->num_dep_edges; i++) {
        DepEdgeScope *sc = &r->dep_edge_scopes[i];
        r->dep_edges[i].owner = find_symbol_by_scope(
            r->doc_symbols, r->num_doc_symbols, sc->scope, sc->scope_n);
        for (int j = 0; j < sc->scope_n; j++) free(sc->scope[j]);
        free(sc->scope);
    }
    free(r->dep_edge_scopes);
    r->dep_edge_scopes = NULL;
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

    /* Build cross-references between tokens, symbols, and dep edges */
    assign_parents(result.doc_symbols, result.num_doc_symbols, NULL);
    assign_token_owners(&result);
    assign_dep_edges(&result);

    return result;
}
