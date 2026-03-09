/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

static int pos_in(LspPos p, LspPos start, LspPos end) {
    int after  = (p.line > start.line)
              || (p.line == start.line && p.character >= start.character);
    int before = (p.line < end.line)
              || (p.line == end.line && p.character <= end.character);
    return after && before;
}

Token token_at(const char *src, LspPos pos) {
    Lexer l;
    lexer_init(&l, src);
    for (;;) {
        Token t = lexer_next(&l);
        if (t.kind == TK_EOF) return t;

        /* Stop once we've passed the requested position */
        if (t.start.line > pos.line
         || (t.start.line == pos.line && t.start.character > pos.character)) {
            token_free(&t);
            return (Token){ TK_EOF, pos, pos, strdup("") };
        }

        if (pos_in(pos, t.start, t.end)) return t;
        token_free(&t);
    }
}

/* ── keyword_docs ────────────────────────────────────────────────────────── */

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
