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

#include "parser.h"
#include "diagnostics.h"
#include "grammar.tab.h"  /* yyparse() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Raw dependency reference accumulator ──────────────────────────────── *
 *
 * grammar.y calls push_dep_ref() for each dependency reference encountered
 * during parsing.  These are stored in a file-local buffer until parse()
 * calls resolve_dep_refs(), which resolves them in place and populates
 * def_links/ref_links on the target DocSymbols.  The RawDepRef type is
 * declared in parser.h so that references that failed in-file resolution
 * can be stashed on ParseResult.cross_file_deps[] for later re-resolution.
 */

static RawDepRef *g_dep_refs     = NULL;
static int        g_num_dep_refs = 0;
static int        g_dep_ref_cap  = 0;

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

/* Split a dot-separated path string into a heap-allocated array of segments. */
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
typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
extern int             yycolumn; /* column tracker defined in lexer.l */
extern int             yylineno; /* line counter managed by flex %option yylineno */

/* ── Shared globals (used by lexer.l and grammar.y via extern) ───────────── */

ParseResult *g_result          = NULL;
TokenSpan   *g_tok_spans       = NULL;
int          g_num_tok_spans   = 0;
int          g_tok_span_cap    = 0;
int          g_num_sem_entries = 0;

/* Returns 1 if a token of the given kind will be emitted as a semantic token.
 * Matches the skip set in classify() in semantic_tokens.c. */
static int is_sem_highlighted(int kind) {
    return kind != TK_LBRACE && kind != TK_RBRACE &&
           kind != TK_BANG   && kind != TK_DOT    && kind != TK_COMMA;
}

/* Called from lexer.l for every token that callers may need to inspect. */
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

/* Frees the heap-allocated text field of t and sets it to NULL. */
void token_free(Token *t) {
    free(t->text);
    t->text = NULL;
}

/* ── DocSymbol helpers ───────────────────────────────────────────────────── */

/* Recursively frees all heap memory owned by s (name, detail, children array)
 * but does not free s itself, as it is typically stored inline in an array.
 */
void doc_symbol_free(DocSymbol *s) {
    free(s->name);
    free(s->id);
    for (int i = 0; i < s->num_children; i++) {
        doc_symbol_free(s->children[i]);
        free(s->children[i]);
    }
    free(s->children);
    for (int i = 0; i < s->num_def_links; i++)
        free(s->def_links[i].target_uri);
    free(s->def_links);
    for (int i = 0; i < s->num_ref_links; i++)
        free(s->ref_links[i].source_uri);
    free(s->ref_links);
}

/* ── ParseResult helpers ─────────────────────────────────────────────────── */

/* Releases all heap memory owned by r (diagnostics, symbols, token spans,
 * definition links, raw dep refs), then zeroes the struct.
 */
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

/* Appends s to r's doc_symbols pointer array, growing it if needed. */
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

/* Append a heap-allocated copy of the unquoted filename from an include
 * statement.  quoted_text is the raw TK_STR token text (e.g. "\"foo.tji\"");
 * the surrounding quotes are stripped before storing.
 */
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

/* ── DocSymbol tree navigation ───────────────────────────────────────────── */

/* Navigate the symbol tree following the path segments path[0..plen-1] and
 * return the children array at that node.  Transparently descends into
 * KW_PROJECT nodes when matching path segments against tasks.
 *
 * syms  — root-level symbols to start from
 * n     — number of entries in syms
 * path  — array of identifier strings to follow (task IDs)
 * plen  — number of segments in path; 0 returns (syms, n) immediately
 * out_n — set to the number of children at the matched node on success, 0 on failure
 *
 * Returns the children array at the matched node, or NULL if not found.
 */
