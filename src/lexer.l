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

%{
#include "parser.h"
#include "grammar.tab.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Column tracking ─────────────────────────────────────────────────────── *
 *
 * yylineno (managed by %option yylineno) counts newlines and starts at 1.
 * We map to 0-based LSP lines with (yylineno - 1).
 *
 * yycolumn is managed manually: advanced by yyleng in YY_USER_ACTION and
 * reset to 0 in newline-consuming rules.
 *
 * NOTE: yycolumn counts *bytes*, not Unicode codepoints or UTF-16 code units.
 * For ASCII-only files this is correct.  Multi-byte UTF-8 characters will
 * produce wrong character positions.
 *
 * TODO: Replace byte counting with codepoint counting (iterate yytext with
 * utf8_char_len) to match LSP character semantics.
 */
int yycolumn = 0;

/* Start position of the current token, saved before each rule executes. */
static uint32_t tok_sl = 0, tok_sc = 0;

#define YY_USER_ACTION                          \
    tok_sl   = (uint32_t)(yylineno - 1);        \
    tok_sc   = (uint32_t)yycolumn;              \
    yycolumn += yyleng;

/* ── Token-array interface (defined in parser.c) ────────────────────────── */

extern void g_push_token(int kind, const char *text,
                         uint32_t sl, uint32_t sc,
                         uint32_t el, uint32_t ec);

/* ── emit(): record token in array and populate yylval ──────────────────── */

static void emit(int kind, const char *text) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_token(kind, text, tok_sl, tok_sc, el, ec);
    yylval.tok.kind  = kind;
    yylval.tok.start = (LspPos){ tok_sl, tok_sc };
    yylval.tok.end   = (LspPos){ el, ec };
    yylval.tok.text  = strdup(text);
}

/* ── String buffer for multi-character tokens ───────────────────────────── */

/* 1 MB should be enough for any TJP source file section. */
static char   sbuf[1 << 20];
static int    sbuf_len = 0;
static uint32_t sbuf_sl = 0, sbuf_sc = 0; /* start of buffered token */

static void sbuf_reset(void) {
    sbuf_len = 0;
    sbuf_sl  = tok_sl;
    sbuf_sc  = tok_sc;
}

static void sbuf_append(void) {
    if (sbuf_len + yyleng < (int)sizeof(sbuf) - 1) {
        memcpy(sbuf + sbuf_len, yytext, yyleng);
        sbuf_len += yyleng;
    }
}

static void sbuf_push_char(char c) {
    if (sbuf_len + 1 < (int)sizeof(sbuf) - 1)
        sbuf[sbuf_len++] = c;
}

/* Emit a token from the accumulated buffer. */
static void sbuf_emit(int kind) {
    sbuf[sbuf_len] = '\0';
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_token(kind, sbuf, sbuf_sl, sbuf_sc, el, ec);
    yylval.tok.kind  = kind;
    yylval.tok.start = (LspPos){ sbuf_sl, sbuf_sc };
    yylval.tok.end   = (LspPos){ el, ec };
    yylval.tok.text  = strdup(sbuf);
}

/* ── Keyword dispatch ────────────────────────────────────────────────────── *
 *
 * Only the five symbol-introducing keywords are returned as distinct tokens.
 * All other TJP keywords (depends, allocate, macro, include, etc.) are
 * returned as plain TK_IDENT so the grammar can treat them uniformly.
 *
 * TODO: Add distinct tokens for declaration keywords (KW_DEPENDS,
 * KW_ALLOCATE, KW_MACRO, etc.) once the grammar rules for those statements
 * are fleshed out.  This will allow precise statement-boundary detection.
 */
static int keyword_token(const char *s) {
    if (strcmp(s, "project")  == 0) return KW_PROJECT;
    if (strcmp(s, "task")     == 0) return KW_TASK;
    if (strcmp(s, "resource") == 0) return KW_RESOURCE;
    if (strcmp(s, "account")  == 0) return KW_ACCOUNT;
    if (strcmp(s, "shift")    == 0) return KW_SHIFT;
    if (strcmp(s, "accountprefix") == 0) return KW_ACCOUNTPREFIX;
    if (strcmp(s, "accountreport") == 0) return KW_ACCOUNTREPORT;
    if (strcmp(s, "accountroot")   == 0) return KW_ACCOUNTROOT;
    if (strcmp(s, "active")   == 0) return KW_ACTIVE;
    if (strcmp(s, "adopt")    == 0) return KW_ADOPT;
    if (strcmp(s, "aggregate") == 0) return KW_AGGREGATE;
    if (strcmp(s, "alert")    == 0) return KW_ALERT;
    if (strcmp(s, "alertlevels") == 0) return KW_ALERTLEVELS;
    if (strcmp(s, "allocate") == 0) return KW_ALLOCATE;
    if (strcmp(s, "alternative") == 0) return KW_ALTERNATIVE;
    if (strcmp(s, "author")   == 0) return KW_AUTHOR;
    if (strcmp(s, "auxdir")   == 0) return KW_AUXDIR;
    if (strcmp(s, "balance")  == 0) return KW_BALANCE;
    if (strcmp(s, "booking")  == 0) return KW_BOOKING;
    if (strcmp(s, "caption")  == 0) return KW_CAPTION;
    if (strcmp(s, "cellcolor") == 0) return KW_CELLCOLOR;
    if (strcmp(s, "celltext") == 0) return KW_CELLTEXT;
    if (strcmp(s, "center")   == 0) return KW_CENTER;
    if (strcmp(s, "charge")   == 0) return KW_CHARGE;
    if (strcmp(s, "chargeset") == 0) return KW_CHARGESET;
    if (strcmp(s, "columns")  == 0) return KW_COLUMNS;
    if (strcmp(s, "complete") == 0) return KW_COMPLETE;
    if (strcmp(s, "copyright") == 0) return KW_COPYRIGHT;
    return TK_IDENT;
}
%}

