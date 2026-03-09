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

#include "signature.h"

#include <stdlib.h>
#include <string.h>

/* ── Keyword classification ──────────────────────────────────────────────── */

static int is_any_known_keyword(const char *s) {
    static const char *kws[] = {
        "project","task","resource","account","shift","macro","include","flags",
        "supplement","scenario","effort","duration","length","milestone","depends",
        "precedes","allocate","start","end","maxstart","maxend","minstart","minend",
        "priority","complete","note","responsible","booking","scheduled","rate",
        "efficiency","vacation","leaves","now","currency","timeformat","timezone",
        "workinghours","timingresolution","extend","journalentry","balance","limits",
        "overtime","statusnote","dailyworkinghours","weeklyworkinghours","outputdir",
        "purge", NULL
    };
    for (int i = 0; kws[i]; i++)
        if (strcmp(s, kws[i]) == 0) return 1;
    return 0;
}

/* ── active_context ──────────────────────────────────────────────────────── */

/* Stack entry: (keyword, brace_depth, arg_count) */
typedef struct { char *kw; uint32_t depth; uint32_t argc; } StackEntry;

ActiveContext active_context(const char *src, LspPos cursor) {
    Lexer l;
    lexer_init(&l, src);

    uint32_t brace_depth = 0;
    StackEntry stack[128];
    int stack_n = 0;

    for (;;) {
        Token tok = lexer_next(&l);
        if (tok.kind == TK_EOF) { token_free(&tok); break; }

        /* Stop once token starts past cursor */
        if (tok.start.line > cursor.line
         || (tok.start.line == cursor.line && tok.start.character > cursor.character)) {
            token_free(&tok);
            break;
        }

        switch (tok.kind) {
        case TK_LINE_COMMENT:
        case TK_BLOCK_COMMENT:
            break;

        case TK_LBRACE:
            brace_depth++;
            break;

        case TK_RBRACE:
            if (brace_depth > 0) brace_depth--;
            /* Pop stack entries at deeper or equal depth */
            while (stack_n > 0 && stack[stack_n - 1].depth > brace_depth) {
                free(stack[--stack_n].kw);
            }
            break;

        case TK_IDENT:
            if (is_any_known_keyword(tok.text)) {
                /* Pop entries at same or deeper depth */
                while (stack_n > 0 && stack[stack_n - 1].depth >= brace_depth) {
                    free(stack[--stack_n].kw);
                }
                /* Push if it has a signature */
                if (build_signature_help_json(tok.text, 0) != NULL) {
                    cJSON *tmp = build_signature_help_json(tok.text, 0);
                    cJSON_Delete(tmp);
                    /* Actually just check via the raw approach */
                    if (stack_n < 128) {
                        stack[stack_n++] = (StackEntry){ strdup(tok.text), brace_depth, 0 };
                    }
                }
            }
            break;

        default: {
            /* Count as completed argument only if token fully before cursor */
            int fully_before = (tok.end.line < cursor.line)
                || (tok.end.line == cursor.line && tok.end.character <= cursor.character);
            if (fully_before) {
                /* Find innermost stack entry at current brace depth */
                for (int i = stack_n - 1; i >= 0; i--) {
                    if (stack[i].depth == brace_depth) {
                        stack[i].argc++;
                        break;
                    }
                }
            }
            break;
        }
        }

        token_free(&tok);
    }

    /* Find innermost entry at current brace_depth */
    for (int i = stack_n - 1; i >= 0; i--) {
        if (stack[i].depth == brace_depth) {
            ActiveContext ac;
            ac.keyword   = stack[i].kw;
            ac.arg_count = stack[i].argc;
            /* Free others */
            for (int j = 0; j < stack_n; j++)
                if (j != i) free(stack[j].kw);
            return ac;
        }
    }

    /* Nothing found */
    for (int i = 0; i < stack_n; i++) free(stack[i].kw);
    return (ActiveContext){ NULL, 0 };
}

/* ── build_signature_help_json ───────────────────────────────────────────── */

typedef struct {
    const char  *label;
    const char **params; /* NULL-terminated */
    const char  *doc;
} SigDef;

