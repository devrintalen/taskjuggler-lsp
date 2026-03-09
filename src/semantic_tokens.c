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

#include "semantic_tokens.h"
#include "parser.h"

#include <stdlib.h>
#include <string.h>

/* Token type index 0 = "string" (only type in the legend). */
#define TOKEN_TYPE_STRING 0

cJSON *build_semantic_tokens_json(const char *src) {
    Lexer l;
    lexer_init(&l, src);

    /* Collect (line, start_char, length) spans. */
    typedef struct { uint32_t line, col, len; } Span;
    Span  *spans = NULL;
    int    ns = 0, caps = 0;

    for (;;) {
        Token tok = lexer_next(&l);
        if (tok.kind == TK_EOF) { token_free(&tok); break; }
        if (tok.kind != TK_MULTI_LINE_STR) { token_free(&tok); continue; }

        /* Split token text on newlines, emit one span per line. */
        uint32_t line = tok.start.line;
        uint32_t col  = tok.start.character;

        const char *p = tok.text;
        while (*p) {
            /* Find end of this line segment. */
            const char *q = p;
            while (*q && *q != '\n') q++;

            /* Length in characters, trim \r */
            const char *end = q;
            if (end > p && *(end - 1) == '\r') end--;

            uint32_t len = (uint32_t)(end - p);

            if (len > 0) {
                if (ns >= caps) {
                    caps = caps ? caps * 2 : 16;
                    spans = realloc(spans, caps * sizeof(Span));
                }
                spans[ns++] = (Span){ line, col, len };
            }

            if (*q == '\n') { q++; line++; col = 0; }
            else break;
            p = q;
        }

        token_free(&tok);
    }

    /* Encode as delta-relative SemanticToken entries: [dl, ds, len, type, mod] */
    cJSON *data = cJSON_CreateArray();
    uint32_t prev_line = 0, prev_start = 0;
    for (int i = 0; i < ns; i++) {
        uint32_t dl = spans[i].line - prev_line;
        uint32_t ds = (dl == 0) ? (spans[i].col - prev_start) : spans[i].col;
        cJSON_AddItemToArray(data, cJSON_CreateNumber(dl));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(ds));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(spans[i].len));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(TOKEN_TYPE_STRING));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(0)); /* no modifiers */
        prev_line  = spans[i].line;
        prev_start = spans[i].col;
    }

    free(spans);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNullToObject(root, "resultId");
    cJSON_AddItemToObject(root, "data", data);
    return root;
}
