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

#include "parser.h"

#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

/**
 * A pinned read-only view of a single document captured at a point in
 * time.  Each entry holds owned copies of the URI and text plus a
 * refcount on the ParseResult, so the snapshot is valid for the entire
 * lifetime of the owning Job — independent of subsequent notifications.
 *
 * `version` records the Document.doc_version at capture time.  Query
 * workers compare it against the current version before sending their
 * response; a mismatch means the document changed mid-query, and the
 * worker replies with LSP ContentModified (-32801).
 */
typedef struct DocSnapshot {
    char        *uri;     /**< owned copy of the document URI */
    char        *text;    /**< owned copy of the document text (may be NULL) */
    ParseResult *parse;   /**< refcount held; released on job free */
    uint64_t     version; /**< Document.doc_version at capture time */
    int          disk_only; /**< 1 = background workspace entry, 0 = editor-managed */
} DocSnapshot;

/**
 * A pinned view of every open / background document in the workspace.
 * Captured once per Job at the start of worker execution; the rest of
 * the handler runs without touching the live document store.
 *
 * `primary_uri` is the URI of the request's target document (when the
 * method has one) — handlers do a linear scan of `docs[]` to find the
 * matching DocSnapshot.  NULL for methods with no target (workspace
 * methods, initialize, etc.).
 */
typedef struct WorkspaceSnapshot {
    DocSnapshot *docs;          /**< pinned views of every active slot */
    size_t       count;         /**< number of entries in docs */
    char        *primary_uri;   /**< owned; URI of the request target, or NULL */
} WorkspaceSnapshot;

/**
 * Release every dynamic resource held by @p snap, including the
 * per-document ParseResult refcounts.  Zeroes the struct on return.
 */
void workspace_snapshot_release(WorkspaceSnapshot *snap);

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
 * `snapshot` is filled by the worker just before running the handler.
 * The reader leaves it zeroed — taking the snapshot at the moment of
 * execution (rather than at enqueue time) keeps it fresh even when a
 * job sits in the queue for a long time.
 *
 * `id` / `has_id` carry the JSON-RPC request id as a first-class field
 * on the Job.  The first per-job operation is $/cancelRequest: the
 * reader walks both queues looking for a matching id and sets
 * `is_cancelled`, so cancellation is deterministic for any Job still
 * resident in a queue when the cancel arrives.  Notifications and
 * string/null ids leave `has_id = 0` and are therefore not cancellable
 * by id (matching the existing int-only policy in server.c).
 */
typedef struct Job {
    yyjson_doc        *request_doc;
    WorkspaceSnapshot  snapshot;
    int                is_notification;
    int                is_cancelled;
    int                has_id;
    int64_t            id;
    struct Job        *next;
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
