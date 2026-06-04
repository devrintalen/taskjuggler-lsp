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

#include "diag_worker.h"
#include "diagnostics.h"
#include "tj3.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── one project's worker ────────────────────────────────────────────────── */

/**
 * One long-lived background worker thread that runs tj3 against
 * successive workspace_snapshots of a single project and publishes
 * the resulting diagnostics.  Workers coalesce: while busy, a newer
 * snapshot replaces any unstarted pending request rather than
 * queuing, so a burst of edits collapses into as few tj3 invocations
 * as possible while still guaranteeing eventual validation of the
 * latest snapshot.
 */
typedef struct diag_worker {
    char            *project_id;     /**< owned; registry key (root document URI) */
    tj3_mode         mode;           /**< fixed for the worker's lifetime */

    pthread_t        thread;         /**< worker thread handle */
    pthread_mutex_t  lock;           /**< guards `pending` / `stop` / `clear_on_stop` */
    pthread_cond_t   cond;           /**< signalled when `pending` or `stop` changes */

    workspace_snapshot *pending;     /**< newest unstarted request; holds 1 ref */
    int              stop;           /**< 1 asks the thread to exit at its next wake */
    int              clear_on_stop;  /**< publish empties on stop (retired project) or not (shutdown) */

    diag_set        *last_published; /**< owned; what this worker last emitted */

    int              seen;           /**< registry bookkeeping (coordinator-only) */
} diag_worker;

/* Publish empties for every URI in @p prev, clearing this worker's marks. */
static void clear_published(diag_set *prev) {
    if (!prev) return;
    diag_set *empty = diag_set_new();
    diag_set_publish(empty, prev);
    diag_set_free(empty);
}

/* Run tj3 once against @p ws for this worker's project and publish the diff.
 * Consumes the snapshot reference held in @p ws. */
static void run_once(diag_worker *w, workspace_snapshot *ws) {
    const ws_project *proj = NULL;
    for (int i = 0; i < ws->num_projects; i++) {
        ws_project *p = ws->projects[i];
        if (p->id && strcmp(p->id, w->project_id) == 0) { proj = p; break; }
    }

    diag_set *set = diag_set_new();
    if (proj) {
        /* Server-level "Missing compile_commands.json" warnings share this
         * diag_set with the tj3 results so the two sources merge per URI.
         * Emitted unconditionally (not behind tj3_available) so the warnings
         * appear even where tj3 is not installed. */
        diag_collect_cc_missing(ws, proj, set);
        tj3_collect_project(ws, proj, w->mode, set);
    }

    diag_set_publish(set, w->last_published);
    diag_set_free(w->last_published);
    w->last_published = set;

    ws_release(ws);
}

static void *diag_worker_main(void *arg) {
    diag_worker *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->pending && !w->stop)
            pthread_cond_wait(&w->cond, &w->lock);
        if (w->stop) {
            if (w->pending) { ws_release(w->pending); w->pending = NULL; }
            pthread_mutex_unlock(&w->lock);
            break;
        }
        workspace_snapshot *ws = w->pending;
        w->pending = NULL;
        pthread_mutex_unlock(&w->lock);

        run_once(w, ws);
    }

    /* Clearing only makes sense when a project was retired mid-session (drop
     * its stale markers); at server shutdown the client is going away, so we
     * skip it — which also keeps the output stream deterministic for tests. */
    if (w->clear_on_stop) clear_published(w->last_published);
    diag_set_free(w->last_published);
    w->last_published = NULL;
    return NULL;
}

static diag_worker *diag_worker_spawn(const char *project_id, tj3_mode mode) {
    diag_worker *w = calloc(1, sizeof(*w));
    if (!w) { return NULL; }
    w->project_id = strdup(project_id);
    w->mode       = mode;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    if (pthread_create(&w->thread, NULL, diag_worker_main, w) != 0) {
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->cond);
        free(w->project_id);
        free(w);
        return NULL;
    }
    return w;
}

static void diag_worker_request(diag_worker *w, workspace_snapshot *ws) {
    ws_acquire(ws);
    pthread_mutex_lock(&w->lock);
    if (w->pending) ws_release(w->pending);
    w->pending = ws;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static void diag_worker_retire(diag_worker *w, int clear_on_stop) {
    pthread_mutex_lock(&w->lock);
    w->clear_on_stop = clear_on_stop;
    w->stop = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);

    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    free(w->project_id);
    free(w);
}

/* ── registry (coordinator-owned, single-threaded) ───────────────────────── */

static diag_worker **g_workers;
static int           g_num_workers;
static int           g_workers_cap;

static diag_worker *registry_find(const char *project_id) {
    for (int i = 0; i < g_num_workers; i++)
        if (strcmp(g_workers[i]->project_id, project_id) == 0)
            return g_workers[i];
    return NULL;
}

static void registry_add(diag_worker *w) {
    if (g_num_workers >= g_workers_cap) {
        int nc = g_workers_cap ? g_workers_cap * 2 : 4;
        diag_worker **t = realloc(g_workers, (size_t)nc * sizeof(*t));
        if (!t) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_workers     = t;
        g_workers_cap = nc;
    }
    g_workers[g_num_workers++] = w;
}

static void registry_remove_at(int idx) {
    g_workers[idx] = g_workers[--g_num_workers];
}

void diag_registry_update(workspace_snapshot *ws) {
    if (!ws) return;

    for (int i = 0; i < g_num_workers; i++)
        g_workers[i]->seen = 0;

    for (int p = 0; p < ws->num_projects; p++) {
        ws_project *proj = ws->projects[p];
        if (!proj->id) continue;
        tj3_mode mode = proj->from_compile_commands ? TJ3_FULL : TJ3_SYNTAX_ONLY;

        diag_worker *w = registry_find(proj->id);
        if (w && w->mode != mode) {
            /* Class flipped (added to / removed from compile_commands.json):
             * retire the old worker and respawn in the new class. */
            for (int i = 0; i < g_num_workers; i++)
                if (g_workers[i] == w) { registry_remove_at(i); break; }
            diag_worker_retire(w, 1);
            w = NULL;
        }
        if (!w) {
            w = diag_worker_spawn(proj->id, mode);
            if (!w) continue;
            registry_add(w);
        }
        w->seen = 1;
        diag_worker_request(w, ws);
    }

    for (int i = 0; i < g_num_workers; ) {
        if (!g_workers[i]->seen) {
            diag_worker *w = g_workers[i];
            registry_remove_at(i);
            diag_worker_retire(w, 1);
        } else {
            i++;
        }
    }
}

void diag_registry_shutdown(void) {
    for (int i = 0; i < g_num_workers; i++)
        diag_worker_retire(g_workers[i], 0);
    free(g_workers);
    g_workers     = NULL;
    g_num_workers = 0;
    g_workers_cap = 0;
}