%option noyywrap
%option yylineno
%option 8bit

/* Start conditions for tokens whose bodies span multiple lexer fragments. */
%x BLOCK_COMMENT
%x SCISSORS
%x DQUOTE

%%

[ \t\r\f\v]+    { /* skip whitespace */ }
\n              { yycolumn = 0; }

#[^\n]*         { emit(TK_LINE_COMMENT, yytext); /* not returned to parser */ }
\/\/[^\n]*      { emit(TK_LINE_COMMENT, yytext); /* not returned to parser */ }

"/*"            { sbuf_reset(); sbuf_append(); BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>{
    "*/"        { sbuf_append(); sbuf_emit(TK_BLOCK_COMMENT); BEGIN(INITIAL); }
    \n          { sbuf_append(); yycolumn = 0; }
    [^*\n]+     { sbuf_append(); }
    "*"         { sbuf_append(); }
    <<EOF>>     { sbuf[sbuf_len] = '\0'; sbuf_emit(TK_ERROR);
                  BEGIN(INITIAL); return TK_ERROR; }
}

    /* Scissors multi-line string: -8<- ... ->8- */
"-8<-"          { sbuf_reset(); sbuf_append(); BEGIN(SCISSORS); }
<SCISSORS>{
    "->8-"      { sbuf_append(); sbuf_emit(TK_MULTI_LINE_STR);
                  BEGIN(INITIAL); return TK_MULTI_LINE_STR; }
    \n          { sbuf_append(); yycolumn = 0; }
    [^-\n]+     { sbuf_append(); }
    "-"         { sbuf_append(); }
    <<EOF>>     { sbuf[sbuf_len] = '\0'; sbuf_emit(TK_ERROR);
                  BEGIN(INITIAL); return TK_ERROR; }
}

\"              { sbuf_reset(); BEGIN(DQUOTE); }
<DQUOTE>{
    \"          { sbuf_emit(TK_STR); BEGIN(INITIAL); return TK_STR; }
    \n          { sbuf_emit(TK_ERROR); yycolumn = 0;
                  BEGIN(INITIAL); return TK_ERROR; }
    \\[^\n]     { sbuf_push_char(yytext[1]); /* store escaped char only */ }
    [^\"\\\n]+  { sbuf_append(); }
    <<EOF>>     { sbuf_emit(TK_ERROR); BEGIN(INITIAL); return TK_ERROR; }
}

    /* Date YYYY-MM-DD[-hh:mm[:ss]][-{+|-}HHMM]: must precede INT so longest match wins.
     * Time, seconds, and timezone suffix are each independently optional per the TJP spec.
     * Timezone is always a leading '-' separator followed by an explicit '+' or '-' sign. */
[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?(-[+-][0-9]{4})?  { emit(TK_DATE, yytext); return TK_DATE; }

[0-9]+"."[0-9]+  { emit(TK_FLOAT, yytext); return TK_FLOAT; }

[0-9]+           { emit(TK_INTEGER, yytext); return TK_INTEGER; }

    /* Identifier / keyword: letters, digits, '_', ':', high UTF-8 bytes.
     * '.' is NOT included; it is emitted as TK_DOT so that dotted dependency
     * paths (e.g. deliveries.start) can be resolved component-wise by the LSP. */
[a-zA-Z_\x80-\xFF][a-zA-Z0-9_:\x80-\xFF]* {
    int tok = keyword_token(yytext);
    emit(TK_IDENT, yytext); /* always TK_IDENT in token array */
    yylval.tok.kind = tok;  /* KW_* for symbol keywords        */
    return tok;
}

"{"     { emit(TK_LBRACE,   yytext); return TK_LBRACE;   }
"}"     { emit(TK_RBRACE,   yytext); return TK_RBRACE;   }
"["     { emit(TK_LBRACKET, yytext); return TK_LBRACKET; }
"]"     { emit(TK_RBRACKET, yytext); return TK_RBRACKET; }
"!"     { emit(TK_BANG,     yytext); return TK_BANG;     }
"+"     { emit(TK_PLUS,     yytext); return TK_PLUS;     }
"-"     { emit(TK_MINUS,    yytext); return TK_MINUS;    }
"."     { emit(TK_DOT,      yytext); return TK_DOT;      }
":"     { emit(TK_COLON,    yytext); return TK_COLON;    }
","     { emit(TK_COMMA,    yytext); return TK_COMMA;    }
"%"     { emit(TK_PERCENT,  yytext); return TK_PERCENT;  }
"$"     { emit(TK_DOLLAR,   yytext); return TK_DOLLAR;   }

.       { char msg[48];
          snprintf(msg, sizeof(msg), "unexpected character: '%s'", yytext);
          emit(TK_ERROR, msg); return TK_ERROR; }

%%
