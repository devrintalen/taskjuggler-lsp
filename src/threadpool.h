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
 * Spawn the mutation worker and the query worker pool, and create the
 * queues that feed them.  Must be called exactly once after
 * server_init() and before the reader thread starts enqueuing jobs.
 */
void threadpool_start(void);

/**
 * Close both queues, allow workers to drain remaining jobs, then join
 * every worker thread.  After this returns, no worker is running and
 * the queues have been destroyed.
 */
void threadpool_stop(void);

/**
 * Push a parsed mutation/lifecycle message onto the work queue.
 * Ownership of @p job transfers to the queue.
 */
void threadpool_enqueue_mutation(Job *job);

/**
 * Push a parsed read-only query message onto the work queue.
 * Ownership of @p job transfers to the queue.
 */
void threadpool_enqueue_query(Job *job);

/**
 * Walk both the reader-facing work queue and the worker-facing pool queue
 * and mark any matching read-only Job as cancelled.  Mutation jobs are
 * not cancellable (see job_queue_mark_cancelled_by_id).  Called by the
 * reader when a $/cancelRequest arrives.
 *
 * Best-effort, not exhaustive: the two queues are locked separately with
 * a gap between, so a Job in transit (popped from work_queue by the
 * coordinator and not yet pushed to query_pool_queue) can be missed.
 * Same for Jobs already popped from query_pool_queue by a worker.  Both
 * windows are sub-microsecond and real LSP clients cancel only requests
 * sent milliseconds earlier, so the gaps are not realistically hit.
 */
void threadpool_cancel_by_id(int64_t id);
