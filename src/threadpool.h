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
 * Hand @p job to the threadpool.  The reader thread classifies the
 * incoming message and calls this for every Job — notifications and
 * requests alike all go through the single arrival-ordered queue,
 * which the coordinator pops in order to enforce the LSP "process in
 * arrival order" rule.
 *
 * @param job  Owned Job (transfer of ownership to the queue).
 */
void threadpool_enqueue_job(Job *job);

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
 *
 * @param id  JSON-RPC request id to cancel.
 */
void threadpool_cancel_by_id(int64_t id);
