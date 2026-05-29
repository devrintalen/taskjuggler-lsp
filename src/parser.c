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

#include "parser.h"
#include "grammar.tab.h"  /* yyparse(), KW_* constants */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── Page-allocation helpers ─────────────────────────────────────────────── */

static size_t g_page_size;

static void init_page_size(void) {
    if (!g_page_size)
        g_page_size = (size_t)sysconf(_SC_PAGESIZE);
}

static size_t round_up_page(size_t n) {
    init_page_size();
    return (n + g_page_size - 1) & ~(g_page_size - 1);
}

/* Align offset up to the given alignment (must be a power of two). */
#define ALIGN_SECTION(off, align) \
    (((off) + (size_t)(align) - 1) & ~((size_t)(align) - 1))

/* ── flex scanner interface ──────────────────────────────────────────────── *
 *
 * These are declared in the flex-generated lexer.yy.c.  We use void * for
 * YY_BUFFER_STATE to avoid pulling in the full flex header.
 */

typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
extern int             yycolumn;
extern int             yylineno;

/* ── Build-context globals (used by lexer.l and grammar.y via extern) ────── *
 *
 * The build context accumulates parse state during yyparse().  After
 * yyparse() returns, parse_slab_compact() walks these arrays and produces
 * the final parse_slab.
 */

extern void reset_pending_include_state(void);

/* Root of the build tree; grammar.y appends top-level nodes here. */
tj_build_node   *g_build_root     = NULL;
/* IncludeRef accumulator; grammar.y calls push_include_to_build(). */
IncludeRef      *g_build_includes = NULL;
int              g_build_num_includes = 0;
int              g_build_includes_cap = 0;
/* TokenSpan accumulator populated by g_push_tok_span() called from lexer.l. */
tok_span_build  *g_tok_spans      = NULL;
int              g_num_tok_spans  = 0;
int              g_tok_span_cap   = 0;
/** Running upper bound on emitted semantic-token entries (one per source line covered). */
int              g_num_sem_entries = 0;

/**
 * Test whether a token of @p kind is emitted as a semantic token.
 * Mirrors the skip set in classify() in semantic_tokens.c.
 */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/**
 * Append one tok_span_build to the global accumulator.  Called from lexer.l
 * for every token that callers may need to inspect.
 */
void g_push_tok_span(int kind,
                     uint32_t sl, uint32_t sc,
                     uint32_t el, uint32_t ec,
                     const char *text) {
    if (g_num_tok_spans >= g_tok_span_cap) {
        g_tok_span_cap = g_tok_span_cap ? g_tok_span_cap * 2 : 64;
        tok_span_build *tmp = realloc(g_tok_spans,
                                      (size_t)g_tok_span_cap * sizeof(tok_span_build));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_tok_spans = tmp;
    }
    g_tok_spans[g_num_tok_spans++] = (tok_span_build){
        .token_kind = kind,
        .start      = { sl, sc },
        .end        = { el, ec },
        .text       = text ? strdup(text) : NULL,
        .owner      = NULL,
    };
    if (is_sem_highlighted(kind))
        g_num_sem_entries += (int)(el - sl + 1);
}

/* ── Token helpers ───────────────────────────────────────────────────────── */

void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

int parse_tjp_date(const char *text, time_t *out) {
    if (!text) return 0;
    int year, month, day;
    if (sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3) return 0;
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    *out = t;
    return 1;
}

/* ── tj_build_node helpers ───────────────────────────────────────────────── */

void tj_build_node_free(tj_build_node *n) {
    if (!n) return;
    free(n->id);
    free(n->name);
    for (int i = 0; i < n->num_dependencies; i++)
        free(n->dependencies[i].path);
    free(n->dependencies);
    for (int i = 0; i < n->num_children; i++)
        tj_build_node_free(n->children[i]);
    free(n->children);
    free(n);
}

