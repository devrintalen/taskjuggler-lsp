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

#include "completion.h"
#include "hover.h"
#include "signature.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── CompletionItemKind values ───────────────────────────────────────────── */
#define CIK_FUNCTION  3
#define CIK_VARIABLE  6
#define CIK_CLASS     7
#define CIK_KEYWORD   14
#define CIK_REFERENCE 18

/* ── block_stack ─────────────────────────────────────────────────────────── */

/* Returns 1 if token kind k introduces a block whose body has a
 * context-specific keyword set (i.e. keywords valid inside that block differ
 * from the top-level set).
 */
static int is_block_opener_kind(int k) {
    return k == KW_PROJECT   || k == KW_TASK        || k == KW_RESOURCE
        || k == KW_ACCOUNT   || k == KW_SHIFT       || k == KW_SCENARIO
        || k == KW_SUPPLEMENT|| k == KW_EXTEND      || k == KW_JOURNALENTRY
        || k == KW_LIMITS    || k == KW_NAVIGATOR   || k == KW_TASKREPORT
        || k == KW_TEXTREPORT|| k == KW_RESOURCEREPORT;
}

/* Build the stack of block-opener keyword names enclosing cursor.
 * Each entry is the keyword that opened the corresponding { block.
 * Returns a heap-allocated array of heap-allocated strings; caller must free
 * with free_block_stack().
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — position used as the scan stop point
 * out_n      — set to the number of entries returned
 */
static char **block_stack(const TokenSpan *tokens, int num_tokens,
                           LspPos cursor, int *out_n) {
    char **stack = NULL;
    int    n = 0, cap = 0;
    char  *pending = NULL;

    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *tok = &tokens[i];
        if (tok->token_kind == TK_EOF || pos_cmp(cursor, tok->start) < 0) break;

        switch (tok->token_kind) {
        case TK_LINE_COMMENT:
        case TK_BLOCK_COMMENT:
            break;

        case TK_LBRACE:
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                stack = realloc(stack, cap * sizeof(char *));
            }
            stack[n++] = pending ? pending : strdup("");
            pending = NULL;
            break;

        case TK_RBRACE:
            free(pending);
            pending = NULL;
            if (n > 0) { free(stack[--n]); }
            break;

        default:
            if (is_block_opener_kind(tok->token_kind) && tok->text) {
                free(pending);
                pending = strdup(tok->text);
            }
            break;
        }
    }

    free(pending);
    *out_n = n;
    return stack;
}

/* Free a block_stack result returned by block_stack(). */
static void free_block_stack(char **stack, int n) {
    for (int i = 0; i < n; i++) free(stack[i]);
    free(stack);
}

/* ── Keyword tables ──────────────────────────────────────────────────────── */

typedef struct { const char *kw; const char *doc; } KwEntry;

static const KwEntry TOPLEVEL_KWS[] = {
    {"project",          "Project header (required once per file)"},
    {"task",             "Work package, summary task, or milestone"},
    {"resource",         "Person, team, or equipment"},
    {"account",          "Cost/revenue account"},
    {"shift",            "Named working-hours shift"},
    {"scenario",         "Alternative scheduling scenario"},
    {"macro",            "Reusable text macro"},
    {"include",          "Include another .tjp/.tji file"},
    {"flags",            "Declare flag identifiers"},
    {"supplement",       "Add attributes to an existing declaration"},
    {"rate",             "Default daily cost rate for all resources"},
    {"leaves",           "Global leave block"},
    {"vacation",         "Global vacation / unavailability period"},
    {"now",              "Reference date for reports"},
    {"currency",         "Currency symbol"},
    {"timeformat",       "Date/time display format string"},
    {"timezone",         "IANA timezone identifier"},
    {"workinghours",     "Working hours definition"},
    {"timingresolution", "Scheduling granularity in minutes"},
    {"balance",          "Profit/loss account pair"},
    {"extend",           "Add custom attributes to a built-in type"},
    {"navigator",        "Navigation bar for HTML reports"},
    {"taskreport",       "Task-based report definition"},
    {"textreport",       "HTML/text report definition"},
    {"resourcereport",   "Resource-based report definition"},
    {NULL, NULL}
};