DocSymbol *const *doc_symbol_find_path(DocSymbol *const *syms, int n,
                                       const char **path, int plen,
                                       int *out_n) {
    if (plen == 0) { *out_n = n; return syms; }
    for (int i = 0; i < n; i++) {
        if (syms[i]->keyword == KW_TASK && syms[i]->id &&
                strcmp(syms[i]->id, path[0]) == 0)
            return doc_symbol_find_path(syms[i]->children, syms[i]->num_children,
                                        path + 1, plen - 1, out_n);
        /* Transparently traverse project containers so that task scope paths
         * rooted inside a project body resolve correctly. */
        if (syms[i]->keyword == KW_PROJECT) {
            DocSymbol *const *found = doc_symbol_find_path(
                syms[i]->children, syms[i]->num_children, path, plen, out_n);
            if (found) return found;
        }
    }
    *out_n = 0;
    return NULL;
}

/* ── DocSymbol tree linkage ─────────────────────────────────────────────── */

/* Recursively set parent pointers for all children in the symbol tree. */
static void assign_parents(DocSymbol **syms, int n, DocSymbol *parent) {
    for (int i = 0; i < n; i++) {
        syms[i]->parent = parent;
        assign_parents(syms[i]->children, syms[i]->num_children, syms[i]);
    }
}

/* ── Token-to-symbol cross-referencing ──────────────────────────────────── */

/* Assign each token span's owner to the deepest enclosing DocSymbol using
 * a single linear sweep.  Both tok_spans[] and symbol ranges are in document
 * order, so we walk them in lockstep with a stack of open symbol scopes.
 *
 * Each stack frame tracks a symbol's children array and the current child
 * index, so we can advance to the next sibling without rescanning.
 *
 * Complexity: O(T + S) where T = num_tok_spans, S = total DocSymbols. */
static void assign_token_owners(ParseResult *r) {
    typedef struct { DocSymbol **syms; int n; int idx; } Frame;
    int frame_cap = 64;
    Frame *stack = malloc((size_t)frame_cap * sizeof(Frame));
    if (!stack) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    int depth = 0;

    /* Seed with the root-level symbols */
    stack[depth++] = (Frame){ r->doc_symbols, r->num_doc_symbols, 0 };

    for (int t = 0; t < r->num_tok_spans; t++) {
        LspPos pos = r->tok_spans[t].start;

        /* Pop frames whose current symbol's range has ended, or that have
         * run out of siblings at this level.  The idx >= n check must come
         * first: after popping and bumping the parent's idx, the parent can
         * itself be exhausted, and we'd otherwise deref past the end. */
        while (depth > 1) {
            Frame *top = &stack[depth - 1];
            if (top->idx < top->n &&
                pos_cmp(pos, top->syms[top->idx]->range.end) < 0)
                break;  /* still inside current sibling at this level */
            /* Back up to parent level and advance to next sibling */
            depth--;
            stack[depth - 1].idx++;
        }

        /* At each level, advance past siblings whose range has ended,
         * then descend into the first child that contains pos. */
        for (;;) {
            Frame *top = &stack[depth - 1];

            /* Skip siblings that ended before this token */
            while (top->idx < top->n &&
                   pos_cmp(pos, top->syms[top->idx]->range.end) >= 0)
                top->idx++;

            if (top->idx >= top->n)
                break;  /* no more siblings at this level */

            DocSymbol *sym = top->syms[top->idx];
            if (pos_cmp(pos, sym->range.start) < 0)
                break;  /* token is before next symbol — gap between symbols */

            /* Token is inside sym; descend into its children */
            if (sym->num_children > 0) {
                if (depth >= frame_cap) {
                    frame_cap *= 2;
                    Frame *tmp = realloc(stack, (size_t)frame_cap * sizeof(Frame));
                    if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
                    stack = tmp;
                }
                stack[depth++] = (Frame){ sym->children, sym->num_children, 0 };
            } else {
                break;  /* leaf symbol — this is the deepest */
            }
        }

        /* Owner is the deepest symbol we're inside, or NULL if outside all */
        if (depth > 1) {
            Frame *top = &stack[depth - 1];
            if (top->idx < top->n) {
                DocSymbol *sym = top->syms[top->idx];
                if (pos_cmp(pos, sym->range.start) >= 0 &&
                    pos_cmp(pos, sym->range.end) < 0) {
                    r->tok_spans[t].owner = sym;
                    continue;
                }
            }
            /* Check parent frames — token may be in a parent but between children */
            for (int d = depth - 2; d >= 1; d--) {
                DocSymbol *sym = stack[d].syms[stack[d].idx];
                if (pos_cmp(pos, sym->range.start) >= 0 &&
                    pos_cmp(pos, sym->range.end) < 0) {
                    r->tok_spans[t].owner = sym;
                    break;
                }
            }
        }
        /* If no frame matched, owner stays NULL (the default) */
    }

    free(stack);
}

