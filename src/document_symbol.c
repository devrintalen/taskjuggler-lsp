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

/** @file */

#include "document_symbol.h"
#include "grammar.tab.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* LSP SymbolKind values — only used for JSON serialization. */
#define SK_MODULE   2
#define SK_FUNCTION 12
#define SK_VARIABLE 13
#define SK_OBJECT   19
#define SK_EVENT    24

int symbol_kind_for(int keyword) {
    switch (keyword) {
    case KW_PROJECT:  return SK_MODULE;
    case KW_RESOURCE: return SK_OBJECT;
    case KW_ACCOUNT:  return SK_VARIABLE;
    case KW_SHIFT:    return SK_EVENT;
    default:          return SK_FUNCTION;
    }
}

/* ── tj_node tree navigation ─────────────────────────────────────────────── */

tj_node *tj_node_find_path(const parse_slab *slab,
                            const tj_idx *child_indices, int n,
                            const char **path, int plen) {
    if (plen == 0 || n == 0) return NULL;
    for (int i = 0; i < n; i++) {
        tj_node *node = slab_node(slab, child_indices[i]);
        if (!node) continue;
        const char *node_id = slab_str(slab, node->id_off);
        if (node->keyword == KW_TASK && node_id &&
                strcmp(node_id, path[0]) == 0) {
            if (plen == 1) return node;
            tj_idx *kids = slab_children(slab, node);
            return tj_node_find_path(slab, kids, node->num_children,
                                     path + 1, plen - 1);
        }
        /* Transparently traverse project containers so that task scope paths
         * rooted inside a project body resolve correctly. */
        if (node->keyword == KW_PROJECT) {
            tj_idx *kids = slab_children(slab, node);
            tj_node *found = tj_node_find_path(
                slab, kids, node->num_children, path, plen);
            if (found) return found;
        }
    }
    return NULL;
}

tj_node *tj_node_at(const parse_slab *slab,
                    const TokenSpan *tokens, int num_tokens, LspPos pos) {
    int lo = 0, hi = num_tokens - 1, found = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (tokens[mid].token_kind == TK_EOF) { hi = mid - 1; continue; }
        if (pos_cmp(tokens[mid].start, pos) <= 0) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0) return NULL;
    tj_node *s = slab_node(slab, tokens[found].owner_idx);
    while (s && pos_cmp(pos, s->range.end) >= 0)
        s = slab_node(slab, s->parent_node);
    return s;
}

/** Convenience macro: push a string literal without calling strlen at runtime. */
#define PUSH_LIT(b, s) buf_push((b), (s), sizeof(s) - 1)

yyjson_mut_val *range_json(yyjson_mut_doc *doc, LspRange r) {
    yyjson_mut_val *s  = yyjson_mut_obj(doc);
    yyjson_mut_val *st = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, st, "line",      r.start.line);
    yyjson_mut_obj_add_uint(doc, st, "character", r.start.character);
    yyjson_mut_val *en = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, en, "line",      r.end.line);
    yyjson_mut_obj_add_uint(doc, en, "character", r.end.character);
    yyjson_mut_obj_add_val(doc, s, "start", st);
    yyjson_mut_obj_add_val(doc, s, "end",   en);
    return s;
}

/* ── Flat-buffer JSON serialization ─────────────────────────────────────── */

/** Growable byte buffer used to assemble the document-symbol JSON payload. */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Buf;

static void buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < b->len + n) new_cap *= 2;
        char *tmp = realloc(b->data, new_cap);
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static int write_uint(char *p, uint32_t v) {
    if (v == 0) { *p = '0'; return 1; }
    char tmp[10];
    int len = 0;
    while (v) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
    }
    memcpy(p, tmp, (size_t)len);
    return len;
}

static void buf_push_uint(Buf *b, uint32_t v) {
    if (b->len + 10 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < b->len + 10) new_cap *= 2;
        char *tmp = realloc(b->data, new_cap);
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        b->data = tmp;
        b->cap  = new_cap;
    }
    b->len += (size_t)write_uint(b->data + b->len, v);
}

static void buf_push_json_str(Buf *b, const char *s) {
    buf_push(b, "\"", 1);
    const char *run = s;
    const char *p;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\' || c < 0x20) {
            if (p > run) buf_push(b, run, (size_t)(p - run));
            if (c == '"')       buf_push(b, "\\\"", 2);
            else if (c == '\\') buf_push(b, "\\\\", 2);
            else {
                char tmp[7];
                int n = sprintf(tmp, "\\u%04x", c);
                buf_push(b, tmp, (size_t)n);
            }
            run = p + 1;
        }
    }
    if (p > run) buf_push(b, run, (size_t)(p - run));
    buf_push(b, "\"", 1);
}

static void write_range_buf(Buf *b, LspRange r) {
    PUSH_LIT(b, "{\"start\":{\"line\":");
    buf_push_uint(b, r.start.line);
    PUSH_LIT(b, ",\"character\":");
    buf_push_uint(b, r.start.character);
    PUSH_LIT(b, "},\"end\":{\"line\":");
    buf_push_uint(b, r.end.line);
    PUSH_LIT(b, ",\"character\":");
    buf_push_uint(b, r.end.character);
    PUSH_LIT(b, "}}");
}

static void write_node_buf(Buf *b, const parse_slab *slab, const tj_node *sym) {
    const char *name = slab_str(slab, sym->name_off);
    const char *id   = slab_str(slab, sym->id_off);
    PUSH_LIT(b, "{\"name\":");
    buf_push_json_str(b, name ? name : "");
    PUSH_LIT(b, ",\"detail\":");
    buf_push_json_str(b, id   ? id   : "");
    PUSH_LIT(b, ",\"kind\":");
    buf_push_uint(b, (uint32_t)symbol_kind_for(sym->keyword));
    PUSH_LIT(b, ",\"range\":");
    write_range_buf(b, sym->range);
    PUSH_LIT(b, ",\"selectionRange\":");
    write_range_buf(b, sym->selection_range);
    if (sym->num_children > 0) {
        tj_idx *kids = slab_children(slab, sym);
        PUSH_LIT(b, ",\"children\":[");
        for (int i = 0; i < sym->num_children; i++) {
            if (i > 0) buf_push(b, ",", 1);
            tj_node *child = slab_node(slab, kids[i]);
            if (child) write_node_buf(b, slab, child);
        }
        buf_push(b, "]", 1);
    }
    buf_push(b, "}", 1);
}

char *build_document_symbols_json(const parse_slab *slab, tj_idx root_idx,
                                   size_t *out_len) {
    tj_node *root = slab_node(slab, root_idx);
    Buf b = {0};
    buf_push(&b, "[", 1);
    if (root) {
        tj_idx *kids = slab_children(slab, root);
        int n = root->num_children;
        for (int i = 0; i < n; i++) {
            if (i > 0) buf_push(&b, ",", 1);
            tj_node *child = slab_node(slab, kids[i]);
            if (child) write_node_buf(&b, slab, child);
        }
    }
    buf_push(&b, "]", 1);
    *out_len = b.len;
    buf_push(&b, "\0", 1);
    return b.data;
}