static const KwEntry TASK_KWS[] = {
    {"task",          "Sub-task"},
    {"effort",        "Work required, e.g. `effort 5d`"},
    {"duration",      "Elapsed wall-clock duration"},
    {"length",        "Duration in working time"},
    {"milestone",     "Mark as a zero-duration milestone"},
    {"scheduled",     "Task has fixed pre-computed dates"},
    {"depends",       "Finish-to-start dependency on task id(s)"},
    {"precedes",      "Reverse dependency: listed tasks start after this one"},
    {"allocate",      "Assign resource(s) to this task"},
    {"start",         "Hard start date constraint (YYYY-MM-DD)"},
    {"end",           "Hard end date constraint (YYYY-MM-DD)"},
    {"maxstart",      "Maximum desired start date"},
    {"minstart",      "Minimum desired start date"},
    {"maxend",        "Maximum desired end date"},
    {"minend",        "Minimum desired end date"},
    {"priority",      "Scheduling priority 1-1000 (default 500)"},
    {"complete",      "Completion percentage 0-100"},
    {"note",          "Attach a text note"},
    {"responsible",   "Resource(s) accountable for this task"},
    {"booking",       "Record actual work performed"},
    {"charge",        "Charge amount to an account"},
    {"chargeset",     "Account to book labor costs to"},
    {"purge",         "Remove an inherited attribute value"},
    {"journalentry",  "Status / journal entry"},
    {"flags",         "Assign flags to this task"},
    {"supplement",    "Add attributes to an existing task"},
    {"limits",        "Effort or allocation limits (dailymax, weeklymax, ...)"},
    {"statusnote",    "Status note for dashboards"},
    {NULL, NULL}
};

static const KwEntry RESOURCE_KWS[] = {
    {"resource",      "Sub-resource"},
    {"rate",          "Daily cost rate"},
    {"efficiency",    "Work output multiplier (default 1.0)"},
    {"vacation",      "Vacation / unavailability period"},
    {"leaves",        "Leave block by type"},
    {"workinghours",  "Working hours override for this resource"},
    {"limits",        "Working limits (dailymax, weeklymax, ...)"},
    {"flags",         "Assign flags"},
    {"managers",      "Manager resource(s)"},
    {"booking",       "Record actual work performed"},
    {"journalentry",  "Journal entry"},
    {"supplement",    "Add attributes to an existing resource"},
    {"shift",         "Apply a working-hours shift"},
    {NULL, NULL}
};

static const KwEntry PROJECT_KWS[] = {
    {"scenario",          "Define a scheduling scenario"},
    {"extend",            "Add custom attributes to a built-in type"},
    {"timezone",          "IANA timezone identifier"},
    {"timeformat",        "Date/time display format string"},
    {"now",               "Reference date for reports"},
    {"currency",          "Currency symbol"},
    {"workinghours",      "Default working hours"},
    {"timingresolution",  "Scheduling granularity in minutes"},
    {"dailyworkinghours", "Hours per working day"},
    {"weeklyworkinghours","Hours per working week"},
    {"balance",           "Profit/loss account pair"},
    {"outputdir",         "Output directory for generated reports"},
    {NULL, NULL}
};

static const KwEntry ACCOUNT_KWS[] = {
    {"account", "Sub-account"},
    {"credit",  "Credit transaction"},
    {"debit",   "Debit transaction"},
    {NULL, NULL}
};

/* Return the keyword table appropriate for the innermost recognized block type.
 * Walks the stack from innermost outward, skipping structural-but-transparent
 * blocks (limits, supplement, etc.) until a recognized type is found.
 * Returns TOPLEVEL_KWS if no recognized block is found.
 *
 * stack — block-opener keyword name array from block_stack()
 * n     — number of entries in stack
 */