void tj_build_node_append_child(tj_build_node *parent, tj_build_node *child) {
    if (parent->num_children >= parent->children_cap) {
        int nc = parent->children_cap ? parent->children_cap * 2 : 4;
        tj_build_node **tmp = realloc(parent->children,
                                      (size_t)nc * sizeof(tj_build_node *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        parent->children     = tmp;
        parent->children_cap = nc;
    }
    parent->children[parent->num_children++] = child;
    child->parent_node = parent;
}

void tj_build_node_push_dep(tj_build_node *task, dep_build dep) {
    if (task->num_dependencies >= task->dependencies_cap) {
        int nc = task->dependencies_cap ? task->dependencies_cap * 2 : 4;
        dep_build *tmp = realloc(task->dependencies,
                                  (size_t)nc * sizeof(dep_build));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        task->dependencies     = tmp;
        task->dependencies_cap = nc;
    }
    dep.resolved_target = NULL;
    task->dependencies[task->num_dependencies++] = dep;
}

/* ── Build-context helpers ───────────────────────────────────────────────── */

static tj_build_node *alloc_build_root(void) {
    tj_build_node *n = calloc(1, sizeof(tj_build_node));
    if (!n) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    return n;
}

void push_include_to_build(const char *quoted_text,
                           const char *task_prefix,
                           const char *resource_prefix,
                           const char *account_prefix,
                           const char *report_prefix) {
    if (!quoted_text) return;
    size_t len = strlen(quoted_text);
    const char *inner = quoted_text;
    size_t inner_len  = len;
    if (len >= 2 && (quoted_text[0] == '"' || quoted_text[0] == '\'')) {
        inner     = quoted_text + 1;
        inner_len = len - 2;
    }
    char *filename = malloc(inner_len + 1);
    if (!filename) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(filename, inner, inner_len);
    filename[inner_len] = '\0';

    if (g_build_num_includes >= g_build_includes_cap) {
        int nc = g_build_includes_cap ? g_build_includes_cap * 2 : 4;
        IncludeRef *tmp = realloc(g_build_includes, (size_t)nc * sizeof(IncludeRef));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_build_includes     = tmp;
        g_build_includes_cap = nc;
    }
    IncludeRef *e = &g_build_includes[g_build_num_includes++];
    e->filename        = filename;
    e->task_prefix     = task_prefix     ? strdup(task_prefix)     : NULL;
    e->resource_prefix = resource_prefix ? strdup(resource_prefix) : NULL;
    e->account_prefix  = account_prefix  ? strdup(account_prefix)  : NULL;
    e->report_prefix   = report_prefix   ? strdup(report_prefix)   : NULL;
}

/* ── Parent-link assignment on the build tree ────────────────────────────── */

static void assign_parent_links_build(tj_build_node *parent,
                                      tj_build_node **children, int n,
                                      tj_build_node *doc_root) {
    for (int i = 0; i < n; i++) {
        children[i]->parent_node = parent;
        children[i]->parent_doc  = doc_root;
        assign_parent_links_build(children[i], children[i]->children,
                                  children[i]->num_children, NULL);
    }
}

/* ── Token-owner assignment on the build tree ────────────────────────────── */

typedef struct {
    tj_build_node **children;
    int             n;
    int             idx;
    tj_build_node  *scope;
} owner_frame;

static int compare_build_node_starts(const void *a, const void *b) {
    const tj_build_node *na = *(const tj_build_node *const *)a;
    const tj_build_node *nb = *(const tj_build_node *const *)b;
    return pos_cmp(na->range.start, nb->range.start);
}

static int range_within(LspRange inner, LspRange outer) {
    return pos_cmp(inner.start, outer.start) >= 0 &&
           pos_cmp(inner.end, outer.end) <= 0;
}

static void assign_token_owners_build(tj_build_node *root,
                                      tok_span_build *spans, int num_spans) {
    if (!root) return;

    tj_build_node *project = NULL;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i]->keyword == KW_PROJECT) {
            project = root->children[i];
            break;
        }

    int hoisted_n = root->num_children - (project ? 1 : 0);
    if (hoisted_n < 0) hoisted_n = 0;
    tj_build_node **hoisted = hoisted_n
        ? malloc((size_t)hoisted_n * sizeof(tj_build_node *))
        : NULL;
    if (hoisted_n && !hoisted) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    int h = 0;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i] != project) hoisted[h++] = root->children[i];

    tj_build_node **top = malloc((size_t)(hoisted_n + 1) * sizeof(tj_build_node *));
    if (!top) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int top_n = 0;
    tj_build_node **proj_kids = NULL;
    int              proj_kids_n = 0;

    if (project) {
        proj_kids = hoisted_n
            ? malloc((size_t)hoisted_n * sizeof(tj_build_node *))
            : NULL;
        if (hoisted_n && !proj_kids) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        for (int i = 0; i < hoisted_n; i++) {
            if (range_within(hoisted[i]->range, project->range))
                proj_kids[proj_kids_n++] = hoisted[i];
            else
                top[top_n++] = hoisted[i];
        }
        top[top_n++] = project;
        if (proj_kids_n > 1)
            qsort(proj_kids, (size_t)proj_kids_n,
                  sizeof(tj_build_node *), compare_build_node_starts);
    } else {
        for (int i = 0; i < hoisted_n; i++) top[top_n++] = hoisted[i];
    }
    if (top_n > 1)
        qsort(top, (size_t)top_n, sizeof(tj_build_node *), compare_build_node_starts);

    int frame_cap = 64;
    owner_frame *stack = malloc((size_t)frame_cap * sizeof(owner_frame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 1;
    stack[0] = (owner_frame){ top, top_n, 0, NULL };

    for (int t = 0; t < num_spans; t++) {
        LspPos pos = spans[t].start;

        while (depth > 1 &&
               pos_cmp(pos, stack[depth - 1].scope->range.end) >= 0)
            depth--;

        for (;;) {
            owner_frame *top_frame = &stack[depth - 1];

            while (top_frame->idx < top_frame->n &&
                   pos_cmp(pos, top_frame->children[top_frame->idx]->range.end) >= 0)
                top_frame->idx++;
            if (top_frame->idx >= top_frame->n) break;

            tj_build_node *child = top_frame->children[top_frame->idx];
            if (pos_cmp(pos, child->range.start) < 0)
                break;

            if (depth >= frame_cap) {
                frame_cap *= 2;
                owner_frame *tmp = realloc(stack,
                                           (size_t)frame_cap * sizeof(owner_frame));
                if (!tmp) {
                    fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
                }
                stack = tmp;
            }
            tj_build_node **kids  = (child == project) ? proj_kids : child->children;
            int             nkids = (child == project) ? proj_kids_n : child->num_children;
            stack[depth++] = (owner_frame){ kids, nkids, 0, child };
        }

        spans[t].owner = (depth > 1) ? stack[depth - 1].scope : NULL;
    }

    free(stack);
    free(top);
    free(proj_kids);
    free(hoisted);
}

/* ── parse_slab_compact() ────────────────────────────────────────────────── *
 *
 * Convert the pointer-based build tree into a flat parse_slab.
 *
 * Strategy (two-pass):
 *   Pass 1 — DFS over the build tree: count nodes, total children-array
 *             entries, total dep entries, and total string bytes.
 *   Pass 2 — allocate flat arrays, copy data, replace pointers with
 *             indices and string-pool offsets.
 */

/* ── String pool builder ── */

typedef struct {
    char  *buf;
    size_t size;
    size_t cap;
} str_pool;

static void str_pool_init(str_pool *p) {
    p->cap  = 256;
    p->buf  = malloc(p->cap);
    if (!p->buf) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    p->buf[0] = '\0';   /* offset 0 reserved as NULL sentinel */
    p->size = 1;
}

static str_off str_pool_push(str_pool *p, const char *s) {
    if (!s || !s[0]) return 0;
    size_t len = strlen(s) + 1;
    while (p->size + len > p->cap) {
        p->cap *= 2;
        char *tmp = realloc(p->buf, p->cap);
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        p->buf = tmp;
    }
    str_off off = (str_off)p->size;
    memcpy(p->buf + p->size, s, len);
    p->size += len;
    return off;
}

/* ── DFS node-numbering ── */

typedef struct {
    tj_build_node *build_ptr;
    tj_idx         index;
} build_idx_pair;

typedef struct {
    build_idx_pair *pairs;
    int             count;
    int             cap;
} build_idx_map;

static void idx_map_push(build_idx_map *m, tj_build_node *ptr, tj_idx idx) {
    if (m->count >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->pairs = realloc(m->pairs, (size_t)m->cap * sizeof(build_idx_pair));
        if (!m->pairs) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    }
    m->pairs[m->count].build_ptr = ptr;
    m->pairs[m->count].index     = idx;
    m->count++;
}

static int idx_map_pair_cmp(const void *a, const void *b) {
    const build_idx_pair *pa = (const build_idx_pair *)a;
    const build_idx_pair *pb = (const build_idx_pair *)b;
    if (pa->build_ptr < pb->build_ptr) return -1;
    if (pa->build_ptr > pb->build_ptr) return  1;
    return 0;
}

/* Sort the map in place so subsequent bsearch lookups are O(log N). */
static void idx_map_sort(build_idx_map *m) {
    qsort(m->pairs, (size_t)m->count, sizeof(build_idx_pair), idx_map_pair_cmp);
}

static tj_idx idx_map_lookup(const build_idx_map *m, const tj_build_node *ptr) {
    if (!ptr) return -1;
    build_idx_pair key;
    key.build_ptr = (tj_build_node *)ptr;
    key.index     = 0;
    const build_idx_pair *found = bsearch(&key, m->pairs, (size_t)m->count,
                                          sizeof(build_idx_pair), idx_map_pair_cmp);
    return found ? found->index : -1;
}

/* Count nodes in a subtree. */
static int count_nodes(const tj_build_node *n) {
    if (!n) return 0;
    int total = 1;
    for (int i = 0; i < n->num_children; i++)
        total += count_nodes(n->children[i]);
    return total;
}

/* DFS walk: assign sequential indices and populate flat arrays. */
static void dfs_compact(const tj_build_node *src, tj_idx node_idx,
                        tj_node *nodes,
                        tj_idx *children_arr, int *children_cursor,
                        Dependency *deps_arr, int *deps_cursor,
                        str_pool *pool,
                        build_idx_map *idx_map) {
    /* We already have the index for this node (pre-assigned by caller). */
    tj_node *dst = &nodes[node_idx];
    dst->keyword   = src->keyword;
    dst->id_off    = str_pool_push(pool, src->id);
    dst->name_off  = str_pool_push(pool, src->name);
    dst->range             = src->range;
    dst->selection_range   = src->selection_range;
    dst->start_date        = src->start_date;
    dst->end_date          = src->end_date;
    dst->has_start         = src->has_start;
    dst->has_end           = src->has_end;
    /* parent_node / parent_doc: filled in pass-2 fixup after all nodes are numbered */
    dst->parent_node = -1;
    dst->parent_doc  = -1;

    /* Dependencies */
    if (src->num_dependencies > 0) {
        dst->dep_start        = (tj_idx)*deps_cursor;
        dst->num_dependencies = src->num_dependencies;
        for (int i = 0; i < src->num_dependencies; i++) {
            const dep_build *sd = &src->dependencies[i];
            Dependency *dd = &deps_arr[(*deps_cursor)++];
            dd->kind         = sd->kind;
            dd->bang_count   = sd->bang_count;
            dd->path_off     = str_pool_push(pool, sd->path);
            dd->source_range = sd->source_range;
            dd->resolved_idx = -1;
        }
    } else {
        dst->dep_start        = -1;
        dst->num_dependencies = 0;
    }

    /* Children: record their indices, recurse */
    if (src->num_children > 0) {
        dst->children_start = (tj_idx)*children_cursor;
        dst->num_children   = src->num_children;

        /* First pass: compute each subtree size once and record the child's
         * flat index into the shared children array.  Sizes are saved so the
         * second pass can advance cur without calling count_nodes again. */
        int *sizes = malloc((size_t)src->num_children * sizeof(int));
        if (!sizes) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        tj_idx cur = node_idx + 1;
        for (int i = 0; i < src->num_children; i++) {
            children_arr[(*children_cursor)++] = cur;
            sizes[i] = count_nodes(src->children[i]);
            cur += sizes[i];
        }
        /* Second pass: recurse with the pre-assigned indices, reusing sizes. */
        cur = node_idx + 1;
        for (int i = 0; i < src->num_children; i++) {
            idx_map_push(idx_map, src->children[i], cur);
            dfs_compact(src->children[i], cur, nodes,
                        children_arr, children_cursor,
                        deps_arr, deps_cursor, pool, idx_map);
            cur += sizes[i];
        }
        free(sizes);
    } else {
        dst->children_start = -1;
        dst->num_children   = 0;
    }
}

static parse_slab *parse_slab_compact(tj_build_node *build_root,
                                      tok_span_build *build_spans,
                                      int             num_build_spans,
                                      int             num_sem_entries,
                                      IncludeRef     *includes,
                                      int             num_includes) {
    int num_nodes = count_nodes(build_root);

    /* Count totals */
    int total_children = 0;
    int total_deps     = 0;
    {
        /* BFS/DFS count */
        tj_build_node **stack = malloc((size_t)num_nodes * sizeof(tj_build_node *));
        if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        int sp = 0;
        stack[sp++] = build_root;
        while (sp > 0) {
            tj_build_node *n = stack[--sp];
            total_children += n->num_children;
            total_deps     += n->num_dependencies;
            for (int i = 0; i < n->num_children; i++)
                stack[sp++] = n->children[i];
        }
        free(stack);
    }

    /* Allocate flat arrays */
    tj_node    *nodes    = calloc((size_t)num_nodes, sizeof(tj_node));
    tj_idx     *children = total_children
        ? malloc((size_t)total_children * sizeof(tj_idx)) : NULL;
    Dependency *deps     = total_deps
        ? malloc((size_t)total_deps * sizeof(Dependency)) : NULL;
    if (!nodes) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    if (total_children && !children) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    if (total_deps && !deps) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }

    str_pool pool;
    str_pool_init(&pool);

    build_idx_map idx_map = {0};
    idx_map_push(&idx_map, build_root, 0);

    int children_cursor = 0;
    int deps_cursor     = 0;
    dfs_compact(build_root, 0, nodes,
                children, &children_cursor,
                deps, &deps_cursor, &pool, &idx_map);

    /* Sort the map by pointer so pass-2 lookups are O(log N) via bsearch. */
    idx_map_sort(&idx_map);

    /* Pass 2: wire parent_node and parent_doc using the idx_map */
    for (int i = 0; i < idx_map.count; i++) {
        const tj_build_node *bn  = idx_map.pairs[i].build_ptr;
        tj_idx               idx = idx_map.pairs[i].index;
        nodes[idx].parent_node = idx_map_lookup(&idx_map, bn->parent_node);
        nodes[idx].parent_doc  = idx_map_lookup(&idx_map, bn->parent_doc);
    }

    /* Build TokenSpan slab entries */
    TokenSpan *tok_spans = num_build_spans
        ? malloc((size_t)num_build_spans * sizeof(TokenSpan)) : NULL;
    if (num_build_spans && !tok_spans) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    for (int i = 0; i < num_build_spans; i++) {
        tok_spans[i].token_kind = build_spans[i].token_kind;
        tok_spans[i].start      = build_spans[i].start;
        tok_spans[i].end        = build_spans[i].end;
        tok_spans[i].text_off   = str_pool_push(&pool, build_spans[i].text);
        tok_spans[i].owner_idx  = idx_map_lookup(&idx_map, build_spans[i].owner);
    }

    free(idx_map.pairs);

    /* ── Pack everything into a single mmap-backed page ── */

    /* Compute section offsets with natural alignment. */
    size_t off = sizeof(parse_page_header);
    off = ALIGN_SECTION(off, _Alignof(tj_node));
    size_t nodes_off = off;
    off += (size_t)num_nodes * sizeof(tj_node);

    off = ALIGN_SECTION(off, _Alignof(tj_idx));
    size_t children_off = off;
    off += (size_t)children_cursor * sizeof(tj_idx);

    off = ALIGN_SECTION(off, _Alignof(Dependency));
    size_t deps_off = off;
    off += (size_t)deps_cursor * sizeof(Dependency);

    off = ALIGN_SECTION(off, _Alignof(TokenSpan));
    size_t spans_off = off;
    off += (size_t)num_build_spans * sizeof(TokenSpan);

    size_t strings_off = off;
    off += pool.size;

    size_t total_size = round_up_page(off);

    void *page = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        fprintf(stderr, "taskjuggler-lsp: mmap failed\n"); exit(1);
    }

    parse_page_header *hdr = (parse_page_header *)page;
    hdr->total_mmap_size = total_size;

    memcpy((char *)page + nodes_off,   nodes,     (size_t)num_nodes      * sizeof(tj_node));
    if (children_cursor)
        memcpy((char *)page + children_off, children, (size_t)children_cursor * sizeof(tj_idx));
    if (deps_cursor)
        memcpy((char *)page + deps_off,    deps,      (size_t)deps_cursor     * sizeof(Dependency));
    if (num_build_spans)
        memcpy((char *)page + spans_off,   tok_spans, (size_t)num_build_spans  * sizeof(TokenSpan));
    memcpy((char *)page + strings_off, pool.buf,  pool.size);

    /* Free the temporary malloc'd intermediate arrays */
    free(nodes);
    free(children);
    free(deps);
    free(tok_spans);
    free(pool.buf);

    /* Assemble the slab, pointing into the page interior */
    parse_slab *slab = calloc(1, sizeof(parse_slab));
    if (!slab) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    slab->page                = hdr;
    slab->nodes               = (tj_node    *)((char *)page + nodes_off);
    slab->num_nodes           = num_nodes;
    slab->children            = children_cursor
        ? (tj_idx      *)((char *)page + children_off) : NULL;
    slab->num_children_entries = children_cursor;
    slab->deps                = deps_cursor
        ? (Dependency  *)((char *)page + deps_off) : NULL;
    slab->num_deps            = deps_cursor;
    slab->tok_spans           = num_build_spans
        ? (TokenSpan   *)((char *)page + spans_off) : NULL;
    slab->num_tok_spans       = num_build_spans;
    slab->num_sem_entries     = num_sem_entries;
    slab->strings             = (char *)page + strings_off;
    slab->strings_size        = pool.size;
    slab->root_idx            = 0;
    slab->includes            = includes;
    slab->num_includes        = num_includes;

    return slab;
}

