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
 * B. Semantic checks (emitted after yyparse() in validate_dep_refs())
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
 * hierarchy relative to the referring task.  After yyparse() completes and
 * the full symbol tree is available, validate_dep_refs() checks every
 * reference recorded by push_dep_ref() against the tree.
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

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Diagnostic accumulation ─────────────────────────────────────────────── */

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

typedef struct {
    int    bang_count;
    char **segs;       /* path segments, heap-copied */
    int    nseg;
    char **scope;      /* snapshot of task scope at parse time, heap-copied */
    int    scope_n;
    LspRange range;
} DepRef;

static DepRef *g_dep_refs     = NULL;
static int     g_num_dep_refs = 0;
static int     g_dep_ref_cap  = 0;

void dep_refs_reset(void) {
    g_dep_refs     = NULL;
    g_num_dep_refs = 0;
    g_dep_ref_cap  = 0;
}

void push_dep_ref(ParseResult *r, int bang_count, const char *path,
                  const char **scope, int scope_n,
                  LspPos start, LspPos end) {
    (void)r; /* not used directly; diagnostics emitted in validate_dep_refs */
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

static int resolve_from(const DocSymbol *syms, int n, const char **segs,
                        int nseg) {
    if (nseg == 0) return 1;
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == SK_FUNCTION
                && strcmp(syms[i].detail, segs[0]) == 0)
            return resolve_from(syms[i].children, syms[i].num_children,
                                segs + 1, nseg - 1);
    }
    return 0;
}

static int validate_ref(const DocSymbol *syms, int nsym,
                        const char **scope, int scope_len,
                        int bang_count, const char **segs, int nseg) {
    const DocSymbol *ctx;
    int nctx;
    if (bang_count == 0) {
        ctx  = syms;
        nctx = nsym;
    } else {
        int k = scope_len;
        if (bang_count > k) return 0;
        ctx = doc_symbol_find_path(syms, nsym, scope, k - bang_count, &nctx);
        if (!ctx) { nctx = 0; ctx = NULL; }
    }
    return resolve_from(ctx, nctx, segs, nseg);
}

void validate_dep_refs(const DocSymbol *syms, int nsym, ParseResult *r) {
    for (int i = 0; i < g_num_dep_refs; i++) {
        const DepRef *dr = &g_dep_refs[i];
        if (!validate_ref(syms, nsym,
                          (const char **)dr->scope, dr->scope_n,
                          dr->bang_count,
                          (const char **)dr->segs, dr->nseg)) {
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

/* ── LSP publishDiagnostics notification ─────────────────────────────────── */

static void write_message(const char *msg) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
    fflush(stdout);
}

void publish_diagnostics(const char *uri, const ParseResult *r) {
    cJSON *diag_arr = cJSON_CreateArray();
    for (int i = 0; i < r->num_diagnostics; i++) {
        const Diagnostic *d = &r->diagnostics[i];
        cJSON *dj = cJSON_CreateObject();

        cJSON *range = cJSON_CreateObject();
        cJSON *start = cJSON_CreateObject();
        cJSON_AddNumberToObject(start, "line",      d->range.start.line);
        cJSON_AddNumberToObject(start, "character", d->range.start.character);
        cJSON *end = cJSON_CreateObject();
        cJSON_AddNumberToObject(end, "line",      d->range.end.line);
        cJSON_AddNumberToObject(end, "character", d->range.end.character);
        cJSON_AddItemToObject(range, "start", start);
        cJSON_AddItemToObject(range, "end",   end);
        cJSON_AddItemToObject(dj, "range", range);
        cJSON_AddNumberToObject(dj, "severity", d->severity);
        cJSON_AddStringToObject(dj, "message",  d->message);
        cJSON_AddItemToArray(diag_arr, dj);
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    cJSON_AddItemToObject(params, "diagnostics", diag_arr);

    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "textDocument/publishDiagnostics");
    cJSON_AddItemToObject(notif, "params", params);

    char *text = cJSON_PrintUnformatted(notif);
    write_message(text);
    cJSON_free(text);
    cJSON_Delete(notif);
}
