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

/*
 * The assembled per-Project tree.
 *
 * server.c builds one ProjectNode tree per Project on every notification
 * by deep-copying each member document's top-level declarations under the
 * includer's prefix target.  Unlike the per-document tj_node trees (which
 * are immutable after parse and own no cross-file edges), a ProjectNode
 * tree is purpose-built to be the cross-file resolution surface: every
 * node carries its own dependency array, and each dependency stores its
 * resolved cross-file edge in-node.
 *
 * Memory: a ProjectNode tree shares NO memory with any document tree.
 * project_node_from_tj() deep-copies every string (id, name, and the
 * source URI) so the tree stays valid independent of the documents it was
 * built from.  resolved_target / target_uri point WITHIN the same tree.
 */

/** Lazy-resolution state for one ProjectDep.  The edge is resolved on the
 *  first query that needs it and memoized in-node. */
typedef enum {
    DEP_UNRESOLVED,         /**< not yet resolved this rebuild cycle */
    DEP_RESOLVED_NULL,      /**< resolved, no matching target */
    DEP_RESOLVED_TARGET     /**< resolved to resolved_target */
} DepResolveState;

typedef struct ProjectNode ProjectNode;

/**
 * One `depends`/`precedes` reference on a ProjectNode task, deep-copied
 * from the originating document's Dependency.  The resolved edge
 * (`resolved_target` / `target_uri`) is filled lazily by
 * project_dep_resolve() and memoized here.
 */
typedef struct {
    DepKind          kind;
    int              bang_count;       /**< number of leading `!` characters */
    char            *path;             /**< owned; dotted identifier path */
    LspRange         source_range;     /**< spans the bang(s) + path in source */
    ProjectNode     *resolved_target;  /**< borrowed within this tree; NULL on a miss */
    const char      *target_uri;       /**< == resolved_target->source_uri, or NULL */
    DepResolveState  state;            /**< DEP_UNRESOLVED until first query */
} ProjectDep;

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
    char         *source_uri;

    /* ── Dependencies (task nodes only) ── */
    ProjectDep   *dependencies;     /**< owned */
    int           num_dependencies;

    /* ── Tree links ── */
    ProjectNode  *parent_node;      /**< parent in this tree; NULL on the synthetic root */
    ProjectNode **children;         /**< owned */
    int           num_children;
    int           num_children_cap;
};

/**
 * Deep-copy a tj_node (and its subtree) from the parse slab into a
 * freshly allocated ProjectNode subtree, stamping @p source_uri onto
 * every node.  Copies id / name strings and the dependency array (with
 * `resolved_target` cleared to DEP_UNRESOLVED).  `parent_node` is wired
 * internally; the returned root's `parent_node` is NULL.
 *
 * @param slab        Parse slab owning @p node_idx and all string data.
 * @param node_idx    Index of the root tj_node in @p slab to copy.
 *                    Returns NULL when @p node_idx == -1.
 * @param source_uri  Owning document URI; deep-copied onto every node.
 * @return Newly allocated independent subtree.
 */
ProjectNode *project_node_from_tj(const parse_slab *slab, tj_idx node_idx,
                                   const char *source_uri);

/**
 * Append @p child under @p parent (growing the children array) and set
 * @p child's `parent_node` to @p parent.
 */
void project_node_append_child(ProjectNode *parent, ProjectNode *child);

/**
 * Recursively free a heap-allocated ProjectNode subtree, including the
 * node itself.  Safe to call with NULL.
 */
void project_node_free(ProjectNode *node);

/**
 * Recursively deep-copy @p src into a new heap-allocated ProjectNode
 * subtree.  Strings (id, name, source_uri, dep.path) are strdup'd.
 * All dependency resolution state is reset to DEP_UNRESOLVED so the
 * copy can re-resolve lazily without cross-tree pointer hazards.
 * `parent_node` is wired internally; the returned root's `parent_node`
 * is NULL.  Safe to call with NULL (returns NULL).
 */
ProjectNode *project_node_deep_copy(const ProjectNode *src);

/**
 * Free every owned child of @p root, leaving @p root as an empty shell.
 * Used to tear down an inline synthetic root before each rebuild.
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
 * Concurrency: this mutates the dependency on first call.  Safe today
 * because the server holds docs_mutex for the full duration of every
 * notification and every query (single query worker).  The
 * TODO(workspace-snapshot) lock-free model must synchronize this memo.
 *
 * @param owner_task    The ProjectNode task declaring the dependency.
 * @param dep_index     Index into @p owner_task's `dependencies` array;
 *                      the caller guarantees 0 <= dep_index < num_dependencies.
 * @param project_root  The Project's synthetic root.
 * @return The resolved target ProjectNode, or NULL on a miss.
 */
ProjectNode *project_dep_resolve(ProjectNode *owner_task, int dep_index,
                                 ProjectNode *project_root);
