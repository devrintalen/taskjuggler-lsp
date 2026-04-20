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

/* ── String utilities ────────────────────────────────────────────────────── */

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

/* ── Cursor suppression ──────────────────────────────────────────────────── */

/* Find the start of the line containing cursor in text.  Returns a pointer
 * into text at the first character of that line.
 */
static const char *find_line_start(const char *text, uint32_t target_line) {
    const char *p = text;
    uint32_t line = 0;
    while (*p && line < target_line) {
        if (*p == '\n') line++;
        p++;
    }
    return p;
}

/* Check whether cursor is inside a double-quoted string.
 *
 * First checks the token spans: if tok_span_at() returns TK_STR the
 * cursor is inside a terminated string and we return immediately.
 *
 * Otherwise, falls back to scanning the cursor's line in the raw text.
 * Double-quoted strings in TaskJuggler cannot span newlines, so we only
 * need to scan from the start of the cursor's line up to the cursor
 * column.  Handles escaped quotes and unterminated strings.
 */
static int cursor_in_dquote(const char *text, LspPos cursor,
                            const TokenSpan *tokens, int num_tokens) {
    /* Fast path: token spans already cover terminated strings. */
    TokenSpan ts = tok_span_at(tokens, num_tokens, cursor);
    int kind = ts.token_kind;
    free(ts.text);
    if (kind == TK_STR)
        return 1;

    /* Slow path for unterminated strings: scan just the cursor's line. */
    const char *line_start = find_line_start(text, cursor.line);
    int in_string = 0;
    uint32_t col = 0;

    for (const char *p = line_start; *p && *p != '\n' && col < cursor.character; p++) {
        if (in_string) {
            if (*p == '\\' && p[1] && p[1] != '\n') {
                p++;    /* skip escaped character */
                col++;
            } else if (*p == '"') {
                in_string = 0;
            }
        } else if (*p == '"') {
            in_string = 1;
        }
        col++;
    }
    return in_string;
}

/* Check whether cursor is inside a scissors block (-8<- ... ->8-).
 *
 * First checks the token spans: if tok_span_at() returns
 * TK_MULTI_LINE_STR the cursor is inside a terminated scissors block.
 * If it returns any other real token, the cursor cannot be inside an
 * unterminated scissors block either (the lexer's SCISSORS state
 * consumes everything from -8<- to EOF, so no tokens would exist at the
 * cursor position if it were inside one).
 *
 * Only when tok_span_at() returns TK_EOF (cursor is in a tokenless
 * region) do we fall back to a backward scan through the raw text.
 * Because the unterminated scissors block has already consumed every
 * character from its opener to EOF, the -8<- delimiter is necessarily
 * between the last real token and the cursor — a bounded distance.
 */
static int cursor_in_scissors(const char *text, LspPos cursor,
                              const TokenSpan *tokens, int num_tokens) {
    /* Fast path: check token spans. */
    TokenSpan ts = tok_span_at(tokens, num_tokens, cursor);
    int kind = ts.token_kind;
    free(ts.text);
    if (kind == TK_MULTI_LINE_STR)
        return 1;
    if (kind != TK_EOF)
        return 0;   /* cursor is on a real token — not inside scissors */

    /* Slow path: cursor is in a tokenless gap.  Scan backwards through
     * the raw text for the nearest scissors delimiter. */
    const char *line_start = find_line_start(text, cursor.line);
    const char *cursor_ptr = line_start;
    uint32_t col = 0;
    while (*cursor_ptr && *cursor_ptr != '\n' && col < cursor.character) {
        cursor_ptr++;
        col++;
    }

    for (const char *p = cursor_ptr - 1; p >= text + 3; p--) {
        if (p[-3] == '-' && p[-2] == '>' && p[-1] == '8' && p[0] == '-')
            return 0;   /* found a close delimiter first — not inside */
        if (p[-3] == '-' && p[-2] == '8' && p[-1] == '<' && p[0] == '-')
            return 1;   /* found an open delimiter first — inside */
    }
    return 0;
}

/* ── Token context ───────────────────────────────────────────────────────── */

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

/* Return the identifier text at cursor if cursor is on a TK_IDENT token,
 * otherwise return an empty heap-allocated string.  Caller must free.
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

/* ── Block stack ─────────────────────────────────────────────────────────── */

/* Build the stack of block-opener keyword constants enclosing cursor
 * (outermost first).  Uses symbol_at() to locate the innermost enclosing
 * DocSymbol, then walks up the parent chain, recording keywords in reverse
 * order into a scratch array and emitting them outermost-first.
 * Returns a heap-allocated array of KW_* values; caller must free.
 */
