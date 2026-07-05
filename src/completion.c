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

#include "completion.h"
#include "document_symbol.h"
#include "hover.h"
#include "signature.h"
#include "rpc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── CompletionItemKind values ───────────────────────────────────────────── */

/** LSP CompletionItemKind for tasks (rendered as a function). */
#define CIK_FUNCTION  3
/** LSP CompletionItemKind for accounts (rendered as a variable). */
#define CIK_VARIABLE  6
/** LSP CompletionItemKind for resources (rendered as a class). */
#define CIK_CLASS     7
/** LSP CompletionItemKind for TJP keywords. */
#define CIK_KEYWORD   14
/** LSP CompletionItemKind for any other identifier reference. */
#define CIK_REFERENCE 18

/* ── String utilities ────────────────────────────────────────────────────── */

/**
 * Test whether @p s begins with @p prefix, comparing case-insensitively.
 *
 * @param s       Heap or stack string to test.
 * @param prefix  Required prefix; may not be NULL.
 * @return 1 on match, 0 otherwise.
 */
static int istarts(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < prefix_len; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

/* ── Cursor suppression ──────────────────────────────────────────────────── */

/**
 * Find the start of the line containing @p target_line in @p text.
 *
 * @param text         NUL-terminated source text.
 * @param target_line  Zero-indexed line whose start position is wanted.
 * @return Pointer into @p text at the first character of @p target_line,
 *         or the trailing NUL when the file is shorter.
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

/**
 * Test whether @p cursor sits inside a double-quoted string.
 *
 * First checks the token spans: if tok_span_at() returns TK_STR the
 * cursor is inside a terminated string and we return immediately.
 *
 * Otherwise, falls back to scanning the cursor's line in the raw text.
 * Double-quoted strings in TaskJuggler cannot span newlines, so we only
 * need to scan from the start of the cursor's line up to the cursor
 * column.  Handles escaped quotes and unterminated strings.
 *
 * @param text        Raw source text.
 * @param cursor      Cursor position.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @return 1 when @p cursor is inside a string, 0 otherwise.
 */
static int cursor_in_dquote(const char *text, LspPos cursor,
                            const TokenSpan *tokens, int num_tokens) {
    /* Fast path: token spans already cover terminated strings. */
    const TokenSpan *ts = tok_span_at(tokens, num_tokens, cursor);
    if (ts && ts->token_kind == TK_STR)
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

/**
 * Test whether @p cursor sits inside a scissors block
 * (`-8<-` ... `->8-`).
 *
 * First checks the token spans: if tok_span_at() returns
 * TK_MULTI_LINE_STR the cursor is inside a terminated scissors block.
 * If it returns any other real token, the cursor cannot be inside an
 * unterminated scissors block either (the lexer's SCISSORS state
 * consumes everything from `-8<-` to EOF, so no tokens would exist at
 * the cursor position if it were inside one).
 *
 * Only when tok_span_at() returns NULL (cursor is in a tokenless
 * region) do we fall back to a backward scan through the raw text.
 * Because the unterminated scissors block has already consumed every
 * character from its opener to EOF, the `-8<-` delimiter is necessarily
 * between the last real token and the cursor — a bounded distance.
 *
 * @param text        Raw source text.
 * @param cursor      Cursor position.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @return 1 when @p cursor is inside a scissors block, 0 otherwise.
 */
static int cursor_in_scissors(const char *text, LspPos cursor,
                              const TokenSpan *tokens, int num_tokens) {
    /* Fast path: check token spans. */
    const TokenSpan *ts = tok_span_at(tokens, num_tokens, cursor);
    if (ts && ts->token_kind == TK_MULTI_LINE_STR)
        return 1;
    if (ts)
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

/**
 * Return the first non-comment identifier token on @p cursor's line.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position; only its line component is used.
 * @return Heap-allocated copy of the identifier text, or NULL when the line
 *         starts with something other than an identifier.
 */
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

/**
 * Test whether @p cursor sits before any non-comment tokens on its line.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return 1 when @p cursor is at the start of a fresh statement, 0 otherwise.
 */
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

/**
 * Return the identifier text @p cursor is sitting on.
 *
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @return Heap-allocated identifier text when @p cursor is on a TK_IDENT
 *         token, or an empty heap-allocated string otherwise.  Caller must
 *         free.
 */
static char *partial_word(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    const TokenSpan *span = tok_span_at(tokens, num_tokens, cursor);
    if (span && span->token_kind == TK_IDENT && span->text)
        return strdup(span->text);
    return strdup("");
}

/* ── Block stack ─────────────────────────────────────────────────────────── */

/**
 * Build the stack of block-opener keyword constants enclosing @p cursor
 * (outermost first).  Uses tj_node_at() to locate the innermost enclosing
 * tj_node, then walks up the parent chain, recording keywords in reverse
 * order into a scratch array and emitting them outermost-first.
 *
 * @param tokens      Token spans of the current document.
 * @param owners      Per-token owner array, parallel to @p tokens.
 * @param num_tokens  Length of @p tokens / @p owners.
 * @param cursor      Cursor position.
 * @param out_n       Receives the stack depth.
 * @return Heap-allocated array of KW_* values; caller must free.  NULL when
 *         @p cursor sits outside every tj_node.
 */
static int *block_stack(const TokenSpan *tokens, tj_node *const *owners,
                        int num_tokens, LspPos cursor, int *out_n) {
    *out_n = 0;

    /* Count the chain depth up from the innermost. */
    int depth = 0;
    for (tj_node *sym = tj_node_at(tokens, owners, num_tokens, cursor);
         sym != NULL; sym = sym->parent_node)
        depth++;
    if (depth == 0) return NULL;

    int *result = malloc((size_t)depth * sizeof(int));
    if (!result) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    /* Fill in reverse so the result reads outermost-first. */
    int i = depth;
    for (tj_node *sym = tj_node_at(tokens, owners, num_tokens, cursor);
         sym != NULL; sym = sym->parent_node)
        result[--i] = sym->keyword;

    *out_n = depth;
    return result;
}

/* ── Keyword tables ──────────────────────────────────────────────────────── */

/** A single keyword completion entry: keyword text and short description. */
typedef struct {
    const char *kw;   /**< keyword text shown to the user */
    const char *doc;  /**< short documentation for the completion item */
} KwEntry;

/** Keywords offered at file scope (outside any block). */
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

/** Keywords offered inside a task block. */
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

/** Keywords offered inside a resource block. */
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

/** Keywords offered inside a project block. */
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

/** Keywords offered inside an account block. */
static const KwEntry ACCOUNT_KWS[] = {
    {"account", "Sub-account"},
    {"credit",  "Credit transaction"},
    {"debit",   "Debit transaction"},
    {NULL, NULL}
};

/**
 * Return the keyword table appropriate for the innermost recognised block
 * type in the enclosing keyword stack.  Walks @p stack from innermost
 * outward, skipping structural-but-transparent blocks (limits, supplement,
 * etc.) until a recognised type is found.
 *
 * @param stack  Stack of KW_* values, outermost first.
 * @param n      Length of @p stack.
 * @return The matching keyword table, or TOPLEVEL_KWS when no recognised
 *         block is found.
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

/** A single identifier completion candidate. */
typedef struct {
    char *id;    /**< TJP identifier, heap-allocated */
    char *name; /**< display name (quoted-string label), heap-allocated */
} IdEntry;

/** Dynamic array of IdEntry used to gather completion candidates. */
typedef struct {
    IdEntry *items;  /**< heap-allocated array of entries */
    int      n;      /**< number of entries */
    int      cap;    /**< allocated capacity */
} IdList;

/**
 * Append a heap-allocated copy of (@p id, @p name) to @p il, growing the
 * backing array if needed.
 *
 * @param il    Target IdList.
 * @param id    Identifier text; copied via strdup().
 * @param name  Display label; copied via strdup().
 */
static void idlist_push(IdList *il, const char *id, const char *name) {
    if (il->n >= il->cap) {
        il->cap = il->cap ? il->cap * 2 : 16;
        IdEntry *tmp = realloc(il->items, il->cap * sizeof(IdEntry));
        if (!tmp) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
        il->items = tmp;
    }
    il->items[il->n++] = (IdEntry){ strdup(id), strdup(name) };
}

/**
 * Free all entries in @p il and the backing array.
 *
 * @param il  IdList to release.
 */
static void idlist_free(IdList *il) {
    for (int i = 0; i < il->n; i++) {
        free(il->items[i].id);
        free(il->items[i].name);
    }
    free(il->items);
}

/**
 * Recursively collect all symbol IDs of @p kind from @p syms, building
 * dot-separated fully-qualified paths relative to @p prefix.  Symbols of
 * other kinds are recursed into but not emitted (so task children of a
 * project container are still collected).
 *
 * @param syms    Sibling symbols to walk.
 * @param n       Length of @p syms.
 * @param kind    KW_* kind to collect.
 * @param prefix  Path prefix already accumulated for the parent scope (may
 *                be `""`).
 * @param out     IdList to append to.
 */
static void collect_ids(tj_node *const *syms, int n, int kind,
                         const char *prefix, IdList *out) {
    for (int i = 0; i < n; i++) {
        if (syms[i]->keyword == kind && syms[i]->id && syms[i]->id[0]) {
            size_t prefix_len = prefix ? strlen(prefix) : 0;
            size_t id_len = strlen(syms[i]->id);
            size_t qualified_len = prefix_len ? prefix_len + 1 + id_len : id_len;
            char *qualified_id = malloc(qualified_len + 1);
            if (!qualified_id) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
            if (!prefix || !prefix[0]) {
                memcpy(qualified_id, syms[i]->id, id_len + 1);
            } else {
                memcpy(qualified_id, prefix, prefix_len);
                qualified_id[prefix_len] = '.';
                memcpy(qualified_id + prefix_len + 1, syms[i]->id, id_len + 1);
            }
            idlist_push(out, qualified_id, syms[i]->name ? syms[i]->name : "");
            collect_ids(syms[i]->children, syms[i]->num_children, kind, qualified_id, out);
            free(qualified_id);
        } else {
            collect_ids(syms[i]->children, syms[i]->num_children, kind, prefix, out);
        }
    }
}

/* ── Scope-aware helpers ─────────────────────────────────────────────────── */

/**
 * Return the ordered list of task IDs enclosing @p cursor (outermost
 * first).  Used to determine the bang-relative dep-ref scope for
 * dependency completions.  Uses tj_node_at() + parent walk to locate the
 * enclosing chain, collects KW_TASK ids in reverse order, then fills the
 * output outermost-first.
 *
 * @param tokens      Token spans of the current document.
 * @param owners      Per-token owner array, parallel to @p tokens.
 * @param num_tokens  Length of @p tokens / @p owners.
 * @param cursor      Cursor position.
 * @param out_n       Receives the depth of the returned array.
 * @return Heap-allocated array of heap-allocated strings; caller must free
 *         each string and the array.  NULL when no enclosing task exists.
 */
static char **current_task_scope(const TokenSpan *tokens, tj_node *const *owners,
                                 int num_tokens, LspPos cursor, int *out_n) {
    *out_n = 0;

    tj_node *innermost = tj_node_at(tokens, owners, num_tokens, cursor);

    int depth = 0;
    for (tj_node *sym = innermost; sym != NULL; sym = sym->parent_node) {
        if (sym->keyword == KW_TASK && sym->id && sym->id[0])
            depth++;
    }
    if (depth == 0) return NULL;

    char **result = malloc((size_t)depth * sizeof(char *));
    if (!result) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }

    int i = depth;
    for (tj_node *sym = innermost; sym != NULL; sym = sym->parent_node) {
        if (sym->keyword == KW_TASK && sym->id && sym->id[0])
            result[--i] = strdup(sym->id);
    }

    *out_n = depth;
    return result;
}

/**
 * Count the consecutive `!` tokens immediately before @p cursor, skipping
 * any trailing partial identifier and any comments.  Used to determine the
 * bang depth for dep-ref scope navigation.
 *
 * @param tokens              Token spans of the current document.
 * @param num_tokens          Length of @p tokens.
 * @param cursor              Cursor position.
 * @param out_first_bang_pos  When non-NULL and the count is > 0, receives
 *                            the start position of the earliest consecutive
 *                            bang.  Otherwise left untouched.
 * @return Number of consecutive bangs before @p cursor.
 */
static int count_leading_bangs(const TokenSpan *tokens, int num_tokens,
                               LspPos cursor, LspPos *out_first_bang_pos) {
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

    /* Count consecutive bangs scanning backwards, skipping comments, and
     * track the earliest bang's start position. */
    int count = 0;
    LspPos first = {0, 0};
    for (int i = last; i >= 0; i--) {
        if (tokens[i].token_kind == TK_LINE_COMMENT || tokens[i].token_kind == TK_BLOCK_COMMENT)
            continue;
        if (tokens[i].token_kind != TK_BANG) break;
        first = tokens[i].start;
        count++;
    }
    if (count > 0 && out_first_bang_pos)
        *out_first_bang_pos = first;
    return count;
}

/* ── Completion builders ─────────────────────────────────────────────────── */

/**
 * Map a KW_* keyword constant to the corresponding CompletionItemKind value.
 *
 * @param keyword  KW_* constant from grammar.tab.h.
 * @return The matching CompletionItemKind value (CIK_FUNCTION, CIK_CLASS,
 *         CIK_VARIABLE, or CIK_REFERENCE).
 */
static int completion_kind_for(int keyword) {
    switch (keyword) {
    case KW_TASK:     return CIK_FUNCTION;
    case KW_RESOURCE: return CIK_CLASS;
    case KW_ACCOUNT:  return CIK_VARIABLE;
    default:          return CIK_REFERENCE;
    }
}

/**
 * Test whether @p fw is a declaration keyword whose id/name the user is
 * typing — in which case completions should be suppressed.
 *
 * @param fw  Candidate keyword text (may be NULL).
 * @return 1 when @p fw is a declaration keyword, 0 otherwise.
 */
static int is_decl_keyword(const char *fw) {
    return fw && (strcmp(fw, "project")    == 0 || strcmp(fw, "task")       == 0
              || strcmp(fw, "resource")   == 0 || strcmp(fw, "account")    == 0
              || strcmp(fw, "shift")      == 0 || strcmp(fw, "scenario")   == 0
              || strcmp(fw, "macro")      == 0 || strcmp(fw, "include")    == 0
              || strcmp(fw, "supplement") == 0);
}

/**
 * Map an active-context keyword string to the KW_* symbol kind whose
 * identifiers form its arguments.
 *
 * @param keyword  Keyword text (e.g. `"depends"`, `"allocate"`).
 * @return The KW_* kind whose identifiers should be offered as completions,
 *         or 0 when @p keyword does not take id arguments.
 */
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

/**
 * Collect IDs from all symbol pools (primary + extras) into @p ids.
 *
 * @param symbols      Top-level symbols of the current document.
 * @param num_symbols  Length of @p symbols.
 * @param extra_pools  Per-document arrays of top-level symbols from other
 *                     open / background documents.
 * @param extra_counts Per-document lengths matching @p extra_pools.
 * @param num_extra    Length of @p extra_pools.
 * @param id_kind      KW_* kind to collect.
 * @param ids          Destination IdList.
 */
static void collect_all_ids(tj_node *const *symbols, int num_symbols,
                            tj_node *const **extra_pools,
                            const int *extra_counts, int num_extra,
                            int id_kind, IdList *ids) {
    collect_ids(symbols, num_symbols, id_kind, "", ids);
    for (int e = 0; e < num_extra; e++)
        collect_ids(extra_pools[e], extra_counts[e], id_kind, "", ids);
}

/**
 * Append one ID completion item to @p items.
 *
 * When @p bang_prefix is non-empty, the label and insertion text include
 * the leading bangs (so the popup shows e.g. `"!!alice.foo"`) and a
 * textEdit replaces the user-typed bang run from @p first_bang_pos through
 * @p cursor.  This keeps insertion idempotent regardless of how the client
 * treats `!` in its word-boundary rules.
 *
 * When @p bang_prefix is empty or NULL, the plain id is used as the label
 * with default client insertion semantics.
 *
 * @param doc             Destination mutable JSON document.
 * @param items           CompletionItem[] array to append to.
 * @param id              Identifier text to insert.
 * @param name            Display name (quoted-string label).
 * @param id_kind         KW_* kind, mapped to CompletionItemKind via
 *                        completion_kind_for().
 * @param bang_prefix     Bang prefix to render and insert; empty or NULL
 *                        when not in a bang context.
 * @param first_bang_pos  Position of the earliest user-typed bang (only
 *                        used when @p bang_prefix is non-empty).
 * @param cursor          Cursor position (used as textEdit end when @p
 *                        bang_prefix is non-empty).
 */
static void emit_id_item(yyjson_mut_doc *doc, yyjson_mut_val *items,
                          const char *id, const char *name, int id_kind,
                          const char *bang_prefix,
                          LspPos first_bang_pos, LspPos cursor) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, item, "kind", (uint64_t)completion_kind_for(id_kind));
    yyjson_mut_obj_add_strcpy(doc, item, "detail", name);
    yyjson_mut_obj_add_str(doc, item, "sortText", "0");

    if (bang_prefix && bang_prefix[0]) {
        size_t prefix_len = strlen(bang_prefix);
        size_t id_len = strlen(id);
        char labeled[1024];
        if (prefix_len + id_len + 1 > sizeof(labeled)) {
            yyjson_mut_obj_add_strcpy(doc, item, "label", id);
        } else {
            memcpy(labeled, bang_prefix, prefix_len);
            memcpy(labeled + prefix_len, id, id_len + 1);
            yyjson_mut_obj_add_strcpy(doc, item, "label", labeled);
            yyjson_mut_val *edit = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, edit, "range",
                                   range_json(doc, (LspRange){first_bang_pos, cursor}));
            yyjson_mut_obj_add_strcpy(doc, edit, "newText", labeled);
            yyjson_mut_obj_add_val(doc, item, "textEdit", edit);
        }
    } else {
        yyjson_mut_obj_add_strcpy(doc, item, "label", id);
    }
    yyjson_mut_arr_add_val(items, item);
}

