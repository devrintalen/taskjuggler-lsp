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

#include "code_lens.h"
#include "document_symbol.h"
#include "grammar.tab.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Working-time defaults matching TaskJuggler: 8 hours per working day,
 * Mon-Fri, no holidays.  The project-level `dailyworkinghours`,
 * `weekdays`, and holiday declarations are not yet captured by the
 * parser, so this lens treats them as fixed. */
#define WORKING_MINUTES_PER_DAY (8 * 60)
#define WORKING_DAYS_PER_WEEK   5
#define WORKING_DAYS_PER_MONTH  22   /* ≈ yearlyworkingdays(260) / 12 */
#define WORKING_DAYS_PER_YEAR   260

/* Calendar approximations for `duration`: a month is 30 days, a year
 * is 365 days. */
#define CALENDAR_SECONDS_PER_DAY   (24L * 60 * 60)

/**
 * Map a duration unit string to a canonical character.  Returns 0 if
 * @p s is not a recognized unit.
 *
 * The returned value uses `'i'` for "min" to disambiguate from `'m'`
 * (month).
 */
static char canon_unit(const char *s) {
    if (!s) return 0;
    if (strcmp(s, "min") == 0) return 'i';
    if (strcmp(s, "h")   == 0) return 'h';
    if (strcmp(s, "d")   == 0) return 'd';
    if (strcmp(s, "w")   == 0) return 'w';
    if (strcmp(s, "m")   == 0) return 'm';
    if (strcmp(s, "y")   == 0) return 'y';
    return 0;
}

/**
 * Parse a compact `TK_DURATION` token text like `"5d"`, `"+4m"`, or
 * `"-2h"` (regex from `lexer.l`).
 */
static int parse_compact_duration(const char *text,
                                  int *out_value, char *out_unit) {
    if (!text || !*text) return 0;
    const char *p = text;
    int sign = 1;
    if (*p == '+') { p++; }
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return 0;
    int value = 0;
    while (isdigit((unsigned char)*p)) {
        value = value * 10 + (*p - '0');
        p++;
    }
    char unit = canon_unit(p);
    if (!unit) return 0;
    *out_value = sign * value;
    *out_unit  = unit;
    return 1;
}

/**
 * Read a `dur_val` (grammar.y:1264) starting at @p start_idx in the
 * token stream — either a compact `TK_DURATION` or
 * `TK_INTEGER`|`TK_FLOAT` + `TK_IDENT`.
 */
static int parse_dur_val_at(const parse_slab *slab,
                            const TokenSpan *spans, int num_spans,
                            int start_idx,
                            int *out_value, char *out_unit) {
    if (start_idx >= num_spans) return 0;
    const TokenSpan *t = &spans[start_idx];
    if (t->token_kind == TK_DURATION) {
        return parse_compact_duration(slab_str(slab, t->text_off),
                                      out_value, out_unit);
    }
    if (t->token_kind == TK_INTEGER || t->token_kind == TK_FLOAT) {
        if (start_idx + 1 >= num_spans) return 0;
        const TokenSpan *u = &spans[start_idx + 1];
        if (u->token_kind != TK_IDENT) return 0;
        char unit = canon_unit(slab_str(slab, u->text_off));
        if (!unit) return 0;
        const char *ttext = slab_str(slab, t->text_off);
        if (!ttext) return 0;
        int value = (int)strtol(ttext, NULL, 10);
        *out_value = value;
        *out_unit  = unit;
        return 1;
    }
    return 0;
}

/**
 * Convert a duration to a count of calendar days.  Sub-day units
 * collapse to zero — the lens shows the same calendar day as the base
 * in that case (the task starts and ends on the same day).
 */
static long duration_to_calendar_days(int value, char unit) {
    switch (unit) {
    case 'i': return 0;
    case 'h': return 0;
    case 'd': return value;
    case 'w': return (long)value * 7;
    case 'm': return (long)value * 30;
    case 'y': return (long)value * 365;
    default:  return 0;
    }
}

/**
 * Convert a `length` value to working days.  Sub-day units collapse to
 * one working day (the task is contained inside a single workday).
 */
static long length_to_working_days(int value, char unit) {
    switch (unit) {
    case 'i': /* fallthrough */
    case 'h': return value > 0 ? 1 : (value < 0 ? -1 : 0);
    case 'd': return value;
    case 'w': return (long)value * WORKING_DAYS_PER_WEEK;
    case 'm': return (long)value * WORKING_DAYS_PER_MONTH;
    case 'y': return (long)value * WORKING_DAYS_PER_YEAR;
    default:  return 0;
    }
}

/** Step @p t forward or backward by one calendar day. */
static time_t step_day(time_t t, int direction) {
    return t + (long)direction * CALENDAR_SECONDS_PER_DAY;
}

/** Non-zero if @p t falls on Saturday or Sunday (UTC). */
static int is_weekend(time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);
    return tm.tm_wday == 0 || tm.tm_wday == 6;
}

