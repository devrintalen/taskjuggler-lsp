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

/*
 * %code requires goes into both grammar.tab.c and grammar.tab.h, so the
 * types used in %union are visible wherever grammar.tab.h is included
 * (including lexer.yy.c and tools/lexer_test.c).
 */
%code requires {
#include "parser.h"

/* Dynamic array of Symbols, used for body children. */
typedef struct { Symbol *arr; int n, cap; } SymArr;

/* Return type for opt_body and body_items rules. */
typedef struct { SymArr syms; LspPos end; } BodyResult;

/* Return type for item rule: either a Symbol or nothing. */
typedef struct { Symbol sym; int has_sym; } ItemResult;
}

%{
#include "parser.h"        /* Token, Symbol, ParseResult, LspRange, etc. */
#include "grammar.tab.h"   /* TK_* / KW_* constants, YYSTYPE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Globals defined in parser.c, shared with lexer.l */
extern ParseResult *g_result;

int  yylex(void);
void yyerror(const char *msg);

/* ── Helpers (declared/defined in parser.c) ─────────────────────────────── */

extern void push_symbol    (ParseResult *r, Symbol s);
extern void push_diagnostic(ParseResult *r, LspRange range, int severity,
                             const char *msg);
extern int  symbol_kind_for(const char *kw);

/* ── Symbol array helper ─────────────────────────────────────────────────── */

static void symarr_push(SymArr *a, Symbol s) {
    if (a->n >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->arr = realloc(a->arr, (size_t)a->cap * sizeof(Symbol));
    }
    a->arr[a->n++] = s;
}

/* ── Build a Symbol from the components of a symbol_decl rule ───────────── */

static Symbol make_symbol(Token kw, Token id, Token name, BodyResult body) {
    Symbol s = {0};
    s.kind = symbol_kind_for(kw.text);

    if (id.text) {
        s.detail          = id.text;   /* take ownership */
        s.selection_range = (LspRange){ id.start, id.end };
    } else {
        s.detail          = strdup(kw.text);
        s.selection_range = (LspRange){ kw.start, kw.end };
    }

    s.name = name.text ? name.text : strdup(s.detail); /* take ownership */

    /* Range: from keyword start to closing brace (or last known token). */
    LspPos range_end = body.end;
    if (range_end.line == 0 && range_end.character == 0)
        range_end = kw.end;  /* TODO: use last arg-token end if available */
    s.range = (LspRange){ kw.start, range_end };

    s.children     = body.syms.arr;
    s.num_children = body.syms.n;
    s.children_cap = body.syms.cap;

    return s;
}
%}

/* ── Value union ─────────────────────────────────────────────────────────── */

%union {
    Token      tok;   /* single token (kind / start / end / text) */
    Symbol     sym;   /* fully built symbol */
    BodyResult body;  /* body: children + closing-brace position */
    ItemResult item;  /* item: optional symbol */
}

/* ── Token declarations ──────────────────────────────────────────────────── *
 *
 * Symbol-introducing keywords get their own token codes so the grammar can
 * distinguish them from plain identifiers and use them as item starters.
 * All other TJP keywords (depends, allocate, macro, …) are returned as
 * TK_IDENT by the lexer; they are handled in the second-pass validate_deps.
 *
 * TODO: Add KW_DEPENDS, KW_PRECEDES (and others) once the grammar rules for
 * those statements are written, enabling precise statement-boundary detection.
 */

%token <tok> KW_PROJECT KW_TASK KW_RESOURCE KW_ACCOUNT KW_SHIFT
%token <tok> KW_ACCOUNTPREFIX KW_ACCOUNTREPORT KW_ACCOUNTROOT KW_ACTIVE
%token <tok> KW_ADOPT KW_AGGREGATE KW_ALERT KW_ALERTLEVELS KW_ALLOCATE
%token <tok> KW_ALTERNATIVE KW_AUTHOR KW_AUXDIR KW_BALANCE KW_BOOKING
%token <tok> KW_CAPTION KW_CELLCOLOR KW_CELLTEXT KW_CENTER KW_CHARGE
%token <tok> KW_CHARGESET KW_COLUMNS KW_COMPLETE KW_COPYRIGHT

