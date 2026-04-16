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

/* ── Raw dependency reference accumulator ──────────────────────────────── *
 *
 * grammar.y calls push_dep_ref() for each dependency reference encountered
 * during parsing.  These are stored in a file-local buffer until parse()
 * calls dep_refs_collect(), which converts them into a local DepEdge array
 * for resolution.
 */

typedef struct {
    int        bang_count;
    char      *path;       /* dot-separated, heap-allocated */
    DocSymbol *owner;      /* owning symbol; stable because individually malloc'd */
    LspRange   range;
} RawDepRef;

/* Intermediate representation of a dependency reference, used between
 * dep_refs_collect() and resolve_dep_edges(). */
typedef struct {
    int        bang_count;
    char     **segs;      /* dot-split path segments, heap-allocated */
    int        nseg;
    LspRange   range;     /* source range of the reference expression */
    DocSymbol *owner;     /* declaring node; set directly from grammar */
    DocSymbol *resolved;  /* resolved target node; NULL until resolution */
} DepEdge;

static RawDepRef *g_dep_refs     = NULL;
static int        g_num_dep_refs = 0;
static int        g_dep_ref_cap  = 0;

static void free_dep_refs(void) {
    for (int i = 0; i < g_num_dep_refs; i++)
        free(g_dep_refs[i].path);
    free(g_dep_refs);
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

void dep_refs_reset(void) {
    free_dep_refs();
}

void push_dep_ref(int bang_count, const char *path,
                  DocSymbol *owner, LspPos start, LspPos end) {
    if (g_dep_ref_cap <= g_num_dep_refs) {
        g_dep_ref_cap = g_dep_ref_cap ? g_dep_ref_cap * 2 : 8;
        RawDepRef *tmp = realloc(g_dep_refs,
                                 (size_t)g_dep_ref_cap * sizeof(RawDepRef));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_dep_refs = tmp;
    }
    RawDepRef *dr = &g_dep_refs[g_num_dep_refs++];
    dr->bang_count = bang_count;
    dr->path       = path && path[0] ? strdup(path) : NULL;
    dr->owner      = owner;
    dr->range      = (LspRange){ start, end };
}

/* Split a dot-separated path string into a heap-allocated array of segments. */
static void split_path(const char *path, char ***out_segs, int *out_nseg) {
    *out_nseg = 0;
    *out_segs = NULL;
    if (!path || !path[0]) return;
    int cap = 1;
    for (const char *p = path; *p; p++) if (*p == '.') cap++;
    *out_segs = malloc((size_t)cap * sizeof(char *));
    char *tmp = strdup(path);
    char *tok = strtok(tmp, ".");
    while (tok) {
        (*out_segs)[(*out_nseg)++] = strdup(tok);
        tok = strtok(NULL, ".");
    }
    free(tmp);
}

/* Convert the global RawDepRef accumulator into a local DepEdge array.
 * Ownership of all heap memory transfers to the output array.
 * Resets the global accumulator to empty. */
static void dep_refs_collect(DepEdge **out_edges, int *out_count) {
    int n = g_num_dep_refs;
    DepEdge *edges = NULL;
    if (n > 0) {
        edges = malloc((size_t)n * sizeof(DepEdge));
        if (!edges) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    }
    for (int i = 0; i < n; i++) {
        RawDepRef *raw = &g_dep_refs[i];
        edges[i] = (DepEdge){
            .bang_count = raw->bang_count,
            .range      = raw->range,
            .owner      = raw->owner,
        };
        split_path(raw->path, &edges[i].segs, &edges[i].nseg);
        free(raw->path);
    }
    /* Free accumulator shell (contents transferred). */
    free(g_dep_refs);
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;

    *out_edges = edges;
    *out_count = n;
}

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
    free(s->id);
    for (int i = 0; i < s->num_children; i++) {
        doc_symbol_free(s->children[i]);
        free(s->children[i]);
    }
    free(s->children);
    for (int i = 0; i < s->num_def_links; i++)
        free(s->def_links[i].target_uri);
    free(s->def_links);
    for (int i = 0; i < s->num_ref_links; i++)
        free(s->ref_links[i].source_uri);
    free(s->ref_links);
}

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

/* Releases all heap memory owned by r (diagnostics, symbols, token spans,
 * definition links, raw dep refs), then zeroes the struct.
 */
