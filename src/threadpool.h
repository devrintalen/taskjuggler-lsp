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

#pragma once

#include "job_queue.h"

/**
 * Spawn the coordinator (which dispatches notifications inline) and the
 * query worker pool, and create the queues that feed them.  Must be
 * called exactly once after server_init() and before the reader thread
 * starts enqueuing jobs.
 */
void threadpool_start(void);

/**
 * Close both queues, allow workers to drain remaining jobs, then join
 * every worker thread.  After this returns, no worker is running and
 * the queues have been destroyed.
 */
void threadpool_stop(void);

/**
 * Push a parsed LSP notification (no `id`) onto the work queue.
 * Ownership of @p job transfers to the queue.  Notifications run
 * inline on the coordinator so arrival order is preserved and any
 * state mutation is visible to subsequent queries.
 */
void threadpool_enqueue_notification(Job *job);

/**
 * Push a parsed read-only query message onto the query queue.  Multiple
 * query workers consume the queue concurrently; per-document
 * cache_lock and the immutable-after-publication ParseResult contract
 * keep that safe.  Ownership of @p job transfers to the queue.
 */
void threadpool_enqueue_query(Job *job);

/**
 * Walk both queues under their mutexes and mark any matching Job as
 * cancelled.  Notification jobs carry no id and are therefore skipped
 * by the id-equality check.  Called by the reader when a
 * $/cancelRequest arrives.
 *
 * Best-effort, not exhaustive: a Job already popped by a worker can be
 * missed.  The window is sub-microsecond and real LSP clients cancel
 * only requests sent milliseconds earlier, so the gap is not
 * realistically hit.
 */
void threadpool_cancel_by_id(int64_t id);
