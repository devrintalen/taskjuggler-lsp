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

#include "threadpool.h"
#include "server.h"

#include <pthread.h>

/* One query worker for now.  The snapshot + immutable-ParseResult
 * machinery is already in place to scale this up, but
 * pthread_mutex_t is not FIFO — multiple workers racing for the
 * docs_mutex during a sem_tokens cache write produce non-deterministic
 * resultIds.  The fix is a per-document ticket / queue (called out in
 * the TODO at the top of parser.c, alongside the doc-symbol-tree
 * follow-ups).  Until that lands, keep parallelism at 1. */
#define NUM_QUERY_WORKERS 1

/* Single arrival-ordered FIFO populated by the reader.  The
 * coordinator pops from it in order, which is what enforces the LSP
 * "messages processed in arrival order" rule across queries and
 * mutations alike. */
static JobQueue *work_queue;

/* Internal handoff queue: the coordinator pushes read-only query jobs
 * (with their snapshot already attached) onto this for the query
 * worker pool to consume.  Mutations never go here — the coordinator
 * dispatches them inline. */
static JobQueue *query_pool_queue;

static pthread_t coordinator_thread;
static pthread_t query_threads[NUM_QUERY_WORKERS];
static int       pool_started = 0;

/* Coordinator thread.  Pops jobs from work_queue in arrival order.
 *
 * Mutation jobs: dispatched inline.  server_dispatch_mutation takes
 * docs_mutex, applies the mutation, releases.  By running inline (not
 * on a worker), the coordinator blocks while the mutation completes,
 * so any subsequent query the reader pushes will observe the mutation.
 *
 * Query jobs: the coordinator takes a workspace snapshot HERE (before
 * handing off), so the snapshot reflects the state at the moment this
 * job's turn in arrival order would have come up.  This is what
 * guarantees correctness when later mutations race ahead while query
 * workers are still computing — the workers run on their own pinned
 * snapshot, not on live docs[]. */
static void *coordinator(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(work_queue);
        if (!job) break;
        if (job->is_mutation) {
            server_dispatch_mutation(job);
            job_free(job);
        } else {
            server_capture_snapshot_for(job);
            job_queue_push(query_pool_queue, job);
        }
    }
    job_queue_close(query_pool_queue);
    return NULL;
}

/* Query worker.  Pops a Job (with snapshot already attached), runs the
 * handler against the snapshot, and emits the response.  No coordination
 * with mutations is needed at this point — the snapshot is independent. */
static void *query_worker(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(query_pool_queue);
        if (!job) break;
        /* Cancellation short-circuit: the reader walked the queues for
         * $/cancelRequest and marked this Job before it was popped.
         * Skip the handler and send the RequestCancelled error. */
        if (job->is_cancelled) {
            server_dispatch_cancelled(job);
        } else {
            server_dispatch_query(job);
        }
        job_free(job);
    }
    return NULL;
}

void threadpool_start(void) {
    if (pool_started) return;
    work_queue       = job_queue_create();
    query_pool_queue = job_queue_create();
    pthread_create(&coordinator_thread, NULL, coordinator, NULL);
    for (int i = 0; i < NUM_QUERY_WORKERS; i++)
        pthread_create(&query_threads[i], NULL, query_worker, NULL);
    pool_started = 1;
}

void threadpool_stop(void) {
    if (!pool_started) return;
    job_queue_close(work_queue);
    pthread_join(coordinator_thread, NULL);
    /* coordinator() closes query_pool_queue once work_queue drains. */
    for (int i = 0; i < NUM_QUERY_WORKERS; i++)
        pthread_join(query_threads[i], NULL);
    job_queue_destroy(work_queue);
    job_queue_destroy(query_pool_queue);
    work_queue       = NULL;
    query_pool_queue = NULL;
    pool_started     = 0;
}

void threadpool_enqueue_mutation(Job *job) {
    job->is_mutation = 1;
    job_queue_push(work_queue, job);
}

void threadpool_enqueue_query(Job *job) {
    job->is_mutation = 0;
    job_queue_push(work_queue, job);
}

void threadpool_cancel_by_id(int64_t id) {
    /* Lock order: work_queue then query_pool_queue.  Matches the
     * coordinator's pop-then-push direction, so no deadlock — the
     * coordinator can hold work_queue.mutex and then call into
     * query_pool_queue.mutex via push, and we acquire the same pair
     * in the same order here. */
    job_queue_mark_cancelled_by_id(work_queue, id);
    job_queue_mark_cancelled_by_id(query_pool_queue, id);
}
