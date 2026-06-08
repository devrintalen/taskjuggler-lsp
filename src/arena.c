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

#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Bytes of payload in a freshly allocated block.  Chosen so a large parse
 *  fills only a few dozen blocks (a typical lexeme is a handful of bytes),
 *  trading a little slack for far fewer malloc/free calls. */
#define ARENA_BLOCK_SIZE (64 * 1024)

/** One contiguous allocation the bump pointer carves strings out of.  Blocks
 *  are singly linked newest-first and never moved or resized once created. */
typedef struct arena_block {
    struct arena_block *next;  /**< previously allocated block, or NULL */
    size_t              used;  /**< bytes of @c data already handed out */
    size_t              cap;   /**< total usable bytes in @c data */
    char                data[];/**< flexible payload */
} arena_block;

struct str_arena {
    arena_block *head;  /**< current (newest) block strings are carved from */
};

str_arena *arena_new(void) {
    str_arena *a = calloc(1, sizeof(*a));
    if (!a) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    return a;
}

/** Allocate a fresh block with at least @p need usable bytes and push it to
 *  the front of @p a's block list. */
static arena_block *arena_grow(str_arena *a, size_t need) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (need > cap) cap = need;
    arena_block *b = malloc(sizeof(*b) + cap);
    if (!b) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    b->next = a->head;
    b->used = 0;
    b->cap  = cap;
    a->head = b;
    return b;
}

void *arena_alloc(str_arena *a, size_t n) {
    arena_block *b = a->head;
    /* Round the bump pointer up to 8 bytes so structs and arrays (e.g. a
     * ProjectNode or a ProjectDep[] with an _Atomic field) come back suitably
     * aligned even when packed strings from arena_strndup() left it odd.  A
     * fresh block's data is already 8-aligned (the header is a multiple of 8). */
    size_t off = b ? (b->used + 7u) & ~(size_t)7u : 0;
    if (!b || off + n > b->cap) {
        b = arena_grow(a, n);
        off = 0;
    }
    char *dst = b->data + off;
    b->used = off + n;
    return dst;
}

char *arena_strndup(str_arena *a, const char *s, size_t len) {
    /* Strings need no alignment, so bump them in tight rather than through
     * arena_alloc()'s 8-byte rounding (a parse interns hundreds of thousands
     * of short lexemes — padding each would waste megabytes). */
    size_t need = len + 1;  /* room for the terminating NUL */
    arena_block *b = a->head;
    if (!b || b->cap - b->used < need)
        b = arena_grow(a, need);

    char *dst = b->data + b->used;
    b->used += need;
    memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

void arena_free(str_arena *a) {
    if (!a) return;
    arena_block *b = a->head;
    while (b) {
        arena_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}