static int *block_stack(const TokenSpan *tokens, int num_tokens,
                        LspPos cursor, int *out_n) {
    *out_n = 0;

    /* Count the chain depth up from the innermost. */
    int depth = 0;
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent)
        depth++;
    if (depth == 0) return NULL;

    int *result = malloc((size_t)depth * sizeof(int));
    if (!result) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    /* Fill in reverse so the result reads outermost-first. */
    int i = depth;
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent)
        result[--i] = sym->keyword;

    *out_n = depth;
    return result;
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
 */
static const KwEntry *kws_for_stack(const int *stack, int n) {
    for (int i = n - 1; i >= 0; i--) {
        int kw = stack[i];
        if (kw == KW_TASK)     return TASK_KWS;
        if (kw == KW_RESOURCE) return RESOURCE_KWS;
        if (kw == KW_PROJECT)  return PROJECT_KWS;
        if (kw == KW_ACCOUNT)  return ACCOUNT_KWS;
        /* Transparent blocks: look through to the parent */
        if (kw == KW_LIMITS    || kw == KW_JOURNALENTRY
         || kw == KW_SUPPLEMENT|| kw == KW_EXTEND
         || kw == KW_SCENARIO  || kw == KW_NAVIGATOR
         || kw == KW_TASKREPORT|| kw == KW_TEXTREPORT
         || kw == KW_RESOURCEREPORT || kw == KW_SHIFT)
            continue;
        break;
    }
    return TOPLEVEL_KWS;
}

/* ── ID collection ───────────────────────────────────────────────────────── */

typedef struct { char *id; char *name; } IdEntry;

typedef struct { IdEntry *items; int n, cap; } IdList;

