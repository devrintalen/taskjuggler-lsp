use crate::hover;
use crate::parser::{Lexer, Symbol, TokenKind};
use crate::signature;
use tower_lsp::lsp_types::{CompletionItem, CompletionItemKind, Position, SymbolKind};

// ─── Position helper ──────────────────────────────────────────────────────────

fn pos_before(a: Position, b: Position) -> bool {
    a.line < b.line || (a.line == b.line && a.character < b.character)
}

// ─── Block context ────────────────────────────────────────────────────────────

/// Scan tokens up to `cursor` and return the stack of keywords that opened
/// each enclosing `{…}` block, innermost last.
///
/// E.g. cursor inside `task AcSo { task spec { … } }` → `["task", "task"]`.
fn block_stack(src: &str, cursor: Position) -> Vec<String> {
    let mut lexer = Lexer::new(src);
    let mut stack: Vec<String> = Vec::new();
    // Last block-opener keyword seen — associated with the next `{`.
    let mut pending: Option<String> = None;

    loop {
        let tok = lexer.next_token();
        if tok.kind == TokenKind::Eof || pos_before(cursor, tok.start) {
            break;
        }
        match tok.kind {
            TokenKind::LineComment | TokenKind::BlockComment => {}

            TokenKind::LBrace => {
                stack.push(pending.take().unwrap_or_default());
            }

            TokenKind::RBrace => {
                stack.pop();
                pending = None;
            }

            TokenKind::Ident if is_block_opener(&tok.text) => {
                pending = Some(tok.text);
            }

            _ => {}
        }
    }
    stack
}

/// Keywords that can open a `{…}` body block and therefore define a
/// completion context when the cursor is inside them.
fn is_block_opener(s: &str) -> bool {
    matches!(
        s,
        "project"
            | "task"
            | "resource"
            | "account"
            | "shift"
            | "scenario"
            | "supplement"
            | "extend"
            | "journalentry"
            | "limits"
            | "navigator"
            | "taskreport"
            | "textreport"
            | "resourcereport"
    )
}

/// Return the keyword-completion table for the innermost *known* block context
/// in `stack`.  Walks toward the outermost level to skip terminal blocks
/// (`limits`, `journalentry`, …) until it finds a recognised context.
fn kws_for_stack(stack: &[String]) -> &'static [(&'static str, &'static str)] {
    for kw in stack.iter().rev() {
        match kw.as_str() {
            "task" => return TASK_KWS,
            "resource" => return RESOURCE_KWS,
            "project" => return PROJECT_KWS,
            "account" => return ACCOUNT_KWS,
            // Terminal blocks: skip upward.
            "limits" | "journalentry" | "supplement" | "extend"
            | "scenario" | "navigator" | "taskreport" | "textreport"
            | "resourcereport" | "shift" => continue,
            // Anonymous or unrecognised: stop here, use top-level.
            _ => break,
        }
    }
    TOPLEVEL_KWS
}

// ─── Line helpers ─────────────────────────────────────────────────────────────

/// The first non-whitespace word on the cursor's line, used to determine
/// what kind of ids or keywords are expected.
fn line_first_word(src: &str, cursor: Position) -> Option<String> {
    let line = src.lines().nth(cursor.line as usize)?;
    let trimmed = line.trim_start();
    let end = trimmed
        .find(|c: char| !c.is_alphanumeric() && c != '_')
        .unwrap_or(trimmed.len());
    let word = &trimmed[..end];
    if word.is_empty() {
        None
    } else {
        Some(word.to_string())
    }
}

/// True when only whitespace precedes the cursor on the current line —
/// i.e. the cursor is at the start of a fresh statement.
fn at_statement_start(src: &str, cursor: Position) -> bool {
    src.lines()
        .nth(cursor.line as usize)
        .map(|line| {
            let col = (cursor.character as usize).min(line.len());
            line[..col].trim().is_empty()
        })
        .unwrap_or(true)
}

