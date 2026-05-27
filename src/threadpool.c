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

/* TODO(workspace-snapshot): one query worker for now.  The snapshot +
 * immutable-parse-result machinery that previously let query workers
 * run lock-free was retired during the tj_node refactor; while
 * server_dispatch_query() serialises under docs_mutex, raising the
 * worker count buys nothing.  See job_queue.h for context. */
#define NUM_QUERY_WORKERS 1

/* Single arrival-ordered FIFO populated by the reader.  The
 * coordinator pops from it in order, which is what enforces the LSP
 * "messages processed in arrival order" rule across queries and
 * notifications alike. */
static JobQueue *work_queue;

/* Internal handoff queue: the coordinator pushes query jobs (with
 * their snapshot already attached) onto this for the query worker
 * pool to consume.  Notifications never go here — the coordinator
 * dispatches them inline. */
static JobQueue *request_queue;

static pthread_t coordinator_thread;
static pthread_t query_threads[NUM_QUERY_WORKERS];
static int       pool_started = 0;

/* Coordinator thread.  Pops jobs from work_queue in arrival order and
 * dispatches each inline so the LSP "messages processed in arrival
 * order" rule is preserved.
 *
 * TODO(workspace-snapshot): the previous design split queries onto a
 * separate worker pool and used a refcounted snapshot taken here so
 * query workers could run lock-free.  That machinery was retired
 * during the tj_node refactor.  request_queue and the worker thread
 * are still allocated below so the public threadpool API does not
 * need to churn; once snapshotting returns, restore the split and
 * re-enable query parallelism. */
static void *coordinator(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(work_queue);
        if (!job) break;
        if (job->is_notification) {
            server_dispatch_notification(job);
        } else if (job->is_cancelled) {
            server_dispatch_cancelled(job);
        } else {
            server_dispatch_query(job);
        }
        job_free(job);
    }
    job_queue_close(request_queue);
    return NULL;
}

/* Query worker.  Currently idle — see coordinator() TODO. */
static void *query_worker(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(request_queue);
        if (!job) break;
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
    request_queue = job_queue_create();
    pthread_create(&coordinator_thread, NULL, coordinator, NULL);
    for (int i = 0; i < NUM_QUERY_WORKERS; i++)
        pthread_create(&query_threads[i], NULL, query_worker, NULL);
    pool_started = 1;
}

void threadpool_stop(void) {
    if (!pool_started) return;
    job_queue_close(work_queue);
    pthread_join(coordinator_thread, NULL);
    /* coordinator() closes request_queue once work_queue drains. */
    for (int i = 0; i < NUM_QUERY_WORKERS; i++)
        pthread_join(query_threads[i], NULL);
    job_queue_destroy(work_queue);
    job_queue_destroy(request_queue);
    work_queue       = NULL;
    request_queue = NULL;
    pool_started     = 0;
}

void threadpool_enqueue_job(Job *job) {
    job_queue_push(work_queue, job);
}

void threadpool_cancel_by_id(int64_t id) {
    /* Lock order: work_queue then request_queue.  Matches the
     * coordinator's pop-then-push direction, so no deadlock — the
     * coordinator can hold work_queue.mutex and then call into
     * request_queue.mutex via push, and we acquire the same pair
     * in the same order here. */
    job_queue_mark_cancelled_by_id(work_queue, id);
    job_queue_mark_cancelled_by_id(request_queue, id);
}
