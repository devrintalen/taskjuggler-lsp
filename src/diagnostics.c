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

/*
 * diagnostics.c — validation checks and LSP diagnostic delivery
 *
 * This file is responsible for two things:
 *
 *   1. Collecting and emitting diagnostics during and after parsing.
 *   2. Sending the textDocument/publishDiagnostics LSP notification to the
 *      client so that errors and warnings appear in the editor.
 *
 * ── Checks performed ─────────────────────────────────────────────────────
 *
 * Currently two categories of checks are performed:
 *
 * A. Syntax errors (emitted during yyparse() via grammar.y error rules)
 * B. Semantic checks (emitted by revalidate_dep_refs() after all documents
 *
 *
 * A. Syntax errors
 * ────────────────
 * These are detected by Bison's error-recovery mechanism.  When the parser
 * encounters an unexpected token it attempts to recover and, in some cases,
 * emits a diagnostic directly via push_diagnostic().
 *
 * Currently detected:
 *
 *   • Unclosed `{` block  (DIAG_ERROR)
 *
 *     A `{` was opened to begin a body block but the matching `}` was never
 *     found before end-of-file or another unrecoverable token.  The
 *     diagnostic range covers the opening `{`.
 *
 *     Failing example:
 *
 *       project acme "ACME" 2026-01-01 +6m {
 *           task design "Design" {
 *               task ui "UI" {
 *                   effort 5d
 *               # missing `}` to close `ui`
 *           }
 *       }
 *
 *     Note: yyerror() is currently a no-op, so generic parse errors are
 *     silently discarded.  Only the explicit error-recovery rules in
 *     grammar.y produce diagnostics.
 *
 *
 * B. Semantic checks: dependency reference validation
 * ────────────────────────────────────────────────────
 * TaskJuggler's `depends` and `precedes` keywords accept task references
 * that use a leading sequence of `!` characters to navigate the task
 * hierarchy relative to the referring task.  Raw references are collected
 * during parsing via push_dep_ref() and transferred to ParseResult.  The
 * server calls revalidate_dep_refs() after every document change, supplying
 * all open documents as extra symbol pools so cross-file references resolve.
 * revalidate_dep_refs() builds a root-level SymMap (hash map) once per call
 * for O(n+m) validation instead of the O(n×m) linear scan it replaced.
 *
 * Reference resolution rules (k = nesting depth of the referring task):
 *
 *   • 0 bangs  — absolute lookup from the project root.
 *   • n bangs  — navigate up n levels from the referring task, then search
 *                that ancestor's children.  The lookup root becomes
 *                scope[0..k-n-1].
 *   • n > k    — the reference escapes beyond the project root; invalid.
 *
 * Currently detected:
 *
 *   • Dependency reference escapes project root  (DIAG_WARNING)
 *
 *     The number of `!` prefixes exceeds the nesting depth of the task
 *     that contains the `depends` or `precedes` clause.
 *
 *     Failing example (top-level task has depth 0, so any `!` escapes):
 *
 *       project acme "ACME" 2026-01-01 +6m {
 *           task frontend "Frontend" {
 *               depends !backend    # `!` navigates above root — invalid
 *           }
 *           task backend "Backend" {}
 *       }
 *
 *     Correct form (no bangs = absolute reference from project root):
 *
 *       depends backend
 *
 *   • Unresolved task dependency  (DIAG_ERROR)
 *
 *     The dependency path does not match any task in the resolved lookup
 *     scope.  Only task symbols (SK_FUNCTION) are searched; resources,
 *     accounts, and shifts cannot be dependency targets.
 *
 *     Failing examples:
 *
 *       project acme "ACME" 2026-01-01 +6m {
 *           task frontend "Frontend" {
 *               depends backend     # `backend` task does not exist
 *           }
 *       }
 *
 *       project acme "ACME" 2026-01-01 +6m {
 *           task software "Software" {
 *               task gui "GUI" {
 *                   depends !spec   # `spec` is not a child of `software`
 *               }
 *               task engine "Engine" {}
 *           }
 *       }
 *
 *       project acme "ACME" 2026-01-01 +6m {
 *           task software "Software" {
 *               task gui "GUI" {
 *                   # dot-path: `engine` is not a child of `gui`
 *                   depends !gui.engine
 *               }
 *               task engine "Engine" {}
 *           }
 *       }
 *
 * ── Adding new checks ─────────────────────────────────────────────────────
 *
 * Checks that can be evaluated on individual tokens or grammar rules should
 * call push_diagnostic() directly from a grammar.y semantic action.
 *
 * Checks that require the complete symbol tree (cross-reference validation,
 * cycle detection, resource over-allocation, etc.) should follow the same
 * pattern as dependency validation: collect raw data during parsing via a
 * push_*() function, then evaluate it in a post-parse pass called from
 * parser.c:parse() between yyparse() and parse_result_free().
 */

