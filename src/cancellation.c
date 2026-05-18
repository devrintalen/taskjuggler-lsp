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

#include "cancellation.h"

#include <pthread.h>

/* Capacity is well above any realistic in-flight depth.  Observed cancels
 * in a heavy editing session peaked at depth 4. */
#define CANCELLATION_CAPACITY 256

static int64_t         entries[CANCELLATION_CAPACITY];
static int             count;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void cancellation_init(void) {
    pthread_mutex_lock(&mu);
    count = 0;
    pthread_mutex_unlock(&mu);
}

void cancellation_mark(int64_t id) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < count; i++) {
        if (entries[i] == id) {
            pthread_mutex_unlock(&mu);
            return;
        }
    }
    if (count < CANCELLATION_CAPACITY) {
        entries[count++] = id;
    }
    pthread_mutex_unlock(&mu);
}

int cancellation_check_and_clear(int64_t id) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < count; i++) {
        if (entries[i] == id) {
            entries[i] = entries[--count];
            pthread_mutex_unlock(&mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

void cancellation_clear(int64_t id) {
    (void)cancellation_check_and_clear(id);
}
