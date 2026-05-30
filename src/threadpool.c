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

/* Number of parallel query workers.  Queries take a workspace_snapshot
 * under docs_mutex, then run lock-free, so multiple workers produce
 * real parallelism on multi-core systems. */
#define NUM_QUERY_WORKERS 4

/* Single arrival-ordered FIFO populated by the reader.  The
 * coordinator pops from it in order, which is what enforces the LSP
 * "messages processed in arrival order" rule: notifications always
 * execute before any later query is handed to a worker. */
static JobQueue *work_queue;

/* Internal handoff queue: the coordinator pushes query jobs onto this
 * for the worker pool to consume.  Notifications never go here —
 * the coordinator dispatches them inline before picking up the next job. */
static JobQueue *request_queue;

static pthread_t coordinator_thread;
static pthread_t query_threads[NUM_QUERY_WORKERS];
static int       pool_started = 0;

/* ── Thread-safety contract ──────────────────────────────────────────────────
 *
 * Three thread classes:
 *
 *   reader      (main.c) — single thread; calls server_process() which
 *               classifies each message and enqueues a Job onto work_queue.
 *               No document state is read or written here.
 *
 *   coordinator (this file, one thread) — pops work_queue in arrival order.
 *               Notifications and lifecycle requests (initialize, shutdown,
 *               semanticTokens/full[/delta]) run inline under docs_mutex so
 *               later messages observe their side effects.  For regular query
 *               jobs it calls server_snapshot_for_job() — a brief docs_mutex
 *               hold to copy slab pages and deep-copy the project tree —
 *               then pushes the enriched job to request_queue.
 *
 *   workers     (this file, NUM_QUERY_WORKERS threads) — pop request_queue
 *               and call server_dispatch_query().  Workers are LOCK-FREE:
 *               they read only the private workspace_snapshot attached to
 *               their job and must NEVER touch:
 *
 *                 • docs[], projects[], num_projects — use only the snapshot
 *                 • g_workspace_root, g_cc_* globals
 *                 • global flex/bison state (g_build_root, yylval, yytext,
 *                   yylineno, yycolumn) — parse() must not be called here
 *                 • non-reentrant libc functions (strtok, localtime, gmtime,
 *                   strerror, rand, …) — use _r variants where applicable
 *
 *               Workers may safely use: their job's workspace_snapshot,
 *               immutable static-const lookup tables in feature files,
 *               and stack-local state.  stdout writes go via lsp_send_message()
 *               which serializes under stdout_mutex.
 *
 * Lock order (when multiple locks must be held simultaneously):
 *   work_queue.mutex → request_queue.mutex  (coordinator push direction)
 *   docs_mutex is never held while work_queue or request_queue is locked.
 * ─────────────────────────────────────────────────────────────────────────── */

/* Coordinator thread.  Pops jobs from work_queue in arrival order.
 * Notifications are dispatched inline (preserving causal ordering with
 * respect to subsequent queries).  Query and cancelled-query jobs are
 * pushed to request_queue for the worker pool; workers own and free them. */
static void *coordinator(void *arg) {
    (void)arg;
    while (1) {
        Job *job = job_queue_pop(work_queue);
        if (!job) break;
        if (job->is_notification || job->is_lifecycle) {
            /* Notifications and lifecycle requests (initialize, shutdown) run
             * inline so subsequent messages observe their state changes before
             * the coordinator picks up the next job. */
            if (job->is_notification)
                server_dispatch_notification(job);
            else
                server_dispatch_query(job);
            job_free(job);
        } else if (job->is_cancelled) {
            server_dispatch_cancelled(job);
            job_free(job);
        } else {
            /* Pre-compute the snapshot at coordinator time so the query
             * worker sees document state consistent with when this request
             * arrived, before any subsequent notification modifies docs[]. */
            job->snapshot = server_snapshot_for_job(job);
            job_queue_push(request_queue, job);
        }
    }
    job_queue_close(request_queue);
    return NULL;
}

/* Query worker.  Pops from request_queue and runs each job lock-free
 * (server_dispatch_query takes a snapshot at the start, then releases
 * docs_mutex before doing any handler work). */
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
