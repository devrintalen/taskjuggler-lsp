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

#include "project_tree.h"
#include "grammar.tab.h"   /* KW_* keyword constants */
#include "hover.h"         /* sym_qualified_id */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Construction ───────────────────────────────────────────────────────── */

void project_node_append_child(ProjectNode *parent, ProjectNode *child) {
    if (parent->num_children >= parent->num_children_cap) {
        int nc = parent->num_children_cap ? parent->num_children_cap * 2 : 4;
        ProjectNode **tmp = realloc(parent->children,
                                    (size_t)nc * sizeof(ProjectNode *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        parent->children         = tmp;
        parent->num_children_cap = nc;
    }
    parent->children[parent->num_children++] = child;
    child->parent_node = parent;
}

/** Copy a NUL-terminated string into @p arena, or return NULL when @p s is
 *  NULL.  The result is owned by the arena, not the caller. */
static char *arena_str(str_arena *arena, const char *s) {
    return s ? arena_strndup(arena, s, strlen(s)) : NULL;
}

/**
 * Copy the dependency list from a parsed tj_node into a ProjectNode.
 *
 * The ProjectDep array and each path string are carved from @p arena (bulk
 * freed with the snapshot; the array's _Atomic memo cell relies on
 * arena_alloc's 8-byte alignment), and the resolved memo cell is initialized
 * to PROJECT_DEP_UNRESOLVED.
 *
 * @param dst    ProjectNode to receive the copied dependency array.
 * @param src    Source tj_node whose dependencies are copied.
 * @param arena  Arena backing the copied dependency array and path strings.
 */
static void copy_dependencies(ProjectNode *dst, const tj_node *src,
                              str_arena *arena) {
    if (src->num_dependencies <= 0) return;
    dst->dependencies =
        arena_alloc(arena, (size_t)src->num_dependencies * sizeof(ProjectDep));
    for (int i = 0; i < src->num_dependencies; i++) {
        const Dependency *s = &src->dependencies[i];
        ProjectDep       *d = &dst->dependencies[i];
        d->kind            = s->kind;
        d->bang_count      = s->bang_count;
        d->path            = arena_str(arena, s->path);
        d->source_range    = s->source_range;
        atomic_init(&d->resolved, PROJECT_DEP_UNRESOLVED);
    }
    dst->num_dependencies = src->num_dependencies;
}

ProjectNode *project_node_from_tj(const tj_node *src, char *source_uri,
                                  str_arena *arena) {
    if (!src) return NULL;
    /* The node struct itself is arena-backed (like its strings and dep array);
     * only its dynamic children array stays heap-allocated, grown by
     * project_node_append_child and freed by project_node_free_children. */
    ProjectNode *dst = arena_alloc(arena, sizeof(ProjectNode));
    memset(dst, 0, sizeof(*dst));
    dst->keyword         = src->keyword;
    dst->id              = arena_str(arena, src->id);
    dst->name            = arena_str(arena, src->name);
    dst->range           = src->range;
    dst->selection_range = src->selection_range;
    dst->source_uri      = source_uri;   /* borrowed; already interned in arena */
    copy_dependencies(dst, src, arena);
    for (int i = 0; i < src->num_children; i++)
        project_node_append_child(dst, project_node_from_tj(src->children[i],
                                                            source_uri, arena));
    return dst;
}

/* ── Teardown ───────────────────────────────────────────────────────────── */

void project_node_free(ProjectNode *node) {
    if (!node) return;
    project_node_free_children(node);
    /* The node struct, its dependency array, and all of its strings live in the
     * owning workspace_snapshot's node arena (freed in bulk by ws_release).
     * Only the dynamic children array (freed by project_node_free_children) is
     * heap-allocated, so nothing is freed here. */
}

void project_node_free_children(ProjectNode *root) {
    if (!root) return;
    for (int i = 0; i < root->num_children; i++)
        project_node_free(root->children[i]);
    free(root->children);
    root->children         = NULL;
    root->num_children     = 0;
    root->num_children_cap = 0;
}

/* ── Resolution ─────────────────────────────────────────────────────────── */

/**
 * Split a dotted path ("a.b.c") into heap-allocated segments.
 *
 * On success @p *out_segs is a freshly allocated array of @p *out_n
 * heap-allocated strings (one per dot-delimited component).  The caller
 * must free each string and then the array itself, e.g. via free_segs().
 * On failure both outputs are set to NULL / 0.
 *
 * @param path      Dot-delimited identifier path to split; may be NULL or empty.
 * @param out_segs  Receives a pointer to the allocated segment array, or NULL on failure.
 * @param out_n     Receives the number of segments produced.
 */
static void split_dotted_path(const char *path, char ***out_segs, int *out_n) {
    *out_n    = 0;
    *out_segs = NULL;
    if (!path || !path[0]) return;
    int cap = 1;
    for (const char *p = path; *p; p++) if (*p == '.') cap++;
    *out_segs = malloc((size_t)cap * sizeof(char *));
    if (!*out_segs) return;
    char *tmp = strdup(path);
    if (!tmp) { free(*out_segs); *out_segs = NULL; return; }
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save))
        (*out_segs)[(*out_n)++] = strdup(tok);
    free(tmp);
}

