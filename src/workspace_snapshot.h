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

/** @file
 *
 * Frozen, self-contained snapshots of the document and project state,
 * captured under docs_mutex at query dispatch time so query workers can
 * run lock-free.
 *
 * The snapshot hierarchy is:
 *
 *   WorkspaceSnapshot
 *     ├─ DocSnapshot[]      — one per in-use Document at capture time
 *     │    ├─ deep-copied tj_node tree (root)
 *     │    ├─ deep-copied TokenSpan array (.owner re-wired into copy)
 *     │    └─ primary_project → ProjectSnapshot (borrowed within snap)
 *     └─ ProjectSnapshot[]  — one per Project at capture time
 *          └─ deep-copied ProjectNode tree (children of root)
 *
 * All pointer fields within a snapshot are either owned (freed by
 * workspace_snapshot_free) or borrowed within the same snapshot
 * (primary_project).  The snapshot shares no memory with the live
 * docs[]/projects[] state.
 */

#pragma once

#include "parser.h"
#include "project_tree.h"
#include "semantic_tokens.h"

typedef struct ProjectSnapshot ProjectSnapshot;

/**
 * Frozen copy of one document's query-relevant state.
 *
 * Field names intentionally mirror Document so that handler code that
 * was written for Document * compiles unchanged after a type substitution.
 *
 * `primary_project` is a borrowed pointer into the containing
 * WorkspaceSnapshot's projects array; it is valid for the lifetime of
 * the WorkspaceSnapshot.
 *
 * `sem_tokens` is a deep copy of the live cache at snapshot time.
 * Semantic-token handlers mutate this copy; the updated state is written
 * back to the live Document by server_dispatch_query() before the
 * snapshot is freed, so future delta requests see the updated result ID.
 */
typedef struct {
    char        *uri;               /**< owned */
    char        *text;              /**< owned */
    tj_node     *root;              /**< owned deep copy; .owner in tok_spans re-wired here */
    TokenSpan   *tok_spans;         /**< owned deep copy */
    int          num_tok_spans;
    int          tok_span_cap;
    int          num_sem_entries;
    char        *task_prefix;       /**< owned; NULL when not set */
    char        *account_prefix;    /**< owned; NULL when not set */
    char        *report_prefix;     /**< owned; NULL when not set */
    char        *resource_prefix;   /**< owned; NULL when not set */
    SemanticTokenResult sem_tokens; /**< deep copy; mutable during handler */
    int          disk_only;
    int          is_cc_root;
    /** Borrowed pointer into WorkspaceSnapshot.projects; NULL for orphans
     *  not yet assigned a project. */
    ProjectSnapshot *primary_project;
} DocSnapshot;

/**
 * Frozen copy of one project's state.
 *
 * `root` is an inline ProjectNode whose children are deep copies of the
 * live Project.root's children.  The inline root itself is not
 * heap-allocated; only root.children[] (and their subtrees) are.  Use
 * project_node_free_children(&ps->root) to release them, not
 * project_node_free (which would attempt to free the inline node).
 */
struct ProjectSnapshot {
    char        *id;        /**< owned */
    int          is_orphan;
    ProjectNode  root;      /**< inline; children owned via deep copy */
};

/**
 * Complete frozen view of the server's document and project state,
 * captured under docs_mutex at query dispatch time.  Each query worker
 * receives one attached to its Job and operates on it without holding
 * any mutex.
 *
 * Lifetime: created by workspace_snapshot_build() in server.c (which
 * has access to the static docs[]/projects[] arrays), stored in
 * Job.snapshot, and freed by workspace_snapshot_free() from job_free().
 * Semantic-token write-back must happen before job_free() is called.
 */
typedef struct WorkspaceSnapshot {
    DocSnapshot     *documents;      /**< owned array of num_documents entries */
    int              num_documents;
    ProjectSnapshot *projects;       /**< owned array of num_projects entries */
    int              num_projects;
    char            *workspace_root; /**< owned; NULL when not set */
} WorkspaceSnapshot;

/**
 * Free all allocations owned by @p snap and @p snap itself.
 * NULL-safe.
 */
void workspace_snapshot_free(WorkspaceSnapshot *snap);

/**
 * Linear scan for the DocSnapshot whose uri equals @p uri.
 * Returns NULL when not found or when @p snap / @p uri is NULL.
 */
DocSnapshot *workspace_snapshot_find_doc(const WorkspaceSnapshot *snap,
                                          const char *uri);