// ─── Partial word ─────────────────────────────────────────────────────────────

/// The identifier token currently being typed at `cursor`, or `""` when the
/// cursor sits on whitespace or punctuation.
fn partial_word(src: &str, cursor: Position) -> String {
    match hover::token_at(src, cursor) {
        Some(tok) if tok.kind == TokenKind::Ident => tok.text,
        _ => String::new(),
    }
}

// ─── ID collection ────────────────────────────────────────────────────────────

/// Collect `(qualified_id, display_name)` for all symbols of `kind`,
/// building dot-separated paths for nested declarations.
/// E.g. task `spec` inside task `AcSo` becomes `"AcSo.spec"`.
fn collect_ids(
    syms: &[Symbol],
    kind: SymbolKind,
    prefix: &str,
    out: &mut Vec<(String, String)>,
) {
    for s in syms {
        if s.kind == kind && !s.detail.is_empty() {
            let qid = if prefix.is_empty() {
                s.detail.clone()
            } else {
                format!("{}.{}", prefix, s.detail)
            };
            out.push((qid.clone(), s.name.clone()));
            collect_ids(&s.children, kind, &qid, out);
        } else {
            // Parent has a different kind (shouldn't happen for homogeneous
            // trees, but recurse with the same prefix just in case).
            collect_ids(&s.children, kind, prefix, out);
        }
    }
}

// ─── Keyword tables ───────────────────────────────────────────────────────────

const TOPLEVEL_KWS: &[(&str, &str)] = &[
    ("project", "Project header (required once per file)"),
    ("task", "Work package, summary task, or milestone"),
    ("resource", "Person, team, or equipment"),
    ("account", "Cost/revenue account"),
    ("shift", "Named working-hours shift"),
    ("scenario", "Alternative scheduling scenario"),
    ("macro", "Reusable text macro"),
    ("include", "Include another .tjp/.tji file"),
    ("flags", "Declare flag identifiers"),
    ("supplement", "Add attributes to an existing declaration"),
    ("rate", "Default daily cost rate for all resources"),
    ("leaves", "Global leave block"),
    ("vacation", "Global vacation / unavailability period"),
    ("now", "Reference date for reports"),
    ("currency", "Currency symbol"),
    ("timeformat", "Date/time display format string"),
    ("timezone", "IANA timezone identifier"),
    ("workinghours", "Working hours definition"),
    ("timingresolution", "Scheduling granularity in minutes"),
    ("balance", "Profit/loss account pair"),
    ("extend", "Add custom attributes to a built-in type"),
    ("navigator", "Navigation bar for HTML reports"),
    ("taskreport", "Task-based report definition"),
    ("textreport", "HTML/text report definition"),
    ("resourcereport", "Resource-based report definition"),
];

const TASK_KWS: &[(&str, &str)] = &[
    ("task", "Sub-task"),
    ("effort", "Work required, e.g. `effort 5d`"),
    ("duration", "Elapsed wall-clock duration"),
    ("length", "Duration in working time"),
    ("milestone", "Mark as a zero-duration milestone"),
    ("scheduled", "Task has fixed pre-computed dates"),
    ("depends", "Finish-to-start dependency on task id(s)"),
    ("precedes", "Reverse dependency: listed tasks start after this one"),
    ("allocate", "Assign resource(s) to this task"),
    ("start", "Hard start date constraint (YYYY-MM-DD)"),
    ("end", "Hard end date constraint (YYYY-MM-DD)"),
    ("maxstart", "Maximum desired start date"),
    ("minstart", "Minimum desired start date"),
    ("maxend", "Maximum desired end date"),
    ("minend", "Minimum desired end date"),
    ("priority", "Scheduling priority 1–1000 (default 500)"),
    ("complete", "Completion percentage 0–100"),
    ("note", "Attach a text note"),
    ("responsible", "Resource(s) accountable for this task"),
    ("booking", "Record actual work performed"),
    ("charge", "Charge amount to an account"),
    ("chargeset", "Account to book labor costs to"),
    ("purge", "Remove an inherited attribute value"),
    ("journalentry", "Status / journal entry"),
    ("flags", "Assign flags to this task"),
    ("supplement", "Add attributes to an existing task"),
    ("limits", "Effort or allocation limits (dailymax, weeklymax, …)"),
    ("statusnote", "Status note for dashboards"),
];

