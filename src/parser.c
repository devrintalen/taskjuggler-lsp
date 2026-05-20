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
#include "document_symbol.h"
#include "diagnostics.h"
#include "grammar.tab.h"  /* yyparse() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── TODO(doc-symbol-tree) ────────────────────────────────────────────────
 *
 * The current DocSymbol storage is an array of pointers to heap-
 * allocated nodes, each with its own malloc'd children[] array of
 * pointers.  parse_result_clone_for_revalidate() below pays for that
 * shape with a three-pass clone and a sorted old→new SymbolRemap.  A
 * uniform representation would make cloning a trivial memcpy.  Two
 * concrete options to evaluate when this becomes a priority:
 *
 *   1. Flat array + indices.  Store every node for a parse in a single
 *      contiguous DocSymbol[] on the ParseResult.  Replace
 *      parent/children pointers with int32_t indices into that array.
 *      Indices stay valid across cloning, so a clone is `memcpy` of
 *      the array plus `memcpy` of the link arrays — no remap needed.
 *      TokenSpan.owner and DefinitionLink.target become indices too;
 *      cross-file targets pair an index with a target_uri.
 *
 *   2. Arena-allocated tree.  Keep pointers, but allocate every
 *      DocSymbol for one parse from a single bump-allocated arena
 *      hung off the ParseResult.  Cloning is `memcpy` of the arena
 *      plus offset-arithmetic pointer fixups.
 *
 * Option 1 is the bigger win — better cache locality during the
 * binary-search-by-position used by hover / completion, simpler
 * ownership in destructors, and trivial cloning.
 *
 * Three related follow-ups worth doing alongside / after that:
 *
 *   - parse_result_clone_for_revalidate() reclones every document on
 *     every mutation.  Many revalidations don't actually need that;
 *     a doc whose cross_file_deps[] do not reference the changed file
 *     is unaffected.  Add an affected-set computation in the
 *     revalidation pipeline so only docs that need new cross-file
 *     links get re-cloned.
 *
 *   - Once (1) lands, SymbolRemap below either degenerates to identity
 *     or vanishes entirely; the symbol_remap_* helpers either become
 *     trivial or disappear.  The three clone passes themselves should
 *     mostly survive — they're just spine copies + translation calls.
 *
 *   - Threadpool is single-threaded for queries today
 *     (NUM_QUERY_WORKERS = 1 in threadpool.c).  The snapshot +
 *     immutable-parse contract is already in place; raising the
 *     worker count just needs per-document FIFO ordering for the
 *     sem_tokens cache (mutex isn't FIFO, so racing workers mint
 *     non-deterministic resultIds).  A per-document ticket counter
 *     or a dedicated sem_tokens worker would do it.
 */

/* ── Raw dependency reference accumulator ──────────────────────────────── *
 *
 * grammar.y calls push_dep_ref() for each dependency reference encountered
 * during parsing.  These are stored in a file-local buffer until parse()
 * calls resolve_dep_refs(), which resolves them in place and populates
 * def_links/ref_links on the target DocSymbols.  The RawDepRef type is
 * declared in parser.h so that references that failed in-file resolution
 * can be stashed on ParseResult.cross_file_deps[] for later re-resolution.
 */

/** Backing storage for the transient dep-ref accumulator. */
static RawDepRef *g_dep_refs     = NULL;
/** Number of entries currently held in #g_dep_refs. */
static int        g_num_dep_refs = 0;
/** Allocated capacity of #g_dep_refs in entries. */
static int        g_dep_ref_cap  = 0;