/* ── Dependency edge resolution ────────────────────────────────────────── */

/* Append a DefinitionLink to a DocSymbol's def_links array. */
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

/* Append a ReferenceLink to a DocSymbol's ref_links array. */
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

/* Find a task DocSymbol by navigating to the parent scope via
 * doc_symbol_find_path(), then scanning for the final segment.
 * Returns the matched node, or NULL if any segment is not found.
 * Only KW_TASK nodes are matched at the leaf level. */
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

/* Build an "unresolved dependency: a.b.c" message from segs[] and push
 * it onto r->diagnostics at the given range. */
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

/* Append a RawDepRef onto r->cross_file_deps[], taking ownership of path. */
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

/* Resolve dep refs accumulated by the grammar: split paths, resolve targets
 * in-file, populate def_links/ref_links on success.  For 0-bang refs that
 * fail in-file lookup, stash them on r->cross_file_deps[] for the server to
 * retry against other open documents — no diagnostic yet.  For bang refs,
 * which are strictly in-file, emit an unresolved-dependency diagnostic on
 * failure.  Frees the global accumulator at the end. */
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

/* Walk s and its descendants, compacting link arrays in place by dropping
 * any entry that carries a cross-document URI.  Frees the URI strings. */
static void strip_cross_file_links(DocSymbol *s) {
    int keep = 0;
    for (int i = 0; i < s->num_def_links; i++) {
        if (s->def_links[i].target_uri) {
            free(s->def_links[i].target_uri);
        } else {
            if (keep != i) s->def_links[keep] = s->def_links[i];
            keep++;
        }
    }
    s->num_def_links = keep;

    keep = 0;
    for (int i = 0; i < s->num_ref_links; i++) {
        if (s->ref_links[i].source_uri) {
            free(s->ref_links[i].source_uri);
        } else {
            if (keep != i) s->ref_links[keep] = s->ref_links[i];
            keep++;
        }
    }
    s->num_ref_links = keep;

    for (int i = 0; i < s->num_children; i++)
        strip_cross_file_links(s->children[i]);
}

void clear_cross_file_state(ParseResult *r) {
    for (int i = r->dep_diag_start; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    r->num_diagnostics = r->dep_diag_start;

    for (int i = 0; i < r->num_doc_symbols; i++)
        strip_cross_file_links(r->doc_symbols[i]);
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

/* ── Public parse() entry point ──────────────────────────────────────────── */

ParseResult parse(const char *src) {
    /* Set up global state for lexer.l and grammar.y */
    ParseResult result = {0};
    g_result          = &result;
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
    result.tok_spans       = g_tok_spans;
    result.num_tok_spans   = g_num_tok_spans;
    result.num_sem_entries = g_num_sem_entries;

    /* Clear globals */
    g_result          = NULL;
    g_tok_spans       = NULL;
    g_num_tok_spans   = 0;
    g_tok_span_cap    = 0;
    g_num_sem_entries = 0;

    /* Build cross-references between tokens, symbols, and dep edges */
    assign_parents(result.doc_symbols, result.num_doc_symbols, NULL);
    assign_token_owners(&result);
    resolve_dep_refs(&result);

    /* Everything up to here is permanent: syntax errors plus in-file dep
     * diagnostics.  Cross-file diagnostics added later by the server's
     * revalidation pass start at this index and are truncated each cycle. */
    result.dep_diag_start = result.num_diagnostics;

    return result;
}
