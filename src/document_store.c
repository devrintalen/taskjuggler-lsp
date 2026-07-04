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

#include "document_store.h"
#include "pathutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Document docs[MAX_DOCS];

pthread_mutex_t docs_mutex = PTHREAD_MUTEX_INITIALIZER;

void docstore_init(void) {
    for (int i = 0; i < MAX_DOCS; i++)
        docs[i].in_use = 0;
}

Document *doc_find(const char *uri) {
    if (!uri) return NULL;
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i].in_use && strcmp(docs[i].uri, uri) == 0)
            return &docs[i];

    char *canon = normalize_uri(uri);
    if (!canon) return NULL;
    Document *found = NULL;
    if (strcmp(canon, uri) != 0) {
        for (int i = 0; i < MAX_DOCS; i++)
            if (docs[i].in_use && strcmp(docs[i].uri, canon) == 0) {
                found = &docs[i];
                break;
            }
    }
    free(canon);
    return found;
}

Document *doc_alloc(const char *uri) {
    char *canon = normalize_uri(uri);
    if (!canon) return NULL;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!docs[i].in_use) {
            docs[i].in_use = 1;
            docs[i].uri    = canon;
            atomic_store(&docs[i].doc_version, 1);
            return &docs[i];
        }
    }
    free(canon);
    return NULL;
}

void doc_clear_parse_state(Document *d) {
    docsnap_release(d->snap);
    d->snap = NULL;
    docsnap_release(d->prev_snap);
    d->prev_snap = NULL;
}

void doc_install_parse(Document *d, ParseOutput *po) {
    doc_snapshot *fresh = NULL;
    if (po) {
        uint64_t version = atomic_fetch_add(&d->doc_version, 1);
        fresh = docsnap_new(d->uri, d->text,
                            po->root, po->tok_spans, po->tok_owners,
                            po->num_tok_spans, po->tok_arena,
                            po->num_sem_entries, version);
        /* Ownership of the tree, token spans + owners, and their backing arena
         * moved into the snapshot; null them out so parse_output_free only
         * releases what po still owns (the includes array and the struct shell). */
        po->root            = NULL;
        po->tok_spans       = NULL;
        po->tok_owners      = NULL;
        po->num_tok_spans   = 0;
        po->tok_arena       = NULL;
        po->num_sem_entries = 0;
        parse_output_free(po);
    }

    docsnap_release(d->prev_snap);
    d->prev_snap = d->snap;   /* retains its existing ref, reassigned */
    d->snap      = fresh;     /* fresh holds ref 1, or NULL on a no-parse */
}

void doc_free(Document *d) {
    free(d->uri);
    free(d->text);
    doc_clear_parse_state(d);
    free(d->task_prefix);
    free(d->account_prefix);
    free(d->report_prefix);
    free(d->resource_prefix);
    for (int i = 0; i < d->num_included_uris; i++)
        free(d->included_uris[i]);
    free(d->included_uris);
    memset(d, 0, sizeof(*d));
}

char *read_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return NULL; }
    rewind(file);
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return NULL; }
    size_t read_count = fread(buffer, 1, (size_t)size, file);
    buffer[read_count] = '\0';
    fclose(file);
    return buffer;
}