/** Release the accumulator's storage and reset its bookkeeping. */
static void free_dep_refs(void) {
    for (int i = 0; i < g_num_dep_refs; i++)
        free(g_dep_refs[i].path);
    free(g_dep_refs);
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

void dep_refs_reset(void) {
    free_dep_refs();
}

void push_dep_ref(int bang_count, const char *path,
                  DocSymbol *owner, LspPos start, LspPos end) {
    if (g_dep_ref_cap <= g_num_dep_refs) {
        g_dep_ref_cap = g_dep_ref_cap ? g_dep_ref_cap * 2 : 8;
        RawDepRef *tmp = realloc(g_dep_refs,
                                 (size_t)g_dep_ref_cap * sizeof(RawDepRef));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        g_dep_refs = tmp;
    }
    RawDepRef *dr = &g_dep_refs[g_num_dep_refs++];
    dr->bang_count = bang_count;
    dr->path       = path && path[0] ? strdup(path) : NULL;
    dr->owner      = owner;
    dr->range      = (LspRange){ start, end };
}

/**
 * Split a dot-separated path string into a heap-allocated array of segments.
 *
 * @param path      Dot-separated reference path (e.g. `"foo.bar.baz"`).
 * @param out_segs  Receives a heap-allocated array of heap-allocated
 *                  segment strings.  Caller must free each segment and
 *                  the array.
 * @param out_nseg  Receives the segment count.
 */
static void split_path(const char *path, char ***out_segs, int *out_nseg) {
    *out_nseg = 0;
    *out_segs = NULL;
    if (!path || !path[0]) return;
    int cap = 1;
    for (const char *p = path; *p; p++) if (*p == '.') cap++;
    *out_segs = malloc((size_t)cap * sizeof(char *));
    char *tmp = strdup(path);
    char *tok = strtok(tmp, ".");
    while (tok) {
        (*out_segs)[(*out_nseg)++] = strdup(tok);
        tok = strtok(NULL, ".");
    }
    free(tmp);
}


/* ── flex scanner interface ──────────────────────────────────────────────── *
 *
 * These are declared in the flex-generated lexer.yy.c.  We use void * for
 * YY_BUFFER_STATE to avoid pulling in the full flex header.
 */

/** Opaque flex input-buffer handle. */
typedef void *YY_BUFFER_STATE;
/**
 * Flex entry point: scan @p str as the next input buffer.
 *
 * @param str  NUL-terminated input to scan.
 * @return Handle to the new buffer; release with yy_delete_buffer().
 */
extern YY_BUFFER_STATE yy_scan_string(const char *str);
/**
 * Flex entry point: release a buffer returned by yy_scan_string().
 *
 * @param buf  Buffer handle to release.
 */
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
/** Column tracker defined in lexer.l. */
extern int             yycolumn;
/** Line counter managed by flex `%option yylineno`. */
extern int             yylineno;

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

/** Currently-being-built ParseResult; lexer/grammar populate this directly. */
ParseResult *g_result          = NULL;
/** Backing storage for the token-span array under construction. */
TokenSpan   *g_tok_spans       = NULL;
/** Number of entries currently held in #g_tok_spans. */
int          g_num_tok_spans   = 0;
/** Allocated capacity of #g_tok_spans in entries. */
int          g_tok_span_cap    = 0;
/** Running upper bound on emitted semantic-token entries (one per source line covered). */
int          g_num_sem_entries = 0;

/**
 * Test whether a token of @p kind is emitted as a semantic token.
 * Mirrors the skip set in classify() in semantic_tokens.c.
 *
 * @param kind  Token kind (TK_* / KW_*) to test.
 * @return 1 when the token contributes a semantic-tokens entry, 0 otherwise.
 */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/**
 * Append one TokenSpan to the global accumulator.  Called from lexer.l for
 * every token that callers may need to inspect.
 *
 * @param kind  TK_* / KW_* token kind from grammar.tab.h.
 * @param sl    Start line.
 * @param sc    Start column.
 * @param el    End line.
 * @param ec    End column (exclusive).
 * @param text  Token lexeme; copied with strdup() (may be NULL).
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

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

void parse_result_free(ParseResult *r) {
    for (int i = 0; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    free(r->diagnostics);
    for (int i = 0; i < r->num_doc_symbols; i++) {
        doc_symbol_free(r->doc_symbols[i]);
        free(r->doc_symbols[i]);
    }
    free(r->doc_symbols);

    for (int i = 0; i < r->num_tok_spans; i++)
        free(r->tok_spans[i].text);
    free(r->tok_spans);

    /* g_dep_refs is freed at the end of resolve_dep_refs(); nothing to do
     * here.  cross_file_deps holds deferred refs that survive parse() and
     * is freed below. */

    for (int i = 0; i < r->num_included_files; i++)
        free(r->included_files[i]);
    free(r->included_files);

    for (int i = 0; i < r->num_cross_file_deps; i++)
        free(r->cross_file_deps[i].path);
    free(r->cross_file_deps);

    memset(r, 0, sizeof(*r));
}

ParseResult *parse_result_acquire(ParseResult *r) {
    atomic_fetch_add(&r->refcount, 1);
    return r;
}

void parse_result_release(ParseResult *r) {
    if (!r) return;
    if (atomic_fetch_sub(&r->refcount, 1) == 1) {
        parse_result_free(r);
        free(r);
    }
}

/**
 * Append @p s to @p r's `doc_symbols` pointer array, growing it if needed.
 *
 * @param r  ParseResult being populated.
 * @param s  Symbol to append (transfer of ownership).
 */
