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

void workspace_snapshot_free(WorkspaceSnapshot *snap) {
    if (!snap) return;

    for (int i = 0; i < snap->num_documents; i++) {
        DocSnapshot *d = &snap->documents[i];
        free(d->uri);
        free(d->text);
        tj_node_free(d->root);
        tok_spans_free(d->tok_spans, d->num_tok_spans);
        free(d->task_prefix);
        free(d->account_prefix);
        free(d->report_prefix);
        free(d->resource_prefix);
        semantic_token_result_release(&d->sem_tokens);
    }
    free(snap->documents);

    for (int i = 0; i < snap->num_projects; i++) {
        ProjectSnapshot *p = &snap->projects[i];
        free(p->id);
        project_node_free_children(&p->root);
    }
    free(snap->projects);

    free(snap->workspace_root);
    free(snap);
}

DocSnapshot *workspace_snapshot_find_doc(const WorkspaceSnapshot *snap,
                                          const char *uri) {
    if (!snap || !uri) return NULL;
    for (int i = 0; i < snap->num_documents; i++) {
        if (snap->documents[i].uri && strcmp(snap->documents[i].uri, uri) == 0)
            return &snap->documents[i];
    }
    return NULL;
}