#include "diagnostics.h"
#include "parser.h"
#include "server.h"

#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Diagnostic accumulation ─────────────────────────────────────────────── */

/* Append a diagnostic to r's diagnostics array, growing it if needed.
 * range    — source range to highlight in the editor
 * severity — DIAG_ERROR or DIAG_WARNING
 * msg      — human-readable message; a heap copy is made and owned by r
 */
void push_diagnostic(ParseResult *r, LspRange range, int severity,
                     const char *msg) {
    if (r->num_diagnostics >= r->diag_cap) {
        int nc = r->diag_cap ? r->diag_cap * 2 : 4;
        r->diagnostics = realloc(r->diagnostics,
                                 (size_t)nc * sizeof(Diagnostic));
        r->diag_cap = nc;
    }
    r->diagnostics[r->num_diagnostics++] =
        (Diagnostic){ range, severity, strdup(msg) };
}

/* ── Dependency reference tracking ──────────────────────────────────────── */

static DepRef *g_dep_refs     = NULL;
static int     g_num_dep_refs = 0;
static int     g_dep_ref_cap  = 0;

/* Reset the global dep-ref accumulator to an empty state.
 * Called at the start of each parse() invocation before yyparse() runs.
 */
void dep_refs_reset(void) {
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

/* Record one raw dependency reference encountered during parsing.
 * r          — current ParseResult (unused directly; diagnostics deferred)
 * bang_count — number of leading '!' characters in the reference
 * path       — dot-separated task path after the bangs (e.g. "gui.engine")
 * scope      — task ID path from root to the containing task at parse time
 * scope_n    — number of segments in scope
 * start/end  — source range of the full reference expression
 */
void push_dep_ref(ParseResult *r, int bang_count, const char *path,
                  const char **scope, int scope_n,
                  LspPos start, LspPos end) {
    (void)r; /* not used directly; diagnostics emitted in revalidate_dep_refs */
    if (g_dep_ref_cap <= g_num_dep_refs) {
        g_dep_ref_cap = g_dep_ref_cap ? g_dep_ref_cap * 2 : 8;
        g_dep_refs = realloc(g_dep_refs, (size_t)g_dep_ref_cap * sizeof(DepRef));
    }
    DepRef *dr = &g_dep_refs[g_num_dep_refs++];

    /* Split path by '.' into segs[] */
    dr->nseg = 0;
    dr->segs = NULL;
    if (path && path[0]) {
        /* Count segments */
        int cap = 1;
        for (const char *p = path; *p; p++) if (*p == '.') cap++;
        dr->segs = malloc((size_t)cap * sizeof(char *));
        char *tmp = strdup(path);
        char *tok = strtok(tmp, ".");
        while (tok) {
            dr->segs[dr->nseg++] = strdup(tok);
            tok = strtok(NULL, ".");
        }
        free(tmp);
    }

    /* Snapshot scope */
    dr->scope_n = scope_n;
    dr->scope   = scope_n ? malloc((size_t)scope_n * sizeof(char *)) : NULL;
    for (int i = 0; i < scope_n; i++)
        dr->scope[i] = strdup(scope[i]);

    dr->bang_count = bang_count;
    dr->range      = (LspRange){ start, end };
}

/* Transfer ownership of the global dep-ref accumulator into r.
 * After this call the global state is reset to empty.
 * Called at the end of parse() to move raw_dep_refs into the ParseResult.
 */
void dep_refs_transfer(ParseResult *r) {
    r->raw_dep_refs     = g_dep_refs;
    r->num_raw_dep_refs = g_num_dep_refs;
    r->raw_dep_ref_cap  = g_dep_ref_cap;
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

/* Free all memory in the global dep-ref accumulator and reset it.
 * Used as a cleanup path if parsing is abandoned before dep_refs_transfer().
 */
void free_dep_refs(void) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        DepRef *dr = &g_dep_refs[i];
        for (int j = 0; j < dr->nseg; j++) free(dr->segs[j]);
        free(dr->segs);
        for (int j = 0; j < dr->scope_n; j++) free(dr->scope[j]);
        free(dr->scope);
    }
    free(g_dep_refs);
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

/* ── SymMap: string-keyed lookup table for DocSymbol pointers ────────────── *
 *
 * Open-addressing hash map with linear probing.  Keys are borrowed pointers
 * into DocSymbol.detail (owned by the ParseResult); the map does not copy or
 * free them.  Cap must be a power of two and at least 2× the entry count.
 */
typedef struct { const char *key; const DocSymbol *val; } SymMapEntry;
typedef struct { SymMapEntry *entries; int cap; } SymMap;

/* Allocate and zero-initialise a SymMap with the given capacity.
 * cap must be a power of two and at least 2× the number of entries that
 * will be inserted to keep the load factor below 0.5.
 */
static void symmap_init(SymMap *m, int cap) {
    m->cap     = cap;
    m->entries = calloc((size_t)cap, sizeof(SymMapEntry));
}

/* Release the entry array of m and reset its fields to zero. */
static void symmap_free(SymMap *m) {
    free(m->entries);
    m->entries = NULL;
    m->cap     = 0;
}

/* FNV-1a 32-bit hash of the NUL-terminated string s. */
static uint32_t symmap_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* Insert (key, val) into m using open addressing with linear probing.
 * Duplicate keys are silently ignored; m must have spare capacity.
 */
static void symmap_insert(SymMap *m, const char *key, const DocSymbol *val) {
    uint32_t i = symmap_hash(key) & (uint32_t)(m->cap - 1);
    while (m->entries[i].key) {
        if (strcmp(m->entries[i].key, key) == 0) return;
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    m->entries[i] = (SymMapEntry){ key, val };
}

/* Look up key in m and return the associated DocSymbol pointer,
 * or NULL if not found.
 */
static const DocSymbol *symmap_get(const SymMap *m, const char *key) {
    uint32_t i = symmap_hash(key) & (uint32_t)(m->cap - 1);
    while (m->entries[i].key) {
        if (strcmp(m->entries[i].key, key) == 0) return m->entries[i].val;
        i = (i + 1) & (uint32_t)(m->cap - 1);
    }
    return NULL;
}

/* Populate map from syms[], recursing into SK_MODULE transparently so that
 * tasks inside a project { } block are included at the same level as
 * top-level tasks. */
static void symmap_populate(SymMap *m, const DocSymbol *syms, int n) {
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION && syms[i].detail)
            symmap_insert(m, syms[i].detail, &syms[i]);
        else if (syms[i].kind == SK_MODULE)
            symmap_populate(m, syms[i].children, syms[i].num_children);
    }
}

