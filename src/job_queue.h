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
 */
typedef struct Job {
    yyjson_doc *request_doc;
    int         is_mutation;
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
