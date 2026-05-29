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

/* Forward declaration; workspace_snapshot.h defines the full type. */
struct workspace_snapshot;

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
 * TODO(workspace-snapshot): The previous design captured a refcounted
 * WorkspaceSnapshot at dispatch time so query workers could run lock-
 * free.  The snapshot model was tied to per-Document ParseResult
 * refcounts and was retired during the tj_node refactor.  Until a
 * replacement snapshotting strategy lands, server_dispatch_query()
 * serialises every handler under docs_mutex.
 *
 * `id` / `has_id` carry the JSON-RPC request id as a first-class field
 * on the Job so $/cancelRequest can mark queued Jobs.  Notifications
 * and string/null ids leave `has_id = 0`.
 */
typedef struct Job {
    yyjson_doc              *request_doc;
    struct workspace_snapshot *snapshot;  /**< pre-computed by coordinator; owned; NULL for notifications */
    int                      is_notification;
    int                      is_lifecycle;  /**< initialize/shutdown — run inline by coordinator */
    int                      is_cancelled;
    int                      has_id;
    int64_t                  id;
    struct Job              *next;
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
 * NULL-ing it before calling.  Used by workers after dispatch.
 */
void      job_free(Job *job);

/**
 * Walk @p q under its mutex, setting is_cancelled=1 on every Job whose
 * has_id is set and whose id equals @p id.  Notification jobs have no
 * id (LSP notifications never carry one) so are skipped implicitly by
 * the has_id check.  Called by the reader when a $/cancelRequest
 * arrives.  The worker checks is_cancelled before dispatching the
 * handler and returns RequestCancelled in its place.
 */
void      job_queue_mark_cancelled_by_id(JobQueue *q, int64_t id);
