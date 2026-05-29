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

#pragma once

#include "parser.h"
#include "project_tree.h"
#include "semantic_tokens.h"

/**
 * Self-contained snapshot of a single Document taken under docs_mutex.
 * All fields are owned by this struct; free with doc_snapshot_free().
 *
 * `page` is the mmap-backed copy of the Document's parse slab page.
 * `slab` holds reconstructed pointer fields into that page so any
 * parse_slab accessor works on the copy.  `slab.includes` is set to
 * NULL/0 (includes are only consumed by notification handlers, not
 * queries).
 *
 * `sem_tokens` is a deep copy of Document.sem_tokens at snapshot time,
 * including heap copies of `data` and `result_id`.  After a
 * semanticTokens handler runs on the snapshot the updated sem_tokens
 * must be written back to the live Document under docs_mutex.
 */
typedef struct {
    char        *uri;
    char        *text;
    char        *task_prefix;
    char        *account_prefix;
    char        *report_prefix;
    char        *resource_prefix;

    parse_page_header *page;   /**< owned mmap copy; NULL when doc has no slab */
    parse_slab   slab;         /**< view into page (valid only when page != NULL) */

    SemanticTokenResult sem_tokens;  /**< deep copy: data and result_id owned */
} doc_snapshot;

/**
 * Immutable view of the workspace at a single point in time, built by
 * copying all relevant Document slabs and deep-copying the primary
 * Project tree under docs_mutex.  Handlers operate on this struct
 * without any lock.
 *
 * `docs[primary_idx]` is the document the query is targeting; all
 * other slots are Documents in the same Project (extra completion
 * pools).  `project_root` is a deep copy of the primary Document's
 * Project tree with all dependency resolution reset to DEP_UNRESOLVED so
 * the copy can re-resolve lazily within its own node pointers.
 */
typedef struct workspace_snapshot {
    doc_snapshot **docs;
    int            num_docs;
    int            primary_idx;   /**< index into docs[]; -1 if none */
    ProjectNode   *project_root;  /**< deep copy of primary project's root; NULL if none */
} workspace_snapshot;

/**
 * Free a doc_snapshot and all owned fields.  NULL-safe.
 */
void doc_snapshot_free(doc_snapshot *ds);

/**
 * Free a workspace_snapshot and all owned fields.  NULL-safe.
 */
void workspace_snapshot_free(workspace_snapshot *snap);