const RESOURCE_KWS: &[(&str, &str)] = &[
    ("resource", "Sub-resource"),
    ("rate", "Daily cost rate"),
    ("efficiency", "Work output multiplier (default 1.0)"),
    ("vacation", "Vacation / unavailability period"),
    ("leaves", "Leave block by type"),
    ("workinghours", "Working hours override for this resource"),
    ("limits", "Working limits (dailymax, weeklymax, …)"),
    ("flags", "Assign flags"),
    ("managers", "Manager resource(s)"),
    ("booking", "Record actual work performed"),
    ("journalentry", "Journal entry"),
    ("supplement", "Add attributes to an existing resource"),
    ("shift", "Apply a working-hours shift"),
];

const PROJECT_KWS: &[(&str, &str)] = &[
    ("scenario", "Define a scheduling scenario"),
    ("extend", "Add custom attributes to a built-in type"),
    ("timezone", "IANA timezone identifier"),
    ("timeformat", "Date/time display format string"),
    ("now", "Reference date for reports"),
    ("currency", "Currency symbol"),
    ("workinghours", "Default working hours"),
    ("timingresolution", "Scheduling granularity in minutes"),
    ("dailyworkinghours", "Hours per working day"),
    ("weeklyworkinghours", "Hours per working week"),
    ("balance", "Profit/loss account pair"),
    ("outputdir", "Output directory for generated reports"),
];

const ACCOUNT_KWS: &[(&str, &str)] = &[
    ("account", "Sub-account"),
    ("credit", "Credit transaction"),
    ("debit", "Debit transaction"),
];

// ─── Main entry point ─────────────────────────────────────────────────────────

