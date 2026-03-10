/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

ParseResult *g_result      = NULL;
Token       *g_tokens      = NULL;
int          g_num_tokens  = 0;
int          g_tok_cap     = 0;

/* Called from lexer.l for every scanned token. */
void g_push_token(int kind, const char *text,
                  uint32_t sl, uint32_t sc,
                  uint32_t el, uint32_t ec) {
    if (g_num_tokens >= g_tok_cap) {
        g_tok_cap = g_tok_cap ? g_tok_cap * 2 : 64;
        g_tokens  = realloc(g_tokens, (size_t)g_tok_cap * sizeof(Token));
    }
    g_tokens[g_num_tokens++] = (Token){
        .kind  = kind,
        .start = { sl, sc },
        .end   = { el, ec },
        .text  = strdup(text),
    };
}

/* ── Token helpers ───────────────────────────────────────────────────────── */

void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

/* ── Symbol helpers ──────────────────────────────────────────────────────── */

void symbol_free(Symbol *s) {
    free(s->name);
    free(s->detail);
    for (int i = 0; i < s->num_children; i++)
        symbol_free(&s->children[i]);
    free(s->children);
}

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

void parse_result_free(ParseResult *r) {
    for (int i = 0; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    free(r->diagnostics);
    for (int i = 0; i < r->num_symbols; i++)
        symbol_free(&r->symbols[i]);
    free(r->symbols);
    free(r->sem_spans);
    for (int i = 0; i < r->num_tokens; i++)
        token_free(&r->tokens[i]);
    free(r->tokens);
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

void push_symbol(ParseResult *r, Symbol s) {
    if (r->num_symbols >= r->sym_cap) {
        int nc = r->sym_cap ? r->sym_cap * 2 : 4;
        r->symbols = realloc(r->symbols, (size_t)nc * sizeof(Symbol));
        r->sym_cap = nc;
    }
    r->symbols[r->num_symbols++] = s;
}

static void push_sem_span(ParseResult *r, uint32_t line, uint32_t col,
                          uint32_t len) {
    if (r->num_sem_spans >= r->sem_cap) {
        int nc = r->sem_cap ? r->sem_cap * 2 : 16;
        r->sem_spans = realloc(r->sem_spans,
                               (size_t)nc * sizeof(SemanticSpan));
        r->sem_cap = nc;
    }
    r->sem_spans[r->num_sem_spans++] = (SemanticSpan){ line, col, len };
}

/* ── Keyword classification ──────────────────────────────────────────────── */

static int is_symbol_keyword(const char *s) {
    return strcmp(s, "project")  == 0 || strcmp(s, "task")     == 0
        || strcmp(s, "resource") == 0 || strcmp(s, "account")  == 0
        || strcmp(s, "shift")    == 0;
}

static int is_task_attr_keyword(const char *s) {
    return strcmp(s, "effort")           == 0 || strcmp(s, "duration")        == 0
        || strcmp(s, "length")           == 0 || strcmp(s, "milestone")       == 0
        || strcmp(s, "scheduled")        == 0 || strcmp(s, "allocate")        == 0
        || strcmp(s, "responsible")      == 0 || strcmp(s, "managers")        == 0
        || strcmp(s, "start")            == 0 || strcmp(s, "end")             == 0
        || strcmp(s, "maxstart")         == 0 || strcmp(s, "maxend")          == 0
        || strcmp(s, "minstart")         == 0 || strcmp(s, "minend")          == 0
        || strcmp(s, "priority")         == 0 || strcmp(s, "complete")        == 0
        || strcmp(s, "note")             == 0 || strcmp(s, "rate")            == 0
        || strcmp(s, "efficiency")       == 0 || strcmp(s, "limits")          == 0
        || strcmp(s, "overtime")         == 0 || strcmp(s, "statusnote")      == 0
        || strcmp(s, "dailyworkinghours") == 0 || strcmp(s, "weeklyworkinghours") == 0
        || strcmp(s, "outputdir")        == 0 || strcmp(s, "purge")           == 0
        || strcmp(s, "charge")           == 0 || strcmp(s, "chargeset")       == 0
        || strcmp(s, "shift")            == 0 || strcmp(s, "credit")          == 0
        || strcmp(s, "debit")            == 0;
}

static int is_decl_keyword(const char *s) {
    if (is_symbol_keyword(s)) return 1;
    return strcmp(s, "macro")           == 0 || strcmp(s, "include")         == 0
        || strcmp(s, "flags")           == 0 || strcmp(s, "supplement")      == 0
        || strcmp(s, "leaves")          == 0 || strcmp(s, "vacation")        == 0
        || strcmp(s, "timingresolution") == 0 || strcmp(s, "now")            == 0
        || strcmp(s, "currency")        == 0 || strcmp(s, "timeformat")      == 0
        || strcmp(s, "timezone")        == 0 || strcmp(s, "workinghours")    == 0
        || strcmp(s, "extend")          == 0 || strcmp(s, "journalentry")    == 0
        || strcmp(s, "balance")         == 0 || strcmp(s, "booking")         == 0;
}

int symbol_kind_for(const char *kw) {
    if (strcmp(kw, "project")  == 0) return SK_MODULE;
    if (strcmp(kw, "resource") == 0) return SK_OBJECT;
    if (strcmp(kw, "account")  == 0) return SK_VARIABLE;
    if (strcmp(kw, "shift")    == 0) return SK_EVENT;
    return SK_FUNCTION;
}

/* ── Semantic span collection ────────────────────────────────────────────── */

/* Scan the token array for scissors strings and split into per-line spans. */
static void collect_sem_spans(ParseResult *r, const Token *tokens,
                               int num_tokens) {
    for (int i = 0; i < num_tokens; i++) {
        if (tokens[i].kind != TK_MULTI_LINE_STR) continue;

        uint32_t    line = tokens[i].start.line;
        uint32_t    col  = tokens[i].start.character;
        const char *p    = tokens[i].text;

        while (*p) {
            const char *q = p;
            while (*q && *q != '\n') q++;
            const char *end = q;
            if (end > p && *(end - 1) == '\r') end--;
            uint32_t len = (uint32_t)(end - p);
            if (len > 0) push_sem_span(r, line, col, len);
            if (*q == '\n') { q++; line++; col = 0; }
            else break;
            p = q;
        }
    }
}

/* ── Dependency validation (second pass over token array) ───────────────── */

static int resolve_from(const Symbol *syms, int n, const char **segs,
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

static const Symbol *find_scope_local(const Symbol *syms, int n,
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

static int validate_ref(const Symbol *syms, int nsym,
                        const char **scope, int scope_len,
                        int bang_count, const char *id_text) {
    const Symbol *ctx;
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

    char buf[512];
    strncpy(buf, id_text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    const char *segs[64];
    int nseg = 0;
    char *tok = strtok(buf, ".");
    while (tok && nseg < 64) { segs[nseg++] = tok; tok = strtok(NULL, "."); }

    return resolve_from(ctx, nctx, segs, nseg);
}

static void validate_deps(const Token *tokens, int num_tokens,
                          const Symbol *syms, int nsym,
                          ParseResult *r) {
    typedef enum { SS_SCAN, SS_EXPECT_ID, SS_BEFORE_LBRACE } SState;

    SState sstate      = SS_SCAN;
    char  *pending_id  = NULL;
    int    brace_depth = 0;
    int    in_deps     = 0;
    int    needs_comma = 0;

    char     *ts_ids[128];
    uint32_t  ts_dep[128];
    int       ts_n = 0;

    int    bang_count       = 0;
    LspPos bang_first_start = {0};
    LspPos bang_last_end    = {0};

    for (int ti = 0; ti < num_tokens; ti++) {
        const Token *t = &tokens[ti];
        if (t->kind == TK_EOF) break;
        if (t->kind == TK_LINE_COMMENT || t->kind == TK_BLOCK_COMMENT)
            continue;

        switch (t->kind) {
        case TK_LBRACE:
            in_deps     = 0;
            needs_comma = 0;
            bang_count  = 0;
            brace_depth++;
            if (sstate == SS_BEFORE_LBRACE && pending_id) {
                if (ts_n < 128) {
                    ts_ids[ts_n] = pending_id;
                    ts_dep[ts_n] = (uint32_t)brace_depth;
                    ts_n++;
                    pending_id = NULL;
                }
            }
            free(pending_id);
            pending_id = NULL;
            sstate     = SS_SCAN;
            break;

        case TK_RBRACE:
            in_deps     = 0;
            needs_comma = 0;
            bang_count  = 0;
            while (ts_n > 0 && ts_dep[ts_n - 1] >= (uint32_t)brace_depth)
                free(ts_ids[--ts_n]);
            if (brace_depth > 0) brace_depth--;
            sstate     = SS_SCAN;
            free(pending_id);
            pending_id = NULL;
            break;

        case TK_BANG:
            if (in_deps) {
                if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                    bang_count  = 0;
                } else {
                    if (bang_count == 0) bang_first_start = t->start;
                    bang_count++;
                    bang_last_end = t->end;
                }
            }
            break;

        case TK_COMMA:
            if (in_deps) {
                bang_count  = 0;
                needs_comma = 0;
            }
            break;

        case TK_IDENT:
            if (in_deps) {
                if (is_decl_keyword(t->text) || is_task_attr_keyword(t->text)) {
                    in_deps     = 0;
                    needs_comma = 0;
                    bang_count  = 0;
                } else if (strcmp(t->text, "depends") == 0
                        || strcmp(t->text, "precedes") == 0) {
                    bang_count  = 0;
                    needs_comma = 0;
                    break;
                } else if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                    bang_count  = 0;
                } else {
                    const char *scope[128];
                    for (int i = 0; i < ts_n; i++) scope[i] = ts_ids[i];

                    if (!validate_ref(syms, nsym, scope, ts_n,
                                      bang_count, t->text)) {
                        if (bang_count > 0 && bang_count > ts_n) {
                            LspRange rng = { bang_first_start, bang_last_end };
                            push_diagnostic(r, rng, DIAG_WARNING,
                                "dependency reference escapes beyond project root");
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "unresolved task: `%s`", t->text);
                            LspRange rng = { t->start, t->end };
                            push_diagnostic(r, rng, DIAG_ERROR, msg);
                        }
                    }
                    bang_count  = 0;
                    needs_comma = 1;
                    break;
                }
            }

            if (strcmp(t->text, "depends") == 0
                    || strcmp(t->text, "precedes") == 0) {
                in_deps     = 1;
                needs_comma = 0;
                bang_count  = 0;
                sstate = SS_SCAN;
            } else if (strcmp(t->text, "task") == 0) {
                sstate = SS_EXPECT_ID;
                free(pending_id);
                pending_id = NULL;
            } else if (sstate == SS_EXPECT_ID) {
                free(pending_id);
                pending_id = strdup(t->text);
                sstate     = SS_BEFORE_LBRACE;
            }
            break;

        default:
            if (in_deps) {
                if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                }
                bang_count = 0;
            }
            if (sstate == SS_EXPECT_ID) {
                sstate = SS_SCAN;
                free(pending_id);
                pending_id = NULL;
            }
            break;
        }
    }

    free(pending_id);
    for (int i = 0; i < ts_n; i++) free(ts_ids[i]);
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseResult parse(const char *src) {
    /* Set up global state for lexer.l and grammar.y */
    ParseResult result = {0};
    g_result     = &result;
    g_tokens     = NULL;
    g_num_tokens = 0;
    g_tok_cap    = 0;
    yycolumn     = 0;

    /* Feed source to flex and run the bison parser */
    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    /* Post-processing passes */
    collect_sem_spans(&result, g_tokens, g_num_tokens);
    validate_deps(g_tokens, g_num_tokens,
                  result.symbols, result.num_symbols, &result);

    /* Transfer token-array ownership to the ParseResult */
    result.tokens     = g_tokens;
    result.num_tokens = g_num_tokens;

    /* Clear globals */
    g_result     = NULL;
    g_tokens     = NULL;
    g_num_tokens = 0;
    g_tok_cap    = 0;

    return result;
}
