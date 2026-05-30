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

#include "workspace_snapshot.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void doc_snapshot_free(doc_snapshot *ds) {
    if (!ds) return;
    free(ds->uri);
    free(ds->text);
    free(ds->task_prefix);
    free(ds->account_prefix);
    free(ds->report_prefix);
    free(ds->resource_prefix);
    if (ds->page)
        munmap(ds->page, ds->page->total_mmap_size);
    free(ds->sem_tokens.data);
    free(ds->sem_tokens.result_id);
    free(ds);
}

void workspace_snapshot_free(workspace_snapshot *snap) {
    if (!snap) return;
    for (int i = 0; i < snap->num_docs; i++)
        doc_snapshot_free(snap->docs[i]);
    free(snap->docs);
    project_node_free(snap->project_root);
    free(snap);
}