/**
 * Build dep completion items for `depends` / `precedes`.
 *
 * If no leading bangs: return all absolute task IDs from all open files.
 * The result is complete — `*out_incomplete` is set to 0.
 * If leading bangs: navigate to the bang-relative scope level and return
 * all task IDs reachable from there.  Labels include the bang run and a
 * textEdit replaces the typed bangs.  Typing an additional `!` widens the
 * scope, so `*out_incomplete` is set to 1 to force a re-request.
 *
 * @param doc            Destination mutable JSON document.
 * @param items          CompletionItem[] array to append to.
 * @param tokens         Token spans of the current document.
 * @param owners         Per-token owner array, parallel to @p tokens.
 * @param num_tokens     Length of @p tokens / @p owners.
 * @param cursor         Cursor position.
 * @param symbols        Top-level symbols of the current document.
 * @param num_symbols    Length of @p symbols.
 * @param extra_pools    Per-document arrays of top-level symbols from other
 *                       open / background documents.
 * @param extra_counts   Per-document lengths matching @p extra_pools.
 * @param num_extra      Length of @p extra_pools.
 * @param id_kind        KW_* kind to collect (always KW_TASK for now).
 * @param out_incomplete Receives the LSP `isIncomplete` flag.
 * @return Number of items added to @p items.
 */