/* Append a heap-allocated copy of (id, name) to il, growing it if needed. */
static void idlist_push(IdList *il, const char *id, const char *name) {
    if (il->n >= il->cap) {
        il->cap = il->cap ? il->cap * 2 : 16;
        IdEntry *tmp = realloc(il->items, il->cap * sizeof(IdEntry));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        il->items = tmp;
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
 */
static void collect_ids(DocSymbol *const *syms, int n, int kind,
                         const char *prefix, IdList *out) {
    for (int i = 0; i < n; i++) {
        if (syms[i]->keyword == kind && syms[i]->id && syms[i]->id[0]) {
            size_t plen = prefix ? strlen(prefix) : 0;
            size_t dlen = strlen(syms[i]->id);
            size_t qlen = plen ? plen + 1 + dlen : dlen;
            char *qid = malloc(qlen + 1);
            if (!qid) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
            if (!prefix || !prefix[0]) {
                memcpy(qid, syms[i]->id, dlen + 1);
            } else {
                memcpy(qid, prefix, plen);
                qid[plen] = '.';
                memcpy(qid + plen + 1, syms[i]->id, dlen + 1);
            }
            idlist_push(out, qid, syms[i]->name ? syms[i]->name : "");
            collect_ids(syms[i]->children, syms[i]->num_children, kind, qid, out);
            free(qid);
        } else {
            collect_ids(syms[i]->children, syms[i]->num_children, kind, prefix, out);
        }
    }
}

/* ── Scope-aware helpers ─────────────────────────────────────────────────── */

/* Return the ordered list of task IDs enclosing cursor (outermost first).
 * Used to determine the bang-relative dep-ref scope for dependency completions.
 * Uses symbol_at() + parent walk to locate the enclosing chain, collects
 * KW_TASK ids in reverse order, then fills the output outermost-first.
 * Returns a heap-allocated array of heap-allocated strings; caller must free
 * each string and the array.
 */
static char **current_task_scope(const TokenSpan *tokens, int num_tokens,
                                 LspPos cursor, int *out_n) {
    *out_n = 0;

    int depth = 0;
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent) {
        if (sym->keyword == KW_TASK && sym->id && sym->id[0])
            depth++;
    }
    if (depth == 0) return NULL;

    char **result = malloc((size_t)depth * sizeof(char *));
    if (!result) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    int i = depth;
    for (DocSymbol *sym = symbol_at(tokens, num_tokens, cursor);
         sym != NULL; sym = sym->parent) {
        if (sym->keyword == KW_TASK && sym->id && sym->id[0])
            result[--i] = strdup(sym->id);
    }

    *out_n = depth;
    return result;
}

/* Count the number of consecutive `!` tokens immediately before the cursor,
 * skipping any trailing partial identifier and any comments.
 * Used to determine the bang depth for dep-ref scope navigation.
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

/* ── Completion builders ─────────────────────────────────────────────────── */

/* Map a KW_* keyword constant to the corresponding CompletionItemKind value. */
static int completion_kind_for(int keyword) {
    switch (keyword) {
    case KW_TASK:     return CIK_FUNCTION;
    case KW_RESOURCE: return CIK_CLASS;
    case KW_ACCOUNT:  return CIK_VARIABLE;
    default:          return CIK_REFERENCE;
    }
}

/* Return 1 if fw is a declaration keyword whose id/name the user is typing
 * (i.e. completions should be suppressed). */
static int is_decl_keyword(const char *fw) {
    return fw && (strcmp(fw, "project")    == 0 || strcmp(fw, "task")       == 0
              || strcmp(fw, "resource")   == 0 || strcmp(fw, "account")    == 0
              || strcmp(fw, "shift")      == 0 || strcmp(fw, "scenario")   == 0
              || strcmp(fw, "macro")      == 0 || strcmp(fw, "include")    == 0
              || strcmp(fw, "supplement") == 0);
}

/* Map an active-context keyword string to the KW_* symbol kind that its
 * arguments reference, or 0 if the keyword does not take ID arguments. */
static int id_kind_for_keyword(const char *keyword) {
    if (!keyword) return 0;
    if (strcmp(keyword, "depends")  == 0 || strcmp(keyword, "precedes") == 0)
        return KW_TASK;
    if (strcmp(keyword, "allocate")   == 0
     || strcmp(keyword, "responsible") == 0
     || strcmp(keyword, "managers")    == 0)
        return KW_RESOURCE;
    if (strcmp(keyword, "chargeset") == 0
     || strcmp(keyword, "balance")   == 0)
        return KW_ACCOUNT;
    return 0;
}

/* Collect IDs from all symbol pools (primary + extras). */
static void collect_all_ids(DocSymbol *const *symbols, int num_symbols,
                            DocSymbol *const **extra_pools,
                            const int *extra_counts, int num_extra,
                            int id_kind, IdList *ids) {
    collect_ids(symbols, num_symbols, id_kind, "", ids);
    for (int e = 0; e < num_extra; e++)
        collect_ids(extra_pools[e], extra_counts[e], id_kind, "", ids);
}

/* Collect dep IDs for depends/precedes, applying scope and bang navigation.
 * Writes the bang prefix string into bang_prefix (must be at least 64 bytes). */
static void collect_dep_ids(const TokenSpan *tokens, int num_tokens,
                            LspPos cursor,
                            DocSymbol *const *symbols, int num_symbols,
                            DocSymbol *const **extra_pools,
                            const int *extra_counts, int num_extra,
                            int id_kind, IdList *ids, char *bang_prefix) {
    int scope_n = 0;
    char **scope = current_task_scope(tokens, num_tokens, cursor, &scope_n);
    int bang_count = count_leading_bangs(tokens, num_tokens, cursor);

    for (int i = 0; i < bang_count; i++)
        strncat(bang_prefix, "!", 64 - strlen(bang_prefix) - 1);

    if (bang_count == 0) {
        /* No bangs — absolute lookup from the file root.  Suggestions are
         * every task in the current file (fully-qualified paths) plus the
         * top-level tasks of every other open file. */
        collect_all_ids(symbols, num_symbols, extra_pools, extra_counts,
                        num_extra, id_kind, ids);
    } else if (bang_count <= scope_n) {
        /* Bang navigation: relative to current scope, file-local only */
        int ch_n;
        DocSymbol *const *ch = doc_symbol_find_path(
            symbols, num_symbols,
            (const char **)scope, scope_n - bang_count, &ch_n);
        if (ch) collect_ids(ch, ch_n, id_kind, "", ids);
    }
    /* bang_count > scope_n: no valid completions */

    for (int i = 0; i < scope_n; i++) free(scope[i]);
    free(scope);
}

/* Build ID completion items for a depends/allocate/chargeset/etc. argument.
 * Returns the number of items added to the items array. */
static int build_id_completions(yyjson_mut_doc *doc, yyjson_mut_val *items,
                                const TokenSpan *tokens, int num_tokens,
                                LspPos cursor, const char *partial,
                                DocSymbol *const *symbols, int num_symbols,
                                DocSymbol *const **extra_pools,
                                const int *extra_counts, int num_extra,
                                int id_kind, const char *keyword) {
    IdList ids = {0};
    char   bang_prefix[64] = "";

    int is_dep = (strcmp(keyword, "depends") == 0
               || strcmp(keyword, "precedes") == 0);

    if (id_kind == KW_TASK && is_dep) {
        collect_dep_ids(tokens, num_tokens, cursor,
                        symbols, num_symbols,
                        extra_pools, extra_counts, num_extra,
                        id_kind, &ids, bang_prefix);
    } else {
        collect_all_ids(symbols, num_symbols, extra_pools, extra_counts,
                        num_extra, id_kind, &ids);
    }

    int count = 0;
    for (int i = 0; i < ids.n; i++) {
        const char *id   = ids.items[i].id;
        const char *name = ids.items[i].name;

        int id_match   = (!partial[0]) || istarts(id, partial);
        int name_match = (partial[0]) && icontains(name, partial);
        if (!id_match && !name_match) continue;

        yyjson_mut_val *item = yyjson_mut_obj(doc);
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
        count++;
    }

    idlist_free(&ids);
    return count;
}

/* Build keyword completion items for the current block context.
 * Returns the number of items added to the items array. */
static int build_keyword_completions(yyjson_mut_doc *doc, yyjson_mut_val *items,
                                     const TokenSpan *tokens, int num_tokens,
                                     LspPos cursor, const char *partial,
                                     const int *stack, int stack_n) {
    int typing = (partial[0] != '\0')
              || at_statement_start(tokens, num_tokens, cursor);
    if (!typing) return 0;

    int count = 0;
    const KwEntry *table = kws_for_stack(stack, stack_n);
    for (int i = 0; table[i].kw; i++) {
        if (!partial[0] || istarts(table[i].kw, partial)) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc,  item, "label",    table[i].kw);
            yyjson_mut_obj_add_uint(doc, item, "kind",     CIK_KEYWORD);
            yyjson_mut_obj_add_str(doc,  item, "detail",   table[i].doc);
            yyjson_mut_obj_add_str(doc,  item, "sortText", "1");
            yyjson_mut_arr_add_val(items, item);
            count++;
        }
    }
    return count;
}

