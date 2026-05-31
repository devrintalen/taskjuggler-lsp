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

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Immutable, refcounted workspace snapshots.
 *
 * Read-only query handlers never see the live document store.  Instead the
 * coordinator publishes an immutable workspace_snapshot after every
 * document-changing notification (built under docs_mutex), and each query
 * pins the current snapshot by bumping one atomic refcount — an O(1) clone,
 * not the O(nodes) deep copy the earlier query_context did.  A concurrent
 * notification builds a *new* snapshot and atomically swaps the published
 * pointer; the old snapshot is freed only when its last in-flight query
 * releases it, so a query always reads one self-consistent revision.
 *
 * Two refcounted layers:
 *
 *   doc_snapshot      — one document's frozen parse output (tj_node tree,
 *                      token spans, source text).  Created once per parse and
 *                      shared by ref: editing document A produces a new
 *                      doc_snapshot for A while every other document's snapshot
 *                      is re-referenced unchanged.  Carries a write-once memo
 *                      for its semantic-token data.
 *
 *   workspace_snapshot — the cross-file view: a ws_doc per in-use document
 *                      (referencing its doc_snapshot plus the include-prefixes
 *                      in force this revision) and a ws_project per assembled
 *                      project (the deep-copied ProjectNode resolution tree
 *                      with its atomic dependency memos).
 *
 * State-mutating lifecycle methods (initialize / shutdown) do not touch a
 * snapshot and run inline on the coordinator; every other query — including
 * semanticTokens, whose data and result id now live in the doc_snapshot — runs
 * on a worker against a pinned snapshot with no lock held.
 */

/* ── Semantic-token data memo ────────────────────────────────────────────── */

/** Computed LSP semantic-token data for one doc_snapshot: the flat five-int
 *  encoded buffer plus its entry count.  Heap-allocated once and published
 *  into doc_snapshot.sem_memo by compare-exchange. */
typedef struct sem_token_data {
    uint32_t *data;   /**< owned; flat uint32 buffer (count entries) */
    size_t    count;  /**< number of uint32 entries (multiple of 5) */
} sem_token_data;

/* ── Per-document snapshot ───────────────────────────────────────────────── */

/**
 * One document's immutable parse output, refcounted and shared.  Every
 * pointer field is owned and freed when the last ref drops.  The tj_node
 * tree and token spans are moved out of a ParseOutput at creation, so the
 * TokenSpan.owner pointers already address nodes within `root`.
 */
typedef struct doc_snapshot {
    _Atomic int  refcount;
    uint64_t     doc_version;     /**< monotonic parse stamp; also the semantic-tokens resultId */

    char        *uri;             /**< owned canonical file:// URI */
    char        *text;            /**< owned copy of the parsed source text */

    tj_node     *root;            /**< owned synthetic root over all top-level decls */
    TokenSpan   *tok_spans;       /**< owned; .owner points within `root` */
    int          num_tok_spans;
    int          num_sem_entries;

    _Atomic(sem_token_data *) sem_memo;  /**< NULL until first request; CAS-published */
} doc_snapshot;

/**
 * Create a doc_snapshot taking ownership of @p root and @p tok_spans (the
 * fields moved out of a ParseOutput) and deep-copying @p uri / @p text.
 * Returned with refcount 1.
 */
doc_snapshot *docsnap_new(const char *uri, const char *text,
                         tj_node *root, TokenSpan *tok_spans,
                         int num_tok_spans, int num_sem_entries,
                         uint64_t doc_version);

/** Bump the refcount and return @p s (NULL-safe, returns NULL). */
doc_snapshot *docsnap_acquire(doc_snapshot *s);

/** Drop one ref; free @p s and everything it owns when the last ref goes. */
void docsnap_release(doc_snapshot *s);

/**
 * Return @p s's semantic-token data, computing and memoizing it on first
 * call.  Lock-free and idempotent: concurrent first callers each compute a
 * buffer and race to publish via compare-exchange; the losers free their
 * buffer and adopt the winner's.  The returned pointer is owned by @p s and
 * valid for its lifetime.
 */
void docsnap_sem_tokens(doc_snapshot *s, const uint32_t **out_data, size_t *out_count);

