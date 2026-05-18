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

#include "mutation_versions.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Capacity matches MAX_DOCS in server.c; the editor never opens more
 * distinct URIs than the document store can hold. */
#define MAX_VERSIONS 128

typedef struct {
    char           *uri;     /* heap-owned, server lifetime */
    _Atomic int64_t version;
} Entry;

static Entry           entries[MAX_VERSIONS];
static int             entry_count;
/* Guards entry_count and entries[].uri pointer writes.  Atomic reads of
 * version do not need it — readers can race with concurrent bumps and
 * will see either the old or the new value, both of which are valid. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void mutation_versions_init(void) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < entry_count; i++) {
        free(entries[i].uri);
        entries[i].uri = NULL;
        atomic_store(&entries[i].version, 0);
    }
    entry_count = 0;
    pthread_mutex_unlock(&mu);
}

/* Find existing entry under lock.  Caller holds mu. */
static Entry *find_locked(const char *uri) {
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].uri, uri) == 0) return &entries[i];
    }
    return NULL;
}

int64_t mutation_versions_snapshot(const char *uri) {
    if (!uri) return 0;
    pthread_mutex_lock(&mu);
    Entry *e = find_locked(uri);
    pthread_mutex_unlock(&mu);
    return e ? atomic_load(&e->version) : 0;
}

int64_t mutation_versions_bump(const char *uri) {
    if (!uri) return 0;
    pthread_mutex_lock(&mu);
    Entry *e = find_locked(uri);
    if (!e) {
        if (entry_count >= MAX_VERSIONS) {
            pthread_mutex_unlock(&mu);
            return 0;
        }
        e = &entries[entry_count++];
        e->uri = strdup(uri);
        atomic_store(&e->version, 0);
    }
    int64_t new_version = atomic_fetch_add(&e->version, 1) + 1;
    pthread_mutex_unlock(&mu);
    return new_version;
}
