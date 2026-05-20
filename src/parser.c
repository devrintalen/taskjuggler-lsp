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
    for (int i = 0; i < n->num_children; i++)
        tj_node_free(n->children[i]);
    free(n->children);
    free(n->included_children);
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

void tj_node_append_included(tj_node *parent, tj_node *child) {
    if (parent->num_included_children >= parent->included_children_cap) {
        int nc = parent->included_children_cap ? parent->included_children_cap * 2 : 4;
        tj_node **tmp = realloc(parent->included_children,
                                (size_t)nc * sizeof(tj_node *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        parent->included_children     = tmp;
        parent->included_children_cap = nc;
    }
    parent->included_children[parent->num_included_children++] = child;
}

void tj_node_clear_included(tj_node *n) {
    n->num_included_children = 0;
}

/* ── ParseOutput helpers ─────────────────────────────────────────────────── */

static tj_node *alloc_synthetic_root(void) {
    tj_node *n = calloc(1, sizeof(tj_node));
    if (!n) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    return n;
}

void parse_output_free(ParseOutput *po) {
    if (!po) return;
    tj_node_free(po->tasks);
    tj_node_free(po->accounts);
    tj_node_free(po->reports);
    tj_node_free(po->resources);
    tj_node_free(po->project);

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
 * Strategy: merge-sort by source position across all four trees' top-level
 * children plus the optional project node, then use the standard
 * single-pass scope-stack algorithm to assign owners.
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

static void assign_token_owners(ParseOutput *po) {
    /* Gather every top-level node from all four trees plus the optional
     * project block into a single merged-by-position array. */
    int merged_cap = po->tasks->num_children + po->accounts->num_children
                   + po->reports->num_children + po->resources->num_children
                   + (po->project ? 1 : 0);
    tj_node **merged = merged_cap
        ? malloc((size_t)merged_cap * sizeof(tj_node *))
        : NULL;
    if (merged_cap && !merged) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    int merged_n = 0;
    if (po->project) merged[merged_n++] = po->project;
    for (int i = 0; i < po->tasks->num_children;     i++) merged[merged_n++] = po->tasks->children[i];
    for (int i = 0; i < po->accounts->num_children;  i++) merged[merged_n++] = po->accounts->children[i];
    for (int i = 0; i < po->reports->num_children;   i++) merged[merged_n++] = po->reports->children[i];
    for (int i = 0; i < po->resources->num_children; i++) merged[merged_n++] = po->resources->children[i];
    if (merged_n > 1)
        qsort(merged, (size_t)merged_n, sizeof(tj_node *), compare_node_starts);

    int frame_cap = 64;
    OwnerFrame *stack = malloc((size_t)frame_cap * sizeof(OwnerFrame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 1;
    stack[0] = (OwnerFrame){ merged, merged_n, 0, NULL };

    for (int t = 0; t < po->num_tok_spans; t++) {
        LspPos pos = po->tok_spans[t].start;

        while (depth > 1 &&
               pos_cmp(pos, stack[depth - 1].scope->range.end) >= 0)
            depth--;

        for (;;) {
            OwnerFrame *top = &stack[depth - 1];

            while (top->idx < top->n &&
                   pos_cmp(pos, top->children[top->idx]->range.end) >= 0)
                top->idx++;
            if (top->idx >= top->n) break;

            tj_node *child = top->children[top->idx];
            if (pos_cmp(pos, child->range.start) < 0)
                break;

            if (depth >= frame_cap) {
                frame_cap *= 2;
                OwnerFrame *tmp = realloc(stack, (size_t)frame_cap * sizeof(OwnerFrame));
                if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
                stack = tmp;
            }
            stack[depth++] = (OwnerFrame){
                child->children, child->num_children, 0, child
            };
        }

        po->tok_spans[t].owner = (depth > 1) ? stack[depth - 1].scope : NULL;
    }

    free(stack);
    free(merged);
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseOutput *parse(const char *src) {
    ParseOutput *po = calloc(1, sizeof(ParseOutput));
    if (!po) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    po->tasks     = alloc_synthetic_root();
    po->accounts  = alloc_synthetic_root();
    po->reports   = alloc_synthetic_root();
    po->resources = alloc_synthetic_root();

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

    /* Wire up parent/owner pointers across the four trees + project. */
    assign_parent_links(po->tasks,     po->tasks->children,     po->tasks->num_children,     po->tasks);
    assign_parent_links(po->accounts,  po->accounts->children,  po->accounts->num_children,  po->accounts);
    assign_parent_links(po->reports,   po->reports->children,   po->reports->num_children,   po->reports);
    assign_parent_links(po->resources, po->resources->children, po->resources->num_children, po->resources);
    if (po->project) {
        po->project->parent_node = NULL;
        po->project->parent_doc  = NULL;
        assign_parent_links(po->project, po->project->children,
                            po->project->num_children, NULL);
    }
    assign_token_owners(po);

    return po;
}
