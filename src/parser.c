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

ParseResult *g_result        = NULL;
TokenSpan   *g_tok_spans     = NULL;
int          g_num_tok_spans = 0;
int          g_tok_span_cap  = 0;

/* Called from lexer.l for every token that callers may need to inspect. */
void g_push_tok_span(int kind,
                     uint32_t sl, uint32_t sc,
                     uint32_t el, uint32_t ec,
                     const char *text) {
    if (g_num_tok_spans >= g_tok_span_cap) {
        g_tok_span_cap = g_tok_span_cap ? g_tok_span_cap * 2 : 64;
        g_tok_spans    = realloc(g_tok_spans,
                                 (size_t)g_tok_span_cap * sizeof(TokenSpan));
    }
    g_tok_spans[g_num_tok_spans++] = (TokenSpan){
        .token_kind = kind,
        .start      = { sl, sc },
        .end        = { el, ec },
        .text       = text ? strdup(text) : NULL,
    };
}

/* ── Token helpers ───────────────────────────────────────────────────────── */

void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

/* ── DocSymbol helpers ───────────────────────────────────────────────────── */

void doc_symbol_free(DocSymbol *s) {
    free(s->name);
    free(s->detail);
    for (int i = 0; i < s->num_children; i++)
        doc_symbol_free(&s->children[i]);
    free(s->children);
}

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

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
    memset(r, 0, sizeof(*r));
}

void push_diagnostic(ParseResult *r, LspRange range, int severity,
                     const char *msg) {
    if (r->num_diagnostics >= r->diag_cap) {
        int nc = r->diag_cap ? r->diag_cap * 2 : 4;
        r->diagnostics = realloc(r->diagnostics,
                                 (size_t)nc * sizeof(Diagnostic));
        r->diag_cap = nc;
    }
    r->diagnostics[r->num_diagnostics++] =
        (Diagnostic){ range, severity, strdup(msg) };
}

void push_doc_symbol(ParseResult *r, DocSymbol s) {
    if (r->num_doc_symbols >= r->doc_sym_cap) {
        int nc = r->doc_sym_cap ? r->doc_sym_cap * 2 : 4;
        r->doc_symbols = realloc(r->doc_symbols, (size_t)nc * sizeof(DocSymbol));
        r->doc_sym_cap = nc;
    }
    r->doc_symbols[r->num_doc_symbols++] = s;
}

/* ── Keyword classification ──────────────────────────────────────────────── */

int symbol_kind_for(const char *kw) {
    if (strcmp(kw, "project")  == 0) return SK_MODULE;
    if (strcmp(kw, "resource") == 0) return SK_OBJECT;
    if (strcmp(kw, "account")  == 0) return SK_VARIABLE;
    if (strcmp(kw, "shift")    == 0) return SK_EVENT;
    return SK_FUNCTION;
}

/* ── Dependency validation ───────────────────────────────────────────────── *
 *
 * Dep refs are collected by grammar.y semantic actions (dep_ref rule) into
 * g_dep_refs[] during yyparse().  After the symbol tree is complete,
 * validate_dep_refs() checks each ref against the tree and emits diagnostics.
 */

typedef struct {
    int    bang_count;
    char **segs;       /* path segments, heap-copied */
    int    nseg;
    char **scope;      /* snapshot of task scope at parse time, heap-copied */
    int    scope_n;
    LspRange range;
} DepRef;

static DepRef *g_dep_refs    = NULL;
static int     g_num_dep_refs = 0;
static int     g_dep_ref_cap  = 0;

void push_dep_ref(ParseResult *r, int bang_count, const char *path,
                  const char **scope, int scope_n,
                  LspPos start, LspPos end) {
    (void)r; /* not used directly; diagnostics emitted in validate_dep_refs */
    if (g_dep_ref_cap <= g_num_dep_refs) {
        g_dep_ref_cap = g_dep_ref_cap ? g_dep_ref_cap * 2 : 8;
        g_dep_refs = realloc(g_dep_refs, (size_t)g_dep_ref_cap * sizeof(DepRef));
    }
    DepRef *dr = &g_dep_refs[g_num_dep_refs++];

    /* Split path by '.' into segs[] */
    dr->nseg = 0;
    dr->segs = NULL;
    if (path && path[0]) {
        /* Count segments */
        int cap = 1;
        for (const char *p = path; *p; p++) if (*p == '.') cap++;
        dr->segs = malloc((size_t)cap * sizeof(char *));
        char *tmp = strdup(path);
        char *tok = strtok(tmp, ".");
        while (tok) {
            dr->segs[dr->nseg++] = strdup(tok);
            tok = strtok(NULL, ".");
        }
        free(tmp);
    }

    /* Snapshot scope */
    dr->scope_n = scope_n;
    dr->scope   = scope_n ? malloc((size_t)scope_n * sizeof(char *)) : NULL;
    for (int i = 0; i < scope_n; i++)
        dr->scope[i] = strdup(scope[i]);

    dr->bang_count = bang_count;
    dr->range      = (LspRange){ start, end };
}

