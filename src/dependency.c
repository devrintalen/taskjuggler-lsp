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

#include "dependency.h"
#include "document_symbol.h"  /* tj_node_at, tj_node_find_path */
#include "grammar.tab.h"      /* KW_TASK, KW_PROJECT */

#include <stdlib.h>
#include <string.h>

/* ── Path helpers ───────────────────────────────────────────────────────── */

/** Split a dotted path ("a.b.c") into heap-allocated segments. */
static void split_dotted_path(const char *path, char ***out_segs, int *out_n) {
    *out_n    = 0;
    *out_segs = NULL;
    if (!path || !path[0]) return;
    int cap = 1;
    for (const char *p = path; *p; p++) if (*p == '.') cap++;
    *out_segs = malloc((size_t)cap * sizeof(char *));
    if (!*out_segs) { return; }
    char *tmp = strdup(path);
    if (!tmp) { free(*out_segs); *out_segs = NULL; return; }
    for (char *tok = strtok(tmp, "."); tok; tok = strtok(NULL, "."))
        (*out_segs)[(*out_n)++] = strdup(tok);
    free(tmp);
}

static void free_segs(char **segs, int n) {
    for (int i = 0; i < n; i++) free(segs[i]);
    free(segs);
}

/** Resolve a dotted task path against @p syms: navigate to the parent
 *  level via tj_node_find_path, then match the final KW_TASK segment.
 *  Project containers are traversed transparently. */
static tj_node *find_task(tj_node *const *syms, int n,
                          char *const *segs, int nseg) {
    if (nseg == 0 || !segs) return NULL;
    int parent_n = 0;
    tj_node *const *parent = tj_node_find_path(
        syms, n, (const char **)segs, nseg - 1, &parent_n);
    if (!parent) return NULL;
    for (int i = 0; i < parent_n; i++) {
        if (parent[i]->keyword == KW_TASK && parent[i]->id &&
                strcmp(parent[i]->id, segs[nseg - 1]) == 0)
            return parent[i];
        if (parent[i]->keyword == KW_PROJECT) {
            tj_node *found = find_task(parent[i]->children,
                                       parent[i]->num_children,
                                       segs + (nseg - 1), 1);
            if (found) return found;
        }
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* TODO: only task targets are resolved today.  Valid reference targets
 * could also be accounts, reports, and resources; extend resolution (and
 * the matching in find_task) to those node kinds. */
tj_node *resolve_dependency(const Dependency *dep, tj_node *owner_task,
                            const ProjectScope *scopes, int num_scopes,
                            int owner_index, const char **out_uri) {
    if (out_uri) *out_uri = NULL;

    char **segs = NULL;
    int    nseg = 0;
    split_dotted_path(dep->path, &segs, &nseg);
    if (nseg == 0) { free(segs); return NULL; }

    tj_node    *resolved = NULL;
    const char *uri      = NULL;

    if (dep->bang_count == 0) {
        /* Absolute: own document first, then the rest of the project. */
        if (owner_index >= 0 && owner_index < num_scopes) {
            resolved = find_task(scopes[owner_index].top,
                                 scopes[owner_index].n, segs, nseg);
            if (resolved) uri = scopes[owner_index].uri;
        }
        for (int p = 0; !resolved && p < num_scopes; p++) {
            if (p == owner_index) continue;
            resolved = find_task(scopes[p].top, scopes[p].n, segs, nseg);
            if (resolved) uri = scopes[p].uri;
        }
    } else {
        /* Relative: climb one level per bang, in-file only.  A synthetic
         * per-kind root (keyword == 0) or a missing parent is the ceiling;
         * climbing onto/past it escapes the document root and does not
         * resolve. */
        tj_node *ancestor = owner_task;
        int escaped = 0;
        for (int b = 0; b < dep->bang_count; b++) {
            tj_node *parent = ancestor ? ancestor->parent_node : NULL;
            if (!parent || parent->keyword == 0) { escaped = 1; break; }
            ancestor = parent;
        }
        if (!escaped && ancestor) {
            resolved = find_task(ancestor->children, ancestor->num_children,
                                 segs, nseg);
            if (resolved && owner_index >= 0 && owner_index < num_scopes)
                uri = scopes[owner_index].uri;
        }
    }

    free_segs(segs, nseg);
    if (out_uri) *out_uri = uri;
    return resolved;
}

static int pos_in_range(LspPos p, LspRange r) {
    int after  = (p.line > r.start.line)
              || (p.line == r.start.line && p.character >= r.start.character);
    int before = (p.line < r.end.line)
              || (p.line == r.end.line && p.character <= r.end.character);
    return after && before;
}

int dependency_at_cursor(const TokenSpan *tokens, int num_tokens,
                         LspPos cursor,
                         tj_node **out_owner, const Dependency **out_dep) {
    for (tj_node *node = tj_node_at(tokens, num_tokens, cursor);
         node != NULL; node = node->parent_node) {
        for (int i = 0; i < node->num_dependencies; i++) {
            if (pos_in_range(cursor, node->dependencies[i].source_range)) {
                if (out_owner) *out_owner = node;
                if (out_dep)   *out_dep   = &node->dependencies[i];
                return 1;
            }
        }
    }
    return 0;
}

tj_node *task_decl_at_cursor(const TokenSpan *tokens, int num_tokens,
                             LspPos cursor) {
    for (tj_node *node = tj_node_at(tokens, num_tokens, cursor);
         node != NULL; node = node->parent_node) {
        if (node->keyword == KW_TASK &&
                pos_in_range(cursor, node->selection_range))
            return node;
    }
    return NULL;
}