/* Build the CompletionList JSON response for textDocument/completion.
 * Returns null if there are no applicable completions at cursor.
 * Completions are returned in two mutually exclusive modes:
 *   — ID completions when inside a depends/precedes/allocate/etc. argument
 *   — Keyword completions when at a statement-start position
 */
yyjson_mut_val *build_completions_json(yyjson_mut_doc *doc,
                                        const TokenSpan *tokens, int num_tokens,
                                        LspPos cursor,
                                        DocSymbol *const *symbols, int num_symbols,
                                        DocSymbol *const **extra_pools,
                                        const int *extra_counts,
                                        int num_extra,
                                        const char *text) {
    /* Suppress completions inside strings and scissors blocks */
    if (cursor_in_dquote(text, cursor, tokens, num_tokens)
        || cursor_in_scissors(text, cursor, tokens, num_tokens))
        return yyjson_mut_null(doc);

    /* Gather cursor context: enclosing block stack, partial word, first word */
    int    stack_n    = 0;
    int   *stack      = block_stack(tokens, num_tokens, cursor, &stack_n);
    char  *partial    = partial_word(tokens, num_tokens, cursor);
    char  *first_word = line_first_word(tokens, num_tokens, cursor);

    yyjson_mut_val *items = yyjson_mut_arr(doc);
    int item_count = 0;

    /* Suppress completions while typing a declaration keyword's id/name */
    if (is_decl_keyword(first_word))
        goto done;

    /* Try ID completions (depends, allocate, chargeset, etc.) */
    {
        ActiveContext ac = active_context(tokens, num_tokens, cursor);
        int id_kind = id_kind_for_keyword(ac.keyword);

        if (id_kind) {
            item_count = build_id_completions(doc, items,
                                              tokens, num_tokens,
                                              cursor, partial,
                                              symbols, num_symbols,
                                              extra_pools, extra_counts, num_extra,
                                              id_kind, ac.keyword);
            free(ac.keyword);
            goto done;  /* Don't mix ID completions with keyword completions */
        }
        free(ac.keyword);
    }

    /* Fall back to keyword completions for the current block context */
    item_count = build_keyword_completions(doc, items,
                                           tokens, num_tokens,
                                           cursor, partial,
                                           stack, stack_n);

done:
    /* Clean up and wrap items in a CompletionList */
    free(stack);
    free(partial);
    free(first_word);

    if (item_count == 0)
        return yyjson_mut_null(doc);

    yyjson_mut_val *list = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, list, "isIncomplete", true);
    yyjson_mut_obj_add_val(doc, list, "items", items);
    return list;
}
