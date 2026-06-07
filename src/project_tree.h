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

#include <stdatomic.h>
#include <stdint.h>

/*
 * The assembled per-Project tree.
 *
 * A workspace_snapshot builds one ProjectNode tree per project on every
 * notification by deep-copying each member document's top-level declarations
 * under the includer's prefix target.  Unlike the per-document tj_node trees
 * (which are immutable after parse and own no cross-file edges), a
 * ProjectNode tree is purpose-built to be the cross-file resolution surface:
 * every node carries its own dependency array, and each dependency memoizes
 * its resolved cross-file edge in-node.
 *
 * Memory: a ProjectNode tree shares NO memory with any document tree.
 * project_node_from_tj() deep-copies every string (id, name, and the
 * source URI) so the tree stays valid independent of the documents it was
 * built from.  A dependency's resolved target points WITHIN the same tree.
 */

typedef struct ProjectNode ProjectNode;

/**
 * One `depends`/`precedes` reference on a ProjectNode task, deep-copied
 * from the originating document's Dependency.  The resolved edge is filled
 * lazily by project_dep_resolve() and memoized write-once in `resolved`.
 *
 * `resolved` is a single atomic word so the memo is lock-free and safe to
 * share across query workers that pin the same snapshot:
 *   0  — unresolved (sentinel; never a valid published value)
 *   1  — resolved, no matching target
 *   else — the resolved ProjectNode * (cast to uintptr_t)
 * The only transition is unresolved -> (deterministic value); resolution is
 * a pure function of the immutable tree, so concurrent first resolvers
 * compute identical results and the idempotent release-store is correct
 * regardless of which lands last.
 */
typedef struct {
    DepKind            kind;           /**< whether this dep is `depends` or `precedes` */
    int                bang_count;     /**< number of leading `!` characters */
    char              *path;           /**< owned; dotted identifier path */
    LspRange           source_range;   /**< spans the bang(s) + path in source */
    _Atomic uintptr_t  resolved;       /**< memo cell; see above */
} ProjectDep;

/** Sentinel value for ProjectDep.resolved meaning "not yet resolved". */
#define PROJECT_DEP_UNRESOLVED   ((uintptr_t)0)
/** Sentinel value for ProjectDep.resolved meaning "resolved, no target". */
#define PROJECT_DEP_RESOLVED_NULL ((uintptr_t)1)

/**
 * One node in the per-project cross-file resolution tree.  Deep-copied
 * from the originating tj_node by project_node_from_tj() so the tree
 * stays valid independent of the documents it was built from.  Holds
 * the dependency edges and the prefix-applied identifier namespace
 * that definition / references / dependency-hover resolve against.
 */
struct ProjectNode {
    /* ── Identity ── */
    int           keyword;          /**< KW_ / TK_ constant; 0 for the synthetic root */
    char         *id;               /**< owned; NULL on the synthetic root */
    char         *name;             /**< owned; NULL on the synthetic root */

    /* ── Source location ── */
    LspRange      range;            /**< full declaration including body */
    LspRange      selection_range;  /**< just the identifier (or keyword) token */

    /* ── Provenance ──
     * URI of the document this node was copied from.  Owned (strdup of
     * Document.uri) so the tree stays valid independent of that document.
     * NULL on the synthetic root. */
    char         *source_uri;       /**< owned; see above */

    /* ── Dependencies (task nodes only) ── */
    ProjectDep   *dependencies;     /**< owned array; @see num_dependencies */
    int           num_dependencies; /**< number of valid entries in `dependencies` */

    /* ── Tree links ── */
    ProjectNode  *parent_node;      /**< parent in this tree; NULL on the synthetic root */
    ProjectNode **children;         /**< owned array; @see num_children */
    int           num_children;     /**< number of valid entries in `children` */
    int           num_children_cap; /**< allocated capacity of `children` */
};

/**
 * Deep-copy a tj_node subtree into a freshly allocated ProjectNode
 * subtree, stamping @p source_uri (deep-copied) onto every node.  Copies
 * id / name strings and the dependency array (each dependency's memo cell
 * initialized to unresolved).  `parent_node` is wired internally; the
 * returned root's `parent_node` is NULL.
 *
 * @param src         Source tj_node.  NULL returns NULL.
 * @param source_uri  Owning document URI; deep-copied onto every node.
 * @return Newly allocated independent subtree.
 */
ProjectNode *project_node_from_tj(const tj_node *src, const char *source_uri);

/**
 * Append @p child under @p parent (growing the children array) and set
 * @p child's `parent_node` to @p parent.
 *
 * @param parent  Owning node.
 * @param child   Child subtree (transfer of ownership).
 */
void project_node_append_child(ProjectNode *parent, ProjectNode *child);

