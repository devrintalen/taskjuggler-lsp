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

#include "hover.h"

#include <stdlib.h>
#include <string.h>

/* ── token_at ────────────────────────────────────────────────────────────── */

/* Returns 1 if position p falls within [start, end] (both endpoints inclusive). */
static int pos_in(LspPos p, LspPos start, LspPos end) {
    int after  = (p.line > start.line)
              || (p.line == start.line && p.character >= start.character);
    int before = (p.line < end.line)
              || (p.line == end.line && p.character <= end.character);
    return after && before;
}

/* Return a copy of the token span that covers pos, or a sentinel TK_EOF span
 * if no token covers that position.  The returned span's text field is always
 * heap-allocated; caller must free it.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * pos        — cursor position to look up
 */
TokenSpan tok_span_at(const TokenSpan *tokens, int num_tokens, LspPos pos) {
    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *t = &tokens[i];
        if (t->token_kind == TK_EOF) break;

        /* Stop once we've passed the requested position */
        if (t->start.line > pos.line
         || (t->start.line == pos.line && t->start.character > pos.character))
            return (TokenSpan){ TK_EOF, pos, pos, strdup("") };

        if (pos_in(pos, t->start, t->end)) {
            TokenSpan copy = *t;
            copy.text = strdup(t->text ? t->text : "");
            return copy;
        }
    }
    return (TokenSpan){ TK_EOF, pos, pos, strdup("") };
}

/* ── scan_kw_stack ───────────────────────────────────────────────────────── */

/* Scan tokens from the start up to cursor, building a stack of active keywords.
 * Each entry records the keyword text, its source range, the brace depth at
 * which it appeared, and (if track_argc) the number of arguments before cursor.
 * Keywords are pushed at their depth and popped when a closing } drops the depth
 * below the entry's recorded depth.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — stop scanning after this position
 * kind_max   — tokens with token_kind < kind_max are treated as keywords to track;
 *              pass KW_DOCS_END for hover, KW_SIG_END for signature help
 * track_argc — if non-zero, count non-keyword, non-comment tokens as arguments
 * stack      — caller-allocated array to hold the keyword stack entries
 * stack_cap  — capacity of stack
 * out_depth  — set to the brace depth at cursor on return
 *
 * Returns the number of entries written to stack.
 */
int scan_kw_stack(const TokenSpan *tokens, int num_tokens, LspPos cursor,
                  int kind_max, int track_argc,
                  KwStackEntry *stack, int stack_cap,
                  uint32_t *out_depth) {
    uint32_t brace_depth = 0;
    int stack_n = 0;

    for (int i = 0; i < num_tokens; i++) {
        const TokenSpan *tok = &tokens[i];
        if (tok->token_kind == TK_EOF) break;

        if (tok->start.line > cursor.line
         || (tok->start.line == cursor.line
          && tok->start.character > cursor.character))
            break;

        switch (tok->token_kind) {
        case TK_LINE_COMMENT:
        case TK_BLOCK_COMMENT:
            break;

        case TK_LBRACE:
            brace_depth++;
            break;

        case TK_RBRACE:
            if (brace_depth > 0) brace_depth--;
            while (stack_n > 0 && stack[stack_n - 1].depth > brace_depth)
                free(stack[--stack_n].kw);
            break;

        default:
            if (tok->token_kind < kind_max && tok->text) {
                while (stack_n > 0 && stack[stack_n - 1].depth >= brace_depth)
                    free(stack[--stack_n].kw);
                if (stack_n < stack_cap) {
                    stack[stack_n++] = (KwStackEntry){
                        strdup(tok->text),
                        (LspRange){ tok->start, tok->end },
                        brace_depth,
                        0
                    };
                }
                break;
            }
            if (track_argc) {
                int fully_before = (tok->end.line < cursor.line)
                    || (tok->end.line == cursor.line
                     && tok->end.character <= cursor.character);
                if (fully_before) {
                    for (int j = stack_n - 1; j >= 0; j--) {
                        if (stack[j].depth == brace_depth) {
                            stack[j].argc++;
                            break;
                        }
                    }
                }
            }
            break;
        }
    }

    *out_depth = brace_depth;
    return stack_n;
}