void push_doc_symbol(ParseResult *r, DocSymbol *s) {
    if (r->num_doc_symbols >= r->doc_sym_cap) {
        int nc = r->doc_sym_cap ? r->doc_sym_cap * 2 : 4;
        DocSymbol **tmp = realloc(r->doc_symbols, (size_t)nc * sizeof(DocSymbol *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->doc_symbols = tmp;
        r->doc_sym_cap = nc;
    }
    r->doc_symbols[r->num_doc_symbols++] = s;
}

void push_included_file(ParseResult *r, const char *quoted_text) {
    if (!quoted_text) return;
    size_t len = strlen(quoted_text);
    /* Strip surrounding quotes if present */
    const char *inner = quoted_text;
    size_t inner_len  = len;
    if (len >= 2 && (quoted_text[0] == '"' || quoted_text[0] == '\'')) {
        inner     = quoted_text + 1;
        inner_len = len - 2;
    }
    char *copy = malloc(inner_len + 1);
    if (!copy) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(copy, inner, inner_len);
    copy[inner_len] = '\0';

    if (r->num_included_files >= r->included_files_cap) {
        int nc = r->included_files_cap ? r->included_files_cap * 2 : 4;
        char **tmp = realloc(r->included_files, (size_t)nc * sizeof(char *));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->included_files     = tmp;
        r->included_files_cap = nc;
    }
    r->included_files[r->num_included_files++] = copy;
}

/* ── DocSymbol tree linkage ─────────────────────────────────────────────── */

/**
 * Recursively set parent pointers for all children in the symbol tree.
 *
 * @param syms    Array of sibling symbols whose parent is @p parent.
 * @param n       Length of @p syms.
 * @param parent  Parent pointer to assign; NULL for top-level symbols.
 */
static void assign_parents(DocSymbol **syms, int n, DocSymbol *parent) {
    for (int i = 0; i < n; i++) {
        syms[i]->parent = parent;
        assign_parents(syms[i]->children, syms[i]->num_children, syms[i]);
    }
}

/* ── Token-to-symbol cross-referencing ──────────────────────────────────── */

/**
 * Assign each token span's owner to the innermost enclosing DocSymbol
 * using a single linear sweep.  Both tok_spans[] and symbol ranges are in
 * document order, so we walk them in lockstep with a stack of open symbol
 * scopes.
 *
 * Each stack frame holds a children array, the next-child index at that
 * level, and a pointer to the scope symbol we descended into (NULL for the
 * root frame, which has no enclosing scope).  The owner of a token is the
 * scope of the deepest currently-open frame.
 *
 * Runs in O(T + S) where T is `num_tok_spans` and S is the total number
 * of DocSymbols.
 *
 * @param r  ParseResult whose tok_spans get `.owner` populated.
 */
static void assign_token_owners(ParseResult *r) {
    typedef struct {
        DocSymbol **children;
        int         n;
        int         idx;
        DocSymbol  *scope;  /* symbol we descended into; NULL at root */
    } Frame;

    int frame_cap = 64;
    Frame *stack = malloc((size_t)frame_cap * sizeof(Frame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 1;
    stack[0] = (Frame){ r->doc_symbols, r->num_doc_symbols, 0, NULL };

    for (int t = 0; t < r->num_tok_spans; t++) {
        LspPos pos = r->tok_spans[t].start;

        /* Pop scopes whose range has ended. */
        while (depth > 1 &&
               pos_cmp(pos, stack[depth - 1].scope->range.end) >= 0)
            depth--;

        /* Descend as long as the next child at the top frame contains pos. */
        for (;;) {
            Frame *top = &stack[depth - 1];

            /* Advance past siblings that ended before this token */
            while (top->idx < top->n &&
                   pos_cmp(pos, top->children[top->idx]->range.end) >= 0)
                top->idx++;
            if (top->idx >= top->n) break;

            DocSymbol *child = top->children[top->idx];
            if (pos_cmp(pos, child->range.start) < 0)
                break;  /* cursor sits before the next sibling */

            if (depth >= frame_cap) {
                frame_cap *= 2;
                Frame *tmp = realloc(stack, (size_t)frame_cap * sizeof(Frame));
                if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
                stack = tmp;
            }
            stack[depth++] = (Frame){
                child->children, child->num_children, 0, child
            };
        }

        r->tok_spans[t].owner = (depth > 1) ? stack[depth - 1].scope : NULL;
    }

    free(stack);
}

/* ── Dependency edge resolution ────────────────────────────────────────── */

/**
 * Append a DefinitionLink to a DocSymbol's def_links array, growing it
 * if needed.
 *
 * @param sym   Target symbol that owns the outgoing reference.
 * @param link  Resolved link to append (transfer of ownership for any
 *              heap-allocated fields).
 */
static void push_def_link(DocSymbol *sym, DefinitionLink link) {
    if (sym->num_def_links >= sym->def_links_cap) {
        int nc = sym->def_links_cap ? sym->def_links_cap * 2 : 4;
        DefinitionLink *tmp = realloc(sym->def_links,
                                      (size_t)nc * sizeof(DefinitionLink));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        sym->def_links = tmp;
        sym->def_links_cap = nc;
    }
    sym->def_links[sym->num_def_links++] = link;
}

/**
 * Append a ReferenceLink to a DocSymbol's ref_links array, growing it
 * if needed.
 *
 * @param sym   Target symbol that the reference points to.
 * @param link  Incoming link to append (transfer of ownership for any
 *              heap-allocated fields).
 */
static void push_ref_link(DocSymbol *sym, ReferenceLink link) {
    if (sym->num_ref_links >= sym->ref_links_cap) {
        int nc = sym->ref_links_cap ? sym->ref_links_cap * 2 : 4;
        ReferenceLink *tmp = realloc(sym->ref_links,
                                     (size_t)nc * sizeof(ReferenceLink));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        sym->ref_links = tmp;
        sym->ref_links_cap = nc;
    }
    sym->ref_links[sym->num_ref_links++] = link;
}

/**
 * Find a task DocSymbol by navigating to the parent scope via
 * doc_symbol_find_path(), then scanning for the final segment.
 * Only KW_TASK nodes are matched at the leaf level.
 *
 * @param syms  Top-level symbols to search.
 * @param n     Length of @p syms.
 * @param segs  Path segments (e.g. `["spec", "gui"]`).
 * @param nseg  Length of @p segs.
 * @return The matched task, or NULL when any segment is not found.
 */
static DocSymbol *find_task(DocSymbol **syms, int n,
                            char **segs, int nseg) {
    if (nseg == 0 || !segs) return NULL;
    int parent_n = 0;
    DocSymbol *const *parent = doc_symbol_find_path(
        syms, n, (const char **)segs, nseg - 1, &parent_n);
    if (!parent) return NULL;
    for (int i = 0; i < parent_n; i++) {
        if (parent[i]->keyword == KW_TASK && parent[i]->id &&
                strcmp(parent[i]->id, segs[nseg - 1]) == 0)
            return parent[i];
        if (parent[i]->keyword == KW_PROJECT) {
            DocSymbol *found = find_task(parent[i]->children,
                                         parent[i]->num_children,
                                         segs + (nseg - 1), 1);
            if (found) return found;
        }
    }
    return NULL;
}

/**
 * Build an `"unresolved dependency: a.b.c"` message from @p segs and push
 * it onto @p r->diagnostics at @p range.
 *
 * @param r      ParseResult to append the diagnostic to.
 * @param range  Source range to mark.
 * @param segs   Path segments that failed to resolve.
 * @param nseg   Length of @p segs.
 */
static void emit_unresolved_dep_diag(ParseResult *r, LspRange range,
                                     char *const *segs, int nseg) {
    size_t msg_len = 32; /* "unresolved dependency: " prefix */
    for (int j = 0; j < nseg; j++) msg_len += strlen(segs[j]) + 1;
    char *msg = malloc(msg_len);
    if (!msg) return;
    strcpy(msg, "unresolved dependency: ");
    for (int j = 0; j < nseg; j++) {
        if (j > 0) strcat(msg, ".");
        strcat(msg, segs[j]);
    }
    push_diagnostic(r, range, DIAG_ERROR, msg);
    free(msg);
}

/**
 * Append a RawDepRef onto @p r->cross_file_deps[], taking ownership of
 * the heap-allocated `path` field.
 *
 * @param r    ParseResult being populated.
 * @param ref  Reference to stash.  Its `path` is moved into the new entry;
 *             do not free it after the call.
 */
static void stash_cross_file_dep(ParseResult *r, const RawDepRef *ref) {
    if (r->num_cross_file_deps >= r->cross_file_deps_cap) {
        int nc = r->cross_file_deps_cap ? r->cross_file_deps_cap * 2 : 4;
        RawDepRef *tmp = realloc(r->cross_file_deps,
                                 (size_t)nc * sizeof(RawDepRef));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->cross_file_deps     = tmp;
        r->cross_file_deps_cap = nc;
    }
    RawDepRef *dst = &r->cross_file_deps[r->num_cross_file_deps++];
    *dst       = *ref;
    dst->path  = ref->path ? strdup(ref->path) : NULL;
}

/**
 * Resolve dep refs accumulated by the grammar.
 *
 * For each entry in the global accumulator: split the path, resolve the
 * target in-file, and populate def_links/ref_links on success.  For
 * 0-bang refs that fail in-file lookup, stash them on
 * @p r->cross_file_deps[] for the server to retry against other open
 * documents — no diagnostic is emitted yet.  Bang refs (which are
 * strictly in-file) produce an unresolved-dependency diagnostic on
 * failure.  The global accumulator is freed at the end.
 *
 * @param r  ParseResult whose dep refs are being resolved.
 */
static void resolve_dep_refs(ParseResult *r) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        RawDepRef *ref = &g_dep_refs[i];

        if (!ref->owner) continue;

        char **segs = NULL;
        int    nseg = 0;
        split_path(ref->path, &segs, &nseg);

        /* Determine the search root based on bang_count:
         *   0 bangs  → absolute lookup from root
         *   n bangs  → walk up n levels from owner, search that ancestor's children
         *   too many → invalid (more bangs than nesting depth) */
        DocSymbol  *resolved   = NULL;
        DocSymbol **search_syms = NULL;
        int         search_n    = 0;

        int escaped = 0;
        if (ref->bang_count == 0) {
            search_syms = r->doc_symbols;
            search_n    = r->num_doc_symbols;
        } else {
            DocSymbol *ancestor = ref->owner;
            for (int b = 0; b < ref->bang_count; b++) {
                if (!ancestor->parent) {
                    /* Too many bangs — can't go higher than root */
                    escaped = 1;
                    break;
                }
                ancestor = ancestor->parent;
            }
            if (!escaped) {
                search_syms = ancestor->children;
                search_n    = ancestor->num_children;
            }
        }

        if (!escaped && search_syms && nseg > 0)
            resolved = find_task(search_syms, search_n, segs, nseg);

        if (escaped) {
            char *msg = strdup("dependency reference escapes beyond project root");
            if (msg) {
                push_diagnostic(r, ref->range, DIAG_WARNING, msg);
                free(msg);
            }
            for (int j = 0; j < nseg; j++) free(segs[j]);
            free(segs);
            continue;
        }

        if (!resolved) {
            /* 0-bang refs may still resolve across files; defer to the
             * server's cross-file pass.  Bang refs are strictly in-file, so
             * a miss is a hard error now. */
            if (ref->bang_count == 0)
                stash_cross_file_dep(r, ref);
            else
                emit_unresolved_dep_diag(r, ref->range, segs, nseg);
            for (int j = 0; j < nseg; j++) free(segs[j]);
            free(segs);
            continue;
        }

        /* Outgoing link on the owner (go-to-definition) */
        push_def_link(ref->owner, (DefinitionLink){
            .source     = ref->range,
            .target     = resolved,
            .target_uri = NULL,
        });

        /* Incoming link on the target (find-references) */
        push_ref_link(resolved, (ReferenceLink){
            .source     = ref->range,
            .origin     = ref->owner,
            .source_uri = NULL,
        });

        for (int j = 0; j < nseg; j++) free(segs[j]);
        free(segs);
    }

    /* Free the global accumulator — fully consumed */
    free_dep_refs();
}

void resolve_cross_file_deps(ParseResult *r,
                             DocSymbol *const *const *extra_roots,
                             const int *extra_counts,
                             const char *const *extra_uris,
                             int num_extra,
                             const char *self_uri) {
    for (int i = 0; i < r->num_cross_file_deps; i++) {
        RawDepRef *ref = &r->cross_file_deps[i];
        if (!ref->owner) continue;

        char **segs = NULL;
        int    nseg = 0;
        split_path(ref->path, &segs, &nseg);
        if (nseg == 0) { free(segs); continue; }

        DocSymbol *resolved = NULL;
        const char *target_uri = NULL;
        for (int p = 0; p < num_extra; p++) {
            resolved = find_task((DocSymbol **)extra_roots[p],
                                 extra_counts[p], segs, nseg);
            if (resolved) { target_uri = extra_uris[p]; break; }
        }

        if (!resolved) {
            emit_unresolved_dep_diag(r, ref->range, segs, nseg);
        } else {
            push_def_link(ref->owner, (DefinitionLink){
                .source     = ref->range,
                .target     = resolved,
                .target_uri = strdup(target_uri),
            });
            push_ref_link(resolved, (ReferenceLink){
                .source     = ref->range,
                .origin     = ref->owner,
                .source_uri = self_uri ? strdup(self_uri) : NULL,
            });
        }

        for (int j = 0; j < nseg; j++) free(segs[j]);
        free(segs);
    }
}

/* ── Symbol-tree clone helpers ───────────────────────────────────────────
 *
 * All DocSymbol-tree-shape knowledge in the clone path is confined to
 * the three helpers below.  Pass 2 (token spans) and pass 3 (per-symbol
 * link arrays) only touch the opaque SymbolRemap API, so a future
 * change to the tree representation (see TODO at top of file) should
 * not need to touch them.
 */

/** One entry in the opaque old→new pointer map. */
typedef struct {
    DocSymbol *old_ptr;
    DocSymbol *new_ptr;
} SymbolRemapEntry;

struct SymbolRemap {
    SymbolRemapEntry *entries;
    size_t            count;
    size_t            cap;
};
typedef struct SymbolRemap SymbolRemap;

/** Sort helper: order entries by `old_ptr` ascending for bsearch. */
static int symbol_remap_cmp(const void *a, const void *b) {
    DocSymbol *pa = ((const SymbolRemapEntry *)a)->old_ptr;
    DocSymbol *pb = ((const SymbolRemapEntry *)b)->old_ptr;
    if (pa < pb) return -1;
    if (pa > pb) return  1;
    return 0;
}

/** Append `(old, new)` to the remap, growing storage if needed. */
static void symbol_remap_push(SymbolRemap *r, DocSymbol *old, DocSymbol *new_) {
    if (r->count >= r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 32;
        SymbolRemapEntry *tmp = realloc(r->entries, nc * sizeof(*tmp));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        r->entries = tmp;
        r->cap     = nc;
    }
    r->entries[r->count].old_ptr = old;
    r->entries[r->count].new_ptr = new_;
    r->count++;
}

/**
 * Translate an old-tree DocSymbol pointer to its new-tree counterpart.
 * NULL → NULL.  Caller must have finished filling and sorting the
 * remap (symbol_tree_clone does both).
 */
static DocSymbol *symbol_remap_lookup(const SymbolRemap *r, DocSymbol *old) {
    if (!old) return NULL;
    SymbolRemapEntry key = { .old_ptr = old, .new_ptr = NULL };
    SymbolRemapEntry *hit = bsearch(&key, r->entries, r->count,
                                    sizeof(*r->entries), symbol_remap_cmp);
    return hit ? hit->new_ptr : NULL;
}

/** Visitor walking every (old, new) pair recorded in the remap. */
static void symbol_remap_for_each(const SymbolRemap *r,
                                  void (*fn)(DocSymbol *old, DocSymbol *new_, void *ctx),
                                  void *ctx) {
    for (size_t i = 0; i < r->count; i++)
        fn(r->entries[i].old_ptr, r->entries[i].new_ptr, ctx);
}

/** Release storage owned by @p r.  Safe with @p r == NULL. */
static void symbol_remap_free(SymbolRemap *r) {
    if (!r) return;
    free(r->entries);
    free(r);
}

/**
 * Recursive DFS clone of one subtree.  Allocates a fresh DocSymbol,
 * deep-copies scalar fields and owned strings, sets parent, recursively
 * clones children, and records the old→new mapping.  Leaves
 * def_links / ref_links empty — those are filled in clone pass 3.
 */
static DocSymbol *clone_symbol_subtree(DocSymbol *src, DocSymbol *parent,
                                       SymbolRemap *remap) {
    DocSymbol *dst = calloc(1, sizeof(DocSymbol));
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    dst->name            = src->name ? strdup(src->name) : NULL;
    dst->id              = src->id   ? strdup(src->id)   : NULL;
    dst->keyword         = src->keyword;
    dst->range           = src->range;
    dst->selection_range = src->selection_range;
    dst->start_date      = src->start_date;
    dst->end_date        = src->end_date;
    dst->has_start       = src->has_start;
    dst->has_end         = src->has_end;
    dst->parent          = parent;
    dst->num_children    = src->num_children;
    dst->children_cap    = src->num_children;  /* exact-fit */
    dst->children        = src->num_children
        ? malloc((size_t)src->num_children * sizeof(DocSymbol *))
        : NULL;
    if (src->num_children && !dst->children) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    /* def_links / ref_links left zero — populated in pass 3 */

    symbol_remap_push(remap, src, dst);

    for (int i = 0; i < src->num_children; i++)
        dst->children[i] = clone_symbol_subtree(src->children[i], dst, remap);
    return dst;
}

/**
 * Clone the entire symbol forest from @p src_roots.  Allocates a fresh
 * roots array (`*out_new_roots`, length `*out_n_new_roots`) and a
 * SymbolRemap ready for lookup.  The returned remap is sorted so
 * symbol_remap_lookup can bsearch it.
 */
static SymbolRemap *symbol_tree_clone(DocSymbol *const *src_roots,
                                      int n_roots,
                                      DocSymbol ***out_new_roots,
                                      int *out_n_new_roots) {
    SymbolRemap *remap = calloc(1, sizeof(*remap));
    if (!remap) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    DocSymbol **new_roots = n_roots
        ? malloc((size_t)n_roots * sizeof(DocSymbol *))
        : NULL;
    if (n_roots && !new_roots) {
        fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
    }
    for (int i = 0; i < n_roots; i++)
        new_roots[i] = clone_symbol_subtree(src_roots[i], NULL, remap);

    qsort(remap->entries, remap->count, sizeof(*remap->entries),
          symbol_remap_cmp);

    *out_new_roots   = new_roots;
    *out_n_new_roots = n_roots;
    return remap;
}

/**
 * Pass 2: copy and translate `tok_spans`, `diagnostics[0..dep_diag_start)`,
 * `included_files`, and `cross_file_deps` from @p src onto the fresh
 * ParseResult @p dst.  Token-span owners are translated through the
 * remap; everything else is a deep copy.
 */
static void clone_spine_and_translate_owners(ParseResult *dst,
                                             const ParseResult *src,
                                             const SymbolRemap *remap) {
    /* tok_spans */
    if (src->num_tok_spans) {
        dst->tok_spans = malloc((size_t)src->num_tok_spans * sizeof(TokenSpan));
        if (!dst->tok_spans) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        memcpy(dst->tok_spans, src->tok_spans,
               (size_t)src->num_tok_spans * sizeof(TokenSpan));
        for (int i = 0; i < src->num_tok_spans; i++) {
            TokenSpan *ts = &dst->tok_spans[i];
            ts->text  = src->tok_spans[i].text
                ? strdup(src->tok_spans[i].text) : NULL;
            ts->owner = symbol_remap_lookup(remap, src->tok_spans[i].owner);
        }
        dst->num_tok_spans = src->num_tok_spans;
        dst->tok_span_cap  = src->num_tok_spans;
    }
    dst->num_sem_entries = src->num_sem_entries;

    /* diagnostics[0..dep_diag_start) — the permanent region */
    if (src->dep_diag_start > 0) {
        dst->diagnostics = malloc((size_t)src->dep_diag_start * sizeof(Diagnostic));
        if (!dst->diagnostics) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        for (int i = 0; i < src->dep_diag_start; i++) {
            dst->diagnostics[i].range    = src->diagnostics[i].range;
            dst->diagnostics[i].severity = src->diagnostics[i].severity;
            dst->diagnostics[i].message  = src->diagnostics[i].message
                ? strdup(src->diagnostics[i].message) : NULL;
        }
        dst->num_diagnostics = src->dep_diag_start;
        dst->diag_cap        = src->dep_diag_start;
    }
    dst->dep_diag_start = src->dep_diag_start;

    /* included_files */
    if (src->num_included_files) {
        dst->included_files = malloc((size_t)src->num_included_files * sizeof(char *));
        if (!dst->included_files) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        for (int i = 0; i < src->num_included_files; i++)
            dst->included_files[i] = src->included_files[i]
                ? strdup(src->included_files[i]) : NULL;
        dst->num_included_files = src->num_included_files;
        dst->included_files_cap = src->num_included_files;
    }

    /* cross_file_deps — owner pointers translated; path deep-copied */
    if (src->num_cross_file_deps) {
        dst->cross_file_deps = malloc((size_t)src->num_cross_file_deps * sizeof(RawDepRef));
        if (!dst->cross_file_deps) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        for (int i = 0; i < src->num_cross_file_deps; i++) {
            const RawDepRef *s = &src->cross_file_deps[i];
            RawDepRef *d = &dst->cross_file_deps[i];
            d->bang_count = s->bang_count;
            d->path       = s->path ? strdup(s->path) : NULL;
            d->owner      = symbol_remap_lookup(remap, s->owner);
            d->range      = s->range;
        }
        dst->num_cross_file_deps = src->num_cross_file_deps;
        dst->cross_file_deps_cap = src->num_cross_file_deps;
    }
}

/** Visitor context for clone_in_file_links(). */
typedef struct { const SymbolRemap *remap; } CloneLinksCtx;

/** Visitor: copy @p old's in-file def_links/ref_links onto @p new_,
 *  translating pointer targets through the remap. */
static void clone_in_file_links_visit(DocSymbol *old, DocSymbol *new_, void *ctx) {
    const SymbolRemap *remap = ((CloneLinksCtx *)ctx)->remap;

    int in_file_def = 0;
    for (int i = 0; i < old->num_def_links; i++)
        if (!old->def_links[i].target_uri) in_file_def++;
    if (in_file_def > 0) {
        new_->def_links     = malloc((size_t)in_file_def * sizeof(DefinitionLink));
        new_->def_links_cap = in_file_def;
        if (!new_->def_links) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        int w = 0;
        for (int i = 0; i < old->num_def_links; i++) {
            if (old->def_links[i].target_uri) continue;
            new_->def_links[w].source     = old->def_links[i].source;
            new_->def_links[w].target     = symbol_remap_lookup(remap,
                                                old->def_links[i].target);
            new_->def_links[w].target_uri = NULL;
            w++;
        }
        new_->num_def_links = w;
    }

    int in_file_ref = 0;
    for (int i = 0; i < old->num_ref_links; i++)
        if (!old->ref_links[i].source_uri) in_file_ref++;
    if (in_file_ref > 0) {
        new_->ref_links     = malloc((size_t)in_file_ref * sizeof(ReferenceLink));
        new_->ref_links_cap = in_file_ref;
        if (!new_->ref_links) {
            fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1);
        }
        int w = 0;
        for (int i = 0; i < old->num_ref_links; i++) {
            if (old->ref_links[i].source_uri) continue;
            new_->ref_links[w].source     = old->ref_links[i].source;
            new_->ref_links[w].origin     = symbol_remap_lookup(remap,
                                                old->ref_links[i].origin);
            new_->ref_links[w].source_uri = NULL;
            w++;
        }
        new_->num_ref_links = w;
    }
}

ParseResult *parse_result_clone_for_revalidate(const ParseResult *src) {
    ParseResult *dst = calloc(1, sizeof(ParseResult));
    if (!dst) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    atomic_store(&dst->refcount, 1);

    /* Pass 1: clone the symbol forest, build the remap. */
    DocSymbol **new_roots = NULL;
    int         n_new_roots = 0;
    SymbolRemap *remap = symbol_tree_clone(src->doc_symbols,
                                           src->num_doc_symbols,
                                           &new_roots, &n_new_roots);
    dst->doc_symbols     = new_roots;
    dst->num_doc_symbols = n_new_roots;
    dst->doc_sym_cap     = n_new_roots;

    /* Pass 2: tok_spans (translated owners), permanent diagnostics,
     * included_files, cross_file_deps. */
    clone_spine_and_translate_owners(dst, src, remap);

    /* Pass 3: per-symbol in-file def_links / ref_links (translated targets). */
    CloneLinksCtx ctx = { .remap = remap };
    symbol_remap_for_each(remap, clone_in_file_links_visit, &ctx);

    symbol_remap_free(remap);
    return dst;
}

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseResult *parse(const char *src) {
    ParseResult *result = calloc(1, sizeof(ParseResult));
    if (!result) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    atomic_store(&result->refcount, 1);

    /* Set up global state for lexer.l and grammar.y */
    g_result          = result;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;
    dep_refs_reset();
    yycolumn        = 0;
    yylineno        = 1;

    /* Feed source to flex and run the bison parser */
    YY_BUFFER_STATE buf = yy_scan_string(src);
    yyparse();
    yy_delete_buffer(buf);

    /* Transfer tok_spans array ownership to the ParseResult */
    result->tok_spans       = g_tok_spans;
    result->num_tok_spans   = g_num_tok_spans;
    result->num_sem_entries = g_num_sem_entries;

    /* Clear globals */
    g_result          = NULL;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;

    /* Build cross-references between tokens, symbols, and dep edges */
    assign_parents(result->doc_symbols, result->num_doc_symbols, NULL);
    assign_token_owners(result);
    resolve_dep_refs(result);

    /* Everything up to here is permanent: syntax errors plus in-file dep
     * diagnostics.  Cross-file diagnostics added later by the server's
     * revalidation pass start at this index and are truncated each cycle. */
    result->dep_diag_start = result->num_diagnostics;

    return result;
}
