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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Dynamic string buffer
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *data;
    size_t len, cap;
} StrBuf;

static void strbuf_push(StrBuf *b, const char *bytes, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 32;
        while (nc < b->len + n + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap  = nc;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* Returns ownership of data (caller must free). */
static char *strbuf_finish(StrBuf *b) {
    if (!b->data) {
        char *e = malloc(1);
        *e = '\0';
        return e;
    }
    char *d = b->data;
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    return d;
}

static void strbuf_free(StrBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   UTF-8 helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Decode one UTF-8 codepoint from src at byte *pos (updated on return). */
static uint32_t utf8_decode(const char *src, size_t *pos, size_t len) {
    if (*pos >= len) return 0;
    unsigned char c = (unsigned char)src[*pos];
    if (c < 0x80) { (*pos)++; return c; }
    if ((c & 0xE0) == 0xC0 && *pos + 1 < len) {
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)src[*pos + 1] & 0x3F);
        *pos += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && *pos + 2 < len) {
        uint32_t cp = ((c & 0x0F) << 12)
                    | (((unsigned char)src[*pos + 1] & 0x3F) << 6)
                    | ((unsigned char)src[*pos + 2] & 0x3F);
        *pos += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && *pos + 3 < len) {
        uint32_t cp = ((c & 0x07) << 18)
                    | (((unsigned char)src[*pos + 1] & 0x3F) << 12)
                    | (((unsigned char)src[*pos + 2] & 0x3F) << 6)
                    | ((unsigned char)src[*pos + 3] & 0x3F);
        *pos += 4;
        return cp;
    }
    (*pos)++;
    return (uint32_t)c;
}

/* Byte length of the codepoint starting at src[pos]. */
static int utf8_char_len(const char *src, size_t pos, size_t len) {
    if (pos >= len) return 0;
    unsigned char c = (unsigned char)src[pos];
    if (c < 0x80)        return 1;
    if ((c & 0xE0) == 0xC0 && pos + 1 < len) return 2;
    if ((c & 0xF0) == 0xE0 && pos + 2 < len) return 3;
    if ((c & 0xF8) == 0xF0 && pos + 3 < len) return 4;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Lexer
   ═══════════════════════════════════════════════════════════════════════════ */

void lexer_init(Lexer *l, const char *src) {
    l->src  = src;
    l->pos  = 0;
    l->len  = strlen(src);
    l->line = 0;
    l->col  = 0;
}

static uint32_t lexer_current(const Lexer *l) {
    if (l->pos >= l->len) return 0;
    size_t p = l->pos;
    return utf8_decode(l->src, &p, l->len);
}

/* Peek at the n-th character after the current position (0 = current). */
static uint32_t lexer_peek_at(const Lexer *l, int n) {
    size_t p = l->pos;
    uint32_t cp = 0;
    for (int i = 0; i <= n; i++) {
        if (p >= l->len) return 0;
        cp = utf8_decode(l->src, &p, l->len);
    }
    return cp;
}

/* Advance lexer, optionally push consumed bytes to buf. */
static uint32_t lexer_advance_opt(Lexer *l, StrBuf *buf) {
    if (l->pos >= l->len) return 0;
    int clen = utf8_char_len(l->src, l->pos, l->len);
    size_t p = l->pos;
    uint32_t cp = utf8_decode(l->src, &p, l->len);
    if (buf) strbuf_push(buf, l->src + l->pos, clen);
    l->pos = p;
    if (cp == '\n') { l->line++; l->col = 0; }
    else            { l->col++; }
    return cp;
}

#define lexer_advance(l)       lexer_advance_opt((l), NULL)
#define lexer_advance_push(l, b) lexer_advance_opt((l), (b))

static LspPos lexer_position(const Lexer *l) {
    return (LspPos){ l->line, l->col };
}

static int is_ws(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
        || cp == '\f' || cp == '\v';
}

static int is_id_start(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
        || cp == '_' || cp > 0x7F;
}

static int is_id_cont(uint32_t cp) {
    return is_id_start(cp) || (cp >= '0' && cp <= '9')
        || cp == '.' || cp == ':';
}

static void lexer_skip_whitespace(Lexer *l) {
    while (l->pos < l->len && is_ws(lexer_current(l)))
        lexer_advance(l);
}

void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

Token lexer_next(Lexer *l) {
    lexer_skip_whitespace(l);

    LspPos start = lexer_position(l);
    uint32_t ch  = lexer_current(l);

    if (!ch) {
        return (Token){ TK_EOF, start, start, strdup("") };
    }

    /* Hash / line comment */
    if (ch == '#') {
        StrBuf b = {0};
        while (l->pos < l->len && lexer_current(l) != '\n')
            lexer_advance_push(l, &b);
        return (Token){ TK_LINE_COMMENT, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* // line comment */
    if (ch == '/' && lexer_peek_at(l, 1) == '/') {
        StrBuf b = {0};
        while (l->pos < l->len && lexer_current(l) != '\n')
            lexer_advance_push(l, &b);
        return (Token){ TK_LINE_COMMENT, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* slash-star block comment */
    if (ch == '/' && lexer_peek_at(l, 1) == '*') {
        StrBuf b = {0};
        lexer_advance_push(l, &b); /* / */
        lexer_advance_push(l, &b); /* * */
        for (;;) {
            if (!lexer_current(l)) {
                strbuf_free(&b);
                return (Token){ TK_ERROR, start, lexer_position(l),
                                strdup("unterminated block comment") };
            }
            if (lexer_current(l) == '*' && lexer_peek_at(l, 1) == '/') {
                lexer_advance_push(l, &b); /* * */
                lexer_advance_push(l, &b); /* / */
                break;
            }
            lexer_advance_push(l, &b);
        }
        return (Token){ TK_BLOCK_COMMENT, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* Single-character tokens */
    switch (ch) {
    case '{': lexer_advance(l); return (Token){ TK_LBRACE,   start, lexer_position(l), strdup("{") };
    case '}': lexer_advance(l); return (Token){ TK_RBRACE,   start, lexer_position(l), strdup("}") };
    case '[': lexer_advance(l); return (Token){ TK_LBRACKET, start, lexer_position(l), strdup("[") };
    case ']': lexer_advance(l); return (Token){ TK_RBRACKET, start, lexer_position(l), strdup("]") };
    case '!': lexer_advance(l); return (Token){ TK_BANG,     start, lexer_position(l), strdup("!") };
    case '+': lexer_advance(l); return (Token){ TK_PLUS,     start, lexer_position(l), strdup("+") };
    case ':': lexer_advance(l); return (Token){ TK_COLON,    start, lexer_position(l), strdup(":") };
    case ',': lexer_advance(l); return (Token){ TK_COMMA,    start, lexer_position(l), strdup(",") };
    default:  break;
    }

    /* '-' or scissors -8<- ... ->8- */
    if (ch == '-') {
        if (lexer_peek_at(l, 1) == '8' && lexer_peek_at(l, 2) == '<'
                && lexer_peek_at(l, 3) == '-') {
            StrBuf b = {0};
            lexer_advance_push(l, &b); /* - */
            lexer_advance_push(l, &b); /* 8 */
            lexer_advance_push(l, &b); /* < */
            lexer_advance_push(l, &b); /* - */
            for (;;) {
                if (!lexer_current(l)) {
                    strbuf_free(&b);
                    return (Token){ TK_ERROR, start, lexer_position(l),
                                    strdup("unterminated scissors string") };
                }
                if (lexer_current(l) == '-' && lexer_peek_at(l, 1) == '>'
                        && lexer_peek_at(l, 2) == '8' && lexer_peek_at(l, 3) == '-') {
                    lexer_advance_push(l, &b); /* - */
                    lexer_advance_push(l, &b); /* > */
                    lexer_advance_push(l, &b); /* 8 */
                    lexer_advance_push(l, &b); /* - */
                    break;
                }
                lexer_advance_push(l, &b);
            }
            return (Token){ TK_MULTI_LINE_STR, start, lexer_position(l), strbuf_finish(&b) };
        }
        lexer_advance(l);
        return (Token){ TK_MINUS, start, lexer_position(l), strdup("-") };
    }

    /* String literal */
    if (ch == '"') {
        lexer_advance(l); /* opening " */
        StrBuf b = {0};
        for (;;) {
            uint32_t c = lexer_current(l);
            if (!c || c == '\n') {
                strbuf_free(&b);
                return (Token){ TK_ERROR, start, lexer_position(l),
                                strdup("unterminated string literal") };
            }
            if (c == '"') { lexer_advance(l); break; }
            if (c == '\\') {
                lexer_advance(l); /* backslash */
                uint32_t esc = lexer_current(l);
                if (esc) lexer_advance_push(l, &b);
            } else {
                lexer_advance_push(l, &b);
            }
        }
        return (Token){ TK_STR, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* Number or date: starts with ASCII digit */
    if (ch >= '0' && ch <= '9') {
        StrBuf b = {0};
        while (l->pos < l->len && lexer_current(l) >= '0' && lexer_current(l) <= '9')
            lexer_advance_push(l, &b);

        /* Check for date YYYY-MM-DD */
        if (b.len == 4 && lexer_current(l) == '-') {
            /* peek: -DD-DD */
            size_t p = l->pos + 1;
            if (p + 5 <= l->len
                    && isdigit((unsigned char)l->src[p])
                    && isdigit((unsigned char)l->src[p + 1])
                    && l->src[p + 2] == '-'
                    && isdigit((unsigned char)l->src[p + 3])
                    && isdigit((unsigned char)l->src[p + 4])) {
                for (int i = 0; i < 6; i++) lexer_advance_push(l, &b);
                return (Token){ TK_DATE, start, lexer_position(l), strbuf_finish(&b) };
            }
        }

        /* Float */
        if (lexer_current(l) == '.') {
            lexer_advance_push(l, &b); /* . */
            while (l->pos < l->len && lexer_current(l) >= '0' && lexer_current(l) <= '9')
                lexer_advance_push(l, &b);
            return (Token){ TK_FLOAT, start, lexer_position(l), strbuf_finish(&b) };
        }

        return (Token){ TK_INTEGER, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* Identifier / keyword */
    if (is_id_start(ch)) {
        StrBuf b = {0};
        while (l->pos < l->len && is_id_cont(lexer_current(l)))
            lexer_advance_push(l, &b);
        return (Token){ TK_IDENT, start, lexer_position(l), strbuf_finish(&b) };
    }

    /* Unknown */
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "unexpected character: '%c'", (char)ch);
        lexer_advance(l);
        return (Token){ TK_ERROR, start, lexer_position(l), strdup(msg) };
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Keyword classification
   ═══════════════════════════════════════════════════════════════════════════ */

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

static int symbol_kind_for(const char *kw) {
    if (strcmp(kw, "project")  == 0) return SK_MODULE;
    if (strcmp(kw, "resource") == 0) return SK_OBJECT;
    if (strcmp(kw, "account")  == 0) return SK_VARIABLE;
    if (strcmp(kw, "shift")    == 0) return SK_EVENT;
    return SK_FUNCTION;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Symbol helpers
   ═══════════════════════════════════════════════════════════════════════════ */

void symbol_free(Symbol *s) {
    free(s->name);
    free(s->detail);
    for (int i = 0; i < s->num_children; i++)
        symbol_free(&s->children[i]);
    free(s->children);
}

/* ═══════════════════════════════════════════════════════════════════════════
   ParseResult helpers
   ═══════════════════════════════════════════════════════════════════════════ */

void parse_result_free(ParseResult *r) {
    for (int i = 0; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    free(r->diagnostics);
    for (int i = 0; i < r->num_symbols; i++)
        symbol_free(&r->symbols[i]);
    free(r->symbols);
    memset(r, 0, sizeof(*r));
}

static void push_diagnostic(ParseResult *r, LspRange range, int severity, const char *msg) {
    if (r->num_diagnostics >= r->diag_cap) {
        int nc = r->diag_cap ? r->diag_cap * 2 : 4;
        r->diagnostics = realloc(r->diagnostics, nc * sizeof(Diagnostic));
        r->diag_cap = nc;
    }
    r->diagnostics[r->num_diagnostics++] = (Diagnostic){ range, severity, strdup(msg) };
}

static void push_symbol(ParseResult *r, Symbol s) {
    if (r->num_symbols >= r->sym_cap) {
        int nc = r->sym_cap ? r->sym_cap * 2 : 4;
        r->symbols = realloc(r->symbols, nc * sizeof(Symbol));
        r->sym_cap = nc;
    }
    r->symbols[r->num_symbols++] = s;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Parser state
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    Token *tokens;
    int    n, pos;
} Parser;

static void parser_init(Parser *p, const char *src) {
    int cap = 0;
    p->tokens = NULL;
    p->n = 0;
    p->pos = 0;
    Lexer l;
    lexer_init(&l, src);
    for (;;) {
        if (p->n >= cap) {
            cap = cap ? cap * 2 : 64;
            p->tokens = realloc(p->tokens, cap * sizeof(Token));
        }
        Token t = lexer_next(&l);
        p->tokens[p->n++] = t;
        if (t.kind == TK_EOF) break;
    }
}

static void parser_free(Parser *p) {
    for (int i = 0; i < p->n; i++) token_free(&p->tokens[i]);
    free(p->tokens);
}

static Token *parser_peek(Parser *p) {
    return &p->tokens[p->pos];
}

static Token parser_advance(Parser *p) {
    Token t = p->tokens[p->pos];
    /* Duplicate text since the token owns it */
    t.text = strdup(t.text);
    if (p->pos + 1 < p->n) p->pos++;
    return t;
}

static int parser_at_eof(Parser *p) {
    return parser_peek(p)->kind == TK_EOF;
}

static void parser_skip_trivia(Parser *p) {
    while (parser_peek(p)->kind == TK_LINE_COMMENT
        || parser_peek(p)->kind == TK_BLOCK_COMMENT)
        p->pos++;
}

/* Forward declarations */
static Symbol *parse_items(Parser *p, ParseResult *r, int *out_n);
static Symbol  try_parse_symbol(Parser *p, ParseResult *r, Token kw, int *ok);
static LspPos  skip_args(Parser *p);
static void    skip_item_tail(Parser *p);
static void    skip_block_body(Parser *p);

static Symbol *parse_items(Parser *p, ParseResult *r, int *out_n) {
    Symbol *syms = NULL;
    int n = 0, cap = 0;

    for (;;) {
        parser_skip_trivia(p);
        TokenKind k = parser_peek(p)->kind;

        if (k == TK_EOF || k == TK_RBRACE) break;

        if (k == TK_ERROR) {
            Token t = parser_advance(p);
            LspRange rng = { t.start, t.end };
            push_diagnostic(r, rng, DIAG_ERROR, t.text);
            token_free(&t);
            continue;
        }

        if (k == TK_IDENT) {
            Token kw = parser_advance(p);
            parser_skip_trivia(p);
            if (is_symbol_keyword(kw.text)) {
                int ok = 0;
                Symbol sym = try_parse_symbol(p, r, kw, &ok);
                /* try_parse_symbol always takes ownership of kw and frees it */
                if (ok) {
                    if (n >= cap) {
                        cap = cap ? cap * 2 : 4;
                        syms = realloc(syms, cap * sizeof(Symbol));
                    }
                    syms[n++] = sym;
                }
            } else {
                skip_item_tail(p);
                token_free(&kw);
            }
            continue;
        }

        p->pos++;
    }

    *out_n = n;
    return syms;
}

static Symbol try_parse_symbol(Parser *p, ParseResult *r, Token kw, int *ok) {
    *ok = 0;

    /* Optional id */
    char *id = NULL;
    LspPos sel_start = kw.start, sel_end = kw.end;
    if (parser_peek(p)->kind == TK_IDENT) {
        Token t = parser_advance(p);
        id       = t.text; /* take ownership */
        sel_start = t.start;
        sel_end   = t.end;
        parser_skip_trivia(p);
    } else if (parser_peek(p)->kind != TK_STR) {
        /* Neither ident nor string — skip */
        skip_item_tail(p);
        token_free(&kw);
        return (Symbol){0};
    }

    /* Optional display name (quoted string) */
    LspPos range_end = sel_end;
    char *name = NULL;
    if (parser_peek(p)->kind == TK_STR) {
        Token t = parser_advance(p);
        name      = t.text; /* take ownership */
        range_end = t.end;
        parser_skip_trivia(p);
    } else if (!id) {
        /* No id and no string — nothing useful */
        skip_item_tail(p);
        token_free(&kw);
        return (Symbol){0};
    } else {
        name = strdup(id);
    }

    /* Skip remaining inline args */
    LspPos args_end = skip_args(p);
    if (args_end.line > range_end.line
            || (args_end.line == range_end.line && args_end.character > range_end.character))
        range_end = args_end;

    /* Optional body block */
    Symbol children_holder = {0};
    if (parser_peek(p)->kind == TK_LBRACE) {
        Token open = parser_advance(p);
        int cn = 0;
        Symbol *ch = parse_items(p, r, &cn);
        children_holder.children = ch;
        children_holder.num_children = cn;
        if (parser_peek(p)->kind == TK_RBRACE) {
            Token close = parser_advance(p);
            range_end = close.end;
            token_free(&close);
        } else {
            LspRange rng = { open.start, open.end };
            push_diagnostic(r, rng, DIAG_ERROR, "unclosed `{`");
        }
        token_free(&open);
    }

    Symbol sym = {0};
    sym.name   = name;
    sym.detail = id ? id : strdup(kw.text);
    sym.kind   = symbol_kind_for(kw.text);
    sym.range  = (LspRange){ kw.start, range_end };
    sym.selection_range = (LspRange){ sel_start, sel_end };
    sym.children     = children_holder.children;
    sym.num_children = children_holder.num_children;
    sym.children_cap = children_holder.num_children;

    token_free(&kw);
    *ok = 1;
    return sym;
}

/* Skip inline args; returns end position of last consumed token (or zero). */
static LspPos skip_args(Parser *p) {
    LspPos last = {0};
    for (;;) {
        parser_skip_trivia(p);
        TokenKind k = parser_peek(p)->kind;
        if (k == TK_LBRACE || k == TK_RBRACE || k == TK_EOF) break;
        if (k == TK_IDENT && is_decl_keyword(parser_peek(p)->text)) break;
        Token t = parser_advance(p);
        last = t.end;
        token_free(&t);
    }
    return last;
}

static void skip_item_tail(Parser *p) {
    for (;;) {
        parser_skip_trivia(p);
        TokenKind k = parser_peek(p)->kind;
        if (k == TK_EOF || k == TK_RBRACE) break;
        if (k == TK_LBRACE) {
            p->pos++;
            skip_block_body(p);
            break;
        }
        if (k == TK_IDENT && is_decl_keyword(parser_peek(p)->text)) break;
        p->pos++;
    }
}

static void skip_block_body(Parser *p) {
    int depth = 1;
    while (depth > 0 && !parser_at_eof(p)) {
        TokenKind k = parser_peek(p)->kind;
        if (k == TK_LBRACE) depth++;
        else if (k == TK_RBRACE) depth--;
        p->pos++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Dependency validation (second pass)
   ═══════════════════════════════════════════════════════════════════════════ */

static int resolve_from(const Symbol *syms, int n, const char **segs, int nseg) {
    if (nseg == 0) return 1;
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION && strcmp(syms[i].detail, segs[0]) == 0)
            return resolve_from(syms[i].children, syms[i].num_children, segs + 1, nseg - 1);
    }
    return 0;
}

static const Symbol *find_scope_local(const Symbol *syms, int n,
                                       const char **path, int plen,
                                       int *out_n) {
    if (plen == 0) { *out_n = n; return syms; }
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION && strcmp(syms[i].detail, path[0]) == 0)
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

    /* Split id_text by '.' */
    char buf[512];
    strncpy(buf, id_text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    const char *segs[64];
    int nseg = 0;
    char *tok = strtok(buf, ".");
    while (tok && nseg < 64) { segs[nseg++] = tok; tok = strtok(NULL, "."); }

    return resolve_from(ctx, nctx, segs, nseg);
}

/* Token queue for bang collection */
typedef struct {
    Token *toks;
    int    n, cap;
} TokBuf;

static void tb_push(TokBuf *tb, Token t) {
    if (tb->n >= tb->cap) {
        tb->cap = tb->cap ? tb->cap * 2 : 8;
        tb->toks = realloc(tb->toks, tb->cap * sizeof(Token));
    }
    tb->toks[tb->n++] = t;
}

static void tb_clear(TokBuf *tb) {
    for (int i = 0; i < tb->n; i++) token_free(&tb->toks[i]);
    tb->n = 0;
}

static void tb_free(TokBuf *tb) {
    tb_clear(tb);
    free(tb->toks);
}

static void validate_deps(const char *src, const Symbol *syms, int nsym,
                           ParseResult *r) {
    typedef enum { SS_SCAN, SS_EXPECT_ID, SS_BEFORE_LBRACE } SState;

    Lexer l;
    lexer_init(&l, src);

    SState sstate     = SS_SCAN;
    char  *pending_id = NULL;
    int    brace_depth = 0;
    int    in_deps    = 0;
    int    needs_comma = 0;

    /* task_stack: (id, depth) pairs — using two parallel arrays */
    char     *ts_ids[128];
    uint32_t  ts_dep[128];
    int       ts_n = 0;

    TokBuf bangs = {0};

    for (;;) {
        Token tok = lexer_next(&l);
        if (tok.kind == TK_EOF) { token_free(&tok); break; }
        if (tok.kind == TK_LINE_COMMENT || tok.kind == TK_BLOCK_COMMENT) {
            token_free(&tok);
            continue;
        }

        switch (tok.kind) {
        case TK_LBRACE:
            in_deps     = 0;
            needs_comma = 0;
            tb_clear(&bangs);
            brace_depth++;
            if (sstate == SS_BEFORE_LBRACE && pending_id) {
                if (ts_n < 128) {
                    ts_ids[ts_n] = pending_id;
                    ts_dep[ts_n] = brace_depth;
                    ts_n++;
                    pending_id = NULL;
                }
            }
            free(pending_id);
            pending_id = NULL;
            sstate     = SS_SCAN;
            token_free(&tok);
            break;

        case TK_RBRACE:
            in_deps     = 0;
            needs_comma = 0;
            tb_clear(&bangs);
            /* pop tasks at this depth */
            while (ts_n > 0 && ts_dep[ts_n - 1] >= (uint32_t)brace_depth) {
                free(ts_ids[--ts_n]);
            }
            if (brace_depth > 0) brace_depth--;
            sstate     = SS_SCAN;
            free(pending_id);
            pending_id = NULL;
            token_free(&tok);
            break;

        case TK_BANG:
            if (in_deps) {
                if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                    tb_clear(&bangs);
                } else {
                    tb_push(&bangs, tok);
                    tok.text = NULL; /* ownership transferred */
                    break;
                }
            }
            token_free(&tok);
            break;

        case TK_COMMA:
            if (in_deps) {
                tb_clear(&bangs);
                needs_comma = 0;
            }
            token_free(&tok);
            break;

        case TK_IDENT:
            if (in_deps) {
                if (is_decl_keyword(tok.text) || is_task_attr_keyword(tok.text)) {
                    in_deps     = 0;
                    needs_comma = 0;
                    tb_clear(&bangs);
                    /* fall through to scope tracking */
                } else if (strcmp(tok.text, "depends") == 0
                        || strcmp(tok.text, "precedes") == 0) {
                    tb_clear(&bangs);
                    needs_comma = 0;
                    token_free(&tok);
                    break;
                } else if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                    tb_clear(&bangs);
                    /* fall through to scope tracking */
                } else {
                    /* Validate this dep reference */
                    const char *scope[128];
                    for (int i = 0; i < ts_n; i++) scope[i] = ts_ids[i];
                    int bang_count = bangs.n;

                    if (!validate_ref(syms, nsym, scope, ts_n, bang_count, tok.text)) {
                        if (bang_count > 0 && bang_count > ts_n) {
                            LspPos bstart = bangs.toks[0].start;
                            LspPos bend   = bangs.toks[bangs.n - 1].end;
                            LspRange rng  = { bstart, bend };
                            push_diagnostic(r, rng, DIAG_WARNING,
                                "dependency reference escapes beyond project root");
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "unresolved task: `%s`", tok.text);
                            LspRange rng = { tok.start, tok.end };
                            push_diagnostic(r, rng, DIAG_ERROR, msg);
                        }
                    }
                    tb_clear(&bangs);
                    needs_comma = 1;
                    token_free(&tok);
                    break;
                }
            }

            /* Scope tracking */
            if (strcmp(tok.text, "depends") == 0 || strcmp(tok.text, "precedes") == 0) {
                in_deps     = 1;
                needs_comma = 0;
                tb_clear(&bangs);
                sstate = SS_SCAN;
            } else if (strcmp(tok.text, "task") == 0) {
                sstate = SS_EXPECT_ID;
                free(pending_id);
                pending_id = NULL;
            } else if (sstate == SS_EXPECT_ID) {
                free(pending_id);
                pending_id = strdup(tok.text);
                sstate     = SS_BEFORE_LBRACE;
            }
            token_free(&tok);
            break;

        default:
            if (in_deps) {
                if (needs_comma) {
                    in_deps     = 0;
                    needs_comma = 0;
                    tb_clear(&bangs);
                } else {
                    tb_clear(&bangs);
                }
            }
            if (sstate == SS_EXPECT_ID) {
                sstate = SS_SCAN;
                free(pending_id);
                pending_id = NULL;
            }
            token_free(&tok);
            break;
        }
    }

    free(pending_id);
    for (int i = 0; i < ts_n; i++) free(ts_ids[i]);
    tb_free(&bangs);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public parse() entry point
   ═══════════════════════════════════════════════════════════════════════════ */

ParseResult parse(const char *src) {
    Parser p;
    parser_init(&p, src);

    ParseResult r = {0};

    for (;;) {
        int n = 0;
        Symbol *syms = parse_items(&p, &r, &n);
        for (int i = 0; i < n; i++) push_symbol(&r, syms[i]);
        free(syms); /* outer array only; symbols were copied */

        parser_skip_trivia(&p);
        if (parser_at_eof(&p)) break;

        if (parser_peek(&p)->kind == TK_RBRACE) {
            Token t = parser_advance(&p);
            LspRange rng = { t.start, t.end };
            push_diagnostic(&r, rng, DIAG_ERROR, "unmatched `}`");
            token_free(&t);
        } else {
            p.pos++;
        }
    }

    validate_deps(src, r.symbols, r.num_symbols, &r);

    parser_free(&p);
    return r;
}