static int build_dep_completions(yyjson_mut_doc *doc, yyjson_mut_val *items,
                                  const TokenSpan *tokens,
                                  tj_node *const *owners, int num_tokens,
                                  LspPos cursor,
                                  tj_node *const *symbols, int num_symbols,
                                  tj_node *const **extra_pools,
                                  const int *extra_counts, int num_extra,
                                  int id_kind, int *out_incomplete) {
    LspPos first_bang_pos = {0, 0};
    int bang_count = count_leading_bangs(tokens, num_tokens, cursor,
                                         &first_bang_pos);
    IdList ids = {0};
    char bang_prefix[64] = "";

    if (bang_count == 0) {
        collect_all_ids(symbols, num_symbols, extra_pools, extra_counts,
                        num_extra, id_kind, &ids);
    } else {
        int scope_n = 0;
        char **scope = current_task_scope(tokens, owners, num_tokens, cursor, &scope_n);

        if (bang_count <= scope_n) {
            int ch_n = 0;
            tj_node *const *ch = tj_node_find_path(
                symbols, num_symbols,
                (const char **)scope, scope_n - bang_count, &ch_n);
            if (ch) collect_ids(ch, ch_n, id_kind, "", &ids);
        }

        memset(bang_prefix, '!', (size_t)bang_count);
        bang_prefix[bang_count] = '\0';

        for (int i = 0; i < scope_n; i++) free(scope[i]);
        free(scope);
    }

    int count = 0;
    for (int i = 0; i < ids.n; i++) {
        emit_id_item(doc, items, ids.items[i].id, ids.items[i].name,
                     id_kind, bang_prefix, first_bang_pos, cursor);
        count++;
    }

    if (out_incomplete) *out_incomplete = (bang_count > 0) ? 1 : 0;

    idlist_free(&ids);
    return count;
}