static const KwEntry *kws_for_stack(char **stack, int n) {
    for (int i = n - 1; i >= 0; i--) {
        const char *kw = stack[i];
        if (strcmp(kw, "task")     == 0) return TASK_KWS;
        if (strcmp(kw, "resource") == 0) return RESOURCE_KWS;
        if (strcmp(kw, "project")  == 0) return PROJECT_KWS;
        if (strcmp(kw, "account")  == 0) return ACCOUNT_KWS;
        /* Terminal / skip-upward blocks */
        if (strcmp(kw, "limits")        == 0 || strcmp(kw, "journalentry") == 0
         || strcmp(kw, "supplement")    == 0 || strcmp(kw, "extend")       == 0
         || strcmp(kw, "scenario")      == 0 || strcmp(kw, "navigator")    == 0
         || strcmp(kw, "taskreport")    == 0 || strcmp(kw, "textreport")   == 0
         || strcmp(kw, "resourcereport") == 0 || strcmp(kw, "shift")       == 0)
            continue;
        break;
    }
    return TOPLEVEL_KWS;
}

/* ── Line helpers ────────────────────────────────────────────────────────── */

/* Returns heap-allocated text of the first non-comment token on cursor's line,
 * or NULL if no such ident exists. */
static char *line_first_word(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *t = &tokens[i];
        if (t->token_kind == TK_EOF) break;
        if (t->token_kind == TK_LINE_COMMENT || t->token_kind == TK_BLOCK_COMMENT) continue;
        if (t->start.line < cursor.line) continue;
        if (t->start.line > cursor.line) break;
        /* First non-comment token on cursor.line */
        if (t->token_kind == TK_IDENT && t->text) return strdup(t->text);
        return NULL;
    }
    return NULL;
}

/* Returns 1 if there are no non-whitespace tokens before cursor on its line. */
static int at_statement_start(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *t = &tokens[i];
        if (t->token_kind == TK_EOF) break;
        if (t->token_kind == TK_LINE_COMMENT || t->token_kind == TK_BLOCK_COMMENT) continue;
        if (t->start.line > cursor.line) break;
        if (t->start.line == cursor.line && t->start.character < cursor.character)
            return 0;
    }
    return 1;
}

/* ── partial_word ────────────────────────────────────────────────────────── */

/* Return the identifier text at cursor if cursor is on a TK_IDENT token,
 * otherwise return an empty heap-allocated string.  Caller must free.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — cursor position
 */
static char *partial_word(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    TokenSpan t = tok_span_at(tokens, num_tokens, cursor);
    if (t.token_kind == TK_IDENT) {
        char *txt = t.text; /* take ownership */
        t.text = NULL;
        return txt;
    }
    free(t.text);
    return strdup("");
}

/* ── ID collection ───────────────────────────────────────────────────────── */

typedef struct { char *id; char *name; } IdEntry;

typedef struct { IdEntry *items; int n, cap; } IdList;

/* Append a heap-allocated copy of (id, name) to il, growing it if needed. */
static void idlist_push(IdList *il, const char *id, const char *name) {
    if (il->n >= il->cap) {
        il->cap = il->cap ? il->cap * 2 : 16;
        il->items = realloc(il->items, il->cap * sizeof(IdEntry));
    }
    il->items[il->n++] = (IdEntry){ strdup(id), strdup(name) };
}

/* Free all entries in il and the backing array. */
static void idlist_free(IdList *il) {
    for (int i = 0; i < il->n; i++) {
        free(il->items[i].id);
        free(il->items[i].name);
    }
    free(il->items);
}

/* Recursively collect all symbol IDs of the given kind from syms[], building
 * dot-separated fully-qualified paths relative to prefix.  Symbols of other
 * kinds are recursed into but not emitted (so task children of a project
 * container are still collected).
 *
 * syms   — symbol array to search
 * n      — number of entries in syms
 * kind   — SK_FUNCTION, SK_OBJECT, etc. — only symbols of this kind are added
 * prefix — dot-path prefix to prepend; "" for the root level
 * out    — IdList to append results to
 */
static void collect_ids(const DocSymbol *syms, int n, int kind,
                         const char *prefix, IdList *out) {
    for (int i = 0; i < n; i++) {
        if (syms[i].kind == kind && syms[i].detail && syms[i].detail[0]) {
            char qid[1024];
            if (!prefix || !prefix[0])
                snprintf(qid, sizeof(qid), "%s", syms[i].detail);
            else
                snprintf(qid, sizeof(qid), "%s.%s", prefix, syms[i].detail);
            idlist_push(out, qid, syms[i].name ? syms[i].name : "");
            collect_ids(syms[i].children, syms[i].num_children, kind, qid, out);
        } else {
            collect_ids(syms[i].children, syms[i].num_children, kind, prefix, out);
        }
    }
}