static void free_dep_refs(void) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        DepRef *dr = &g_dep_refs[i];
        for (int j = 0; j < dr->nseg; j++) free(dr->segs[j]);
        free(dr->segs);
        for (int j = 0; j < dr->scope_n; j++) free(dr->scope[j]);
        free(dr->scope);
    }
    free(g_dep_refs);
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

static int resolve_from(const DocSymbol *syms, int n, const char **segs,
                        int nseg) {
    if (nseg == 0) return 1;
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION
                && strcmp(syms[i].detail, segs[0]) == 0)
            return resolve_from(syms[i].children, syms[i].num_children,
                                segs + 1, nseg - 1);
    }
    return 0;
}

static const DocSymbol *find_scope_local(const DocSymbol *syms, int n,
                                         const char **path, int plen,
                                         int *out_n) {
    if (plen == 0) { *out_n = n; return syms; }
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION
                && strcmp(syms[i].detail, path[0]) == 0)
            return find_scope_local(syms[i].children, syms[i].num_children,
                                    path + 1, plen - 1, out_n);
    }
    *out_n = 0;
    return NULL;
}

static int validate_ref(const DocSymbol *syms, int nsym,
                        const char **scope, int scope_len,
                        int bang_count, const char **segs, int nseg) {
    const DocSymbol *ctx;
    int nctx;
    if (bang_count == 0) {
        ctx  = syms;
        nctx = nsym;
    } else {
        int k = scope_len;
        if (bang_count > k) return 0;
        ctx = find_scope_local(syms, nsym, scope, k - bang_count, &nctx);
        if (!ctx) { nctx = 0; ctx = NULL; }
    }
    return resolve_from(ctx, nctx, segs, nseg);
}

static void validate_dep_refs(const DocSymbol *syms, int nsym, ParseResult *r) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        const DepRef *dr = &g_dep_refs[i];
        if (!validate_ref(syms, nsym,
                          (const char **)dr->scope, dr->scope_n,
                          dr->bang_count,
                          (const char **)dr->segs, dr->nseg)) {
            if (dr->bang_count > dr->scope_n) {
                push_diagnostic(r, dr->range, DIAG_WARNING,
                    "dependency reference escapes beyond project root");
            } else {
                char path[256] = "";
                for (int j = 0; j < dr->nseg; j++) {
                    if (j > 0) strncat(path, ".", sizeof(path) - strlen(path) - 1);
                    strncat(path, dr->segs[j], sizeof(path) - strlen(path) - 1);
                }
                char msg[320];
                snprintf(msg, sizeof(msg), "unresolved task: `%s`", path);
                push_diagnostic(r, dr->range, DIAG_ERROR, msg);
            }
        }
    }
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseResult parse(const char *src) {
    /* Set up global state for lexer.l and grammar.y */
    ParseResult result = {0};
    g_result        = &result;
    g_tok_spans     = NULL;
    g_num_tok_spans = 0;
    g_tok_span_cap  = 0;
    g_dep_refs      = NULL;
    g_num_dep_refs  = 0;
    g_dep_ref_cap   = 0;
    yycolumn        = 0;
    yylineno        = 1;

    /* Feed source to flex and run the bison parser */
    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    /* Post-processing: validate dep refs against the now-complete symbol tree */
    validate_dep_refs(result.doc_symbols, result.num_doc_symbols, &result);
    free_dep_refs();

    /* Transfer tok_spans array ownership to the ParseResult */
    result.tok_spans     = g_tok_spans;
    result.num_tok_spans = g_num_tok_spans;

    /* Clear globals */
    g_result        = NULL;
    g_tok_spans     = NULL;
    g_num_tok_spans = 0;
    g_tok_span_cap  = 0;

    return result;
}
