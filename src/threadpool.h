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
 * Push a parsed textDocument/didChange message onto the work queue,
 * coalescing with a same-URI didChange at the tail when possible.
 * If coalescing happens, @p job is consumed (freed) and the previously
 * queued job's `request_doc` is replaced with this job's; otherwise
 * @p job is enqueued normally.  Ownership of @p job transfers to the
 * queue (or is consumed) in both cases.
 *
 * @param job  Job whose `request_doc` carries the didChange envelope.
 * @param uri  Document URI being edited; copied internally.
 */
void threadpool_enqueue_didchange(Job *job, const char *uri);

/**
 * Push a parsed read-only query message onto the work queue.
 * Ownership of @p job transfers to the queue.
 */
void threadpool_enqueue_query(Job *job);

/**
 * Walk both the reader-facing work queue and the worker-facing pool queue
 * and set is_cancelled=1 on any Job whose id matches @p id.  Called by
 * the reader when a $/cancelRequest arrives.  Deterministic for any Job
 * still resident in either queue at the time of the call; Jobs that have
 * already been popped by a worker proceed uncancelled (LSP clients only
 * cancel in-flight requests sent milliseconds earlier, so the post-pop
 * race window is never realistically hit).
 */
void threadpool_cancel_by_id(int64_t id);
