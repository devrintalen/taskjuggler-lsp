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
#include "grammar.tab.h"   /* KW_TASK */

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

/**
 * Copy the dependency list from a parsed tj_node into a ProjectNode.
 *
 * Each Dependency entry is deep-copied: the path string is duplicated and the
 * resolved pointer is initialized to PROJECT_DEP_UNRESOLVED.
 *
 * @param dst  ProjectNode to receive the copied dependency array.
 * @param src  Source tj_node whose dependencies are copied.
 */
static void copy_dependencies(ProjectNode *dst, const tj_node *src) {
    if (src->num_dependencies <= 0) return;
    dst->dependencies = calloc((size_t)src->num_dependencies, sizeof(ProjectDep));
    if (!dst->dependencies) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    for (int i = 0; i < src->num_dependencies; i++) {
        const Dependency *s = &src->dependencies[i];
        ProjectDep       *d = &dst->dependencies[i];
        d->kind            = s->kind;
        d->bang_count      = s->bang_count;
        d->path            = s->path ? strdup(s->path) : NULL;
        d->source_range    = s->source_range;
        atomic_init(&d->resolved, PROJECT_DEP_UNRESOLVED);
    }
    dst->num_dependencies = src->num_dependencies;
}

ProjectNode *project_node_from_tj(const tj_node *src, const char *source_uri) {
    if (!src) return NULL;
    ProjectNode *dst = calloc(1, sizeof(ProjectNode));
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    dst->keyword         = src->keyword;
    dst->id              = src->id   ? strdup(src->id)   : NULL;
    dst->name            = src->name ? strdup(src->name) : NULL;
    dst->range           = src->range;
    dst->selection_range = src->selection_range;
    dst->source_uri      = source_uri ? strdup(source_uri) : NULL;
    copy_dependencies(dst, src);
    for (int i = 0; i < src->num_children; i++)
        project_node_append_child(dst, project_node_from_tj(src->children[i],
                                                            source_uri));
    return dst;
}

/* ── Teardown ───────────────────────────────────────────────────────────── */

void project_node_free(ProjectNode *node) {
    if (!node) return;
    project_node_free_children(node);
    free(node->id);
    free(node->name);
    free(node->source_uri);
    for (int i = 0; i < node->num_dependencies; i++)
        free(node->dependencies[i].path);
    free(node->dependencies);
    free(node);
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