/* ── Scope-aware helpers ─────────────────────────────────────────────────── */

typedef struct { char **ids; uint32_t *depths; int n, cap; } ScopeStack;

/* Append (id, depth) to ss, growing it if needed. */
static void ss_push(ScopeStack *ss, const char *id, uint32_t depth) {
    if (ss->n >= ss->cap) {
        ss->cap = ss->cap ? ss->cap * 2 : 8;
        ss->ids    = realloc(ss->ids,    ss->cap * sizeof(char *));
        ss->depths = realloc(ss->depths, ss->cap * sizeof(uint32_t));
    }
    ss->ids[ss->n]    = strdup(id);
    ss->depths[ss->n] = depth;
    ss->n++;
}

/* Walk tokens up to cursor and return the ordered list of task IDs enclosing
 * the cursor position (outermost first).  Used to determine the bang-relative
 * dep-ref scope for dependency completions.
 * Returns a heap-allocated array of heap-allocated strings; caller must free
 * each string and the array.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — position used as the scan stop point
 * out_n      — set to the number of task IDs returned
 */
static char **current_task_scope(const TokenSpan *tokens, int num_tokens,
                                  LspPos cursor, int *out_n) {
    typedef enum { SS_SCAN, SS_EXPECT_ID, SS_BEFORE_LBRACE } SState;

    SState sstate     = SS_SCAN;
    char  *pending_id = NULL;
    uint32_t brace_depth = 0;
    ScopeStack ts = {0};

    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *tok = &tokens[i];
        if (tok->token_kind == TK_EOF || pos_cmp(cursor, tok->start) < 0) break;

        switch (tok->token_kind) {
        case TK_LINE_COMMENT:
        case TK_BLOCK_COMMENT:
            break;

        case TK_LBRACE:
            brace_depth++;
            if (sstate == SS_BEFORE_LBRACE && pending_id) {
                ss_push(&ts, pending_id, brace_depth);
                free(pending_id);
                pending_id = NULL;
            }
            sstate = SS_SCAN;
            break;

        case TK_RBRACE:
            while (ts.n > 0 && ts.depths[ts.n - 1] >= brace_depth)
                free(ts.ids[--ts.n]);
            if (brace_depth > 0) brace_depth--;
            sstate = SS_SCAN;
            free(pending_id);
            pending_id = NULL;
            break;

        case KW_TASK:
            sstate = SS_EXPECT_ID;
            free(pending_id);
            pending_id = NULL;
            break;

        case TK_IDENT:
            if (sstate == SS_EXPECT_ID && tok->text) {
                free(pending_id);
                pending_id = strdup(tok->text);
                sstate     = SS_BEFORE_LBRACE;
            }
            break;

        default:
            if (sstate == SS_EXPECT_ID) {
                sstate = SS_SCAN;
                free(pending_id);
                pending_id = NULL;
            }
            break;
        }
    }

    free(pending_id);

    /* Extract just the ids */
    char **result = NULL;
    if (ts.n > 0) {
        result = malloc(ts.n * sizeof(char *));
        for (int i = 0; i < ts.n; i++) result[i] = ts.ids[i];
        free(ts.ids);
        ts.ids = NULL;
    }
    *out_n = ts.n;
    free(ts.depths);
    return result;
}

/* Count the number of consecutive `!` tokens immediately before the cursor,
 * skipping any trailing partial identifier and any comments.
 * Used to determine the bang depth for dep-ref scope navigation.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — position used as the scan stop point
 */
