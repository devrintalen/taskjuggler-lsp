use crate::parser::{Lexer, TokenKind};
use tower_lsp::lsp_types::{
    Documentation, MarkupContent, MarkupKind, ParameterInformation, ParameterLabel, Position,
    SignatureHelp, SignatureInformation,
};

// ─── Position helpers ─────────────────────────────────────────────────────────

fn pos_lt(a: Position, b: Position) -> bool {
    a.line < b.line || (a.line == b.line && a.character < b.character)
}

fn pos_leq(a: Position, b: Position) -> bool {
    a.line < b.line || (a.line == b.line && a.character <= b.character)
}

// ─── Keyword classification ───────────────────────────────────────────────────

/// Returns true for any TJP keyword that begins a new statement, used to
/// prevent argument tokens from being attributed to the wrong keyword.
fn is_any_known_keyword(s: &str) -> bool {
    matches!(
        s,
        "project"
            | "task"
            | "resource"
            | "account"
            | "shift"
            | "macro"
            | "include"
            | "flags"
            | "supplement"
            | "scenario"
            | "effort"
            | "duration"
            | "length"
            | "milestone"
            | "depends"
            | "precedes"
            | "allocate"
            | "start"
            | "end"
            | "maxstart"
            | "maxend"
            | "minstart"
            | "minend"
            | "priority"
            | "complete"
            | "note"
            | "responsible"
            | "booking"
            | "scheduled"
            | "rate"
            | "efficiency"
            | "vacation"
            | "leaves"
            | "now"
            | "currency"
            | "timeformat"
            | "timezone"
            | "workinghours"
            | "timingresolution"
            | "extend"
            | "journalentry"
            | "balance"
            | "limits"
            | "overtime"
            | "statusnote"
            | "dailyworkinghours"
            | "weeklyworkinghours"
            | "outputdir"
            | "purge"
    )
}

// ─── Context scanner ──────────────────────────────────────────────────────────

/// Scan tokens up to `cursor` and return `(keyword, arg_count)` for the
/// innermost active statement, where `arg_count` is the number of argument
/// tokens whose end position is ≤ cursor (i.e. fully typed before the cursor).
pub fn active_context(src: &str, cursor: Position) -> Option<(String, u32)> {
    let mut lexer = Lexer::new(src);
    let mut brace_depth: u32 = 0;
    // (keyword, depth_when_pushed, arg_count)
    let mut stack: Vec<(String, u32, u32)> = Vec::new();

    loop {
        let tok = lexer.next_token();
        if tok.kind == TokenKind::Eof {
            break;
        }
        // Once the token starts past the cursor we have everything we need.
        if pos_lt(cursor, tok.start) {
            break;
        }

        match &tok.kind {
            TokenKind::LineComment | TokenKind::BlockComment => {}

            TokenKind::LBrace => {
                brace_depth += 1;
            }

            TokenKind::RBrace => {
                if brace_depth > 0 {
                    brace_depth -= 1;
                }
                stack.retain(|(_, d, _)| *d <= brace_depth);
            }

            TokenKind::Ident if is_any_known_keyword(&tok.text) => {
                // New statement: pop entries at the same or deeper depth,
                // then push this keyword if it has a signature.
                stack.retain(|(_, d, _)| *d < brace_depth);
                if has_signature(&tok.text) {
                    stack.push((tok.text.clone(), brace_depth, 0));
                }
            }

            _ => {
                // Count as a completed argument only when the token has fully
                // passed the cursor (so the "currently typed" token counts as
                // the argument being entered, not the next one).
                if pos_leq(tok.end, cursor) {
                    if let Some(entry) = stack
                        .iter_mut()
                        .rev()
                        .find(|(_, d, _)| *d == brace_depth)
                    {
                        entry.2 += 1;
                    }
                }
            }
        }
    }

    stack
        .into_iter()
        .rev()
        .find(|(_, d, _)| *d == brace_depth)
        .map(|(kw, _, arg_count)| (kw, arg_count))
}

// ─── Signature definitions ────────────────────────────────────────────────────

fn has_signature(kw: &str) -> bool {
    build_signature_help(kw, 0).is_some()
}

