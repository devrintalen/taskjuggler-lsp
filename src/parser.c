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

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

/** Currently-being-built ParseOutput; lexer/grammar populate this directly. */
/* Defined in grammar.y.  Called at the start of every parse() so that a
 * partial include-body parse from a previous run cannot leak its pending
 * prefix strings into the next parse. */
extern void reset_pending_include_state(void);

ParseOutput *g_output         = NULL;
/** Backing storage for the token-span array under construction. */
TokenSpan   *g_tok_spans      = NULL;
int          g_num_tok_spans  = 0;
int          g_tok_span_cap   = 0;
/** Running upper bound on emitted semantic-token entries (one per source line covered). */
int          g_num_sem_entries = 0;

/**
 * Test whether a token of @p kind is emitted as a semantic token.
 * Mirrors the skip set in classify() in semantic_tokens.c.
 */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/**
 * Append one TokenSpan to the global accumulator.  Called from lexer.l for
 * every token that callers may need to inspect.
 */
void g_push_tok_span(int kind,
                     uint32_t sl, uint32_t sc,
                     uint32_t el, uint32_t ec,
                     const char *text) {
    if (g_num_tok_spans >= g_tok_span_cap) {
        g_tok_span_cap = g_tok_span_cap ? g_tok_span_cap * 2 : 64;
        TokenSpan *tmp = realloc(g_tok_spans,
                                 (size_t)g_tok_span_cap * sizeof(TokenSpan));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_tok_spans = tmp;
    }
    g_tok_spans[g_num_tok_spans++] = (TokenSpan){
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

/* ── tj_node helpers ─────────────────────────────────────────────────────── */

void tj_node_free(tj_node *n) {
    if (!n) return;
    free(n->id);
    free(n->name);
    for (int i = 0; i < n->num_dependencies; i++)
        free(n->dependencies[i].path);
    free(n->dependencies);
    for (int i = 0; i < n->num_children; i++)
        tj_node_free(n->children[i]);
    free(n->children);
    free(n);
}

void tj_node_append_child(tj_node *parent, tj_node *child) {
    if (parent->num_children >= parent->children_cap) {
        int nc = parent->children_cap ? parent->children_cap * 2 : 4;
        tj_node **tmp = realloc(parent->children, (size_t)nc * sizeof(tj_node *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        parent->children     = tmp;
        parent->children_cap = nc;
    }
    parent->children[parent->num_children++] = child;
    child->parent_node = parent;
}

void tj_node_push_dependency(tj_node *task, Dependency dep) {
    if (task->num_dependencies >= task->dependencies_cap) {
        int nc = task->dependencies_cap ? task->dependencies_cap * 2 : 4;
        Dependency *tmp = realloc(task->dependencies,
                                  (size_t)nc * sizeof(Dependency));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        task->dependencies     = tmp;
        task->dependencies_cap = nc;
    }
    dep.resolved_target = NULL;
    task->dependencies[task->num_dependencies++] = dep;
}

/* ── tj_node deep copy ───────────────────────────────────────────────────── *
 *
 * Used by the query-context clone path to give each query worker a private,
 * relocatable copy of a document's tj_node tree (see query_context.c).
 */

static void tj_node_map_push(tj_node_map *map, const tj_node *from, tj_node *to) {
    if (!map) return;
    if (map->count >= map->cap) {
        int nc = map->cap ? map->cap * 2 : 64;
        tj_node_map_entry *tmp = realloc(map->entries,
                                         (size_t)nc * sizeof(tj_node_map_entry));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        map->entries = tmp;
        map->cap     = nc;
    }
    map->entries[map->count++] = (tj_node_map_entry){ .from = from, .to = to };
}

static int compare_map_entries(const void *a, const void *b) {
    const tj_node *fa = ((const tj_node_map_entry *)a)->from;
    const tj_node *fb = ((const tj_node_map_entry *)b)->from;
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

static tj_node *tj_node_copy_rec(const tj_node *src, tj_node *parent,
                                 tj_node_map *map) {
    if (!src) return NULL;
    tj_node *dst = calloc(1, sizeof(tj_node));
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    dst->keyword         = src->keyword;
    dst->id              = src->id   ? strdup(src->id)   : NULL;
    dst->name            = src->name ? strdup(src->name) : NULL;
    dst->range           = src->range;
    dst->selection_range = src->selection_range;
    dst->start_date      = src->start_date;
    dst->end_date        = src->end_date;
    dst->has_start       = src->has_start;
    dst->has_end         = src->has_end;
    dst->parent_node     = parent;

    if (src->num_dependencies > 0) {
        dst->dependencies = calloc((size_t)src->num_dependencies, sizeof(Dependency));
        if (!dst->dependencies) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        for (int i = 0; i < src->num_dependencies; i++) {
            const Dependency *s = &src->dependencies[i];
            Dependency       *d = &dst->dependencies[i];
            d->kind            = s->kind;
            d->bang_count      = s->bang_count;
            d->path            = s->path ? strdup(s->path) : NULL;
            d->source_range    = s->source_range;
            d->resolved_target = NULL;
        }
        dst->num_dependencies = src->num_dependencies;
        dst->dependencies_cap = src->num_dependencies;
    }

    if (src->num_children > 0) {
        dst->children = malloc((size_t)src->num_children * sizeof(tj_node *));
        if (!dst->children) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        dst->children_cap = src->num_children;
        for (int i = 0; i < src->num_children; i++)
            dst->children[dst->num_children++] =
                tj_node_copy_rec(src->children[i], dst, map);
    }

    tj_node_map_push(map, src, dst);
    return dst;
}

tj_node *tj_node_deep_copy(const tj_node *src, tj_node_map *map) {
    tj_node *root = tj_node_copy_rec(src, NULL, map);
    /* Mirror assign_parent_links(): the synthetic root's direct children are
     * the document's top-level entries and carry parent_doc == root. */
    if (root)
        for (int i = 0; i < root->num_children; i++)
            root->children[i]->parent_doc = root;
    if (map && map->count > 1)
        qsort(map->entries, (size_t)map->count, sizeof(tj_node_map_entry),
              compare_map_entries);
    return root;
}

tj_node *tj_node_map_lookup(const tj_node_map *map, const tj_node *from) {
    if (!map || !from || map->count == 0) return NULL;
    int lo = 0, hi = map->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const tj_node *f = map->entries[mid].from;
        if (f == from) return map->entries[mid].to;
        if (f < from) lo = mid + 1;
        else          hi = mid - 1;
    }
    return NULL;
}

void tj_node_map_free(tj_node_map *map) {
    if (!map) return;
    free(map->entries);
    map->entries = NULL;
    map->count   = 0;
    map->cap     = 0;
}

/* ── ParseOutput helpers ─────────────────────────────────────────────────── */

static tj_node *alloc_synthetic_root(void) {
    tj_node *n = calloc(1, sizeof(tj_node));
    if (!n) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    return n;
}

void parse_output_free(ParseOutput *po) {
    if (!po) return;
    tj_node_free(po->root);

    for (int i = 0; i < po->num_tok_spans; i++)
        free(po->tok_spans[i].text);
    free(po->tok_spans);

    for (int i = 0; i < po->num_includes; i++) {
        free(po->includes[i].filename);
        free(po->includes[i].task_prefix);
        free(po->includes[i].resource_prefix);
        free(po->includes[i].account_prefix);
        free(po->includes[i].report_prefix);
    }
    free(po->includes);

    free(po);
}

void push_include(ParseOutput *po, const char *quoted_text,
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

    if (po->num_includes >= po->includes_cap) {
        int nc = po->includes_cap ? po->includes_cap * 2 : 4;
        IncludeRef *tmp = realloc(po->includes, (size_t)nc * sizeof(IncludeRef));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        po->includes     = tmp;
        po->includes_cap = nc;
    }
    IncludeRef *e = &po->includes[po->num_includes++];
    e->filename        = filename;
    e->task_prefix     = task_prefix     ? strdup(task_prefix)     : NULL;
    e->resource_prefix = resource_prefix ? strdup(resource_prefix) : NULL;
    e->account_prefix  = account_prefix  ? strdup(account_prefix)  : NULL;
    e->report_prefix   = report_prefix   ? strdup(report_prefix)   : NULL;
}

/* ── tj_node tree linkage ────────────────────────────────────────────────── *
 *
 * After the grammar finishes building the trees, we walk each per-kind
 * root to set parent_node / parent_doc pointers consistently, and to
 * assign owners to every TokenSpan.
 */

/** Recursively set parent_node and parent_doc fields across one subtree. */
static void assign_parent_links(tj_node *parent, tj_node **children, int n,
                                tj_node *doc_root) {
    /* doc_root is non-NULL only for the top-level frame (children directly
     * under a per-kind synthetic root).  Once we recurse into a real child,
     * deeper descendants live INSIDE that child, so their parent_doc must
     * be NULL — they are no longer top-level entries. */
    for (int i = 0; i < n; i++) {
        children[i]->parent_node = parent;
        children[i]->parent_doc  = doc_root;
        assign_parent_links(children[i], children[i]->children,
                            children[i]->num_children, NULL);
    }
}

/**
 * Walk the four per-kind trees in source order and assign each TokenSpan
 * its innermost-enclosing tj_node as `owner`.
 *
 * Strategy: take root's top-level children (already in source order),
 * then use the standard single-pass scope-stack algorithm to assign
 * owners.
 */
typedef struct {
    tj_node **children;
    int       n;
    int       idx;
    tj_node  *scope;
} OwnerFrame;

static int compare_node_starts(const void *a, const void *b) {
    const tj_node *na = *(const tj_node *const *)a;
    const tj_node *nb = *(const tj_node *const *)b;
    return pos_cmp(na->range.start, nb->range.start);
}

static int range_within(LspRange inner, LspRange outer) {
    return pos_cmp(inner.start, outer.start) >= 0 &&
           pos_cmp(inner.end, outer.end) <= 0;
}

static void assign_token_owners(ParseOutput *po) {
    tj_node *root = po->root;
    if (!root) return;

    /* The project block (if any) is a top-level child like the rest, but
     * its body declarations were hoisted to siblings under root (the
     * project node keeps no children) while its range still spans them.
     * For ownership we must restore that nesting: any sibling whose range
     * falls within the project's range becomes a scope-child of the
     * project, so tokens inside it resolve to the node rather than
     * dead-ending on the childless project.  Siblings outside the
     * project's range (declared after the project block) stay as true
     * top-level entries. */
    tj_node *project = NULL;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i]->keyword == KW_PROJECT) {
            project = root->children[i];
            break;
        }

    /* hoisted = every top-level child except the project node itself.
     * (When a project node is present num_children is at least 1, so this
     * cannot go negative; the clamp makes that explicit for the size
     * computations below.) */
    int hoisted_n = root->num_children - (project ? 1 : 0);
    if (hoisted_n < 0) hoisted_n = 0;
    tj_node **hoisted = hoisted_n
        ? malloc((size_t)hoisted_n * sizeof(tj_node *))
        : NULL;
    if (hoisted_n && !hoisted) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    int h = 0;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i] != project) hoisted[h++] = root->children[i];

    /* Top-level scope frame plus the project's synthetic scope-children. */
    tj_node **top = malloc((size_t)(hoisted_n + 1) * sizeof(tj_node *));
    if (!top) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int top_n = 0;
    tj_node **proj_kids = NULL;
    int       proj_kids_n = 0;

    if (project) {
        proj_kids = hoisted_n
            ? malloc((size_t)hoisted_n * sizeof(tj_node *))
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
            qsort(proj_kids, (size_t)proj_kids_n, sizeof(tj_node *), compare_node_starts);
    } else {
        for (int i = 0; i < hoisted_n; i++) top[top_n++] = hoisted[i];
    }
    if (top_n > 1)
        qsort(top, (size_t)top_n, sizeof(tj_node *), compare_node_starts);

    int frame_cap = 64;
    OwnerFrame *stack = malloc((size_t)frame_cap * sizeof(OwnerFrame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 1;
    stack[0] = (OwnerFrame){ top, top_n, 0, NULL };

    for (int t = 0; t < po->num_tok_spans; t++) {
        LspPos pos = po->tok_spans[t].start;

        while (depth > 1 &&
               pos_cmp(pos, stack[depth - 1].scope->range.end) >= 0)
            depth--;

        for (;;) {
            OwnerFrame *top_frame = &stack[depth - 1];

            while (top_frame->idx < top_frame->n &&
                   pos_cmp(pos, top_frame->children[top_frame->idx]->range.end) >= 0)
                top_frame->idx++;
            if (top_frame->idx >= top_frame->n) break;

            tj_node *child = top_frame->children[top_frame->idx];
            if (pos_cmp(pos, child->range.start) < 0)
                break;

            if (depth >= frame_cap) {
                frame_cap *= 2;
                OwnerFrame *tmp = realloc(stack, (size_t)frame_cap * sizeof(OwnerFrame));
                if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
                stack = tmp;
            }
            /* The project node carries its hoisted body decls as
             * scope-children here even though its own `children` array is
             * empty. */
            tj_node **kids = (child == project) ? proj_kids : child->children;
            int       nkids = (child == project) ? proj_kids_n : child->num_children;
            stack[depth++] = (OwnerFrame){ kids, nkids, 0, child };
        }

        po->tok_spans[t].owner = (depth > 1) ? stack[depth - 1].scope : NULL;
    }

    free(stack);
    free(top);
    free(proj_kids);
    free(hoisted);
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseOutput *parse(const char *src) {
    ParseOutput *po = calloc(1, sizeof(ParseOutput));
    if (!po) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    po->root = alloc_synthetic_root();

    /* Set up global state for lexer.l and grammar.y */
    g_output          = po;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;
    yycolumn          = 0;
    yylineno          = 1;
    reset_pending_include_state();

    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    po->tok_spans       = g_tok_spans;
    po->num_tok_spans   = g_num_tok_spans;
    po->tok_span_cap    = g_tok_span_cap;
    po->num_sem_entries = g_num_sem_entries;

    g_output          = NULL;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;

    /* The grammar appends top-level declarations to root as they reduce,
     * which interleaves a project block's hoisted body children ahead of
     * the project node itself.  Sort by source position so root->children
     * is in true source order, then wire parent/owner pointers. */
    if (po->root->num_children > 1)
        qsort(po->root->children, (size_t)po->root->num_children,
              sizeof(tj_node *), compare_node_starts);
    assign_parent_links(po->root, po->root->children,
                        po->root->num_children, po->root);
    assign_token_owners(po);

    return po;
}