/* ── parse_slab_free() ───────────────────────────────────────────────────── */

void parse_slab_free(parse_slab *slab) {
    if (!slab) return;
    if (slab->page) {
        munmap(slab->page, slab->page->total_mmap_size);
    } else {
        /* Fallback for any slab built without the mmap path. */
        free(slab->nodes);
        free(slab->children);
        free(slab->deps);
        free(slab->tok_spans);
        free(slab->strings);
    }
    for (int i = 0; i < slab->num_includes; i++) {
        free(slab->includes[i].filename);
        free(slab->includes[i].task_prefix);
        free(slab->includes[i].resource_prefix);
        free(slab->includes[i].account_prefix);
        free(slab->includes[i].report_prefix);
    }
    free(slab->includes);
    free(slab);
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

parse_slab *parse(const char *src) {
    /* Initialize build-phase globals */
    g_build_root          = alloc_build_root();
    g_build_includes      = NULL;
    g_build_num_includes  = 0;
    g_build_includes_cap  = 0;
    g_tok_spans           = NULL;
    g_num_tok_spans       = 0;
    g_tok_span_cap        = 0;
    g_num_sem_entries     = 0;
    yycolumn              = 0;
    yylineno              = 1;
    reset_pending_include_state();

    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    /* Sort root children by source position */
    if (g_build_root->num_children > 1)
        qsort(g_build_root->children, (size_t)g_build_root->num_children,
              sizeof(tj_build_node *), compare_build_node_starts);
    assign_parent_links_build(g_build_root, g_build_root->children,
                              g_build_root->num_children, g_build_root);
    assign_token_owners_build(g_build_root, g_tok_spans, g_num_tok_spans);

    /* Compact into slab */
    parse_slab *slab = parse_slab_compact(g_build_root,
                                          g_tok_spans,
                                          g_num_tok_spans,
                                          g_num_sem_entries,
                                          g_build_includes,
                                          g_build_num_includes);

    /* Free the build context */
    tj_build_node_free(g_build_root);
    for (int i = 0; i < g_num_tok_spans; i++)
        free(g_tok_spans[i].text);
    free(g_tok_spans);
    /* Note: g_build_includes is now owned by slab (no free here) */

    g_build_root          = NULL;
    g_build_includes      = NULL;
    g_build_num_includes  = 0;
    g_build_includes_cap  = 0;
    g_tok_spans           = NULL;
    g_num_tok_spans       = 0;
    g_tok_span_cap        = 0;
    g_num_sem_entries     = 0;

    return slab;
}