static cJSON *make_sig_json(const SigDef *def, uint32_t active_param) {
    /* Count params */
    int nparams = 0;
    if (def->params) while (def->params[nparams]) nparams++;

    uint32_t clamped = (nparams == 0) ? 0
                     : (active_param < (uint32_t)nparams ? active_param : (uint32_t)(nparams - 1));

    cJSON *param_arr = cJSON_CreateArray();
    for (int i = 0; i < nparams; i++) {
        cJSON *pi = cJSON_CreateObject();
        cJSON_AddStringToObject(pi, "label", def->params[i]);
        cJSON_AddItemToArray(param_arr, pi);
    }

    cJSON *sig = cJSON_CreateObject();
    cJSON_AddStringToObject(sig, "label", def->label);
    cJSON *doc_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(doc_obj, "kind", "markdown");
    cJSON_AddStringToObject(doc_obj, "value", def->doc);
    cJSON_AddItemToObject(sig, "documentation", doc_obj);
    cJSON_AddItemToObject(sig, "parameters", param_arr);

    cJSON *root = cJSON_CreateObject();
    cJSON *sigs = cJSON_CreateArray();
    cJSON_AddItemToArray(sigs, sig);
    cJSON_AddItemToObject(root, "signatures", sigs);
    cJSON_AddNumberToObject(root, "activeSignature", 0);
    if (nparams > 0)
        cJSON_AddNumberToObject(root, "activeParameter", (double)clamped);
    else
        cJSON_AddNullToObject(root, "activeParameter");

    return root;
}