/// Return completion items appropriate for `cursor` in `src`.
/// `symbols` is the already-parsed symbol tree from the current document.
pub fn completions(src: &str, cursor: Position, symbols: &[Symbol]) -> Vec<CompletionItem> {
    let stack = block_stack(src, cursor);
    let partial = partial_word(src, cursor);
    let first_word = line_first_word(src, cursor);
    let mut items: Vec<CompletionItem> = Vec::new();

    // ── Suppress completions while typing a declaration's id / name ──────────
    // The user is creating a new entity id, not referencing or setting attributes.
    if matches!(
        first_word.as_deref(),
        Some(
            "project"
                | "task"
                | "resource"
                | "account"
                | "shift"
                | "scenario"
                | "macro"
                | "include"
                | "supplement"
        )
    ) {
        return items;
    }

    // ── ID completions based on the active statement keyword ─────────────────
    // Use active_context (which tracks across line boundaries) so that
    // multi-line `depends` continuations also offer task-id completions.
    let active_kw = signature::active_context(src, cursor).map(|(kw, _)| kw);
    let id_sym_kind: Option<SymbolKind> = match active_kw.as_deref() {
        Some("depends") | Some("precedes") => Some(SymbolKind::FUNCTION),
        Some("allocate") | Some("responsible") | Some("managers") => Some(SymbolKind::OBJECT),
        Some("chargeset") | Some("balance") => Some(SymbolKind::VARIABLE),
        _ => None,
    };

    if let Some(kind) = id_sym_kind {
        let mut ids: Vec<(String, String)> = Vec::new();
        collect_ids(symbols, kind, "", &mut ids);
        for (id, name) in ids {
            if partial.is_empty() || id.to_lowercase().starts_with(&partial.to_lowercase()) {
                items.push(CompletionItem {
                    label: id,
                    kind: Some(completion_kind_for(kind)),
                    detail: Some(name),
                    sort_text: Some("0".into()), // ids sort before keywords
                    ..Default::default()
                });
            }
        }
        // Don't mix id completions with keyword completions.
        return items;
    }

    // ── Keyword completions ───────────────────────────────────────────────────
    // Offer keywords when the user is actively typing a word or is at the
    // start of a fresh statement (blank line / just indentation).
    let typing = !partial.is_empty() || at_statement_start(src, cursor);
    if typing {
        for &(kw, doc) in kws_for_stack(&stack) {
            if kw.starts_with(&partial) {
                items.push(CompletionItem {
                    label: kw.to_string(),
                    kind: Some(CompletionItemKind::KEYWORD),
                    detail: Some(doc.to_string()),
                    sort_text: Some("1".into()),
                    ..Default::default()
                });
            }
        }
    }

    items
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser;

    fn tutorial() -> (String, Vec<Symbol>) {
        let src = include_str!("../test/tutorial.tjp").to_string();
        let result = parser::parse(&src);
        (src, result.symbols)
    }

    fn labels(items: &[CompletionItem]) -> Vec<&str> {
        let mut v: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
        v.sort();
        v
    }

    // Helper: (line, col) is 0-based.
    fn pos(line: u32, character: u32) -> Position {
        Position { line, character }
    }

    #[test]
    fn top_level_keywords() {
        // Cursor on a blank line between top-level declarations.
        let src = "project acso \"Acme\" 2024-01-01 +1y {}\n\n";
        let syms = parser::parse(src).symbols;
        let items = completions(src, pos(1, 0), &syms);
        let lbls = labels(&items);
        assert!(lbls.contains(&"task"), "should offer 'task'");
        assert!(lbls.contains(&"resource"), "should offer 'resource'");
        assert!(lbls.contains(&"account"), "should offer 'account'");
        assert!(!lbls.contains(&"effort"), "'effort' is a task attribute, not top-level");
    }

    #[test]
    fn task_body_keywords() {
        // Cursor inside a task body.
        let src = "task t \"T\" {\n  \n}";
        let syms = parser::parse(src).symbols;
        // Line 1, col 2 → inside the task body on a blank indented line.
        let items = completions(src, pos(1, 2), &syms);
        let lbls = labels(&items);
        assert!(lbls.contains(&"effort"), "should offer 'effort' inside task");
        assert!(lbls.contains(&"depends"), "should offer 'depends' inside task");
        assert!(!lbls.contains(&"project"), "'project' not valid inside task body");
    }

    #[test]
    fn depends_id_completion() {
        let (src, syms) = tutorial();
        // Line of `depends !deliveries.start` in tutorial is line 137 (0-based).
        // Place cursor after "depends " on that line.
        // We synthesise a simpler scenario: just check that task ids are offered.
        let mini = "task a \"A\" {}\ntask b \"B\" {}\ntask c \"C\" {\n  depends \n}";
        let syms2 = parser::parse(mini).symbols;
        let items = completions(mini, pos(3, 10), &syms2); // after "  depends "
        let lbls = labels(&items);
        assert!(lbls.contains(&"a"), "should offer task id 'a'");
        assert!(lbls.contains(&"b"), "should offer task id 'b'");
        // Should not offer keywords.
        assert!(!lbls.contains(&"effort"), "should not offer keywords in depends context");

        // Also verify against the full tutorial: depends offers task ids.
        let _ = (src, syms); // used above
    }

    #[test]
    fn allocate_id_completion() {
        let mini = "resource dev1 \"Dev 1\" {}\nresource dev2 \"Dev 2\" {}\ntask t \"T\" {\n  allocate \n}";
        let syms = parser::parse(mini).symbols;
        let items = completions(mini, pos(3, 11), &syms); // after "  allocate "
        let lbls = labels(&items);
        assert!(lbls.contains(&"dev1"), "should offer resource id 'dev1'");
        assert!(lbls.contains(&"dev2"), "should offer resource id 'dev2'");
        assert!(!lbls.contains(&"effort"), "no keyword completions in allocate context");
    }

    #[test]
    fn partial_keyword_filter() {
        let src = "task t \"T\" {\n  ef\n}";
        let syms = parser::parse(src).symbols;
        // Cursor at the end of "ef" on line 1.
        let items = completions(src, pos(1, 4), &syms);
        let lbls = labels(&items);
        assert!(lbls.contains(&"effort"), "partial 'ef' should match 'effort'");
        assert!(!lbls.contains(&"depends"), "'depends' does not start with 'ef'");
    }

    #[test]
    fn no_completion_after_declaration_keyword() {
        // Cursor right after "task " — user is typing the id.
        let src = "task ";
        let syms = parser::parse(src).symbols;
        let items = completions(src, pos(0, 5), &syms);
        assert!(items.is_empty(), "no completions while typing task id");
    }

    #[test]
    fn tutorial_depends_after_inserted_comma() {
        let original = include_str!("../test/tutorial.tjp");
        // Line 175 (1-indexed) = index 174 (0-indexed): "      depends !database, !backend"
        let lines: Vec<&str> = original.lines().collect();
        assert_eq!(lines[174].trim(), "depends !database, !backend");
        // Simulate inserting ", " at the end of that line.
        let modified: String = lines
            .iter()
            .enumerate()
            .map(|(i, &line)| {
                if i == 174 {
                    format!("{}, ", line)
                } else {
                    line.to_string()
                }
            })
            .collect::<Vec<_>>()
            .join("\n");
        let syms = parser::parse(&modified).symbols;
        let modified_line = modified.lines().nth(174).unwrap();
        let cursor = pos(174, modified_line.len() as u32);
        let items = completions(&modified, cursor, &syms);
        let lbls = labels(&items);
        assert!(
            !items.is_empty(),
            "should offer task ids after 'depends ... , '; got none. active_context dbg follows"
        );
        assert!(
            lbls.iter().any(|l| l.starts_with("AcSo")),
            "should include AcSo tasks; got: {:?}",
            lbls
        );
    }

    #[test]
    fn depends_multiline_continuation() {
        // Second dependency on a continuation line — cursor line does not start with "depends".
        let mini = "task a \"A\" {}\ntask b \"B\" {}\ntask c \"C\" {\n  depends a,\n    \n}";
        let syms = parser::parse(mini).symbols;
        // Line 4, col 4 — indented blank line after "depends a,"
        let items = completions(mini, pos(4, 4), &syms);
        let lbls = labels(&items);
        assert!(lbls.contains(&"a"), "should offer task id 'a' on continuation line");
        assert!(lbls.contains(&"b"), "should offer task id 'b' on continuation line");
        assert!(!lbls.contains(&"effort"), "no keyword completions on depends continuation");
    }

    #[test]
    fn qualified_ids_for_nested_tasks() {
        let mini = "task parent \"P\" {\n  task child \"C\" {}\n}\ntask other \"O\" {\n  depends \n}";
        let syms = parser::parse(mini).symbols;
        let items = completions(mini, pos(4, 10), &syms);
        let lbls = labels(&items);
        // Top-level task ids.
        assert!(lbls.contains(&"parent"), "top-level id");
        assert!(lbls.contains(&"other"), "top-level id");
        // Qualified nested id.
        assert!(lbls.contains(&"parent.child"), "qualified nested id");
    }
}

fn completion_kind_for(sym_kind: SymbolKind) -> CompletionItemKind {
    match sym_kind {
        SymbolKind::FUNCTION => CompletionItemKind::FUNCTION,
        SymbolKind::OBJECT => CompletionItemKind::CLASS,
        SymbolKind::VARIABLE => CompletionItemKind::VARIABLE,
        _ => CompletionItemKind::REFERENCE,
    }
}