pub fn build_signature_help(kw: &str, active_param: u32) -> Option<SignatureHelp> {
    let (label, params, docs): (&str, &[&str], &str) = match kw {
        // ── Declarations ────────────────────────────────────────────────────
        "project" => (
            "project [<id>] <name> [<version>] <start> <end|+duration>",
            &["[<id>]", "<name>", "[<version>]", "<start>", "<end|+duration>"],
            "Project header. Required exactly once per file.",
        ),
        "task" => (
            "task [<id>] <name>",
            &["[<id>]", "<name>"],
            "Work package, summary task, or milestone.",
        ),
        "resource" => (
            "resource [<id>] <name>",
            &["[<id>]", "<name>"],
            "Resource (person, team, or equipment).",
        ),
        "account" => (
            "account [<id>] <name>",
            &["[<id>]", "<name>"],
            "Cost/revenue account.",
        ),
        "shift" => (
            "shift [<id>] <name>",
            &["[<id>]", "<name>"],
            "Named working-hours shift.",
        ),
        "macro" => (
            "macro <ID> <MACRO>",
            &["<ID>", "<MACRO>"],
            "Reusable text macro. Body is enclosed in `[ ]`. Use `${1}`, `${2}`, … for arguments.",
        ),
        "include" => (
            "include <filename> [{ <attributes> }]",
            &["<filename>"],
            "Include another TaskJuggler file.",
        ),
        "flags" => (
            "flags <ID> [, <ID>...]",
            &["<ID> [, <ID>...]"],
            "Declare flag identifiers.",
        ),
        "supplement" => (
            "supplement (account|resource|task) <id> [{ <attributes> }]",
            &["account|resource|task", "<id>"],
            "Add attributes to a previously-defined account, resource, or task.",
        ),
        "scenario" => (
            "scenario <id> <name> [{ <attributes> }]",
            &["<id>", "<name>"],
            "Alternative scheduling scenario.",
        ),

        // ── Task attributes ──────────────────────────────────────────────────
        "effort" => (
            "effort <value> (min|h|d|w|m|y)",
            &["<value>", "min|h|d|w|m|y"],
            "Work required (e.g. `effort 5 d`, `effort 8 h`). Distributed across allocated resources.",
        ),
        "duration" => (
            "duration <value> (min|h|d|w|m|y)",
            &["<value>", "min|h|d|w|m|y"],
            "Elapsed (wall-clock) duration, ignoring resource availability.",
        ),
        "length" => (
            "length <value> (min|h|d|w|m|y)",
            &["<value>", "min|h|d|w|m|y"],
            "Duration in working time.",
        ),
        "milestone" => ("milestone", &[], "Mark as a zero-duration milestone."),
        "scheduled" => ("scheduled", &[], "Task has fixed, pre-computed dates. Requires manual bookings."),
        "depends" => (
            "depends <id> [{ <attributes> }] [, <id> [{ <attributes> }]...]",
            &["<id> [, <id>...]"],
            "Finish-to-start dependency. Use `!` prefix for absolute paths, `!!` to go up a level.",
        ),
        "precedes" => (
            "precedes <id> [{ <attributes> }] [, <id> [{ <attributes> }]...]",
            &["<id> [, <id>...]"],
            "Reverse dependency: listed tasks cannot start before this one finishes.",
        ),
        "allocate" => (
            "allocate <resource> [{ <attributes> }] [, <resource> [{ <attributes> }]...]",
            &["<resource> [, <resource>...]"],
            "Assign resources to this task.",
        ),
        "start" => (
            "start <date>",
            &["<date>"],
            "Hard constraint on start date (`YYYY-MM-DD[-HH:MM]`). Task will not start earlier.",
        ),
        "end" => (
            "end <date>",
            &["<date>"],
            "Hard constraint on end date (`YYYY-MM-DD[-HH:MM]`). Task will not end later.",
        ),
        "maxstart" => (
            "maxstart <date>",
            &["<date>"],
            "Maximum desired start date. Reported as error if violated after scheduling.",
        ),
        "minstart" => (
            "minstart <date>",
            &["<date>"],
            "Minimum desired start date. Reported as error if violated after scheduling.",
        ),
        "maxend" => (
            "maxend <date>",
            &["<date>"],
            "Maximum desired end date. Reported as error if violated after scheduling.",
        ),
        "minend" => (
            "minend <date>",
            &["<date>"],
            "Minimum desired end date. Reported as error if violated after scheduling.",
        ),
        "priority" => (
            "priority <value>",
            &["<value>"],
            "Scheduling priority 1–1000 (default 500). Higher value wins contested resources.",
        ),
        "complete" => (
            "complete <percent>",
            &["<percent>"],
            "Completion percentage (0–100) for progress tracking.",
        ),
        "note" => (
            "note <STRING>",
            &["<STRING>"],
            "Attach a note or description to the task.",
        ),
        "responsible" => (
            "responsible <resource> [, <resource>...]",
            &["<resource> [, <resource>...]"],
            "Resource(s) accountable for this task (informational only, not used in scheduling).",
        ),
        "booking" => (
            "booking <resource> <interval> [, <interval>...] [{ <attributes> }]",
            &["<resource>", "<interval> [, <interval>...]"],
            "Record actual work already performed on this task.",
        ),

        // ── Resource attributes ──────────────────────────────────────────────
        "rate" => (
            "rate (<INTEGER>|<FLOAT>)",
            &["<INTEGER>|<FLOAT>"],
            "Daily cost rate in the project currency.",
        ),
        "efficiency" => (
            "efficiency (<INTEGER>|<FLOAT>)",
            &["<INTEGER>|<FLOAT>"],
            "Work output multiplier (default 1.0; >1.0 above average, <1.0 reduced).",
        ),
        "vacation" => (
            "vacation <name> <interval> [, <interval>...]",
            &["<name>", "<interval> [, <interval>...]"],
            "Define a vacation / unavailability period.",
        ),
        "leaves" => (
            "leaves (project|annual|special|sick|unpaid|holiday|unemployed) [<name>] <interval> [, ...]",
            &["project|annual|special|sick|unpaid|holiday|unemployed", "[<name>]", "<interval>"],
            "Leave block by type. Name is optional.",
        ),

        // ── Project attributes ───────────────────────────────────────────────
        "now" => (
            "now <date>",
            &["<date>"],
            "Reference date for reports and progress calculations (default: system clock).",
        ),
        "currency" => (
            "currency <symbol>",
            &["<symbol>"],
            "Currency unit for all monetary values (e.g. `\"USD\"`, `\"EUR\"`).",
        ),
        "timeformat" => (
            "timeformat <format>",
            &["<format>"],
            "Date/time format string using `%` specifiers (e.g. `\"%Y-%m-%d\"`).",
        ),
        "timezone" => (
            "timezone <zone>",
            &["<zone>"],
            "IANA timezone for all date calculations (e.g. `\"Europe/Berlin\"`, `\"America/New_York\"`).",
        ),
        "workinghours" => (
            "workinghours <weekday> [- <weekday>] [, ...] (off | <HH:MM> - <HH:MM> [, <HH:MM> - <HH:MM>...])",
            &["<weekday> [- <weekday>]", "off | <HH:MM> - <HH:MM>"],
            "Working hours. Days: `mon`, `tue`, `wed`, `thu`, `fri`, `sat`, `sun`, ranges like `mon-fri`, or `off`.",
        ),
        "timingresolution" => (
            "timingresolution <INTEGER> min",
            &["<INTEGER>"],
            "Scheduling granularity in minutes (valid: 5–60, default 60). The `min` unit suffix is required.",
        ),

        _ => return None,
    };

    let clamped_param = if params.is_empty() {
        None
    } else {
        Some(active_param.min(params.len() as u32 - 1))
    };

    let param_infos: Vec<ParameterInformation> = params
        .iter()
        .map(|p| ParameterInformation {
            label: ParameterLabel::Simple(p.to_string()),
            documentation: None,
        })
        .collect();

    Some(SignatureHelp {
        signatures: vec![SignatureInformation {
            label: label.to_string(),
            documentation: Some(Documentation::MarkupContent(MarkupContent {
                kind: MarkupKind::Markdown,
                value: docs.to_string(),
            })),
            parameters: Some(param_infos),
            active_parameter: None,
        }],
        active_signature: Some(0),
        active_parameter: clamped_param,
    })
}