cJSON *build_signature_help_json(const char *kw, uint32_t active_param) {
    if (!kw) return NULL;

/* SIG0: keyword with no parameters */
#define SIG0(lbl, doc) \
    do { \
        SigDef d = { lbl, NULL, doc }; \
        return make_sig_json(&d, active_param); \
    } while(0)

/* SIG: keyword with one or more parameters */
#define SIG(lbl, doc, ...) \
    do { \
        static const char *params[] = { __VA_ARGS__, NULL }; \
        SigDef d = { lbl, params, doc }; \
        return make_sig_json(&d, active_param); \
    } while(0)

    if (strcmp(kw, "project") == 0) SIG(
        "project [<id>] <name> [<version>] <start> <end|+duration>",
        "Project header. Required exactly once per file.",
        "[<id>]", "<name>", "[<version>]", "<start>", "<end|+duration>");

    if (strcmp(kw, "task") == 0) SIG(
        "task [<id>] <name>",
        "Work package, summary task, or milestone.",
        "[<id>]", "<name>");

    if (strcmp(kw, "resource") == 0) SIG(
        "resource [<id>] <name>",
        "Resource (person, team, or equipment).",
        "[<id>]", "<name>");

    if (strcmp(kw, "account") == 0) SIG(
        "account [<id>] <name>",
        "Cost/revenue account.",
        "[<id>]", "<name>");

    if (strcmp(kw, "shift") == 0) SIG(
        "shift [<id>] <name>",
        "Named working-hours shift.",
        "[<id>]", "<name>");

    if (strcmp(kw, "macro") == 0) SIG(
        "macro <ID> <MACRO>",
        "Reusable text macro. Body is enclosed in `[ ]`. Use `${1}`, `${2}`, ... for arguments.",
        "<ID>", "<MACRO>");

    if (strcmp(kw, "include") == 0) SIG(
        "include <filename> [{ <attributes> }]",
        "Include another TaskJuggler file.",
        "<filename>");

    if (strcmp(kw, "flags") == 0) SIG(
        "flags <ID> [, <ID>...]",
        "Declare flag identifiers.",
        "<ID> [, <ID>...]");

    if (strcmp(kw, "supplement") == 0) SIG(
        "supplement (account|resource|task) <id> [{ <attributes> }]",
        "Add attributes to a previously-defined account, resource, or task.",
        "account|resource|task", "<id>");

    if (strcmp(kw, "scenario") == 0) SIG(
        "scenario <id> <name> [{ <attributes> }]",
        "Alternative scheduling scenario.",
        "<id>", "<name>");

    if (strcmp(kw, "effort") == 0) SIG(
        "effort <value> (min|h|d|w|m|y)",
        "Work required (e.g. `effort 5 d`, `effort 8 h`). Distributed across allocated resources.",
        "<value>", "min|h|d|w|m|y");

    if (strcmp(kw, "duration") == 0) SIG(
        "duration <value> (min|h|d|w|m|y)",
        "Elapsed (wall-clock) duration, ignoring resource availability.",
        "<value>", "min|h|d|w|m|y");

    if (strcmp(kw, "length") == 0) SIG(
        "length <value> (min|h|d|w|m|y)",
        "Duration in working time.",
        "<value>", "min|h|d|w|m|y");

    if (strcmp(kw, "milestone") == 0) SIG0(
        "milestone", "Mark as a zero-duration milestone.");

    if (strcmp(kw, "scheduled") == 0) SIG0(
        "scheduled", "Task has fixed, pre-computed dates. Requires manual bookings.");

    if (strcmp(kw, "depends") == 0) SIG(
        "depends <id> [{ <attributes> }] [, <id> [{ <attributes> }]...]",
        "Finish-to-start dependency. Use `!` prefix for relative paths, `!!` to go up a level.",
        "<id> [, <id>...]");

    if (strcmp(kw, "precedes") == 0) SIG(
        "precedes <id> [{ <attributes> }] [, <id> [{ <attributes> }]...]",
        "Reverse dependency: listed tasks cannot start before this one finishes.",
        "<id> [, <id>...]");

    if (strcmp(kw, "allocate") == 0) SIG(
        "allocate <resource> [{ <attributes> }] [, <resource> [{ <attributes> }]...]",
        "Assign resources to this task.",
        "<resource> [, <resource>...]");

    if (strcmp(kw, "start") == 0) SIG(
        "start <date>",
        "Hard constraint on start date (`YYYY-MM-DD[-HH:MM]`). Task will not start earlier.",
        "<date>");

    if (strcmp(kw, "end") == 0) SIG(
        "end <date>",
        "Hard constraint on end date (`YYYY-MM-DD[-HH:MM]`). Task will not end later.",
        "<date>");

    if (strcmp(kw, "maxstart") == 0) SIG(
        "maxstart <date>",
        "Maximum desired start date. Reported as error if violated after scheduling.",
        "<date>");

    if (strcmp(kw, "minstart") == 0) SIG(
        "minstart <date>",
        "Minimum desired start date. Reported as error if violated after scheduling.",
        "<date>");

    if (strcmp(kw, "maxend") == 0) SIG(
        "maxend <date>",
        "Maximum desired end date. Reported as error if violated after scheduling.",
        "<date>");

    if (strcmp(kw, "minend") == 0) SIG(
        "minend <date>",
        "Minimum desired end date. Reported as error if violated after scheduling.",
        "<date>");

    if (strcmp(kw, "priority") == 0) SIG(
        "priority <value>",
        "Scheduling priority 1-1000 (default 500). Higher value wins contested resources.",
        "<value>");

    if (strcmp(kw, "complete") == 0) SIG(
        "complete <percent>",
        "Completion percentage (0-100) for progress tracking.",
        "<percent>");

    if (strcmp(kw, "note") == 0) SIG(
        "note <STRING>",
        "Attach a note or description to the task.",
        "<STRING>");

    if (strcmp(kw, "responsible") == 0) SIG(
        "responsible <resource> [, <resource>...]",
        "Resource(s) accountable for this task (informational only, not used in scheduling).",
        "<resource> [, <resource>...]");

    if (strcmp(kw, "booking") == 0) SIG(
        "booking <resource> <interval> [, <interval>...] [{ <attributes> }]",
        "Record actual work already performed on this task.",
        "<resource>", "<interval> [, <interval>...]");

    if (strcmp(kw, "rate") == 0) SIG(
        "rate (<INTEGER>|<FLOAT>)",
        "Daily cost rate in the project currency.",
        "<INTEGER>|<FLOAT>");

    if (strcmp(kw, "efficiency") == 0) SIG(
        "efficiency (<INTEGER>|<FLOAT>)",
        "Work output multiplier (default 1.0; >1.0 above average, <1.0 reduced).",
        "<INTEGER>|<FLOAT>");

    if (strcmp(kw, "vacation") == 0) SIG(
        "vacation <name> <interval> [, <interval>...]",
        "Define a vacation / unavailability period.",
        "<name>", "<interval> [, <interval>...]");

    if (strcmp(kw, "leaves") == 0) SIG(
        "leaves (project|annual|special|sick|unpaid|holiday|unemployed) [<name>] <interval> [, ...]",
        "Leave block by type. Name is optional.",
        "project|annual|special|sick|unpaid|holiday|unemployed", "[<name>]", "<interval>");

    if (strcmp(kw, "now") == 0) SIG(
        "now <date>",
        "Reference date for reports and progress calculations (default: system clock).",
        "<date>");

    if (strcmp(kw, "currency") == 0) SIG(
        "currency <symbol>",
        "Currency unit for all monetary values (e.g. `\"USD\"`, `\"EUR\"`).",
        "<symbol>");

    if (strcmp(kw, "timeformat") == 0) SIG(
        "timeformat <format>",
        "Date/time format string using `%` specifiers (e.g. `\"%Y-%m-%d\"`).",
        "<format>");

    if (strcmp(kw, "timezone") == 0) SIG(
        "timezone <zone>",
        "IANA timezone for all date calculations (e.g. `\"Europe/Berlin\"`, `\"America/New_York\"`).",
        "<zone>");

    if (strcmp(kw, "workinghours") == 0) SIG(
        "workinghours <weekday> [- <weekday>] [, ...] (off | <HH:MM> - <HH:MM> [, <HH:MM> - <HH:MM>...])",
        "Working hours. Days: `mon`, `tue`, `wed`, `thu`, `fri`, `sat`, `sun`, ranges like `mon-fri`, or `off`.",
        "<weekday> [- <weekday>]", "off | <HH:MM> - <HH:MM>");

    if (strcmp(kw, "timingresolution") == 0) SIG(
        "timingresolution <INTEGER> min",
        "Scheduling granularity in minutes (valid: 5-60, default 60). The `min` unit suffix is required.",
        "<INTEGER>");

#undef SIG

    return NULL;
}
