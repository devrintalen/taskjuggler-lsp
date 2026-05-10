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

/* Convenience macro: push a string literal without calling strlen at runtime. */
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
    char   *data;  /**< heap-allocated bytes; not NUL-terminated */
    size_t  len;   /**< number of bytes currently in use */
    size_t  cap;   /**< allocated capacity */
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


/**
 * Hand-rolled uint32 formatter; avoids sprintf overhead.
 *
 * @param p  Destination buffer with at least 10 bytes of space.
 * @param v  Value to format.
 * @return Number of bytes written to @p p.
 */
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
    /* 10 digits max for uint32 */
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

/**
 * Write a JSON-escaped string value including surrounding double quotes.
 * Bulk-copies runs of safe characters to avoid per-byte call overhead.
 *
 * @param b  Destination buffer.
 * @param s  NUL-terminated string to encode.
 */
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
    /* flush remaining safe run (p now points at NUL) */
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

static void write_sym_buf(Buf *b, const DocSymbol *sym) {
    PUSH_LIT(b, "{\"name\":");
    buf_push_json_str(b, sym->name   ? sym->name   : "");
    PUSH_LIT(b, ",\"detail\":");
    buf_push_json_str(b, sym->id ? sym->id : "");
    PUSH_LIT(b, ",\"kind\":");
    buf_push_uint(b, (uint32_t)symbol_kind_for(sym->keyword));
    PUSH_LIT(b, ",\"range\":");
    write_range_buf(b, sym->range);
    PUSH_LIT(b, ",\"selectionRange\":");
    write_range_buf(b, sym->selection_range);
    if (sym->num_children > 0) {
        PUSH_LIT(b, ",\"children\":[");
        for (int i = 0; i < sym->num_children; i++) {
            if (i > 0) buf_push(b, ",", 1);
            write_sym_buf(b, sym->children[i]);
        }
        buf_push(b, "]", 1);
    }
    buf_push(b, "}", 1);
}

char *build_document_symbols_json(DocSymbol *const *syms, int n, size_t *out_len) {
    Buf b = {0};
    buf_push(&b, "[", 1);
    for (int i = 0; i < n; i++) {
        if (i > 0) buf_push(&b, ",", 1);
        write_sym_buf(&b, syms[i]);
    }
    buf_push(&b, "]", 1);
    *out_len = b.len;
    buf_push(&b, "\0", 1);
    return b.data;
}