/* Recursively resolve segs[0..nseg-1] starting from the syms[] array.
 * Returns the matching DocSymbol leaf, or NULL if the path is not found.
 * SK_MODULE (project) nodes are traversed transparently so that tasks
 * inside a project body can be found without explicitly naming the project.
 * syms/n — symbol array to search at the current depth
 * segs   — remaining path segments to match
 * nseg   — number of remaining segments
 */
static const DocSymbol *resolve_from(const DocSymbol *syms, int n,
                                     const char **segs, int nseg) {
    if (nseg == 0) return NULL;
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION
                && strcmp(syms[i].detail, segs[0]) == 0) {
            if (nseg == 1) return &syms[i];
            return resolve_from(syms[i].children, syms[i].num_children,
                                segs + 1, nseg - 1);
        }
        /* Transparently search through project containers (SK_MODULE): tasks
         * always live inside a project body, so the project acts as a
         * namespace wrapper that should not require explicit path traversal. */
        if (syms[i].kind == SK_MODULE) {
            const DocSymbol *found = resolve_from(syms[i].children,
                                                  syms[i].num_children,
                                                  segs, nseg);
            if (found) return found;
        }
    }
    return NULL;
}

/* Like resolve_from() but uses a pre-built SymMap for the first path segment.
 * Falls back to the regular linear resolve_from() for subsequent segments
 * (child arrays are small so linear scan is fine there). */
static const DocSymbol *resolve_from_map(const SymMap *map,
                                          const char **segs, int nseg) {
    if (nseg == 0) return NULL;
    const DocSymbol *first = symmap_get(map, segs[0]);
    if (!first) return NULL;
    if (nseg == 1) return first;
    return resolve_from(first->children, first->num_children, segs + 1, nseg - 1);
}

/* Resolve a single raw dep ref against the symbol tree, applying bang
 * navigation first.  Returns the matching DocSymbol on success, NULL on failure.
 * syms/nsym        — root-level symbol array
 * scope/scope_len  — task IDs from root to the containing task
 * bang_count       — number of leading '!' characters in the reference
 * segs/nseg        — dot-split path segments after the bangs
 */
