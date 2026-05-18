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
 * Per-URI monotonic mutation counter used to detect queries that have
 * been superseded by a later same-URI mutation.
 *
 * The reader thread calls mutation_versions_bump() when it enqueues a
 * mutation, and mutation_versions_snapshot() when it enqueues a query.
 * The query worker reads the current version with mutation_versions_snapshot()
 * at pop time and compares against the snapshot stored on the Job; if the
 * version has advanced the query is "stale" (the client has typed since)
 * and the worker returns ContentModified instead of running the handler.
 *
 * Entries are added lazily and never removed (server lifetime).  The
 * fixed capacity bounds memory; a full map silently drops further bumps,
 * which simply degrades to "stale detection unavailable for this URI"
 * — the query still runs and the result is correct, just possibly unwanted.
 */

#ifndef MUTATION_VERSIONS_H
#define MUTATION_VERSIONS_H

#include <stdint.h>

/** Reset all entries.  Call once at server startup. */
void mutation_versions_init(void);

/** Current version for @p uri (0 if @p uri has never been bumped). */
int64_t mutation_versions_snapshot(const char *uri);

/** Atomically increment the version for @p uri and return the new value.
 *  Lazily creates an entry on first call.  Returns 0 if the entry table
 *  is full (silently dropped). */
int64_t mutation_versions_bump(const char *uri);

#endif