/**
 * Free a segment array produced by split_dotted_path().
 *
 * @param segs  Array of heap-allocated strings to free; may be NULL.
 * @param n     Number of valid entries in @p segs.
 */
static void free_segs(char **segs, int n) {
    for (int i = 0; i < n; i++) free(segs[i]);
    free(segs);
}

/**
 * Find the direct KW_TASK child of @p parent whose id matches @p id.
 *
 * @param parent  ProjectNode whose children are searched.
 * @param id      Task identifier string to match.
 * @return Pointer to the matching child, or NULL if not found.
 */
static ProjectNode *child_task(ProjectNode *parent, const char *id) {
    for (int i = 0; i < parent->num_children; i++) {
        ProjectNode *c = parent->children[i];
        if (c->keyword == KW_TASK && c->id && strcmp(c->id, id) == 0)
            return c;
    }
    return NULL;
}

/**
 * Navigate a dotted task path beginning among @p children.
 *
 * The assembled tree holds no project containers (they stay document-local),
 * so this is a straight id-by-id descent over KW_TASK nodes.
 *
 * TODO: only task targets are resolved today.  Valid reference targets
 * could also be accounts, resources, and reports; extending support means
 * capturing those references at parse time (grammar.y currently records
 * only depends/precedes via tj_node_push_dependency) and relaxing this
 * KW_TASK filter.
 *
 * @param children  Array of sibling ProjectNode pointers to search at the first level.
 * @param n         Number of entries in @p children.
 * @param segs      Dot-split path segments produced by split_dotted_path().
 * @param nseg      Number of segments in @p segs.
 * @return Pointer to the matching ProjectNode, or NULL if the path does not resolve.
 */
static ProjectNode *find_task(ProjectNode *const *children, int n,
                              char *const *segs, int nseg) {
    if (nseg == 0 || !segs) return NULL;
    ProjectNode *cur = NULL;
    for (int i = 0; i < n && !cur; i++) {
        ProjectNode *c = children[i];
        if (c->keyword == KW_TASK && c->id && strcmp(c->id, segs[0]) == 0)
            cur = c;
    }
    for (int s = 1; cur && s < nseg; s++)
        cur = child_task(cur, segs[s]);
    return cur;
}

