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

#include "job_queue.h"
#include "query_context.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct JobQueue {
    Job             *head;
    Job             *tail;
    int              closed;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
};

JobQueue *job_queue_create(void) {
    JobQueue *q = calloc(1, sizeof(JobQueue));
    if (!q) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}

void job_queue_destroy(JobQueue *q) {
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
    free(q);
}

void job_queue_push(JobQueue *q, Job *job) {
    job->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail) q->tail->next = job;
    else         q->head = job;
    q->tail = job;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

Job *job_queue_pop(JobQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL && !q->closed)
        pthread_cond_wait(&q->cond, &q->mutex);
    Job *job = q->head;
    if (job) {
        q->head = job->next;
        if (!q->head) q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mutex);
    return job;
}

void job_queue_close(JobQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->closed = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void job_free(Job *job) {
    if (!job) return;
    yyjson_doc_free(job->request_doc);
    query_context_free(job->context);
    free(job);
}

void job_queue_mark_cancelled_by_id(JobQueue *q, int64_t id) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    for (Job *j = q->head; j != NULL; j = j->next) {
        /* Notifications carry no id (the LSP spec forbids it), so the
         * has_id guard already excludes them; the explicit check is
         * kept as a self-documenting belt-and-suspenders. */
        if (!j->is_notification && j->has_id && j->id == id) {
            j->is_cancelled = 1;
        }
    }
    pthread_mutex_unlock(&q->mutex);
}