%token <tok> TK_IDENT
%token <tok> TK_STR TK_INTEGER TK_FLOAT TK_DATE
%token <tok> TK_LBRACE TK_RBRACE
%token <tok> TK_LBRACKET TK_RBRACKET
%token <tok> TK_BANG TK_PLUS TK_MINUS TK_DOT TK_COLON TK_COMMA TK_PERCENT TK_DOLLAR
%token <tok> TK_MULTI_LINE_STR
%token <tok> TK_ERROR
%token       TK_LINE_COMMENT TK_BLOCK_COMMENT  /* stored in token array only; never returned to parser */

/* ── Non-terminal types ──────────────────────────────────────────────────── */

%type <sym>  symbol_decl
%type <tok>  sym_kw opt_id opt_name
%type <body> opt_body body_items
%type <item> item

/* ── Grammar ─────────────────────────────────────────────────────────────── */

%%

/* Top-level file: zero or more items. */
file
    : items
    ;

items
    : /* empty */
    | items item
        {
            if ($2.has_sym)
                push_symbol(g_result, $2.sym);
        }
    ;

/*
 * item: one top-level (or nested) statement.
 *
 * symbol_decl  – recognised, returns a Symbol.
 * TK_IDENT …   – any other statement (depends, allocate, macro, etc.);
 *                body children are discarded.
 * TK_RBRACE    – unmatched closing brace; emit a diagnostic.
 * error        – bison error recovery: skip tokens until the next item.
 *
 * LIMITATION: Because all non-symbol keywords are plain TK_IDENT, the
 * `opt_args` rule is greedy and will consume consecutive TK_IDENT tokens
 * across implicit statement boundaries.  Symbol extraction is correct
 * because KW_* tokens (not TK_IDENT) break out of opt_args.  Only the
 * argument ranges of non-symbol statements may be imprecise.
 *
 * TODO: Add distinct tokens for declaration keywords so that `opt_args`
 * ends cleanly at each keyword.
 */
item
    : symbol_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | TK_IDENT opt_args opt_body
        {
            /* Non-symbol statement: free any children from the body. */
            for (int i = 0; i < $3.syms.n; i++)
                symbol_free(&$3.syms.arr[i]);
            free($3.syms.arr);
            token_free(&$1);
            $$.has_sym = 0;
        }
    | TK_RBRACE
        {
            LspRange rng = { $1.start, $1.end };
            push_diagnostic(g_result, rng, DIAG_ERROR, "unmatched `}`");
            token_free(&$1);
            $$.has_sym = 0;
        }
    | error
        { yyerrok; $$.has_sym = 0; }
    ;

/*
 * symbol_decl: task / project / resource / account / shift declaration.
 *
 *   sym_kw  opt_id  opt_name  opt_args  opt_body
 *   ^^^^^   ^^^^^^  ^^^^^^^^  ^^^^^^^^  ^^^^^^^^
 *   task    gui     "GUI"     <args>    { children }
 *
 * opt_id is omitted for `project` if no id is given (uncommon but valid).
 * opt_name is a quoted string display name (optional).
 * opt_args consumes any remaining inline arguments up to `{` or a KW_*.
 */
symbol_decl
    : sym_kw opt_id opt_name opt_args opt_body
        {
            $$ = make_symbol($1, $2, $3, $5);
            token_free(&$1); /* make_symbol strdup'd kw.text where needed */
        }
    ;

sym_kw
    : KW_PROJECT  { $$ = $1; }
    | KW_TASK     { $$ = $1; }
    | KW_RESOURCE { $$ = $1; }
    | KW_ACCOUNT  { $$ = $1; }
    | KW_SHIFT    { $$ = $1; }
    ;

/* Optional identifier (the TJP id, e.g. "gui" in `task gui "GUI" {}`). */
opt_id
    : /* empty */  { $$ = (Token){0}; }
    | TK_IDENT     { $$ = $1; }
    ;

/* Optional display-name string (e.g. "GUI Frontend"). */
opt_name
    : /* empty */  { $$ = (Token){0}; }
    | TK_STR       { $$ = $1; }
    ;

/*
 * opt_args: zero or more non-structural argument tokens.
 *
 * Deliberately excludes TK_LBRACE, TK_RBRACE, and KW_* tokens so that
 * brace bodies and nested symbol declarations are never consumed here.
 *
 * Token text is discarded; positions are not currently tracked.
 * TODO: track the end position of the last arg for accurate symbol ranges.
 */