/**
 * Build ID completion items for an `allocate` / `chargeset` / etc.
 * argument.
 *
 * For KW_TASK, delegates to build_dep_completions() which handles bang
 * scoping.  For the other ID kinds (resources, accounts) bangs are not
 * meaningful, so the list is always complete and `*out_incomplete` is set
 * to 0.
 *
 * @param doc            Destination mutable JSON document.
 * @param items          CompletionItem[] array to append to.
 * @param tokens         Token spans of the current document.
 * @param owners         Per-token owner array, parallel to @p tokens.
 * @param num_tokens     Length of @p tokens / @p owners.
 * @param cursor         Cursor position.
 * @param symbols        Top-level symbols of the current document.
 * @param num_symbols    Length of @p symbols.
 * @param extra_pools    Per-document arrays of top-level symbols from other
 *                       open / background documents.
 * @param extra_counts   Per-document lengths matching @p extra_pools.
 * @param num_extra      Length of @p extra_pools.
 * @param id_kind        KW_* kind to collect.
 * @param out_incomplete Receives the LSP `isIncomplete` flag.
 * @return Number of items added to @p items.
 */
static int build_id_completions(yyjson_mut_doc *doc, yyjson_mut_val *items,
                                const TokenSpan *tokens,
                                tj_node *const *owners, int num_tokens,
                                LspPos cursor,
                                tj_node *const *symbols, int num_symbols,
                                tj_node *const **extra_pools,
                                const int *extra_counts, int num_extra,
                                int id_kind, int *out_incomplete) {
    if (id_kind == KW_TASK)
        return build_dep_completions(doc, items, tokens, owners, num_tokens, cursor,
                                     symbols, num_symbols,
                                     extra_pools, extra_counts, num_extra,
                                     id_kind, out_incomplete);

    IdList ids = {0};
    collect_all_ids(symbols, num_symbols, extra_pools, extra_counts,
                    num_extra, id_kind, &ids);

    int count = 0;
    for (int i = 0; i < ids.n; i++) {
        emit_id_item(doc, items, ids.items[i].id, ids.items[i].name,
                     id_kind, NULL, (LspPos){0, 0}, (LspPos){0, 0});
        count++;
    }

    if (out_incomplete) *out_incomplete = 0;

    idlist_free(&ids);
    return count;
}