static const DocSymbol *validate_ref(const DocSymbol *syms, int nsym,
                                     const char **scope, int scope_len,
                                     int bang_count, const char **segs,
                                     int nseg) {
    const DocSymbol *ctx;
    int nctx;
    if (bang_count == 0) {
        ctx  = syms;
        nctx = nsym;
    } else {
        int k = scope_len;
        if (bang_count > k) return NULL;
        ctx = doc_symbol_find_path(syms, nsym, scope, k - bang_count, &nctx);
        if (!ctx) { nctx = 0; ctx = NULL; }
    }
    return resolve_from(ctx, nctx, segs, nseg);
}

/* Append a DefinitionLink to r's def_links array, growing it if needed.
 * source     — range of the reference expression in the current document
 * target     — selection range of the matching task symbol declaration
 * target_uri — URI of the file containing the target (NULL = same file)
 */
static void push_def_link(ParseResult *r, LspRange source, LspRange target,
                          const char *target_uri) {
    if (r->num_def_links >= r->def_link_cap) {
        int nc = r->def_link_cap ? r->def_link_cap * 2 : 4;
        r->def_links = realloc(r->def_links,
                               (size_t)nc * sizeof(DefinitionLink));
        r->def_link_cap = nc;
    }
    r->def_links[r->num_def_links++] = (DefinitionLink){
        source, target, target_uri ? strdup(target_uri) : NULL
    };
}

/* Validate dep refs from the global accumulator against syms[].
 * Emits diagnostics into r for invalid references and DefinitionLinks for
 * valid ones.  Uses a linear scan.  Note: this function is not called in
 * the normal server code path — see revalidate_dep_refs() for the live
 * cross-file implementation.
 */
void validate_dep_refs(const DocSymbol *syms, int nsym, ParseResult *r) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        const DepRef *dr = &g_dep_refs[i];
        const DocSymbol *sym = validate_ref(syms, nsym,
                                            (const char **)dr->scope,
                                            dr->scope_n,
                                            dr->bang_count,
                                            (const char **)dr->segs,
                                            dr->nseg);
        if (sym) {
            /* Valid reference — record a definition link for go-to-definition */
            push_def_link(r, dr->range, sym->selection_range, NULL);
        } else {
            /* Invalid reference — emit diagnostic */
            if (dr->bang_count > dr->scope_n) {
                /* The number of leading `!` characters exceeds the nesting
                 * depth of the containing task.  e.g. a top-level task
                 * (scope_n=0) writing `depends !other` has one bang but zero
                 * levels to ascend. */
                push_diagnostic(r, dr->range, DIAG_WARNING,
                    "dependency reference escapes beyond project root");
            } else {
                /* The bang count is valid but the resulting path does not
                 * match any task in the resolved lookup scope.  Reconstruct
                 * the dot-separated path for the error message. */
                char path[256] = "";
                for (int j = 0; j < dr->nseg; j++) {
                    if (j > 0) strncat(path, ".", sizeof(path) - strlen(path) - 1);
                    strncat(path, dr->segs[j], sizeof(path) - strlen(path) - 1);
                }
                char msg[320];
                snprintf(msg, sizeof(msg), "unresolved task: `%s`", path);
                push_diagnostic(r, dr->range, DIAG_ERROR, msg);
            }
        }
    }
}

/* ── Cross-file dependency revalidation ──────────────────────────────────── */

/* Re-run dep-ref validation against the current document and extra symbol
 * pools from all other open documents.  Called after every document change.
 *
 * r            — ParseResult for the document being validated; its diagnostics
 *                from dep_diag_start onward and all def_links are replaced
 * extra_pools  — doc_symbols arrays from other open documents
 * extra_counts — corresponding num_doc_symbols counts
 * extra_uris   — corresponding document URIs (used in cross-file DefinitionLinks)
 * num_extra    — number of extra documents
 *
 * Uses a root-level SymMap (O(1) lookups) for absolute references and a
 * linear scan of bounded child arrays for relative (bang-navigated) ones,
 * giving O(n+m) overall instead of O(n×m).
 */