opt_args
    : /* empty */
    | opt_args arg_token
    ;

arg_token
    : TK_IDENT     { token_free(&$1); }
    | TK_STR       { token_free(&$1); }
    | TK_INTEGER   { token_free(&$1); }
    | TK_FLOAT     { token_free(&$1); }
    | TK_DATE      { token_free(&$1); }
    | TK_BANG      { token_free(&$1); }
    | TK_PLUS      { token_free(&$1); }
    | TK_MINUS     { token_free(&$1); }
    | TK_DOT       { token_free(&$1); }
    | TK_COLON     { token_free(&$1); }
    | TK_COMMA     { token_free(&$1); }
    | TK_PERCENT   { token_free(&$1); }
    | TK_DOLLAR    { token_free(&$1); }
    | TK_LBRACKET  { token_free(&$1); }
    | TK_RBRACKET  { token_free(&$1); }
    | TK_MULTI_LINE_STR { token_free(&$1); }
    | TK_ERROR     { token_free(&$1); }
    /* All KW_* tokens that are not sym_kw must be listed here so the grammar
     * can consume them in argument/attribute positions inside any block. */
    | KW_ACCOUNTPREFIX { token_free(&$1); }
    | KW_ACCOUNTREPORT { token_free(&$1); }
    | KW_ACCOUNTROOT   { token_free(&$1); }
    | KW_ACTIVE        { token_free(&$1); }
    | KW_ADOPT         { token_free(&$1); }
    | KW_AGGREGATE     { token_free(&$1); }
    | KW_ALERT         { token_free(&$1); }
    | KW_ALERTLEVELS   { token_free(&$1); }
    | KW_ALLOCATE      { token_free(&$1); }
    | KW_ALTERNATIVE   { token_free(&$1); }
    | KW_AUTHOR        { token_free(&$1); }
    | KW_AUXDIR        { token_free(&$1); }
    | KW_BALANCE       { token_free(&$1); }
    | KW_BOOKING       { token_free(&$1); }
    | KW_CAPTION       { token_free(&$1); }
    | KW_CELLCOLOR     { token_free(&$1); }
    | KW_CELLTEXT      { token_free(&$1); }
    | KW_CENTER        { token_free(&$1); }
    | KW_CHARGE        { token_free(&$1); }
    | KW_CHARGESET     { token_free(&$1); }
    | KW_COLUMNS       { token_free(&$1); }
    | KW_COMPLETE      { token_free(&$1); }
    | KW_COPYRIGHT     { token_free(&$1); }
    ;

/*
 * opt_body: optional `{ items }` block.
 *
 * Returns a BodyResult with the collected child symbols and the end
 * position of the closing `}`.
 *
 * The second alternative uses bison's built-in `error` token for error
 * recovery when the closing brace is missing.
 */
opt_body
    : /* empty */
        { $$.syms = (SymArr){0}; $$.end = (LspPos){0}; }
    | TK_LBRACE body_items TK_RBRACE
        {
            $$.syms = $2.syms;
            $$.end  = $3.end;
            token_free(&$1);
            token_free(&$3);
        }
    | TK_LBRACE body_items error
        {
            $$.syms = $2.syms;
            $$.end  = (LspPos){0};
            push_diagnostic(g_result,
                            (LspRange){ $1.start, $1.end },
                            DIAG_ERROR, "unclosed `{`");
            token_free(&$1);
        }
    ;

/*
 * body_items: items collected inside a `{…}` block.
 *
 * Returns a BodyResult (reusing the <body> union slot; .end is always zero
 * here and gets filled in by the enclosing opt_body rule from the `}`).
 *
 * Unlike the top-level `items` rule which pushes to g_result directly,
 * body_items accumulates symbols so they become children of the enclosing
 * symbol_decl.
 */
body_items
    : /* empty */
        { $$.syms = (SymArr){0}; $$.end = (LspPos){0}; }
    | body_items item
        {
            $$ = $1;
            if ($2.has_sym)
                symarr_push(&$$.syms, $2.sym);
        }
    ;

%%

void yyerror(const char *msg) {
    /*
     * TODO: capture the position of the offending token (e.g. via a
     * `g_last_token` global updated by the lexer) and emit a proper
     * LSP diagnostic instead of printing to stderr.
     */
    (void)msg;
}
