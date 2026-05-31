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

/*
 * Query context: a frozen, self-contained snapshot of exactly the document
 * and project state a read-only query handler needs, cloned out of the live
 * document store under docs_mutex at job-dispatch time.
 *
 * The coordinator thread builds one query_context per query job while
 * holding docs_mutex, attaches it to the Job, releases the lock, and hands
 * the job to a query worker.  The worker runs the handler entirely against
 * the query_context with no lock held, then frees it.  Because every field
 * is deep-copied and owned here, a concurrent notification mutating the live
 * store cannot disturb an in-flight query.
 *
 * State-mutating methods (initialize / shutdown) and the semanticTokens
 * methods (which write back result ids and depend on sequential
 * accumulation) do NOT use a query_context; the coordinator dispatches them
 * inline under docs_mutex against the live store.
 */

/**
 * One document's frozen read state.  All pointers are owned by this struct
 * and freed by query_context_free().  Field names mirror the live Document
 * so handlers read them unchanged.
 *
 * `root` is a deep copy of the document's tj_node tree; `tok_spans` is a
 * clone whose `owner` pointers have been relocated into that copied tree.
 */
typedef struct {
    char      *uri;
    char      *text;
    char      *task_prefix;
    char      *account_prefix;
    char      *report_prefix;
    char      *resource_prefix;

    tj_node   *root;            /**< deep-copied per-doc tree (owned) */
    TokenSpan *tok_spans;       /**< cloned; owner relocated into `root` */
    int        num_tok_spans;
    int        num_sem_entries;

    int        is_primary;      /**< 1 for the requested document */
} query_doc;

/**
 * Plain description of one live document the caller wants cloned.  Built by
 * server.c from a Document slot under docs_mutex and handed to
 * query_context_build(), which performs the deep copies.  Holds only
 * borrowed pointers into the live store — valid for the duration of the
 * build call only.
 */
typedef struct {
    const char      *uri;
    const char      *text;
    const char      *task_prefix;
    const char      *account_prefix;
    const char      *report_prefix;
    const char      *resource_prefix;
    const tj_node   *root;
    const TokenSpan *tok_spans;
    int              num_tok_spans;
    int              num_sem_entries;
    int              is_primary;
} query_doc_src;

/**
 * Immutable, fully-owned view of the workspace at a single point in time.
 *
 * `docs[primary_idx]` is the requested document; other slots are sibling
 * documents in the same project (extra completion / workspace-symbol
 * pools).  `primary_idx` is -1 when there is no primary document (e.g.
 * workspace/symbol, which spans every document).  `project_root` is a deep
 * copy of the primary document's assembled Project tree, or NULL when the
 * primary has no project.
 */
typedef struct query_context {
    query_doc   *docs;
    int          num_docs;
    int          primary_idx;
    ProjectNode *project_root;   /**< deep copy of primary project's root; may be NULL */
    char        *project_id;     /**< owned copy of the project id; may be NULL */
} query_context;

/**
 * Build a query_context by deep-copying every document in @p srcs and the
 * provided @p project_root.  Called by the coordinator while holding
 * docs_mutex; the returned context shares no memory with the live store.
 *
 * @param srcs          Array of @p num_docs document descriptors.
 * @param num_docs      Number of entries in @p srcs (may be 0).
 * @param primary_idx   Index of the primary document in @p srcs, or -1.
 * @param project_root  Live primary-project root to clone, or NULL.
 * @param project_id    Project id to copy, or NULL.
 * @return Newly allocated context; free with query_context_free().
 */
query_context *query_context_build(const query_doc_src *srcs, int num_docs,
                                   int primary_idx,
                                   const ProjectNode *project_root,
                                   const char *project_id);

/** The primary query_doc, or NULL when primary_idx < 0. */
const query_doc *query_context_primary(const query_context *qc);

/** Free a query_context and every field it owns.  NULL-safe. */
void query_context_free(query_context *qc);
