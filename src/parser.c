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
#include "debug.h"
#include "grammar.tab.h"  /* yyparse(), KW_* constants */

#include <malloc.h>       /* malloc_usable_size() for tok_spans recycling */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── flex scanner interface ──────────────────────────────────────────────── *
 *
 * These are declared in the flex-generated lexer.yy.c.  We use void * for
 * YY_BUFFER_STATE to avoid pulling in the full flex header.
 */

/** Opaque flex scanner buffer handle; defined in lexer.yy.c. */
typedef void *YY_BUFFER_STATE;
/**
 * Install @p str as the current flex input buffer.  Defined in lexer.yy.c.
 *
 * @param str  NUL-terminated input the scanner will tokenize.
 * @return Buffer handle to release with yy_delete_buffer().
 */
extern YY_BUFFER_STATE yy_scan_string(const char *str);
/**
 * Release a buffer returned by yy_scan_string().  Defined in lexer.yy.c.
 *
 * @param buf  Buffer handle to free.
 */
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
/** Current column reported by the flex scanner.  Defined in lexer.yy.c. */
extern int             yycolumn;
/** Current line reported by the flex scanner.  Defined in lexer.yy.c. */
extern int             yylineno;

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

/**
 * Defined in grammar.y.  Called at the start of every parse() so that a
 * partial include-body parse from a previous run cannot leak its pending
 * prefix strings into the next parse.
 */
extern void reset_pending_include_state(void);

/**
 * Reset the yylex statement-terminator (TK_EOL) wrapper state.  Defined in
 * lexer.l; called at the start of every parse() so a previous run's lookahead
 * buffer or "previous token" flag cannot bleed into the next parse.
 */
extern void reset_eol_state(void);

/** Currently-being-built ParseOutput; lexer/grammar populate this directly. */
ParseOutput *g_output         = NULL;
/** Backing storage for the token-span array under construction. */
TokenSpan   *g_tok_spans      = NULL;
/** Number of valid entries currently in `g_tok_spans`. */
int          g_num_tok_spans  = 0;
/** Allocated capacity of `g_tok_spans`. */
int          g_tok_span_cap   = 0;
/** Arena backing every `g_tok_spans[i].text` for the parse in progress;
 *  transferred to the resulting ParseOutput when the parse finishes. */
str_arena   *g_tok_arena      = NULL;
/** Running upper bound on emitted semantic-token entries (one per source line covered). */
int          g_num_sem_entries = 0;

/* ── Token-span buffer recycling ─────────────────────────────────────────── *
 *
 * The token-span array is the largest single allocation a parse makes (≈40 B ×
 * every token, tens of MB on a big file).  Because didChange re-parses the same
 * document on every keystroke, freeing it with free() (a munmap for a multi-MB
 * block) and mmap-ing a fresh one next parse means re-faulting every page —
 * profiling showed this is the dominant kernel cost on the didChange path.
 *
 * Instead we keep one retired buffer in a lock-free single slot and hand it
 * back to the next parse, so its pages stay mapped and resident.  parse() takes
 * from the slot (coordinator-only); the buffer is released back when a snapshot
 * is freed (docsnap_release, which can run on a query worker) or a transient
 * ParseOutput is discarded — hence the atomics.  Only one buffer is ever
 * cached; any extra retired buffer is freed normally. */
/** Single lock-free slot caching one retired token-span buffer for reuse by
 *  the next parse (see the note above). */
static _Atomic(TokenSpan *) g_tok_spans_recycle = NULL;

/** Only buffers at least this large are worth caching.  glibc keeps freed
 *  blocks below its dynamic mmap threshold (which rises to ~32 MB as large
 *  blocks are freed) on its own heap and hands them straight back to the next
 *  same-size malloc — no munmap, no re-fault — so recycling them just adds
 *  overhead.  Only blocks past that ceiling are munmap'd on free() and re-fault
 *  on the next parse, which is exactly what this cache avoids. */
#define TOK_SPANS_RECYCLE_MIN_BYTES (32u * 1024 * 1024)

/** Take the recycled token-span buffer for a new parse, or NULL when the slot
 *  is empty.
 *  @param out_cap  Receives the buffer's capacity (derived from its actual
 *                  allocation size), 0 when none.
 *  @return The recycled buffer, or NULL when the slot is empty. */
static TokenSpan *tok_spans_take(int *out_cap) {
    TokenSpan *buf = atomic_exchange(&g_tok_spans_recycle, NULL);
    *out_cap = buf ? (int)(malloc_usable_size(buf) / sizeof(TokenSpan)) : 0;
    return buf;
}

