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

/*
 * tools/grammar_test.c - Standalone grammar/parser test driver
 *
 * Feeds a TJP source file through the full lex + bison pipeline and prints
 * the resulting symbol tree and diagnostics to stdout.  Useful for verifying
 * that the grammar correctly recognises declarations and builds the symbol
 * tree before hooking up the full LSP server.
 *
 * Usage:
 *   ./grammar-test test/tutorial.tjp
 *   ./grammar-test < test/tutorial.tjp
 *
 * Exit code:
 *   0  — parse succeeded with no ERROR-severity diagnostics
 *   1  — one or more ERROR diagnostics were emitted
 *
 * Build:  make grammar-test
 */

#include "../src/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── kind_name ───────────────────────────────────────────────────────────── */

static const char *kind_name(int k) {
    switch (k) {
    case SK_MODULE:   return "project";
    case SK_FUNCTION: return "task";
    case SK_VARIABLE: return "account";
    case SK_OBJECT:   return "resource";
    case SK_EVENT:    return "shift";
    default:          return "symbol";
    }
}

/* ── print_doc_symbol ────────────────────────────────────────────────────────── */

static void print_doc_symbol(const DocSymbol *s, int depth) {
    /* Indentation: 2 spaces per level */
    for (int i = 0; i < depth * 2; i++) putchar(' ');

    /* [kind] "name" (detail)  line:col-line:col */
    printf("[%-8s] \"%s\"", kind_name(s->kind),
           s->name   ? s->name   : "(null)");
    if (s->detail && s->detail[0])
        printf("  (%s)", s->detail);
    printf("  %u:%u-%u:%u\n",
           s->range.start.line, s->range.start.character,
           s->range.end.line,   s->range.end.character);

    for (int i = 0; i < s->num_children; i++)
        print_doc_symbol(&s->children[i], depth + 1);
}

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

    ParseResult r = parse(src);
    free(src);

    /* ── DocSymbol tree ── */
    if (r.num_doc_symbols == 0) {
        puts("(no symbols)");
    } else {
        for (int i = 0; i < r.num_doc_symbols; i++)
            print_doc_symbol(&r.doc_symbols[i], 0);
    }

    /* ── Diagnostics ── */
    int num_errors = 0;
    if (r.num_diagnostics > 0) {
        putchar('\n');
        for (int i = 0; i < r.num_diagnostics; i++) {
            const Diagnostic *d = &r.diagnostics[i];
            const char *sev = (d->severity == DIAG_ERROR) ? "error" : "warning";
            if (d->severity == DIAG_ERROR) num_errors++;
            printf("%s %u:%u-%u:%u  %s\n",
                   sev,
                   d->range.start.line, d->range.start.character,
                   d->range.end.line,   d->range.end.character,
                   d->message ? d->message : "");
        }
    }

    /* ── Summary ── */
    putchar('\n');
    printf("%d symbol%s, %d diagnostic%s (%d error%s)\n",
           r.num_doc_symbols,    r.num_doc_symbols    == 1 ? "" : "s",
           r.num_diagnostics, r.num_diagnostics == 1 ? "" : "s",
           num_errors,        num_errors        == 1 ? "" : "s");

    parse_result_free(&r);
    return num_errors ? 1 : 0;
}
