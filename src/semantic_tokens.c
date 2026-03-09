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

/* Token type index 0 = "string" (only type in the legend). */
#define TOKEN_TYPE_STRING 0

cJSON *build_semantic_tokens_json(const ParseResult *r) {
    /* Encode stored spans as delta-relative SemanticToken entries:
     * [deltaLine, deltaStart, length, tokenType, tokenModifiers] */
    cJSON *data = cJSON_CreateArray();
    uint32_t prev_line = 0, prev_start = 0;
    for (int i = 0; i < r->num_sem_spans; i++) {
        const SemanticSpan *s = &r->sem_spans[i];
        uint32_t dl = s->line - prev_line;
        uint32_t ds = (dl == 0) ? (s->col - prev_start) : s->col;
        cJSON_AddItemToArray(data, cJSON_CreateNumber(dl));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(ds));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(s->len));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(TOKEN_TYPE_STRING));
        cJSON_AddItemToArray(data, cJSON_CreateNumber(0)); /* no modifiers */
        prev_line  = s->line;
        prev_start = s->col;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNullToObject(root, "resultId");
    cJSON_AddItemToObject(root, "data", data);
    return root;
}