static int count_leading_bangs(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    /* Find index past the last non-comment token before (or at) cursor */
    int last = -1;
    for (int i = 0; i < num_tokens; i++) {
        if (tokens[i].token_kind == TK_EOF) break;
        if (pos_cmp(cursor, tokens[i].start) < 0) break;
        if (tokens[i].token_kind != TK_LINE_COMMENT && tokens[i].token_kind != TK_BLOCK_COMMENT)
            last = i;
    }

    /* Skip trailing ident (partial word being typed) */
    if (last >= 0 && tokens[last].token_kind == TK_IDENT) last--;

    /* Count consecutive bangs scanning backwards, skipping comments */
    int count = 0;
    for (int i = last; i >= 0; i--) {
        if (tokens[i].token_kind == TK_LINE_COMMENT || tokens[i].token_kind == TK_BLOCK_COMMENT)
            continue;
        if (tokens[i].token_kind != TK_BANG) break;
        count++;
    }
    return count;
}

/* ── Helper: check if needle is a case-insensitive substring of haystack ── */

/* Returns 1 if needle is a case-insensitive substring of haystack. */
static int icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i <= hn - nn; i++) {
        int match = 1;
        for (size_t j = 0; j < nn; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* Returns 1 if s begins with prefix (case-insensitive). */
static int istarts(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    size_t pn = strlen(prefix);
    for (size_t i = 0; i < pn; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

/* Map a SymbolKind constant to the corresponding CompletionItemKind value. */
static int completion_kind_for(int sym_kind) {
    if (sym_kind == SK_FUNCTION) return CIK_FUNCTION;
    if (sym_kind == SK_OBJECT)   return CIK_CLASS;
    if (sym_kind == SK_VARIABLE) return CIK_VARIABLE;
    return CIK_REFERENCE;
}

/* ── build_completions_json ──────────────────────────────────────────────── */

/* Build the CompletionList JSON response for textDocument/completion.
 * Returns null if there are no applicable completions at cursor.
 * Completions are returned in two mutually exclusive modes:
 *   — ID completions when inside a depends/precedes/allocate/etc. argument
 *   — Keyword completions when at a statement-start position
 *
 * doc         — the mutable JSON document that will own the returned value
 * tokens      — token span array from the ParseResult
 * num_tokens  — number of entries in tokens
 * cursor      — cursor position from the textDocument/completion request
 * symbols     — root-level symbol array for ID completions
 * num_symbols — number of entries in symbols
 */
yyjson_mut_val *build_completions_json(yyjson_mut_doc *doc,
                                        const TokenSpan *tokens, int num_tokens,
                                        LspPos cursor,
                                        const DocSymbol *symbols, int num_symbols) {
    int    stack_n = 0;
    char **stack   = block_stack(tokens, num_tokens, cursor, &stack_n);
    char  *partial = partial_word(tokens, num_tokens, cursor);
    char  *fw      = line_first_word(tokens, num_tokens, cursor);

    yyjson_mut_val *items = yyjson_mut_arr(doc);
    int item_count = 0;

    /* Suppress completions while typing a declaration keyword's id/name */
    if (fw && (strcmp(fw, "project")    == 0 || strcmp(fw, "task")       == 0
            || strcmp(fw, "resource")   == 0 || strcmp(fw, "account")    == 0
            || strcmp(fw, "shift")      == 0 || strcmp(fw, "scenario")   == 0
            || strcmp(fw, "macro")      == 0 || strcmp(fw, "include")    == 0
            || strcmp(fw, "supplement") == 0)) {
        goto done;
    }

    /* ID completions based on active statement keyword */
    {
        ActiveContext ac = active_context(tokens, num_tokens, cursor);
        int id_kind = 0;
        if (ac.keyword) {
            if (strcmp(ac.keyword, "depends")  == 0 || strcmp(ac.keyword, "precedes") == 0)
                id_kind = SK_FUNCTION;
            else if (strcmp(ac.keyword, "allocate")   == 0
                  || strcmp(ac.keyword, "responsible") == 0
                  || strcmp(ac.keyword, "managers")    == 0)
                id_kind = SK_OBJECT;
            else if (strcmp(ac.keyword, "chargeset") == 0
                  || strcmp(ac.keyword, "balance")   == 0)
                id_kind = SK_VARIABLE;
        }

        if (id_kind) {
            IdList ids = {0};
            char   bang_prefix[64] = "";

            if (id_kind == SK_FUNCTION
                    && ac.keyword
                    && (strcmp(ac.keyword, "depends") == 0
                     || strcmp(ac.keyword, "precedes") == 0)) {
                int scope_n = 0;
                char **scope = current_task_scope(tokens, num_tokens, cursor, &scope_n);
                int bang_count = count_leading_bangs(tokens, num_tokens, cursor);

                for (int i = 0; i < bang_count; i++)
                    strncat(bang_prefix, "!", sizeof(bang_prefix) - strlen(bang_prefix) - 1);

                if (scope_n == 0) {
                    collect_ids(symbols, num_symbols, id_kind, "", &ids);
                } else if (bang_count == 0) {
                    const DocSymbol *ch;
                    int           ch_n;
                    ch = doc_symbol_find_path(symbols, num_symbols,
                                             (const char **)scope, scope_n, &ch_n);
                    if (ch) collect_ids(ch, ch_n, id_kind, "", &ids);
                } else if (bang_count <= scope_n) {
                    const DocSymbol *ch;
                    int           ch_n;
                    ch = doc_symbol_find_path(symbols, num_symbols,
                                             (const char **)scope, scope_n - bang_count,
                                             &ch_n);
                    if (ch) collect_ids(ch, ch_n, id_kind, "", &ids);
                }
                /* bang_count > scope_n: empty (no valid completions) */

                for (int i = 0; i < scope_n; i++) free(scope[i]);
                free(scope);
            } else {
                collect_ids(symbols, num_symbols, id_kind, "", &ids);
            }

            for (int i = 0; i < ids.n; i++) {
                const char *id   = ids.items[i].id;
                const char *name = ids.items[i].name;

                int id_match   = (!partial[0]) || istarts(id, partial);
                int name_match = (partial[0]) && icontains(name, partial);

                if (!id_match && !name_match) continue;

                yyjson_mut_val *item = yyjson_mut_obj(doc);
                /* Use strcpy variants: id/name come from IdList which is freed
                 * before make_response, and ft is a local stack buffer. */
                yyjson_mut_obj_add_strcpy(doc, item, "label",    id);
                yyjson_mut_obj_add_uint(doc,   item, "kind",     (uint64_t)completion_kind_for(id_kind));
                yyjson_mut_obj_add_strcpy(doc, item, "detail",   name);
                yyjson_mut_obj_add_str(doc,    item, "sortText", "0");

                if (name_match && !id_match) {
                    yyjson_mut_obj_add_strcpy(doc, item, "filterText", partial);
                } else if (bang_prefix[0] && !partial[0]) {
                    char ft[1024];
                    snprintf(ft, sizeof(ft), "%s%s", bang_prefix, id);
                    yyjson_mut_obj_add_strcpy(doc, item, "filterText", ft);
                }

                yyjson_mut_arr_add_val(items, item);
                item_count++;
            }

            idlist_free(&ids);
            free(ac.keyword);

            /* Don't mix id completions with keyword completions */
            goto done;
        }
        free(ac.keyword);
    }

    /* Keyword completions: offer context-sensitive keywords when typing at
     * the start of a statement or when the partial word prefix has no ID match */
    {
        int typing = (partial[0] != '\0') || at_statement_start(tokens, num_tokens, cursor);
        if (typing) {
            const KwEntry *table = kws_for_stack(stack, stack_n);
            for (int i = 0; table[i].kw; i++) {
                if (!partial[0] || istarts(table[i].kw, partial)) {
                    yyjson_mut_val *item = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_str(doc,  item, "label",    table[i].kw);
                    yyjson_mut_obj_add_uint(doc, item, "kind",     CIK_KEYWORD);
                    yyjson_mut_obj_add_str(doc,  item, "detail",   table[i].doc);
                    yyjson_mut_obj_add_str(doc,  item, "sortText", "1");
                    yyjson_mut_arr_add_val(items, item);
                    item_count++;
                }
            }
        }
    }

done:
    free_block_stack(stack, stack_n);
    free(partial);
    free(fw);

    if (item_count == 0)
        return yyjson_mut_null(doc);

    yyjson_mut_val *list = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, list, "isIncomplete", true);
    yyjson_mut_obj_add_val(doc, list, "items", items);
    return list;
}
