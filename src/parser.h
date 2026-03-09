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

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Positions / Ranges ──────────────────────────────────────────────────── */

typedef struct { uint32_t line, character; } LspPos;
typedef struct { LspPos start, end; } LspRange;

/* ── Token ───────────────────────────────────────────────────────────────── */

typedef enum {
    TK_IDENT, TK_STR, TK_INTEGER, TK_FLOAT, TK_DATE,
    TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_BANG, TK_PLUS, TK_MINUS, TK_COLON, TK_COMMA,
    TK_LINE_COMMENT, TK_BLOCK_COMMENT, TK_MULTI_LINE_STR,
    TK_EOF, TK_ERROR
} TokenKind;

typedef struct {
    TokenKind kind;
    LspPos    start, end;
    char     *text; /* heap-allocated; caller must free */
} Token;

void token_free(Token *t);

/* ── Lexer ───────────────────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    uint32_t    line, col;
} Lexer;

void  lexer_init(Lexer *l, const char *src);
Token lexer_next(Lexer *l);

/* ── Symbol kinds (LSP SymbolKind values) ────────────────────────────────── */

#define SK_MODULE   2
#define SK_FUNCTION 12
#define SK_VARIABLE 13
#define SK_OBJECT   19
#define SK_EVENT    24

/* ── Symbol ──────────────────────────────────────────────────────────────── */

typedef struct Symbol Symbol;
struct Symbol {
    char    *name;           /* display name, heap-allocated */
    char    *detail;         /* TJP identifier, heap-allocated */
    int      kind;           /* SK_* constant */
    LspRange range;
    LspRange selection_range;
    Symbol  *children;       /* dynamic array */
    int      num_children;
    int      children_cap;
};

/* ── Diagnostic severity ─────────────────────────────────────────────────── */

#define DIAG_ERROR   1
#define DIAG_WARNING 2

/* ── Diagnostic ──────────────────────────────────────────────────────────── */

typedef struct {
    LspRange range;
    int       severity;
    char     *message; /* heap-allocated */
} Diagnostic;

/* ── ParseResult ─────────────────────────────────────────────────────────── */

typedef struct {
    Diagnostic *diagnostics;
    int         num_diagnostics;
    int         diag_cap;
    Symbol     *symbols;
    int         num_symbols;
    int         sym_cap;
} ParseResult;

/* ── Public API ──────────────────────────────────────────────────────────── */

ParseResult parse(const char *src);
void        parse_result_free(ParseResult *r);
void        symbol_free(Symbol *s);

/* Compare two positions. Returns -1, 0, or 1. */
static inline int pos_cmp(LspPos a, LspPos b) {
    if (a.line != b.line) return (a.line < b.line) ? -1 : 1;
    if (a.character != b.character) return (a.character < b.character) ? -1 : 1;
    return 0;
}
