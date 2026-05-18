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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* Coalesce a same-URI didChange into the tail of the queue.  Only
     * tail coalescing is safe: any other element between the new job
     * and an older same-URI didChange could observe the older parse,
     * so collapsing across it would change visible LSP semantics. */
    if (q->tail
        && q->tail->coalesce_uri
        && job->coalesce_uri
        && strcmp(q->tail->coalesce_uri, job->coalesce_uri) == 0) {
        yyjson_doc_free(q->tail->request_doc);
        q->tail->request_doc = job->request_doc;
        job->request_doc = NULL;
        pthread_mutex_unlock(&q->mutex);
        job_free(job);
        return;
    }
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
    free(job->coalesce_uri);
    free(job);
}
