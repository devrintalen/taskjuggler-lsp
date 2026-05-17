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
#include <stdatomic.h>
#include <stdlib.h>

/* Single query worker for V1.  The plan calls for four, but the lazy
 * caches on Document (doc_symbols_json, sem_tokens_data) and the
 * monotonic resultId counter in server.c are written by query handlers
 * and are not yet thread-safe.  A later step adds per-document cache
 * mutexes and atomics so the pool can grow to its intended size. */
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
            yyjson_doc_free(job->request_doc);
            free(job);
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

static void *query_worker(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(query_pool_queue);
        if (!job) break;
        server_dispatch_query(job->request_doc);
        yyjson_doc_free(job->request_doc);
        free(job);
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

void threadpool_enqueue_mutation(Job *job) {
    job->is_mutation = 1;
    job_queue_push(work_queue, job);
}

void threadpool_enqueue_query(Job *job) {
    job->is_mutation = 0;
    job_queue_push(work_queue, job);
}
