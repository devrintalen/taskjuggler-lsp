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
#include "mutation_versions.h"
#include "server.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* One query worker.  Per-document cache mutexes (Document.cache_lock
 * in server.c) and the per-document sem-tokens result counter are in
 * place for cache-safety once this scales up, but scaling to multiple
 * workers also requires per-document query *ordering* — pthread_mutex_t
 * is not FIFO, so two workers racing for a same-document handler can
 * produce non-deterministic resultIds even when the cache writes are
 * serialized.  Adding a per-document ticketed queue is the next step. */
#define NUM_QUERY_WORKERS 1

/* Single ordered FIFO populated by the reader thread.  Preserving arrival
 * order through one queue is what guarantees LSP semantics: a query
 * enqueued after a mutation always observes that mutation's state, and
 * mutations enqueued after queries always wait for those queries to finish. */
static JobQueue *work_queue;

/* Internal handoff queue: the coordinator pushes read-only query jobs onto
 * this for the query worker pool to consume.  Mutations never go here —
 * the coordinator dispatches them synchronously. */
static JobQueue *query_pool_queue;

static pthread_t coordinator_thread;
static pthread_t query_threads[NUM_QUERY_WORKERS];
static int       pool_started = 0;

/* Number of query jobs currently in flight (handed off to a query worker
 * but not yet completed).  The coordinator waits for this to fall to zero
 * before dispatching a mutation, so mutations are mutually exclusive with
 * any in-flight queries even though no rwlock protects the doc store. */
static _Atomic int in_flight_queries = 0;
static pthread_mutex_t in_flight_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  in_flight_cond  = PTHREAD_COND_INITIALIZER;

static void in_flight_inc(void) {
    atomic_fetch_add(&in_flight_queries, 1);
}

static void in_flight_dec(void) {
    pthread_mutex_lock(&in_flight_mutex);
    if (atomic_fetch_sub(&in_flight_queries, 1) == 1)
        pthread_cond_broadcast(&in_flight_cond);
    pthread_mutex_unlock(&in_flight_mutex);
}

static void wait_for_in_flight_drain(void) {
    if (atomic_load(&in_flight_queries) == 0) return;
    pthread_mutex_lock(&in_flight_mutex);
    while (atomic_load(&in_flight_queries) > 0)
        pthread_cond_wait(&in_flight_cond, &in_flight_mutex);
    pthread_mutex_unlock(&in_flight_mutex);
}

/* Coordinator thread.  Pops jobs from work_queue in arrival order.
 * Mutations dispatch synchronously after waiting for any in-flight queries
 * to drain.  Queries are pushed onto query_pool_queue and run on the
 * worker pool in parallel. */
static void *coordinator(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(work_queue);
        if (!job) break;
        if (job->is_mutation) {
            wait_for_in_flight_drain();
            server_dispatch_mutation(job->request_doc);
            job_free(job);
        } else {
            in_flight_inc();
            job_queue_push(query_pool_queue, job);
        }
    }
    /* Reader is done.  Wait for any in-flight queries before tearing down
     * the worker pool, then close the pool queue so workers can exit. */
    wait_for_in_flight_drain();
    job_queue_close(query_pool_queue);
    return NULL;
}

/* Query handlers in server_dispatch_query read Document.parse without
 * calling parse_result_acquire().  That is safe today because the
 * coordinator blocks every mutation on wait_for_in_flight_drain(), so
 * no mutation can release or swap a ParseResult while a worker holds
 * a pointer to it.  If a future change drops the barrier (e.g. to
 * unblock mutations during long-running queries), handlers must start
 * acquiring/releasing around their access to Document.parse. */
static void *query_worker(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(query_pool_queue);
        if (!job) break;
        /* Stale-query short-circuit: if a same-URI mutation has been
         * enqueued since this query was, the client has typed past the
         * state this query was asked against.  Skip the handler and
         * return a ContentModified error instead.  Only applied to
         * queries flagged droppable — semanticTokens variants opt out
         * because dropping their baseline corrupts the delta chain.
         *
         * Two staleness signals, either one fires:
         *  - is_marked_stale was set by a same-URI mutation enqueue under
         *    the queue mutex; deterministic for queries still in a queue
         *    when the mutation arrived.
         *  - version mismatch catches queries that had already moved past
         *    both queues when the mutation arrived (i.e., this worker
         *    popped them between the mutation's queue walk and its bump).
         */
        int stale = job->is_stale_droppable
                 && job->uri
                 && (job->is_marked_stale
                     || mutation_versions_snapshot(job->uri) != job->snapshot_version);
        if (stale) {
            server_dispatch_stale(job->request_doc);
        } else {
            server_dispatch_query(job->request_doc);
        }
        job_free(job);
        in_flight_dec();
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
    /* coordinator() closed query_pool_queue once in-flight drained */
    for (int i = 0; i < NUM_QUERY_WORKERS; i++)
        pthread_join(query_threads[i], NULL);
    job_queue_destroy(work_queue);
    job_queue_destroy(query_pool_queue);
    work_queue       = NULL;
    query_pool_queue = NULL;
    pool_started     = 0;
}

/* Mark any same-URI droppable queries currently in either work or query-pool
 * queue as stale, atomically with respect to concurrent pops, BEFORE pushing
 * the mutation.  This makes the stale-detection deterministic for queries
 * still residing in a queue at mutation-push time, eliminating the race
 * between the reader's mutation_versions_bump and the worker's snapshot read.
 * Queries that have moved past both queues into a worker's hands are still
 * covered by the version-check fallback in query_worker(). */
static void mark_stale_for_uri_in_all_queues(const char *uri) {
    if (!uri) return;
    /* Lock order: work_queue then query_pool_queue.  Matches the coordinator's
     * pop-then-push direction, so no deadlock. */
    job_queue_mark_stale_for_uri(work_queue, uri);
    job_queue_mark_stale_for_uri(query_pool_queue, uri);
}

void threadpool_enqueue_mutation(Job *job) {
    job->is_mutation = 1;
    mark_stale_for_uri_in_all_queues(job->uri);
    job_queue_push(work_queue, job);
}

void threadpool_enqueue_didchange(Job *job, const char *uri) {
    job->is_mutation     = 1;
    job->is_coalesceable = 1;
    if (!job->uri) job->uri = strdup(uri);
    mark_stale_for_uri_in_all_queues(job->uri);
    job_queue_push(work_queue, job);
}

void threadpool_enqueue_query(Job *job) {
    job->is_mutation = 0;
    job_queue_push(work_queue, job);
}
