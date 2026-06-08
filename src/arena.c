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
    if (!b || b->cap - b->used < n)
        b = arena_grow(a, n);

    char *dst = b->data + b->used;
    b->used += n;
    return dst;
}

char *arena_strndup(str_arena *a, const char *s, size_t len) {
    char *dst = arena_alloc(a, len + 1);  /* +1 for the terminating NUL */
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