void revalidate_dep_refs(ParseResult *r,
                         const DocSymbol * const *extra_pools,
                         const int *extra_counts,
                         const char * const *extra_uris,
                         int num_extra) {
    /* Trim dep-validation diagnostics, keeping syntax errors */
    for (int i = r->dep_diag_start; i < r->num_diagnostics; i++)
        free(r->diagnostics[i].message);
    r->num_diagnostics = r->dep_diag_start;

    /* Clear definition links */
    for (int i = 0; i < r->num_def_links; i++)
        free(r->def_links[i].target_uri);
    r->num_def_links = 0;

    /* Build root-level lookup map once for all dep refs in this document. */
    int map_cap = 16;
    while (map_cap < r->num_doc_symbols * 2 + 1) map_cap <<= 1;
    SymMap root_map;
    symmap_init(&root_map, map_cap);
    symmap_populate(&root_map, r->doc_symbols, r->num_doc_symbols);

    for (int i = 0; i < r->num_raw_dep_refs; i++) {
        const DepRef      *dr        = &r->raw_dep_refs[i];
        const DocSymbol   *sym       = NULL;
        const char        *found_uri = NULL;

        int k = dr->scope_n;

        /* Too many bangs: reference escapes the project root. */
        if (dr->bang_count > k) {
            push_diagnostic(r, dr->range, DIAG_WARNING,
                "dependency reference escapes beyond project root");
            continue;
        }

        /* nav_len: scope levels to navigate before searching.
         * bang_count == 0 is an absolute reference — always searches from
         * the project root (nav_len = 0).
         * bang_count > 0 navigates up (k - bang_count) levels; if that also
         * reaches the root, use the hash map too. */
        int nav_len = (dr->bang_count == 0) ? 0 : (k - dr->bang_count);

        if (nav_len == 0) {
            /* Root-level lookup — use the hash map. */
            sym = resolve_from_map(&root_map,
                                   (const char **)dr->segs, dr->nseg);
        } else {
            /* Subtree lookup — navigate to the ancestor, then linear scan.
             * Child arrays are bounded by the tree branching factor so this
             * path stays fast. */
            int nctx;
            const DocSymbol *ctx = doc_symbol_find_path(
                r->doc_symbols, r->num_doc_symbols,
                (const char **)dr->scope, nav_len, &nctx);
            if (ctx)
                sym = resolve_from(ctx, nctx,
                                   (const char **)dr->segs, dr->nseg);
        }

        /* For absolute references (no bangs), also search other open files. */
        if (!sym && dr->bang_count == 0) {
            for (int p = 0; p < num_extra; p++) {
                sym = resolve_from(extra_pools[p], extra_counts[p],
                                   (const char **)dr->segs, dr->nseg);
                if (sym) { found_uri = extra_uris[p]; break; }
            }
        }

        if (sym) {
            push_def_link(r, dr->range, sym->selection_range, found_uri);
        } else {
            char path[256] = "";
            for (int j = 0; j < dr->nseg; j++) {
                if (j > 0) strncat(path, ".", sizeof(path) - strlen(path) - 1);
                strncat(path, dr->segs[j], sizeof(path) - strlen(path) - 1);
            }
            char msg[320];
            snprintf(msg, sizeof(msg), "unresolved task: `%s`", path);
            push_diagnostic(r, dr->range, DIAG_ERROR, msg);
        }
    }

    symmap_free(&root_map);
}

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

/* Send a textDocument/publishDiagnostics notification to the editor.
 * Serialises r->diagnostics[] as a JSON-RPC notification and writes it to
 * stdout via lsp_send_message().  Passing a zeroed ParseResult (r->num_diagnostics=0)
 * clears any editor-side errors for the document.
 * uri — document URI to include in the notification params
 * r   — ParseResult whose diagnostics[] to publish
 */
void publish_diagnostics(const char *uri, const ParseResult *r) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

    yyjson_mut_val *diag_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < r->num_diagnostics; i++) {
        const Diagnostic *d = &r->diagnostics[i];
        yyjson_mut_val *dj = yyjson_mut_obj(doc);

        yyjson_mut_val *range = yyjson_mut_obj(doc);
        yyjson_mut_val *start = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, start, "line",      d->range.start.line);
        yyjson_mut_obj_add_uint(doc, start, "character", d->range.start.character);
        yyjson_mut_val *end = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, end, "line",      d->range.end.line);
        yyjson_mut_obj_add_uint(doc, end, "character", d->range.end.character);
        yyjson_mut_obj_add_val(doc, range, "start", start);
        yyjson_mut_obj_add_val(doc, range, "end",   end);
        yyjson_mut_obj_add_val(doc,  dj, "range",    range);
        yyjson_mut_obj_add_uint(doc, dj, "severity", (uint64_t)d->severity);
        yyjson_mut_obj_add_str(doc,  dj, "message",  d->message);
        yyjson_mut_arr_add_val(diag_arr, dj);
    }

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, params, "uri", uri);
    yyjson_mut_obj_add_val(doc, params, "diagnostics", diag_arr);

    yyjson_mut_val *notif = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, notif, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, notif, "method",  "textDocument/publishDiagnostics");
    yyjson_mut_obj_add_val(doc, notif, "params",  params);

    yyjson_mut_doc_set_root(doc, notif);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    lsp_send_message(text);
    free(text);
}
