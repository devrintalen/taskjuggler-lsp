/*
 * tools/lexer_test.c - Standalone lexer test driver
 *
 * Feeds a TJP source file to the flex-generated lexer and prints every token
 * with its position and text to stdout.  Useful for verifying lexer output
 * before hooking up the full parser.
 *
 * Usage:
 *   ./lexer-test test/tutorial.tjp
 *   ./lexer-test < test/tutorial.tjp
 *
 * Build:  make lexer-test
 */

#include "../src/parser.h"
#include "../src/grammar.tab.h"  /* token codes + YYSTYPE definition */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── token_kind_name ─────────────────────────────────────────────────────── */

static const char *token_kind_name(int k) {
    switch (k) {
    case TK_IDENT:          return "IDENT";
    case TK_STR:            return "STR";
    case TK_INTEGER:        return "INTEGER";
    case TK_FLOAT:          return "FLOAT";
    case TK_DATE:           return "DATE";
    case TK_LBRACE:         return "LBRACE";
    case TK_RBRACE:         return "RBRACE";
    case TK_LBRACKET:       return "LBRACKET";
    case TK_RBRACKET:       return "RBRACKET";
    case TK_BANG:           return "BANG";
    case TK_PLUS:           return "PLUS";
    case TK_MINUS:          return "MINUS";
    case TK_DOT:            return "DOT";
    case TK_COLON:          return "COLON";
    case TK_COMMA:          return "COMMA";
    case TK_PERCENT:        return "PERCENT";
    case TK_DOLLAR:         return "DOLLAR";
    case TK_LINE_COMMENT:   return "LINE_COMMENT";
    case TK_BLOCK_COMMENT:  return "BLOCK_COMMENT";
    case TK_MULTI_LINE_STR: return "MULTI_LINE_STR";
    case TK_EOF:            return "EOF";
    case TK_ERROR:          return "ERROR";
    default:                return "?";
    }
}

/* ── Stubs required by lexer.yy.c ───────────────────────────────────────── */

/* g_push_token: normally accumulates tokens into a ParseResult; here we just
 * print each token as it arrives. */
void g_push_token(int kind, const char *text,
                  uint32_t sl, uint32_t sc,
                  uint32_t el, uint32_t ec) {
    /* Collapse newlines in text so each token fits on one line. */
    char display[48];
    int j = 0;
    for (int i = 0; text[i] && j < (int)sizeof(display) - 4; i++) {
        if (text[i] == '\n') {
            display[j++] = '\\';
            display[j++] = 'n';
        } else {
            display[j++] = text[i];
        }
    }
    if (text[j] != '\0') { display[j++] = '.'; display[j++] = '.'; display[j++] = '.'; }
    display[j] = '\0';

    printf("%3u:%-3u  %3u:%-3u  %-18s  %s\n",
           sl, sc, el, ec, token_kind_name(kind), display);
}

/* yylval: normally defined by bison's grammar.tab.c; provide it here so the
 * lexer can populate it even though we never call yyparse(). */
YYSTYPE yylval;

/* ── flex interface ──────────────────────────────────────────────────────── */

typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
extern int             yylex(void);

/* ── File reading ────────────────────────────────────────────────────────── */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { perror("ftell"); fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fputs("out of memory\n", stderr); fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        perror("fread"); free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static char *read_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fputs("out of memory\n", stderr); return NULL; }
    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { fputs("out of memory\n", stderr); return NULL; }
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    char *src;

    if (argc >= 2) {
        src = read_file(argv[1]);
    } else {
        src = read_stdin();
    }
    if (!src) return 1;

    printf("  line:col   line:col   %-18s  text\n", "kind");
    printf("  ----:---   ----:---   %-18s  ----\n", "----");

    YY_BUFFER_STATE buf = yy_scan_string(src);

    /* Drive the lexer: g_push_token() prints each token as a side effect. */
    while (yylex() != 0)
        ;

    yy_delete_buffer(buf);
    free(src);
    return 0;
}
