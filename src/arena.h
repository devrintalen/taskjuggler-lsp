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

/** @file
 *
 * Append-only string arena.
 *
 * A parse of a large document captures hundreds of thousands of token
 * lexemes.  Giving each its own strdup() turns into one malloc() per token
 * at parse time and one free() per token when the snapshot is retired —
 * pure allocator overhead that dominates both the parse and the cleanup.
 *
 * This arena copies those lexemes into a small linked list of large blocks
 * via a bump pointer, so capturing a lexeme is an offset bump (no per-string
 * malloc) and discarding the whole batch frees a handful of blocks instead
 * of one chunk per string.  Blocks are never reallocated once allocated, so
 * every pointer the arena hands back stays valid until arena_free().
 *
 * The arena is single-threaded: it is filled during a parse and thereafter
 * read-only (the immutable doc_snapshot that owns it shares it by reference).
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/** Opaque append-only string arena.  @see arena.c. */
typedef struct str_arena str_arena;

/** Allocate an empty arena.  Aborts the process on allocation failure.
 *  @return A new arena; release it with arena_free(). */
str_arena *arena_new(void);

/** Carve @p n uninitialized bytes off the arena's bump pointer.  The returned
 *  region is valid until arena_free() and is byte-aligned (callers store
 *  strings, which need no stricter alignment).  Aborts on allocation failure.
 *  @param a  Arena to allocate from (must be non-NULL).
 *  @param n  Number of bytes to reserve.
 *  @return Pointer to @p n uninitialized arena-owned bytes. */
void *arena_alloc(str_arena *a, size_t n);

/** Copy @p len bytes of @p s into the arena and append a terminating NUL,
 *  returning a pointer to the arena-owned copy.  The returned string is
 *  valid until arena_free().  Aborts on allocation failure.
 *  @param a    Arena to copy into (must be non-NULL).
 *  @param s    Source bytes (need not be NUL-terminated within @p len).
 *  @param len  Number of bytes to copy.
 *  @return Pointer to the NUL-terminated arena copy. */
char *arena_strndup(str_arena *a, const char *s, size_t len);

/** Free every block held by @p a and the arena itself.  Safe on NULL.
 *  All pointers previously returned by arena_strndup() become invalid.
 *  @param a  Arena to release, or NULL. */
void arena_free(str_arena *a);

#endif /* ARENA_H */