void parse_result_free(ParseResult *r) {
    for (int i = 0; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    free(r->diagnostics);
    for (int i = 0; i < r->num_doc_symbols; i++) {
        doc_symbol_free(r->doc_symbols[i]);
        free(r->doc_symbols[i]);
    }
    free(r->doc_symbols);

    for (int i = 0; i < r->num_tok_spans; i++)
        free(r->tok_spans[i].text);
    free(r->tok_spans);

    /* dep_edges are freed by resolve_dep_edges(); nothing to do here */

    for (int i = 0; i < r->num_included_files; i++)
        free(r->included_files[i]);
    free(r->included_files);

    memset(r, 0, sizeof(*r));
}

/* Appends s to r's doc_symbols pointer array, growing it if needed. */
void push_doc_symbol(ParseResult *r, DocSymbol *s) {
    if (r->num_doc_symbols >= r->doc_sym_cap) {
        int nc = r->doc_sym_cap ? r->doc_sym_cap * 2 : 4;
        DocSymbol **tmp = realloc(r->doc_symbols, (size_t)nc * sizeof(DocSymbol *));
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
DocSymbol *const *doc_symbol_find_path(DocSymbol *const *syms, int n,
                                       const char **path, int plen,
                                       int *out_n) {
    if (plen == 0) { *out_n = n; return syms; }
    for (int i = 0; i < n; i++) {
        if (syms[i]->kind == SK_FUNCTION && strcmp(syms[i]->id, path[0]) == 0)
            return doc_symbol_find_path(syms[i]->children, syms[i]->num_children,
                                        path + 1, plen - 1, out_n);
        /* Transparently traverse project containers so that task scope paths
         * rooted inside a project body resolve correctly. */
        if (syms[i]->kind == SK_MODULE) {
            DocSymbol *const *found = doc_symbol_find_path(
                syms[i]->children, syms[i]->num_children, path, plen, out_n);
            if (found) return found;
        }
    }
    *out_n = 0;
    return NULL;
}

/* ── DocSymbol tree linkage ─────────────────────────────────────────────── */

/* Recursively set parent pointers for all children in the symbol tree. */
static void assign_parents(DocSymbol **syms, int n, DocSymbol *parent) {
    for (int i = 0; i < n; i++) {
        syms[i]->parent = parent;
        assign_parents(syms[i]->children, syms[i]->num_children, syms[i]);
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
static DocSymbol *find_deepest_symbol(DocSymbol **syms, int n, LspPos pos) {
    for (int i = 0; i < n; i++) {
        if (!range_contains(syms[i]->range, pos))
            continue;
        /* This symbol contains pos; check if a child is more specific. */
        DocSymbol *child = find_deepest_symbol(syms[i]->children,
                                               syms[i]->num_children, pos);
        return child ? child : syms[i];
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

/* ── Dependency edge resolution ────────────────────────────────────────── */

/* Append a DefinitionLink to a DocSymbol's def_links array. */
static void push_def_link(DocSymbol *sym, DefinitionLink link) {
    if (sym->num_def_links >= sym->def_links_cap) {
        int nc = sym->def_links_cap ? sym->def_links_cap * 2 : 4;
        DefinitionLink *tmp = realloc(sym->def_links,
                                      (size_t)nc * sizeof(DefinitionLink));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        sym->def_links = tmp;
        sym->def_links_cap = nc;
    }
    sym->def_links[sym->num_def_links++] = link;
}

/* Append a ReferenceLink to a DocSymbol's ref_links array. */
static void push_ref_link(DocSymbol *sym, ReferenceLink link) {
    if (sym->num_ref_links >= sym->ref_links_cap) {
        int nc = sym->ref_links_cap ? sym->ref_links_cap * 2 : 4;
        ReferenceLink *tmp = realloc(sym->ref_links,
                                     (size_t)nc * sizeof(ReferenceLink));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        sym->ref_links = tmp;
        sym->ref_links_cap = nc;
    }
    sym->ref_links[sym->num_ref_links++] = link;
}

/* Resolve dep edges: resolve targets, populate def_links/ref_links, emit
 * diagnostics for broken links, then free the edges array. */
static void resolve_dep_edges(ParseResult *r, DepEdge *edges, int num_edges) {
    for (int i = 0; i < num_edges; i++) {
        DepEdge *edge = &edges[i];

        if (!edge->owner) continue;

        /* TODO: resolve edge->segs using edge->bang_count and edge->owner
         * to find the target DocSymbol.  This involves navigating up
         * bang_count levels from owner via parent pointers, then walking
         * down through segs[].  On success set edge->resolved; on failure
         * emit a diagnostic. */

        if (!edge->resolved) {
            /* TODO: build a descriptive error message from segs[] */
            push_diagnostic(r, edge->range, DIAG_ERROR,
                            "unresolved dependency reference");
            continue;
        }

        /* Outgoing link on the owner (go-to-definition) */
        push_def_link(edge->owner, (DefinitionLink){
            .source     = edge->range,
            .target     = edge->resolved,
            .target_uri = NULL,
        });

        /* Incoming link on the target (find-references) */
        push_ref_link(edge->resolved, (ReferenceLink){
            .source     = edge->range,
            .origin     = edge->owner,
            .source_uri = NULL,
        });
    }

    /* Free edges — fully consumed */
    for (int i = 0; i < num_edges; i++) {
        DepEdge *e = &edges[i];
        for (int j = 0; j < e->nseg; j++) free(e->segs[j]);
        free(e->segs);
    }
    free(edges);
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

    /* Record where dep-validation diagnostics will start (after syntax errors). */
    result.dep_diag_start = result.num_diagnostics;

    /* Collect raw dep refs into a local array for resolution. */
    DepEdge *dep_edges = NULL;
    int num_dep_edges = 0;
    dep_refs_collect(&dep_edges, &num_dep_edges);
    // TODO Figure out how dep_refs_collect() gets simplified from the
    // work done in grammar.y. We've changed RawDepRef to collect a
    // DocSymbol pointer instead of a string, so work on that
    // RawDepRef -> DepEdge logic. It doesn't seem like any real work
    // gets done during dep_refs_collect() and we could just use the
    // RawDepRefs or eliminate RawDepRefs and just call them DepEdge
    // instead.
    

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
    resolve_dep_edges(&result, dep_edges, num_dep_edges);

    return result;
}