void tok_spans_release(TokenSpan *buf) {
    if (!buf) return;
    /* Below the threshold glibc already recycles the block efficiently; only
     * cache the big buffers whose free()/malloc() would otherwise munmap and
     * re-fault. */
    if (malloc_usable_size(buf) < TOK_SPANS_RECYCLE_MIN_BYTES) {
        free(buf);
        return;
    }
    /* Publish into the empty slot; if another buffer is already cached, this
     * one is surplus and freed.  The exchange/CAS pair never loses or
     * double-owns a buffer under concurrent release/take. */
    TokenSpan *expected = NULL;
    if (!atomic_compare_exchange_strong(&g_tok_spans_recycle, &expected, buf))
        free(buf);
}

/**
 * Test whether a token of @p kind is emitted as a semantic token.
 * Mirrors the skip set in classify() in semantic_tokens.c.
 *
 * @param kind  Raw TK_/KW_ token-kind constant to test.
 * @return Non-zero if the token contributes a semantic-token entry, zero if it is skipped.
 */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/**
 * Append one TokenSpan to the global accumulator.  Called from lexer.l for
 * every token that callers may need to inspect.
 *
 * @param kind  Raw TK_/KW_ constant the lexer matched.
 * @param sl    Start line.
 * @param sc    Start column.
 * @param el    End line.
 * @param ec    End column.
 * @param text  Borrowed lexeme; copied into the parse's token arena and
 *              pointed at by the new TokenSpan, or NULL.
 * @param len   Length of @p text in bytes (the lexer already knows it as
 *              yyleng / str_len, so we avoid a redundant strlen here).
 * @return      The interned, NUL-terminated arena copy of @p text (stable for
 *              the arena's lifetime), or NULL when @p text was NULL.  The
 *              lexer reuses this pointer as the value-stack Token.text instead
 *              of taking a second strdup, so a lexeme is copied at most once.
 */
