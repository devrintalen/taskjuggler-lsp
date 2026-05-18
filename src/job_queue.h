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

#include <stdint.h>
#include <yyjson.h>

/**
 * One pending unit of work parsed off stdin by the reader thread and
 * waiting in a queue for a worker to consume.
 *
 * `request_doc` owns the parsed JSON-RPC envelope.  The worker is
 * responsible for freeing it after dispatch.
 *
 * `is_mutation` is set by the reader when classifying the message;
 * the coordinator pops in arrival order and uses it to choose between
 * synchronous dispatch (mutations) and handing the job off to a query
 * worker (read-only queries).
 *
 * `uri` is a heap-owned copy of the document URI this job targets, set
 * for any per-document mutation (didOpen/didChange/didClose) and for any
 * query whose params carry a textDocument.uri.  NULL for workspace-wide
 * operations (workspace/symbol, shutdown, etc.).  Used both for
 * didChange coalescing and for staleness detection against the
 * mutation_versions counter.
 *
 * `is_coalesceable` is set on textDocument/didChange jobs.
 * job_queue_push collapses two adjacent same-URI didChange jobs into
 * one: the queued job's request_doc is replaced with the newer one and
 * the older parse is skipped.  Coalescing only happens with the queue's
 * tail, so a query or different-URI mutation between two didChanges
 * defeats it — which preserves observable LSP ordering.
 *
 * `snapshot_version` is the mutation_versions value for `uri` captured
 * at enqueue time, used by the query worker to detect queries that have
 * been overtaken by a later same-URI mutation.  Only meaningful for
 * queries flagged is_stale_droppable.
 *
 * `is_stale_droppable` is set on read-only queries whose result can be
 * safely discarded if a later same-URI mutation has overtaken them.
 * Most queries are droppable; semanticTokens/full and its delta variant
 * are NOT, because they maintain per-document baseline state on the
 * server (sem_tokens_data + result_id) that a subsequent delta diffs
 * against — silently dropping the baseline would corrupt the delta chain.
 */
typedef struct Job {
    yyjson_doc *request_doc;
    int         is_mutation;
    char       *uri;
    int         is_coalesceable;
    int         is_stale_droppable;
    int64_t     snapshot_version;
    struct Job *next;
} Job;

/** Opaque thread-safe FIFO of Job pointers. */
typedef struct JobQueue JobQueue;

/**
 * Create an empty job queue.
 *
 * @return Newly allocated queue; release with job_queue_destroy().
 */
JobQueue *job_queue_create(void);

/**
 * Destroy a queue and free its synchronization primitives.  The queue
 * must already be drained and closed.
 */
void      job_queue_destroy(JobQueue *q);

/**
 * Push @p job onto the tail of the queue.  Wakes one waiting worker.
 * Ownership of @p job is transferred to the queue.
 */
void      job_queue_push(JobQueue *q, Job *job);

/**
 * Block until a job is available, then return it.  Returns NULL only
 * once the queue is both closed and empty, signalling worker shutdown.
 * Ownership of the returned job is transferred to the caller.
 */
Job      *job_queue_pop(JobQueue *q);

/**
 * Mark the queue as closed.  Workers blocked in job_queue_pop() are
 * woken; they continue popping pending jobs until the queue is empty,
 * then their next pop returns NULL.
 */
void      job_queue_close(JobQueue *q);

/**
 * Free a Job and any heap fields it owns.  NULL-safe per field, so
 * callers may transfer ownership of an owned field elsewhere by
 * NULL-ing it before calling.  Used by workers after dispatch and by
 * the coalesce branch of job_queue_push when collapsing a redundant
 * job.
 */
void      job_free(Job *job);
