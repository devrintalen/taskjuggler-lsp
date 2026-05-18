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
 * Tracks request ids the client has asked to cancel via $/cancelRequest.
 *
 * The reader thread inserts into the set inline when a cancel notification
 * arrives, racing ahead of the queued request being picked up by a worker.
 * The query worker checks-and-clears at the top of every dispatch; if the
 * id is present, the handler short-circuits with a RequestCancelled error
 * instead of running.
 *
 * Storage is a small fixed array with a mutex.  Cancel traffic is sparse
 * (single-digit set occupancy under realistic load); a more elaborate
 * structure would add complexity without measurable benefit.
 */

#ifndef CANCELLATION_H
#define CANCELLATION_H

#include <stdint.h>

/** Reset the cancellation set.  Call once at server startup. */
void cancellation_init(void);

/** Record that the client cancelled request @p id.  Idempotent.
 *  If the set is full the call is dropped, leaving the request to run
 *  as if uncancelled — same behaviour as before this module existed. */
void cancellation_mark(int64_t id);

/** Atomically test-and-remove.  Returns 1 if @p id was in the set, 0 otherwise.
 *  Worker calls this at the top of dispatch to decide whether to short-circuit. */
int  cancellation_check_and_clear(int64_t id);

/** Remove @p id if present.  Worker calls this at the end of every dispatch
 *  to evict cancels that arrived after the handler started — without this,
 *  late-arriving cancels would linger in the set indefinitely. */
void cancellation_clear(int64_t id);

#endif