/**
 * Build keyword completion items for the current block context.
 *
 * @param doc         Destination mutable JSON document.
 * @param items       CompletionItem[] array to append to.
 * @param tokens      Token spans of the current document.
 * @param num_tokens  Length of @p tokens.
 * @param cursor      Cursor position.
 * @param partial     Partial identifier the user is typing (may be `""`).
 * @param stack       Enclosing block-keyword stack from block_stack().
 * @param stack_n     Length of @p stack.
 * @return Number of items added to @p items.
 */
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
                                        const TokenSpan *tokens,
                                        tj_node *const *owners, int num_tokens,
                                        LspPos cursor,
                                        tj_node *const *symbols, int num_symbols,
                                        tj_node *const **extra_pools,
                                        const int *extra_counts,
                                        int num_extra,
                                        const char *text) {
    /* Suppress completions inside strings and scissors blocks */
    if (cursor_in_dquote(text, cursor, tokens, num_tokens)
        || cursor_in_scissors(text, cursor, tokens, num_tokens))
        return yyjson_mut_null(doc);

    /* Gather cursor context: enclosing block stack, partial word, first word */
    int    stack_n    = 0;
    int   *stack      = block_stack(tokens, owners, num_tokens, cursor, &stack_n);
    char  *partial    = partial_word(tokens, num_tokens, cursor);
    char  *first_word = line_first_word(tokens, num_tokens, cursor);

    yyjson_mut_val *items = yyjson_mut_arr(doc);
    int item_count = 0;
    int is_incomplete = 0;

    /* Suppress completions while typing a declaration keyword's id/name */
    if (is_decl_keyword(first_word))
        goto done;

    /* Try ID completions (depends, allocate, chargeset, etc.).
     *
     * Skip when the partial is the first token on its line: that means we're
     * starting a new statement, not filling arguments of a keyword from a
     * previous line.  (Keywords like `charge` have token_kind > KW_SIG_END so
     * scan_kw_stack treats them as arguments of the preceding keyword, which
     * would incorrectly return e.g. `depends` as the active context.) */
    if (!first_word || !partial[0] || strcmp(first_word, partial) != 0)
    {
        ActiveContext ac = active_context(tokens, num_tokens, cursor);
        int id_kind = id_kind_for_keyword(ac.keyword);

        if (id_kind) {
            item_count = build_id_completions(doc, items,
                                              tokens, owners, num_tokens,
                                              cursor,
                                              symbols, num_symbols,
                                              extra_pools, extra_counts, num_extra,
                                              id_kind, &is_incomplete);
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
    yyjson_mut_obj_add_bool(doc, list, "isIncomplete", (bool)is_incomplete);
    yyjson_mut_obj_add_val(doc, list, "items", items);
    return list;
}

/** Gather the cross-file completion sources for a query: every non-primary
 *  sibling document's top-level symbol pool. Every non-primary doc in the
 *  context is a sibling in the requester's project (the clone step already
 *  restricted membership); an orphan has no siblings, so it yields no extras.
 *  @param qc            Query context to scan.
 *  @param extra_pools   Receives up to MAX_DOCS sibling symbol pools.
 *  @param extra_counts  Receives each pool's length, parallel to @p extra_pools.
 *  @param out_num_extra Receives the number of pools collected. */
static void collect_sibling_pools(const query_context *qc,
                                  tj_node *const **extra_pools,
                                  int *extra_counts, int *out_num_extra) {
    int num_extra = 0;
    for (int i = 0; i < qc->num_docs && num_extra < MAX_DOCS; i++) {
        if (qc->docs[i].is_primary) continue;
        tj_node *const *top; int n;
        doc_symbol_pool(&qc->docs[i], &top, &n);
        if (!top) continue;
        extra_pools[num_extra]  = top;
        extra_counts[num_extra] = n;
        num_extra++;
    }
    *out_num_extra = num_extra;
}

yyjson_mut_val *handle_completion(yyjson_mut_doc *doc, yyjson_val *id,
                                  yyjson_val *params, const query_context *qc,
                                  const query_doc *d) {
    if (!params) return make_response(doc, id, yyjson_mut_null(doc));
    yyjson_val *tdp = yyjson_obj_get(params, "textDocumentPosition");
    if (!tdp) tdp = params;

    yyjson_val *pos_obj = yyjson_obj_get(tdp, "position");
    if (!pos_obj) pos_obj = yyjson_obj_get(params, "position");
    if (!pos_obj || !d || !d->root) return make_response(doc, id, yyjson_mut_null(doc));

    tj_node *const *extra_pools[MAX_DOCS];
    int             extra_counts[MAX_DOCS];
    int             num_extra = 0;
    collect_sibling_pools(qc, extra_pools, extra_counts, &num_extra);

    tj_node *const *self_top; int self_n;
    doc_symbol_pool(d, &self_top, &self_n);

    LspPos pos             = json_to_pos(pos_obj);
    yyjson_mut_val *result = build_completions_json(doc,
                                                    d->tok_spans,
                                                    d->tok_owners,
                                                    d->num_tok_spans,
                                                    pos,
                                                    self_top, self_n,
                                                    extra_pools,
                                                    extra_counts,
                                                    num_extra,
                                                    d->text);
    return make_response(doc, id, result);
}