ProjectNode *project_dep_resolve(ProjectNode *owner_task, int dep_index,
                                 ProjectNode *project_root) {
    ProjectDep *dep = &owner_task->dependencies[dep_index];

    uintptr_t cached = atomic_load_explicit(&dep->resolved, memory_order_acquire);
    if (cached != PROJECT_DEP_UNRESOLVED)
        return (cached == PROJECT_DEP_RESOLVED_NULL) ? NULL : (ProjectNode *)cached;

    char **segs = NULL;
    int    nseg = 0;
    split_dotted_path(dep->path, &segs, &nseg);

    ProjectNode *resolved = NULL;
    if (nseg > 0) {
        if (dep->bang_count == 0) {
            /* Absolute: one prefix-applied namespace under the root. */
            resolved = find_task(project_root->children,
                                  project_root->num_children, segs, nseg);
        } else {
            /* Relative: climb one level per bang.  The synthetic root
             * (keyword == 0) or a missing parent is the ceiling; climbing
             * onto/past it escapes the project root and does not resolve. */
            ProjectNode *ancestor = owner_task;
            int escaped = 0;
            for (int b = 0; b < dep->bang_count; b++) {
                ProjectNode *parent = ancestor ? ancestor->parent_node : NULL;
                if (!parent || parent->keyword == 0) { escaped = 1; break; }
                ancestor = parent;
            }
            if (!escaped && ancestor)
                resolved = find_task(ancestor->children,
                                     ancestor->num_children, segs, nseg);
        }
    }

    free_segs(segs, nseg);

    uintptr_t to_publish = resolved ? (uintptr_t)resolved : PROJECT_DEP_RESOLVED_NULL;
    atomic_store_explicit(&dep->resolved, to_publish, memory_order_release);
    return resolved;
}

/* ── Tree navigation / id-namespace classification ───────────────────────── */

NodeKind node_kind_of(int keyword) {
    switch (keyword) {
    case KW_TASK:     return NODE_KIND_TASK;
    case KW_ACCOUNT:  return NODE_KIND_ACCOUNT;
    case KW_RESOURCE:
    case KW_SHIFT:    return NODE_KIND_RESOURCE;
    case KW_PROJECT:  return NODE_KIND_OTHER;
    default:          return NODE_KIND_REPORT;
    }
}

ProjectNode *find_node_by_dotted_path(ProjectNode *start, const char *path,
                                      NodeKind kind) {
    if (!start) return NULL;
    if (!path || !path[0]) return start;

    char *copy = strdup(path);
    if (!copy) return NULL;
    ProjectNode *cur = start;
    char *save = NULL;
    for (char *seg = strtok_r(copy, ".", &save); seg && cur;
         seg = strtok_r(NULL, ".", &save)) {
        ProjectNode *next = NULL;
        for (int i = 0; i < cur->num_children && !next; i++) {
            ProjectNode *child = cur->children[i];
            if (node_kind_of(child->keyword) == kind &&
                child->id && strcmp(child->id, seg) == 0)
                next = child;
        }
        cur = next;
    }
    free(copy);
    return cur;
}

ProjectNode *project_node_for_doc_task(ProjectNode *project_root,
                                       const char *task_prefix,
                                       const tj_node *per_doc_task) {
    if (!project_root || !per_doc_task) return NULL;

    char *qualified_id = sym_qualified_id(per_doc_task);   /* unprefixed in-file path */
    if (!qualified_id || !qualified_id[0]) { free(qualified_id); return NULL; }

    ProjectNode *cur = find_node_by_dotted_path(project_root, task_prefix,
                                                NODE_KIND_TASK);
    char *save = NULL;
    for (char *seg = strtok_r(qualified_id, ".", &save); seg && cur;
         seg = strtok_r(NULL, ".", &save)) {
        ProjectNode *next = NULL;
        for (int i = 0; i < cur->num_children && !next; i++) {
            ProjectNode *child = cur->children[i];
            if (child->keyword == KW_TASK && child->id &&
                strcmp(child->id, seg) == 0)
                next = child;
        }
        cur = next;
    }
    free(qualified_id);
    return cur;
}

ProjectNode *project_resolve_dep_ref(ProjectNode *project_root,
                                     const char *task_prefix,
                                     const tj_node *owner,
                                     const Dependency *dep) {
    ProjectNode *merged_owner =
        project_node_for_doc_task(project_root, task_prefix, owner);
    if (!merged_owner) return NULL;

    int ordinal = (int)(dep - owner->dependencies);
    if (ordinal < 0 || ordinal >= merged_owner->num_dependencies) return NULL;
    return project_dep_resolve(merged_owner, ordinal, project_root);
}