/* ── active_keyword_at ───────────────────────────────────────────────────── */

/* Return the innermost keyword that has hover documentation and whose brace
 * depth matches the cursor's brace depth.  The returned keyword string is
 * heap-allocated; caller must free it.  Returns {NULL, ...} if none found.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — cursor position from the textDocument/hover request
 */
ActiveKeyword active_keyword_at(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    KwStackEntry stack[128];
    uint32_t brace_depth;
    int stack_n = scan_kw_stack(tokens, num_tokens, cursor,
                                KW_DOCS_END, 0, stack, 128, &brace_depth);

    for (int i = stack_n - 1; i >= 0; i--) {
        if (stack[i].depth == brace_depth) {
            ActiveKeyword ak = { stack[i].kw, stack[i].range };
            for (int j = 0; j < stack_n; j++)
                if (j != i) free(stack[j].kw);
            return ak;
        }
    }

    for (int i = 0; i < stack_n; i++) free(stack[i].kw);
    return (ActiveKeyword){ NULL, {{0,0},{0,0}} };
}

/* ── keyword_docs ────────────────────────────────────────────────────────── *
 *
 * Returns a Markdown documentation string for a keyword, or NULL if the
 * keyword has no hover entry.
 *
 * Coverage: 39 of ~200 TaskJuggler keywords are currently documented.
 *
 * Covered (39):
 *   Declarations:  project, task, resource, account, shift, macro, include,
 *                  flags, supplement
 *   Task attrs:    effort, duration, length, milestone, depends, precedes,
 *                  allocate, start, end, maxstart, minstart, maxend, minend,
 *                  priority, complete, note, responsible, booking, scheduled
 *   Resource attrs: rate, efficiency, vacation, leaves
 *   Project attrs: now, currency, timeformat, timezone, workinghours,
 *                  timingresolution, scenario
 *
 * Not yet covered (examples):
 *   Declarations:  taskprefix, resourceprefix, extend
 *   Task attrs:    limits, charge, chargeset, journalentry, statusnote,
 *                  projectid, overtime, forward, backward, scheduling
 *   Resource attrs: workerperday, managers, fail, warn, purge
 *   Report types:  taskreport, resourcereport, textreport, tracereport,
 *                  export, accountreport (and all report sub-attributes)
 *   Misc:          alertlevel, columns, caption, center, formats, headline,
 *                  hidetask, hideresource, rolluptask, rollupresource, sortby
 */

