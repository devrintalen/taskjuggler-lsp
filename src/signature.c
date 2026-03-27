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

#include "signature.h"
#include "hover.h"

#include <stdlib.h>
#include <string.h>

/* ── has_signature_help ──────────────────────────────────────────────────── */

/* Sorted list of all keywords that have a signature-help entry.
 * Must be kept in sync with build_signature_help_json(). */
static const char * const sig_keywords[] = {
    "account", "allocate", "booking", "complete", "currency",
    "depends", "duration", "efficiency", "effort", "end",
    "flags", "include", "leaves", "length", "macro",
    "maxend", "maxstart", "milestone", "minend", "minstart",
    "note", "now", "precedes", "priority", "project",
    "rate", "resource", "responsible", "scenario", "scheduled",
    "shift", "start", "supplement", "task", "timeformat",
    "timingresolution", "timezone", "vacation", "workinghours",
};
static const int num_sig_keywords =
    (int)(sizeof(sig_keywords) / sizeof(sig_keywords[0]));

/* Returns 1 if kw has a signature help entry (binary search in sig_keywords). */
int has_signature_help(const char *kw) {
    if (!kw) return 0;
    int lo = 0, hi = num_sig_keywords - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(kw, sig_keywords[mid]);
        if (cmp == 0) return 1;
        if (cmp < 0) hi = mid - 1;
        else         lo = mid + 1;
    }
    return 0;
}

/* ── active_context ──────────────────────────────────────────────────────── */

/* Determine the active keyword context and argument index at cursor.
 * Returns the innermost keyword that has signature help and whose brace depth
 * matches the cursor's, along with the number of arguments before the cursor.
 * The returned keyword string is heap-allocated; caller must free it.
 * Returns {NULL, 0} if no relevant keyword context is found.
 *
 * tokens     — token span array from the ParseResult
 * num_tokens — number of entries in tokens
 * cursor     — cursor position from the textDocument/signatureHelp request
 */
ActiveContext active_context(const TokenSpan *tokens, int num_tokens, LspPos cursor) {
    KwStackEntry stack[128];
    uint32_t brace_depth;
    int stack_n = scan_kw_stack(tokens, num_tokens, cursor,
                                has_signature_help, 1, stack, 128, &brace_depth);

    for (int i = stack_n - 1; i >= 0; i--) {
        if (stack[i].depth == brace_depth) {
            ActiveContext ac;
            ac.keyword   = stack[i].kw;
            ac.arg_count = stack[i].argc;
            for (int j = 0; j < stack_n; j++)
                if (j != i) free(stack[j].kw);
            return ac;
        }
    }

    for (int i = 0; i < stack_n; i++) free(stack[i].kw);
    return (ActiveContext){ NULL, 0 };
}

/* ── build_signature_help_json ───────────────────────────────────────────── */

typedef struct {
    const char  *label;
    const char **params; /* NULL-terminated */
    const char  *doc;
} SigDef;

/* Build a SignatureHelp JSON response object from a SigDef.
 * Wraps the signature in a {"signatures": [...], "activeSignature": 0} envelope.
 * active_param is clamped to the last parameter if it exceeds the param count.
 *
 * doc          — the mutable JSON document that will own the returned value
 * def          — signature definition (label, parameter list, documentation)
 * active_param — zero-based index of the argument currently being typed
 */
static yyjson_mut_val *make_sig_json(yyjson_mut_doc *doc, const SigDef *def,
                                      uint32_t active_param) {
    /* Count parameters and clamp active_param to a valid index */
    int nparams = 0;
    if (def->params) while (def->params[nparams]) nparams++;

    uint32_t clamped = (nparams == 0) ? 0
                     : (active_param < (uint32_t)nparams ? active_param : (uint32_t)(nparams - 1));

    /* Build the ParameterInformation array */
    yyjson_mut_val *param_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < nparams; i++) {
        yyjson_mut_val *pi = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, pi, "label", def->params[i]);
        yyjson_mut_arr_add_val(param_arr, pi);
    }

    yyjson_mut_val *sig = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, sig, "label", def->label);
    yyjson_mut_val *doc_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, doc_obj, "kind",  "markdown");
    yyjson_mut_obj_add_str(doc, doc_obj, "value", def->doc);
    yyjson_mut_obj_add_val(doc, sig, "documentation", doc_obj);
    yyjson_mut_obj_add_val(doc, sig, "parameters", param_arr);
    /* LSP 3.16: activeParameter lives inside SignatureInformation */
    if (nparams > 0)
        yyjson_mut_obj_add_uint(doc, sig, "activeParameter", clamped);
    else
        yyjson_mut_obj_add_null(doc, sig, "activeParameter");

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *sigs = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_val(sigs, sig);
    yyjson_mut_obj_add_val(doc, root, "signatures", sigs);
    yyjson_mut_obj_add_uint(doc, root, "activeSignature", 0);

    return root;
}

/* Build a SignatureHelp JSON response for the given keyword and argument index.
 * Returns NULL if kw has no signature entry (server will return null to editor).
 *
 * doc          — the mutable JSON document that will own the returned value
 * kw           — the active keyword (e.g. "task", "effort")
 * active_param — zero-based index of the argument currently being typed
 */
yyjson_mut_val *build_signature_help_json(yyjson_mut_doc *doc, const char *kw,
                                           uint32_t active_param) {
    if (!kw) return NULL;

/* SIG0: keyword with no parameters */
#define SIG0(lbl, sigdoc) \
    do { \
        SigDef d = { lbl, NULL, sigdoc }; \
        return make_sig_json(doc, &d, active_param); \
    } while(0)

/* SIG: keyword with one or more parameters.
 *
 * Coverage: 39 of ~200 TaskJuggler keywords have signature help entries.
 *
 * Covered (39):
 *   Declarations:  project, task, resource, account, shift, macro, include,
 *                  flags, supplement, scenario
 *   Task attrs:    effort, duration, length, milestone, scheduled, depends,
 *                  precedes, allocate, start, end, maxstart, minstart,
 *                  maxend, minend, priority, complete, note, responsible,
 *                  booking
 *   Resource attrs: rate, efficiency, vacation, leaves
 *   Project attrs: now, currency, timeformat, timezone, workinghours,
 *                  timingresolution
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
#define SIG(lbl, sigdoc, ...) \
    do { \
        static const char *params[] = { __VA_ARGS__, NULL }; \
        SigDef d = { lbl, params, sigdoc }; \
        return make_sig_json(doc, &d, active_param); \
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
