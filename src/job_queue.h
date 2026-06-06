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

#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

/** Forward declaration; the full struct is defined in query_context.h. */
struct query_context;

/**
 * One pending unit of work parsed off stdin by the reader thread and
 * waiting in a queue for a worker to consume.
 *
 * `request_doc` owns the parsed JSON-RPC envelope.  The worker is
 * responsible for freeing it after dispatch.
 *
 * `is_notification` is set by the reader when classifying the message;
 * it determines which queue the Job lands on (the notification path runs
 * on a single worker; the query path feeds a pool).  Only true LSP
 * notifications (no `id`) ride the notification path; LSP requests —
 * including state-mutating lifecycle ones like initialize/shutdown —
 * are queries.
 *
 * `context` is the frozen query_context the coordinator clones (under
 * docs_mutex) before handing a query Job to a worker, so the worker runs
 * its handler lock-free against private memory.  NULL for notifications and
 * for inline-dispatched query methods (initialize / shutdown /
 * semanticTokens); owned by the Job and released by job_free().
 *
 * `id` / `has_id` carry the JSON-RPC request id as a first-class field
 * on the Job so $/cancelRequest can mark queued Jobs.  Notifications
 * and string/null ids leave `has_id = 0`.
 */
typedef struct Job {
    yyjson_doc            *request_doc;     /**< owned JSON-RPC envelope */
    int                    is_notification; /**< 1 for LSP notifications, 0 for requests */
    int                    is_cancelled;    /**< set by $/cancelRequest while queued */
    int                    has_id;          /**< 1 when `id` is meaningful */
    int64_t                id;              /**< JSON-RPC request id; valid only if `has_id` */
    struct query_context  *context;         /**< owned pinned snapshot for query workers; NULL otherwise */
    struct Job            *next;            /**< intrusive next link inside a queue */
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
 *
 * @param q  Queue to destroy.
 */
void      job_queue_destroy(JobQueue *q);

/**
 * Push @p job onto the tail of the queue.  Wakes one waiting worker.
 * Ownership of @p job is transferred to the queue.
 *
 * @param q    Target queue.
 * @param job  Job to enqueue (transfer of ownership).
 */
void      job_queue_push(JobQueue *q, Job *job);

/**
 * Block until a job is available, then return it.  Returns NULL only
 * once the queue is both closed and empty, signalling worker shutdown.
 * Ownership of the returned job is transferred to the caller.
 *
 * @param q  Queue to pop from.
 * @return Owned Job pointer, or NULL when the queue is drained-and-closed.
 */
Job      *job_queue_pop(JobQueue *q);

/**
 * Mark the queue as closed.  Workers blocked in job_queue_pop() are
 * woken; they continue popping pending jobs until the queue is empty,
 * then their next pop returns NULL.
 *
 * @param q  Queue to close.
 */
void      job_queue_close(JobQueue *q);

/**
 * Free a Job and any heap fields it owns.  NULL-safe per field, so
 * callers may transfer ownership of an owned field elsewhere by
 * NULL-ing it before calling.  Used by workers after dispatch.
 *
 * @param job  Job to free.
 */
void      job_free(Job *job);

/**
 * Walk @p q under its mutex, setting is_cancelled=1 on every Job whose
 * has_id is set and whose id equals @p id.  Notification jobs have no
 * id (LSP notifications never carry one) so are skipped implicitly by
 * the has_id check.  Called by the reader when a $/cancelRequest
 * arrives.  The worker checks is_cancelled before dispatching the
 * handler and returns RequestCancelled in its place.
 *
 * @param q   Queue to scan.
 * @param id  Request id to mark cancelled.
 */
void      job_queue_mark_cancelled_by_id(JobQueue *q, int64_t id);