char *g_push_tok_span(int kind,
                      uint32_t sl, uint32_t sc,
                      uint32_t el, uint32_t ec,
                      const char *text, size_t len) {
    if (g_num_tok_spans >= g_tok_span_cap) {
        g_tok_span_cap = g_tok_span_cap ? g_tok_span_cap * 2 : 64;
        TokenSpan *tmp = realloc(g_tok_spans,
                                 (size_t)g_tok_span_cap * sizeof(TokenSpan));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_tok_spans = tmp;
    }
    char *interned = text ? arena_strndup(g_tok_arena, text, len) : NULL;
    g_tok_spans[g_num_tok_spans++] = (TokenSpan){
        .token_kind = kind,
        .start      = { sl, sc },
        .end        = { el, ec },
        .text       = interned,
    };
    if (is_sem_highlighted(kind))
        g_num_sem_entries += (int)(el - sl + 1);
    return interned;
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

/* ── tj_node helpers ─────────────────────────────────────────────────────── *
 *
 * Every tj_node — and its children / dependencies arrays — lives in the
 * parse's token arena (g_tok_arena), alongside the lexemes its id / name /
 * dep paths borrow.  Nothing here is ever freed individually: the whole
 * tree is reclaimed when the owning doc_snapshot releases the arena.
 * Growing an array allocates a doubled copy in the arena and abandons the
 * old block — bounded waste traded for zero per-node free() calls and no
 * discard bookkeeping on grammar error-recovery paths. */

tj_node *tj_node_new(void) {
    tj_node *n = arena_alloc(g_tok_arena, sizeof(tj_node));
    memset(n, 0, sizeof(*n));
    return n;
}

void tj_node_append_child(tj_node *parent, tj_node *child) {
    if (parent->num_children >= parent->children_cap) {
        int nc = parent->children_cap ? parent->children_cap * 2 : 4;
        tj_node **grown = arena_alloc(g_tok_arena,
                                      (size_t)nc * sizeof(tj_node *));
        memcpy(grown, parent->children,
               (size_t)parent->num_children * sizeof(tj_node *));
        parent->children     = grown;
        parent->children_cap = nc;
    }
    parent->children[parent->num_children++] = child;
    child->parent_node = parent;
}

void tj_node_push_dependency(tj_node *task, Dependency dep) {
    if (task->num_dependencies >= task->dependencies_cap) {
        int nc = task->dependencies_cap ? task->dependencies_cap * 2 : 4;
        Dependency *grown = arena_alloc(g_tok_arena,
                                        (size_t)nc * sizeof(Dependency));
        memcpy(grown, task->dependencies,
               (size_t)task->num_dependencies * sizeof(Dependency));
        task->dependencies     = grown;
        task->dependencies_cap = nc;
    }
    task->dependencies[task->num_dependencies++] = dep;
}

/* ── ParseOutput helpers ─────────────────────────────────────────────────── */

void parse_output_free(ParseOutput *po) {
    if (!po) return;

    /* The tj_node tree and token lexemes live in tok_arena, not in individual
     * allocations, so freeing the arena below reclaims both.  The span buffer
     * goes back to the recycle slot for the next parse to reuse. */
    tok_spans_release(po->tok_spans);
    free(po->tok_owners);
    arena_free(po->tok_arena);

    for (int i = 0; i < po->num_includes; i++) {
        free(po->includes[i].filename);
        prefix_set_clear(&po->includes[i].prefixes);
    }
    free(po->includes);

    free(po);
}

void prefix_set_clear(prefix_set *ps) {
    for (int k = 0; k < PREFIX_KIND_COUNT; k++) {
        free(ps->by_kind[k]);
        ps->by_kind[k] = NULL;
    }
}

void prefix_set_copy(prefix_set *dst, const prefix_set *src) {
    for (int k = 0; k < PREFIX_KIND_COUNT; k++) {
        free(dst->by_kind[k]);
        dst->by_kind[k] = (src && src->by_kind[k]) ? strdup(src->by_kind[k]) : NULL;
    }
}

void push_include(ParseOutput *po, const char *quoted_text,
                  const prefix_set *prefixes) {
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
    e->filename = filename;
    memset(&e->prefixes, 0, sizeof(e->prefixes));
    prefix_set_copy(&e->prefixes, prefixes);
}

/* ── tj_node tree linkage ────────────────────────────────────────────────── *
 *
 * After the grammar finishes building the trees, we walk each per-kind
 * root to set parent_node pointers consistently, and to assign owners to
 * every TokenSpan.
 */

/**
 * Recursively set the parent_node field across one subtree.
 *
 * @param parent    Node to set as parent_node for each element of @p children.
 * @param children  Array of child pointers to process.
 * @param n         Number of entries in @p children.
 */
static void assign_parent_links(tj_node *parent, tj_node **children, int n) {
    for (int i = 0; i < n; i++) {
        children[i]->parent_node = parent;
        assign_parent_links(children[i], children[i]->children,
                            children[i]->num_children);
    }
}

/** One frame on the scope stack used by assign_token_owners(). */
typedef struct {
    tj_node **children; /**< borrowed sibling array being walked at this depth */
    int       n;        /**< number of valid entries in `children` */
    int       idx;      /**< next child index this frame will visit */
    tj_node  *scope;    /**< node whose range covers tokens currently being owned */
} OwnerFrame;

/** Top-level scope layout for the token-owner walk, built by
 *  build_owner_scope(). Both arrays are heap-allocated and owned by the
 *  caller (assign_token_owners frees them after the walk). */
typedef struct {
    tj_node **top;             /**< top-level scope frame, sorted by start pos */
    int       top_n;           /**< number of entries in `top` */
    tj_node **project_kids;    /**< project node's synthetic scope-children, sorted */
    int       project_kids_n;  /**< number of entries in `project_kids` */
} owner_scope;

/**
 * qsort comparator that orders tj_node pointers by their range start position.
 *
 * @param a  Pointer to the first element (a `tj_node *`).
 * @param b  Pointer to the second element (a `tj_node *`).
 * @return Negative if @p a starts before @p b, zero if equal, positive if after.
 */
static int compare_node_starts(const void *a, const void *b) {
    const tj_node *na = *(const tj_node *const *)a;
    const tj_node *nb = *(const tj_node *const *)b;
    return pos_cmp(na->range.start, nb->range.start);
}

/**
 * Test whether @p inner is wholly contained within @p outer.
 *
 * @param inner  Range to test as the contained range.
 * @param outer  Range to test as the containing range.
 * @return Non-zero if @p inner lies entirely within @p outer, zero otherwise.
 */
static int range_within(LspRange inner, LspRange outer) {
    return pos_cmp(inner.start, outer.start) >= 0 &&
           pos_cmp(inner.end, outer.end) <= 0;
}

/**
 * Build the top-level scope layout for the token-owner walk.
 *
 * The project block (if any) is a top-level child like the rest, but its body
 * declarations were hoisted to siblings under root (the project node keeps no
 * children) while its range still spans them. For ownership we restore that
 * nesting: any sibling whose range falls within the project's range becomes a
 * scope-child of the project, so tokens inside it resolve to the node rather
 * than dead-ending on the childless project. Siblings outside the project's
 * range (declared after the project block) stay as true top-level entries.
 * Both result arrays are sorted by source position.
 *
 * @param root     Synthetic root whose children are the top-level decls.
 * @param project  The project node among root's children, or NULL if none.
 * @return Heap-allocated scope arrays the caller must free (top, project_kids).
 */
static owner_scope build_owner_scope(tj_node *root, tj_node *project) {
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

    free(hoisted);
    return (owner_scope){ top, top_n, proj_kids, proj_kids_n };
}

/**
 * Compute, for every token span in @p po, its innermost-enclosing tj_node,
 * storing the result in the parallel @p po->tok_owners array (allocated here).
 *
 * Uses a single-pass scope-stack algorithm over the top-level children sorted
 * by source position.  The project node (if present) is handled specially:
 * its hoisted body declarations are treated as scope-children for ownership
 * purposes even though they are siblings under root in the tj_node tree.
 *
 * @param po  ParseOutput whose tok_owners array is allocated and filled.
 */
static void assign_token_owners(ParseOutput *po) {
    int num = po->num_tok_spans;
    po->tok_owners = num ? malloc((size_t)num * sizeof(tj_node *)) : NULL;
    if (num && !po->tok_owners) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }

    tj_node *root = po->root;
    if (!root) {
        for (int t = 0; t < num; t++) po->tok_owners[t] = NULL;
        return;
    }

    tj_node *project = NULL;
    for (int i = 0; i < root->num_children; i++)
        if (root->children[i]->keyword == KW_PROJECT) {
            project = root->children[i];
            break;
        }

    owner_scope scope = build_owner_scope(root, project);

    int frame_cap = 64;
    OwnerFrame *stack = malloc((size_t)frame_cap * sizeof(OwnerFrame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 1;
    stack[0] = (OwnerFrame){ scope.top, scope.top_n, 0, NULL };

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
            tj_node **kids = (child == project) ? scope.project_kids : child->children;
            int       nkids = (child == project) ? scope.project_kids_n : child->num_children;
            stack[depth++] = (OwnerFrame){ kids, nkids, 0, child };
        }

        po->tok_owners[t] = (depth > 1) ? stack[depth - 1].scope : NULL;
    }

    free(stack);
    free(scope.top);
    free(scope.project_kids);
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

/** Install the shared lexer/parser global state for a fresh parse of @p po.
 *  Reuses a retired token-span buffer when one is cached (its pages are
 *  already mapped), falling back to a fresh allocation on first parse.
 *  @param po  ParseOutput the parse will populate. */
static void install_parse_globals(ParseOutput *po) {
    g_output          = po;
    g_tok_spans       = tok_spans_take(&g_tok_span_cap);
    g_num_tok_spans   = 0;
    g_tok_arena       = arena_new();
    g_num_sem_entries = 0;
    yycolumn          = 0;
    yylineno          = 1;
    reset_pending_include_state();
    reset_eol_state();
}

/** Move the token spans / arena / counts accumulated in the parser globals
 *  into @p po, then clear the shared global state for the next parse.
 *  @param po  ParseOutput that receives the accumulated parse state. */
static void harvest_parse_globals(ParseOutput *po) {
    po->tok_spans       = g_tok_spans;
    po->num_tok_spans   = g_num_tok_spans;
    po->tok_arena       = g_tok_arena;
    po->num_sem_entries = g_num_sem_entries;

    g_output          = NULL;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_tok_arena       = NULL;
    g_num_sem_entries = 0;
}

ParseOutput *parse(const char *src) {
    ParseOutput *po = calloc(1, sizeof(ParseOutput));
    if (!po) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    install_parse_globals(po);
    /* The synthetic root lives in the arena install_parse_globals() just
     * created, like every node the grammar will hang under it. */
    po->root = tj_node_new();

#if DEBUG_PARSER >= LOG_VERBOSE
    struct timespec parse_t0, parse_t1, parse_t2;
    clock_gettime(CLOCK_MONOTONIC, &parse_t0);
#endif
    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);
#if DEBUG_PARSER >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &parse_t1);
#endif

    harvest_parse_globals(po);

    /* The grammar appends top-level declarations to root as they reduce,
     * which interleaves a project block's hoisted body children ahead of
     * the project node itself.  Sort by source position so root->children
     * is in true source order, then wire parent/owner pointers. */
    if (po->root->num_children > 1)
        qsort(po->root->children, (size_t)po->root->num_children,
              sizeof(tj_node *), compare_node_starts);
    assign_parent_links(po->root, po->root->children,
                        po->root->num_children);
    assign_token_owners(po);
#if DEBUG_PARSER >= LOG_VERBOSE
    clock_gettime(CLOCK_MONOTONIC, &parse_t2);
    DLOG(DEBUG_PARSER, LOG_VERBOSE,
         "parse phases: lex+yyparse=%.1f postprocess=%.1f ms (tokens=%d)",
         (parse_t1.tv_sec - parse_t0.tv_sec) * 1000.0
             + (parse_t1.tv_nsec - parse_t0.tv_nsec) / 1e6,
         (parse_t2.tv_sec - parse_t1.tv_sec) * 1000.0
             + (parse_t2.tv_nsec - parse_t1.tv_nsec) / 1e6,
         po->num_tok_spans);
#endif

    DLOG(DEBUG_PARSER, LOG_VERBOSE,
         "parsed %zu bytes -> %d tokens, %d top-level nodes, %d includes",
         src ? strlen(src) : 0, po->num_tok_spans, po->root->num_children,
         po->num_includes);

    return po;
}