const char *keyword_docs(const char *kw) {
    if (!kw) return NULL;

    if (strcmp(kw, "project") == 0) return
        "**`project`** *(declaration)*\n\n"
        "Defines the project header. Required exactly once per TJP file.\n\n"
        "**Syntax:** `project <id> \"<name>\" <start> <end|duration> { \xe2\x80\xa6 }`\n\n"
        "**Example:**\n"
        "```\n"
        "project acso \"Accounting Software\" 2024-01-01 +6m { }\n"
        "```";

    if (strcmp(kw, "task") == 0) return
        "**`task`** *(declaration)*\n\n"
        "Defines a work package, summary task, or milestone. Tasks may be nested "
        "to form a work-breakdown structure.\n\n"
        "**Syntax:** `task <id> \"<name>\" { \xe2\x80\xa6 }`\n\n"
        "**Common body attributes:** `effort`, `duration`, `depends`, `allocate`, "
        "`milestone`, `start`, `end`, `priority`, `complete`";

    if (strcmp(kw, "resource") == 0) return
        "**`resource`** *(declaration)*\n\n"
        "Defines a resource (person, team, or piece of equipment) that can be "
        "allocated to tasks.\n\n"
        "**Syntax:** `resource <id> \"<name>\" { \xe2\x80\xa6 }`\n\n"
        "**Common body attributes:** `rate`, `workinghours`, `vacation`, `leaves`, "
        "`efficiency`, `limits`";

    if (strcmp(kw, "account") == 0) return
        "**`account`** *(declaration)*\n\n"
        "Defines a cost/revenue account used for project accounting.\n\n"
        "**Syntax:** `account <id> \"<name>\" { \xe2\x80\xa6 }`";

    if (strcmp(kw, "shift") == 0) return
        "**`shift`** *(declaration)*\n\n"
        "Defines a named set of working hours that can be assigned to resources or tasks.\n\n"
        "**Syntax:** `shift <id> \"<name>\" { workinghours \xe2\x80\xa6 }`";

    if (strcmp(kw, "macro") == 0) return
        "**`macro`** *(declaration)*\n\n"
        "Defines a reusable text macro. The body is enclosed in `[ \xe2\x80\xa6 ]` and may "
        "contain `${1}`, `${2}`, \xe2\x80\xa6 placeholders for arguments.\n\n"
        "**Syntax:** `macro <id> [ <body> ]`";

    if (strcmp(kw, "include") == 0) return
        "**`include`** *(directive)*\n\n"
        "Includes another TaskJuggler file at this point in the project.\n\n"
        "**Syntax:** `include \"<filename>\"`";

    if (strcmp(kw, "flags") == 0) return
        "**`flags`** *(declaration)*\n\n"
        "Declares one or more flag identifiers that can later be set on tasks, "
        "resources, or accounts and used in reports.\n\n"
        "**Syntax:** `flags <id> [, <id> \xe2\x80\xa6]`";

    if (strcmp(kw, "supplement") == 0) return
        "**`supplement`** *(declaration)*\n\n"
        "Adds attributes to a previously-defined task or resource without "
        "redefining it. Useful when splitting definitions across included files.\n\n"
        "**Syntax:** `supplement task|resource <id> { \xe2\x80\xa6 }`";

    if (strcmp(kw, "effort") == 0) return
        "**`effort`** *(task attribute)*\n\n"
        "The amount of work required to complete the task, expressed in time units. "
        "The scheduler distributes this across allocated resources.\n\n"
        "**Syntax:** `effort <value><unit>`\n\n"
        "**Units:** `min`, `h`, `d` (days), `w` (weeks), `m` (months)\n\n"
        "**Example:** `effort 5d`";

    if (strcmp(kw, "duration") == 0) return
        "**`duration`** *(task attribute)*\n\n"
        "The elapsed (wall-clock) duration of the task, regardless of resource "
        "availability or working hours.\n\n"
        "**Syntax:** `duration <value><unit>`\n\n"
        "**Units:** `min`, `h`, `d`, `w`, `m`";

    if (strcmp(kw, "length") == 0) return
        "**`length`** *(task attribute)*\n\n"
        "The duration of the task measured in working days/hours, as opposed to "
        "calendar time (`duration`).\n\n"
        "**Syntax:** `length <value><unit>`";

    if (strcmp(kw, "milestone") == 0) return
        "**`milestone`** *(task attribute)*\n\n"
        "Marks the task as a milestone (zero duration). A milestone represents a "
        "point-in-time event rather than a work package.\n\n"
        "**Syntax:** `milestone`";

    if (strcmp(kw, "depends") == 0) return
        "**`depends`** *(task attribute)*\n\n"
        "Specifies finish-to-start dependencies on other tasks. The current task "
        "cannot start until all listed tasks have finished.\n\n"
        "**Syntax:** `depends <task-id> [{ gap <duration> }] [, \xe2\x80\xa6]`\n\n"
        "**Example:** `depends !design, !prototype`";

    if (strcmp(kw, "precedes") == 0) return
        "**`precedes`** *(task attribute)*\n\n"
        "The reverse of `depends`: lists tasks that must not start before this "
        "task finishes.\n\n"
        "**Syntax:** `precedes <task-id> [, \xe2\x80\xa6]`";

    if (strcmp(kw, "allocate") == 0) return
        "**`allocate`** *(task attribute)*\n\n"
        "Assigns one or more resources to the task for scheduling purposes.\n\n"
        "**Syntax:** `allocate <resource-id> [{ \xe2\x80\xa6 }] [, \xe2\x80\xa6]`\n\n"
        "**Example:** `allocate alice, bob`";

    if (strcmp(kw, "start") == 0) return
        "**`start`** *(task / project attribute)*\n\n"
        "Pins the start date of a task or defines the project start date.\n\n"
        "**Syntax:** `start <YYYY-MM-DD[-HH:MM]>`";

    if (strcmp(kw, "end") == 0) return
        "**`end`** *(task / project attribute)*\n\n"
        "Pins the end date of a task or defines the project end date.\n\n"
        "**Syntax:** `end <YYYY-MM-DD[-HH:MM]>`";

    if (strcmp(kw, "maxstart") == 0) return
        "**`maxstart`** *(task attribute)*\n\n"
        "Sets a latest-allowed start date. The scheduler reports an error if the "
        "task would start later than this date.\n\n"
        "**Syntax:** `maxstart <YYYY-MM-DD>`";

    if (strcmp(kw, "minstart") == 0) return
        "**`minstart`** *(task attribute)*\n\n"
        "Sets an earliest-allowed start date.\n\n"
        "**Syntax:** `minstart <YYYY-MM-DD>`";

    if (strcmp(kw, "maxend") == 0) return
        "**`maxend`** *(task attribute)*\n\n"
        "Sets a latest-allowed end date.\n\n"
        "**Syntax:** `maxend <YYYY-MM-DD>`";

    if (strcmp(kw, "minend") == 0) return
        "**`minend`** *(task attribute)*\n\n"
        "Sets an earliest-allowed end date.\n\n"
        "**Syntax:** `minend <YYYY-MM-DD>`";

    if (strcmp(kw, "priority") == 0) return
        "**`priority`** *(task attribute)*\n\n"
        "Sets the scheduling priority. Higher values (0\xe2\x80\x93" "1000, default 500) cause "
        "the task to be scheduled before lower-priority tasks when resources are "
        "contested.\n\n"
        "**Syntax:** `priority <0\xe2\x80\x93" "1000>`";

    if (strcmp(kw, "complete") == 0) return
        "**`complete`** *(task attribute)*\n\n"
        "The percentage of the task that is already done (0\xe2\x80\x93" "100). Used in progress "
        "tracking and reports.\n\n"
        "**Syntax:** `complete <0\xe2\x80\x93" "100>`";

    if (strcmp(kw, "note") == 0) return
        "**`note`** *(task / resource attribute)*\n\n"
        "Attaches a free-text note to the task or resource.\n\n"
        "**Syntax:** `note \"<text>\"`";

    if (strcmp(kw, "responsible") == 0) return
        "**`responsible`** *(task attribute)*\n\n"
        "Names the resource(s) accountable for completing the task. Informational "
        "only; does not affect scheduling.\n\n"
        "**Syntax:** `responsible <resource-id> [, \xe2\x80\xa6]`";

    if (strcmp(kw, "booking") == 0) return
        "**`booking`** *(task attribute)*\n\n"
        "Records actual work already performed on a task by a resource on a "
        "specific date, used for progress tracking.\n\n"
        "**Syntax:** `booking <resource-id> <date> <duration> { \xe2\x80\xa6 }`";

    if (strcmp(kw, "scheduled") == 0) return
        "**`scheduled`** *(task attribute)*\n\n"
        "Marks a task as having fixed, pre-computed start and end dates that the "
        "scheduler should not change.\n\n"
        "**Syntax:** `scheduled`";

    if (strcmp(kw, "rate") == 0) return
        "**`rate`** *(resource attribute)*\n\n"
        "The daily cost rate of the resource in the project currency, used for "
        "cost calculations.\n\n"
        "**Syntax:** `rate <number>`";

    if (strcmp(kw, "efficiency") == 0) return
        "**`efficiency`** *(resource attribute)*\n\n"
        "A multiplier (default 1.0) applied to the resource\xe2\x80\x99s work output. "
        "`efficiency 0.5` means the resource works at half speed.\n\n"
        "**Syntax:** `efficiency <number>`";

    if (strcmp(kw, "vacation") == 0) return
        "**`vacation`** *(resource / project attribute)*\n\n"
        "Defines a vacation period during which the resource is unavailable (or "
        "a project-wide holiday).\n\n"
        "**Syntax:** `vacation \"<name>\" <date> [+<duration> | - <date>]`";

    if (strcmp(kw, "leaves") == 0) return
        "**`leaves`** *(resource / project attribute)*\n\n"
        "Defines a leave block (holiday, sick leave, etc.) similar to `vacation` "
        "but with a typed category.\n\n"
        "**Syntax:** `leaves <type> <date> [+<duration>]`\n\n"
        "**Types:** `annual`, `sick`, `special`, `unpaid`";

    if (strcmp(kw, "now") == 0) return
        "**`now`** *(project attribute)*\n\n"
        "Sets the reference date used as \xe2\x80\x9ctoday\xe2\x80\x9d in reports and progress "
        "calculations. Defaults to the current date.\n\n"
        "**Syntax:** `now <YYYY-MM-DD>`";

    if (strcmp(kw, "currency") == 0) return
        "**`currency`** *(project attribute)*\n\n"
        "Sets the currency symbol displayed in cost/revenue reports.\n\n"
        "**Syntax:** `currency \"<symbol>\"`\n\n"
        "**Example:** `currency \"EUR\"`";

    if (strcmp(kw, "timeformat") == 0) return
        "**`timeformat`** *(project attribute)*\n\n"
        "Sets the `strftime`-style format string used when displaying dates.\n\n"
        "**Syntax:** `timeformat \"<format>\"`\n\n"
        "**Example:** `timeformat \"%Y-%m-%d\"`";

    if (strcmp(kw, "timezone") == 0) return
        "**`timezone`** *(project attribute)*\n\n"
        "Sets the timezone for all date calculations. Uses IANA tz database names.\n\n"
        "**Syntax:** `timezone \"<tz>\"`\n\n"
        "**Example:** `timezone \"America/New_York\"`";

    if (strcmp(kw, "workinghours") == 0) return
        "**`workinghours`** *(project / resource / shift attribute)*\n\n"
        "Defines which hours of the day and days of the week are working time.\n\n"
        "**Syntax:** `workinghours <days> <HH:MM> - <HH:MM> [, \xe2\x80\xa6]`\n\n"
        "**Days:** `mon`, `tue`, `wed`, `thu`, `fri`, `sat`, `sun`, ranges like `mon-fri`, or `off`\n\n"
        "**Example:** `workinghours mon-fri 9:00 - 17:00`";

    if (strcmp(kw, "timingresolution") == 0) return
        "**`timingresolution`** *(project attribute)*\n\n"
        "Sets the scheduling granularity in minutes (default 60). Smaller values "
        "give finer-grained schedules at the cost of longer computation.\n\n"
        "**Syntax:** `timingresolution <minutes>`";

    if (strcmp(kw, "scenario") == 0) return
        "**`scenario`** *(project attribute)*\n\n"
        "Defines a scheduling scenario for what-if analysis. The default scenario "
        "is always `plan`.\n\n"
        "**Syntax:** `scenario <id> \"<name>\" { \xe2\x80\xa6 }`";

    return NULL;
}