/**
 * Walk @p days working days from @p base, inclusive of @p base itself
 * (snapped to the nearest working day in the requested @p direction
 * first).
 *
 * For `length 5d` starting Mon, this returns the following Friday
 * (Mon=1, Tue=2, Wed=3, Thu=4, Fri=5).  Symmetric for backward walks.
 */
static time_t walk_working_days_inclusive(time_t base, long days,
                                          int direction) {
    if (days <= 0) return base;
    time_t cur = base;
    while (is_weekend(cur)) cur = step_day(cur, direction);
    long remaining = days - 1;
    while (remaining > 0) {
        cur = step_day(cur, direction);
        if (!is_weekend(cur)) remaining--;
    }
    return cur;
}

/**
 * Walk @p days calendar days from @p base, inclusive of @p base.
 *
 * For `duration 5d` starting Mon, this returns Fri (Mon=1, …, Fri=5).
 */
static time_t walk_calendar_days_inclusive(time_t base, long days,
                                           int direction) {
    if (days <= 0) return base;
    return base + (long)direction * (days - 1) * CALENDAR_SECONDS_PER_DAY;
}

/** Format @p t as `YYYY-MM-DD` into @p buf (must hold ≥ 16 bytes). */
static void format_date(time_t t, char *buf, size_t buf_size) {
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, buf_size, "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/** Add a `Position` object {line, character} under @p key. */
static void put_position(yyjson_mut_doc *doc, yyjson_mut_val *parent,
                          const char *key, LspPos p) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, obj, "line",      p.line);
    yyjson_mut_obj_add_uint(doc, obj, "character", p.character);
    yyjson_mut_obj_add_val(doc, parent, key, obj);
}

/**
 * Append one CodeLens object to @p arr.  Inert: `command.command` is
 * the empty string so the editor renders only the title.
 */
static void push_lens(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                       LspRange range, const char *title) {
    yyjson_mut_val *lens = yyjson_mut_obj(doc);

    yyjson_mut_val *range_obj = yyjson_mut_obj(doc);
    put_position(doc, range_obj, "start", range.start);
    put_position(doc, range_obj, "end",   range.end);
    yyjson_mut_obj_add_val(doc, lens, "range", range_obj);

    yyjson_mut_val *cmd = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, cmd, "title",   title);
    yyjson_mut_obj_add_str   (doc, cmd, "command", "");
    yyjson_mut_obj_add_val   (doc, lens, "command", cmd);

    yyjson_mut_arr_add_val(arr, lens);
}

/* Build the JSON array for a textDocument/codeLens response.
 *
 * Single linear scan of tok_spans[]: for each `length` / `duration`
 * keyword whose owning task has an explicit `start` or `end` date
 * (precomputed during parse and stored on the tj_node), compute the
 * complementary endpoint and emit a lens.
 *
 * The `symbols` / `num_symbols` parameters are unused — endpoint data
 * is read directly from each token's `owner` — but kept for symmetry
 * with other build_*_json entry points.
 */
yyjson_mut_val *build_code_lens_json(yyjson_mut_doc *doc,
                                     const parse_slab *slab,
                                     const TokenSpan *spans, int num_spans,
                                     tj_node *const *symbols,
                                     int num_symbols) {
    (void)symbols;
    (void)num_symbols;

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (!spans || num_spans == 0) return arr;

    for (int i = 0; i < num_spans; i++) {
        const TokenSpan *t = &spans[i];
        if (t->token_kind != KW_LENGTH && t->token_kind != KW_DURATION) continue;
        tj_node *owner = slab_node(slab, t->owner_idx);
        if (!owner || owner->keyword != KW_TASK) continue;
        if (!owner->has_start && !owner->has_end) continue;

        int value;
        char unit;
        if (!parse_dur_val_at(slab, spans, num_spans, i + 1, &value, &unit)) continue;
        if (value == 0) continue;

        int direction;
        time_t base;
        const char *arrow;
        if (owner->has_start) {
            base = owner->start_date;
            direction = +1;
            arrow = "\xE2\x86\x92 ends ";   /* "→ ends " */
        } else {
            base = owner->end_date;
            direction = -1;
            arrow = "\xE2\x86\x90 starts "; /* "← starts " */
        }

        time_t result;
        if (t->token_kind == KW_DURATION) {
            long days = duration_to_calendar_days(value, unit);
            result = walk_calendar_days_inclusive(base, labs(days), direction);
        } else {
            long days = length_to_working_days(value, unit);
            result = walk_working_days_inclusive(base, labs(days), direction);
        }

        char date_buf[48];
        format_date(result, date_buf, sizeof(date_buf));
        char title[80];
        snprintf(title, sizeof(title), "%s%s", arrow, date_buf);

        LspRange range = { t->start, t->end };
        push_lens(doc, arr, range, title);
    }

    return arr;
}