/**
 * Recursively free a heap-allocated ProjectNode subtree, including the
 * node itself.  Safe to call with NULL.
 *
 * @param node  Root of the subtree to free.
 */
void project_node_free(ProjectNode *node);

/**
 * Free every owned child of @p root, leaving @p root as an empty shell.
 * Used to tear down an inline synthetic root before each rebuild.
 *
 * @param root  Synthetic root whose children are released.
 */
void project_node_free_children(ProjectNode *root);

/**
 * Resolve @p owner_task's dependency at @p dep_index against
 * @p project_root, memoizing the result in-node.
 *
 * A dependency is always owned by the task it is declared on, so the
 * dependency is addressed by (owner_task, dep_index) rather than passed
 * as a bare pointer — that keeps the resolved edge and the bang-climb
 * anchor consistent by construction.
 *
 * Absolute (zero-bang) references are looked up in the project's single
 * prefix-applied task namespace.  Relative (bang) references climb
 * @p owner_task's `parent_node` chain one level per `!`, bounded by the
 * synthetic root.  On a second call the memoized result is returned
 * without recomputing.
 *
 * Concurrency: lock-free.  The result is published into the dependency's
 * atomic `resolved` cell with a release store; readers load it with acquire.
 * Multiple query workers pinning the same snapshot may resolve the same
 * cold cell concurrently — they compute identical results (resolution is a
 * pure function of the immutable tree) and the idempotent store is correct
 * whichever lands last.
 *
 * @param owner_task    The ProjectNode task declaring the dependency.
 * @param dep_index     Index into @p owner_task's `dependencies` array;
 *                      the caller guarantees 0 <= dep_index < num_dependencies.
 * @param project_root  The Project's synthetic root.
 * @return The resolved target ProjectNode, or NULL on a miss.
 */
ProjectNode *project_dep_resolve(ProjectNode *owner_task, int dep_index,
                                 ProjectNode *project_root);

/* ── Tree navigation / id-namespace classification ───────────────────────── */

/**
 * Coarse kind bucket for a node, collapsing the keyword set into the four
 * id namespaces TaskJuggler keeps separate.  KW_PROJECT maps to
 * NODE_KIND_OTHER but is never inserted into a project tree (it stays
 * document-local).
 */
typedef enum {
    NODE_KIND_TASK,     /**< KW_TASK */
    NODE_KIND_ACCOUNT,  /**< KW_ACCOUNT */
    NODE_KIND_RESOURCE, /**< KW_RESOURCE / KW_SHIFT */
    NODE_KIND_REPORT,   /**< report-family keywords (KW_REPORT, KW_NAVIGATOR, …) */
    NODE_KIND_OTHER     /**< anything else; never inserted into a project tree */
} NodeKind;

/**
 * Map a Bison keyword constant to the coarse NodeKind bucket that governs
 * its id namespace in the project tree.
 *
 * @param keyword  A KW_* keyword constant from grammar.tab.h.
 * @return The NodeKind bucket for @p keyword.
 */
NodeKind node_kind_of(int keyword);

/**
 * Walk @p start's children along the dot-separated @p path and return the
 * matched node, considering only children whose kind matches @p kind.
 * Returns @p start when @p path is NULL or empty.  The kind filter keeps
 * same-named declarations in different namespaces from colliding now that
 * all kinds share one root.
 *
 * @param start  Root node to begin the walk from; NULL returns NULL.
 * @param path   Dot-separated segment string (e.g. "proj.sub"); may be NULL.
 * @param kind   NodeKind filter applied to each child during the walk.
 * @return The deepest matching node, or NULL when the path fails to resolve.
 */
ProjectNode *find_node_by_dotted_path(ProjectNode *start, const char *path,
                                      NodeKind kind);

/**
 * Map a per-document task tj_node to its clone in an assembled Project tree.
 * Cursor lookups (dependency_at_cursor / task_decl_at_cursor) return
 * per-document nodes, but cross-file resolution lives in the ProjectNode
 * tree, so callers bridge through here first.
 *
 * The per-document task's unprefixed dotted id (its in-file ancestry) is
 * appended to @p task_prefix's target inside the Project tree.
 *
 * @param project_root  The assembled project's synthetic root; NULL returns NULL.
 * @param task_prefix   The document's task-namespace prefix; may be NULL.
 * @param per_doc_task  Per-document tj_node representing the task declaration.
 * @return Matching ProjectNode in the merged tree, or NULL when the project
 *         tree is absent, the prefix target is missing, or the path does not
 *         resolve.
 */
ProjectNode *project_node_for_doc_task(ProjectNode *project_root,
                                       const char *task_prefix,
                                       const tj_node *per_doc_task);