/* ── Workspace snapshot ──────────────────────────────────────────────────── */

/** One document's place in a workspace_snapshot: its (ref'd) doc_snapshot, the
 *  include-prefixes applied to it this revision (owned copies), and the index
 *  of the project it belongs to. */
typedef struct ws_doc {
    doc_snapshot *snap;            /**< ref'd; +1 held by the owning workspace_snapshot */
    char        *task_prefix;     /**< owned; may be NULL */
    char        *account_prefix;
    char        *report_prefix;
    char        *resource_prefix;
    int          project_index;   /**< index into workspace_snapshot.projects, or -1 */
} ws_doc;

/** One assembled project: its canonical id and the synthetic root of the
 *  deep-copied cross-file ProjectNode resolution tree.  Heap-allocated
 *  individually so `root`'s address is stable (children link back to it via
 *  parent_node). */
typedef struct ws_project {
    char        *id;              /**< owned; canonical .tjp URI or doc URI */
    ProjectNode  root;            /**< embedded synthetic root over all kinds */
} ws_project;

/** Immutable, refcounted cross-file view of the whole workspace. */
typedef struct workspace_snapshot {
    _Atomic int  refcount;
    ws_doc       *docs;            /**< owned array[num_docs] */
    int          num_docs;
    ws_project  **projects;        /**< owned array of owned ws_project* */
    int          num_projects;
    int          projects_cap;
} workspace_snapshot;

/** Allocate a workspace_snapshot with @p num_docs zeroed ws_doc slots (caller
 *  fills them) and an empty project list.  Returned with refcount 1. */
workspace_snapshot *ws_alloc(int num_docs);

/** Append a new empty ws_project with id @p id (deep-copied) and return its
 *  index; the caller populates its `root` tree. */
int ws_add_project(workspace_snapshot *ws, const char *id);

/** Bump the refcount and return @p ws (NULL-safe, returns NULL). */
workspace_snapshot *ws_acquire(workspace_snapshot *ws);

/** Drop one ref; free @p ws, its project trees, and release every ws_doc's
 *  doc_snapshot ref when the last ref goes. */
void ws_release(workspace_snapshot *ws);

/* ── Per-query context ───────────────────────────────────────────────────── */

/**
 * One document's read view as the handlers consume it.  All pointers are
 * borrowed from a snapshot pinned by the enclosing query_context and are
 * valid for the duration of the query; nothing here is owned.  Field names
 * mirror the old live Document so handlers read them unchanged.
 */
typedef struct query_doc {
    const char  *uri;
    const char  *text;
    const char  *task_prefix;
    const char  *account_prefix;
    const char  *report_prefix;
    const char  *resource_prefix;

    tj_node     *root;            /**< borrowed from snap */
    TokenSpan   *tok_spans;       /**< borrowed from snap */
    int          num_tok_spans;
    int          num_sem_entries;

    int          is_primary;      /**< 1 for the requested document */
    doc_snapshot *snap;            /**< borrowed backing snapshot (sem-token memo + version) */
} query_doc;

/**
 * A query's pinned, lock-free view of the workspace.  Holds one ref on the
 * workspace_snapshot (which transitively pins every doc_snapshot and project
 * tree it reads) plus, for semanticTokens/delta, one ref on the immediately
 * previous doc_snapshot of the primary document.  The query_doc array borrows
 * into the snapshot; only the array itself and the two refs are released by
 * query_context_free.
 */
typedef struct query_context {
    workspace_snapshot *ws;          /**< 1 ref; pins docs + project trees. */
    doc_snapshot       *prev_snap;   /**< 1 ref; primary's previous parse, for delta. May be NULL. */

    query_doc   *docs;              /**< owned array; entries borrow into `ws`. */
    int          num_docs;
    int          primary_idx;       /**< index of the requested doc, or -1. */

    ProjectNode *project_root;      /**< borrowed primary project root; may be NULL. */
    const char  *project_id;        /**< borrowed; may be NULL. */
} query_context;

/** The primary query_doc, or NULL when primary_idx < 0. */
const query_doc *query_context_primary(const query_context *qc);

/** Free a query_context: release its snapshot refs and the docs array.
 *  NULL-safe. */
void query_context_free(query_context *qc);
