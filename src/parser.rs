use tower_lsp::lsp_types::{Diagnostic, DiagnosticSeverity, Position, Range, SymbolKind};

// ─── Token types ─────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    Ident,
    Str,
    Integer,
    Float,
    Date,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Bang,
    Plus,
    Minus,
    Colon,
    Comma,
    LineComment,
    BlockComment,
    /// Multi-line string delimited by `-8<-` ... `->8-` (scissors notation).
    MultiLineStr,
    Eof,
    Error,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub start: Position,
    pub end: Position,
    pub text: String,
}

// ─── Lexer ────────────────────────────────────────────────────────────────────

pub struct Lexer<'a> {
    src: &'a str,
    pos: usize,
    line: u32,
    col: u32,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a str) -> Self {
        Self {
            src,
            pos: 0,
            line: 0,
            col: 0,
        }
    }

    fn current(&self) -> Option<char> {
        self.src[self.pos..].chars().next()
    }

    fn peek_at(&self, offset: usize) -> Option<char> {
        self.src[self.pos..].chars().nth(offset)
    }

    fn advance(&mut self) -> Option<char> {
        let ch = self.current()?;
        self.pos += ch.len_utf8();
        if ch == '\n' {
            self.line += 1;
            self.col = 0;
        } else {
            self.col += 1;
        }
        Some(ch)
    }

    fn position(&self) -> Position {
        Position {
            line: self.line,
            character: self.col,
        }
    }

    fn skip_whitespace(&mut self) {
        while let Some(ch) = self.current() {
            if ch.is_whitespace() {
                self.advance();
            } else {
                break;
            }
        }
    }

    pub fn next_token(&mut self) -> Token {
        self.skip_whitespace();

        let start = self.position();

        let ch = match self.current() {
            None => {
                return Token {
                    kind: TokenKind::Eof,
                    start,
                    end: start,
                    text: String::new(),
                }
            }
            Some(c) => c,
        };

        // Hash line comment
        if ch == '#' {
            let mut text = String::new();
            while let Some(c) = self.current() {
                if c == '\n' {
                    break;
                }
                text.push(c);
                self.advance();
            }
            return Token {
                kind: TokenKind::LineComment,
                start,
                end: self.position(),
                text,
            };
        }

        // Line comment
        if ch == '/' && self.peek_at(1) == Some('/') {
            let mut text = String::new();
            while let Some(c) = self.current() {
                if c == '\n' {
                    break;
                }
                text.push(c);
                self.advance();
            }
            return Token {
                kind: TokenKind::LineComment,
                start,
                end: self.position(),
                text,
            };
        }

        // Block comment
        if ch == '/' && self.peek_at(1) == Some('*') {
            let mut text = String::new();
            self.advance(); // /
            self.advance(); // *
            text.push_str("/*");
            loop {
                match self.current() {
                    None => {
                        return Token {
                            kind: TokenKind::Error,
                            start,
                            end: self.position(),
                            text: "unterminated block comment".into(),
                        }
                    }
                    Some('*') if self.peek_at(1) == Some('/') => {
                        self.advance();
                        self.advance();
                        text.push_str("*/");
                        break;
                    }
                    Some(c) => {
                        text.push(c);
                        self.advance();
                    }
                }
            }
            return Token {
                kind: TokenKind::BlockComment,
                start,
                end: self.position(),
                text,
            };
        }

        // Single-char tokens
        match ch {
            '{' => {
                self.advance();
                return Token {
                    kind: TokenKind::LBrace,
                    start,
                    end: self.position(),
                    text: "{".into(),
                };
            }
            '}' => {
                self.advance();
                return Token {
                    kind: TokenKind::RBrace,
                    start,
                    end: self.position(),
                    text: "}".into(),
                };
            }
            '[' => {
                self.advance();
                return Token {
                    kind: TokenKind::LBracket,
                    start,
                    end: self.position(),
                    text: "[".into(),
                };
            }
            ']' => {
                self.advance();
                return Token {
                    kind: TokenKind::RBracket,
                    start,
                    end: self.position(),
                    text: "]".into(),
                };
            }
            '!' => {
                self.advance();
                return Token {
                    kind: TokenKind::Bang,
                    start,
                    end: self.position(),
                    text: "!".into(),
                };
            }
            '+' => {
                self.advance();
                return Token {
                    kind: TokenKind::Plus,
                    start,
                    end: self.position(),
                    text: "+".into(),
                };
            }
            '-' => {
                // Check for scissors open: -8<-  ...  ->8-
                if self.peek_at(1) == Some('8')
                    && self.peek_at(2) == Some('<')
                    && self.peek_at(3) == Some('-')
                {
                    self.advance(); // -
                    self.advance(); // 8
                    self.advance(); // <
                    self.advance(); // -
                    let mut text = String::from("-8<-");
                    loop {
                        match self.current() {
                            None => {
                                return Token {
                                    kind: TokenKind::Error,
                                    start,
                                    end: self.position(),
                                    text: "unterminated scissors string".into(),
                                };
                            }
                            Some('-')
                                if self.peek_at(1) == Some('>')
                                    && self.peek_at(2) == Some('8')
                                    && self.peek_at(3) == Some('-') =>
                            {
                                self.advance(); // -
                                self.advance(); // >
                                self.advance(); // 8
                                self.advance(); // -
                                text.push_str("->8-");
                                break;
                            }
                            Some(c) => {
                                text.push(c);
                                self.advance();
                            }
                        }
                    }
                    return Token {
                        kind: TokenKind::MultiLineStr,
                        start,
                        end: self.position(),
                        text,
                    };
                }
                self.advance();
                return Token {
                    kind: TokenKind::Minus,
                    start,
                    end: self.position(),
                    text: "-".into(),
                };
            }
            ':' => {
                self.advance();
                return Token {
                    kind: TokenKind::Colon,
                    start,
                    end: self.position(),
                    text: ":".into(),
                };
            }
            ',' => {
                self.advance();
                return Token {
                    kind: TokenKind::Comma,
                    start,
                    end: self.position(),
                    text: ",".into(),
                };
            }
            _ => {}
        }

        // String literal
        if ch == '"' {
            self.advance();
            let mut text = String::new();
            loop {
                match self.current() {
                    None | Some('\n') => {
                        return Token {
                            kind: TokenKind::Error,
                            start,
                            end: self.position(),
                            text: "unterminated string literal".into(),
                        }
                    }
                    Some('"') => {
                        self.advance();
                        break;
                    }
                    Some('\\') => {
                        self.advance();
                        if let Some(esc) = self.advance() {
                            text.push(esc);
                        }
                    }
                    Some(c) => {
                        text.push(c);
                        self.advance();
                    }
                }
            }
            return Token {
                kind: TokenKind::Str,
                start,
                end: self.position(),
                text,
            };
        }

        // Number or Date: starts with digit
        if ch.is_ascii_digit() {
            let mut text = String::new();
            while let Some(c) = self.current() {
                if c.is_ascii_digit() {
                    text.push(c);
                    self.advance();
                } else {
                    break;
                }
            }
            // Check for date: YYYY-MM-DD
            if text.len() == 4
                && self.current() == Some('-')
                && self
                    .src
                    .get(self.pos..)
                    .map(|s| {
                        s.len() >= 6
                            && s.chars().nth(1).map(|c| c.is_ascii_digit()).unwrap_or(false)
                            && s.chars().nth(2).map(|c| c.is_ascii_digit()).unwrap_or(false)
                            && s.chars().nth(3) == Some('-')
                            && s.chars().nth(4).map(|c| c.is_ascii_digit()).unwrap_or(false)
                            && s.chars().nth(5).map(|c| c.is_ascii_digit()).unwrap_or(false)
                    })
                    .unwrap_or(false)
            {
                // consume -MM-DD
                for _ in 0..6 {
                    if let Some(c) = self.advance() {
                        text.push(c);
                    }
                }
                return Token {
                    kind: TokenKind::Date,
                    start,
                    end: self.position(),
                    text,
                };
            }
            // Float
            if self.current() == Some('.') {
                text.push('.');
                self.advance();
                while let Some(c) = self.current() {
                    if c.is_ascii_digit() {
                        text.push(c);
                        self.advance();
                    } else {
                        break;
                    }
                }
                return Token {
                    kind: TokenKind::Float,
                    start,
                    end: self.position(),
                    text,
                };
            }
            return Token {
                kind: TokenKind::Integer,
                start,
                end: self.position(),
                text,
            };
        }

        // Identifier or keyword
        if ch.is_alphabetic() || ch == '_' {
            let mut text = String::new();
            while let Some(c) = self.current() {
                if c.is_alphanumeric() || c == '_' || c == '.' || c == ':' {
                    text.push(c);
                    self.advance();
                } else {
                    break;
                }
            }
            return Token {
                kind: TokenKind::Ident,
                start,
                end: self.position(),
                text,
            };
        }

        // Unknown character — emit Error token
        self.advance();
        Token {
            kind: TokenKind::Error,
            start,
            end: self.position(),
            text: format!("unexpected character: {:?}", ch),
        }
    }
}

// ─── Symbols ──────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub detail: String,
    pub kind: SymbolKind,
    pub range: Range,
    pub selection_range: Range,
    pub children: Vec<Symbol>,
}

pub struct ParseResult {
    pub diagnostics: Vec<Diagnostic>,
    pub symbols: Vec<Symbol>,
}

// ─── Parser ───────────────────────────────────────────────────────────────────

/// Keywords that produce named symbols in the document outline.
fn is_symbol_keyword(s: &str) -> bool {
    matches!(s, "project" | "task" | "resource" | "account" | "shift")
}

/// Task-body statement keywords that can appear on the line after a `depends`
/// list.  Used to terminate dep-arg collection when no comma precedes them.
fn is_task_attr_keyword(s: &str) -> bool {
    matches!(
        s,
        "effort"
            | "duration"
            | "length"
            | "milestone"
            | "scheduled"
            | "allocate"
            | "responsible"
            | "managers"
            | "start"
            | "end"
            | "maxstart"
            | "maxend"
            | "minstart"
            | "minend"
            | "priority"
            | "complete"
            | "note"
            | "rate"
            | "efficiency"
            | "limits"
            | "overtime"
            | "statusnote"
            | "dailyworkinghours"
            | "weeklyworkinghours"
            | "outputdir"
            | "purge"
            | "charge"
            | "chargeset"
            | "shift"
            | "credit"
            | "debit"
    )
}

/// Broader set of declaration keywords used as "stop" markers when skipping
/// argument lists, to avoid accidentally consuming a following declaration.
fn is_decl_keyword(s: &str) -> bool {
    is_symbol_keyword(s)
        || matches!(
            s,
            "macro"
                | "include"
                | "flags"
                | "supplement"
                | "leaves"
                | "vacation"
                | "timingresolution"
                | "now"
                | "currency"
                | "timeformat"
                | "timezone"
                | "workinghours"
                | "extend"
                | "journalentry"
                | "balance"
                | "booking"
        )
}

fn symbol_kind_for(kw: &str) -> SymbolKind {
    match kw {
        "project" => SymbolKind::MODULE,
        "resource" => SymbolKind::OBJECT,
        "account" => SymbolKind::VARIABLE,
        "shift" => SymbolKind::EVENT,
        _ => SymbolKind::FUNCTION, // task and anything else
    }
}

struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    fn new(src: &str) -> Self {
        let mut lexer = Lexer::new(src);
        let mut tokens = Vec::new();
        loop {
            let tok = lexer.next_token();
            let done = tok.kind == TokenKind::Eof;
            tokens.push(tok);
            if done {
                break;
            }
        }
        Self { tokens, pos: 0 }
    }

    fn peek(&self) -> &Token {
        &self.tokens[self.pos]
    }

    fn advance(&mut self) -> Token {
        let tok = self.tokens[self.pos].clone();
        if self.pos + 1 < self.tokens.len() {
            self.pos += 1;
        }
        tok
    }

    fn at_eof(&self) -> bool {
        self.peek().kind == TokenKind::Eof
    }

    fn skip_trivia(&mut self) {
        while matches!(
            self.peek().kind,
            TokenKind::LineComment | TokenKind::BlockComment
        ) {
            self.advance();
        }
    }

    /// Parse a sequence of items until `}` or EOF.
    fn parse_items(&mut self, diagnostics: &mut Vec<Diagnostic>) -> Vec<Symbol> {
        let mut symbols = Vec::new();
        loop {
            self.skip_trivia();
            match self.peek().kind {
                TokenKind::Eof | TokenKind::RBrace => break,

                TokenKind::Error => {
                    let tok = self.advance();
                    diagnostics.push(Diagnostic {
                        range: Range {
                            start: tok.start,
                            end: tok.end,
                        },
                        severity: Some(DiagnosticSeverity::ERROR),
                        message: tok.text,
                        ..Default::default()
                    });
                }

                TokenKind::Ident => {
                    let kw = self.advance();
                    self.skip_trivia();
                    if is_symbol_keyword(&kw.text) {
                        if let Some(sym) = self.try_parse_symbol(kw, diagnostics) {
                            symbols.push(sym);
                        }
                    } else {
                        self.skip_item_tail();
                    }
                }

                _ => {
                    self.advance();
                }
            }
        }
        symbols
    }

    /// Parse a named declaration: `<kw> <id> ["<name>"] [args…] [{ body }]`.
    /// `kw` has already been consumed by the caller.
    fn try_parse_symbol(
        &mut self,
        kw: Token,
        diagnostics: &mut Vec<Diagnostic>,
    ) -> Option<Symbol> {
        // id is optional — if the next token is a string, the id was omitted
        let (id, selection_start, selection_end) = if self.peek().kind == TokenKind::Ident {
            let t = self.advance();
            self.skip_trivia();
            let (s, e) = (t.start, t.end);
            (t.text, s, e)
        } else if self.peek().kind == TokenKind::Str {
            // No id; use the keyword position as the selection range
            (String::new(), kw.start, kw.end)
        } else {
            self.skip_item_tail();
            return None;
        };

        // optional quoted display name
        let mut range_end = selection_end;
        let name = if self.peek().kind == TokenKind::Str {
            let tok = self.advance();
            range_end = tok.end;
            self.skip_trivia();
            tok.text
        } else if id.is_empty() {
            // No id and no string — nothing useful to show
            self.skip_item_tail();
            return None;
        } else {
            id.clone()
        };

        // skip remaining inline arguments, extending range_end to cover them
        if let Some(end) = self.skip_args() {
            range_end = end;
        }
        let children = if self.peek().kind == TokenKind::LBrace {
            let open = self.advance(); // consume {
            let ch = self.parse_items(diagnostics);
            if self.peek().kind == TokenKind::RBrace {
                range_end = self.advance().end;
            } else {
                diagnostics.push(Diagnostic {
                    range: Range {
                        start: open.start,
                        end: open.end,
                    },
                    severity: Some(DiagnosticSeverity::ERROR),
                    message: "unclosed `{`".into(),
                    ..Default::default()
                });
            }
            ch
        } else {
            vec![]
        };

        Some(Symbol {
            name,
            // Show the TJP identifier as detail (e.g. "AcSo", "spec") so
            // the cross-reference id is visible in lsp-ui-imenu alongside
            // the human-readable name.  Fall back to the keyword if no id.
            detail: if id.is_empty() {
                kw.text.clone()
            } else {
                id.clone()
            },
            kind: symbol_kind_for(&kw.text),
            range: Range {
                start: kw.start,
                end: range_end,
            },
            selection_range: Range {
                start: selection_start,
                end: selection_end,
            },
            children,
        })
    }

    /// Skip inline argument tokens: stop at `{`, `}`, EOF, or a declaration keyword.
    /// Returns the end position of the last consumed token, or None if nothing was consumed.
    fn skip_args(&mut self) -> Option<Position> {
        let mut last_end: Option<Position> = None;
        loop {
            self.skip_trivia();
            match self.peek().kind {
                TokenKind::LBrace | TokenKind::RBrace | TokenKind::Eof => break,
                TokenKind::Ident if is_decl_keyword(&self.peek().text) => break,
                _ => {
                    let tok = self.advance();
                    last_end = Some(tok.end);
                }
            }
        }
        last_end
    }

    /// Skip the tail of a non-symbol item (args + optional body block).
    /// Called after the leading keyword has already been consumed.
    fn skip_item_tail(&mut self) {
        loop {
            self.skip_trivia();
            match self.peek().kind {
                TokenKind::Eof | TokenKind::RBrace => break,
                TokenKind::LBrace => {
                    self.advance();
                    self.skip_block_body();
                    break;
                }
                TokenKind::Ident if is_decl_keyword(&self.peek().text) => break,
                _ => {
                    self.advance();
                }
            }
        }
    }

    /// Skip tokens inside a `{ … }` block (assumes `{` already consumed).
    fn skip_block_body(&mut self) {
        let mut depth = 1u32;
        while depth > 0 && !self.at_eof() {
            match self.peek().kind {
                TokenKind::LBrace => {
                    depth += 1;
                    self.advance();
                }
                TokenKind::RBrace => {
                    depth -= 1;
                    self.advance();
                }
                _ => {
                    self.advance();
                }
            }
        }
    }
}

// ─── Dependency validation ────────────────────────────────────────────────────

/// Recursively resolve a dotted path into the symbol tree.
/// `segments` is the result of splitting a task reference by `.`.
fn resolve_from(syms: &[Symbol], segments: &[&str]) -> bool {
    if segments.is_empty() {
        return true;
    }
    for sym in syms {
        if sym.kind == SymbolKind::FUNCTION && sym.detail == segments[0] {
            return resolve_from(&sym.children, &segments[1..]);
        }
    }
    false
}

/// Navigate the symbol tree following `path` and return the children of the
/// matched node (local copy for use within this module).
fn find_scope_children_local<'a>(syms: &'a [Symbol], path: &[String]) -> &'a [Symbol] {
    if path.is_empty() {
        return syms;
    }
    for sym in syms {
        if sym.kind == SymbolKind::FUNCTION && sym.detail == path[0] {
            return find_scope_children_local(&sym.children, &path[1..]);
        }
    }
    &[]
}

/// Returns `true` when `id` resolves correctly given `scope` and `bang_count`.
///
/// - `bang_count == 0`: absolute reference — resolved from root.
/// - `bang_count >= 1`: relative — look in `scope[..k - bang_count]`.
/// - `bang_count > scope.len()`: too many `!`, returns `false`.
fn validate_ref_check(symbols: &[Symbol], scope: &[String], bang_count: usize, id: &str) -> bool {
    let scope_children: &[Symbol] = if bang_count == 0 {
        symbols
    } else {
        let k = scope.len();
        if bang_count > k {
            return false;
        }
        find_scope_children_local(symbols, &scope[..k - bang_count])
    };
    let segments: Vec<&str> = id.split('.').collect();
    resolve_from(scope_children, &segments)
}

/// Second-pass validator: scan the source for `depends`/`precedes` arguments
/// and emit diagnostics for references that don't resolve.
///
/// WARNING  — emitted when the number of `!` tokens exceeds the task depth.
/// ERROR    — emitted when the dotted path after `!*` is not found in the tree.
fn validate_deps(src: &str, symbols: &[Symbol]) -> Vec<Diagnostic> {
    let mut lexer = Lexer::new(src);
    let mut diagnostics = Vec::new();

    // Scope-tracking state (mirrors current_task_scope in completion.rs).
    #[derive(PartialEq)]
    enum SState { Scan, ExpectId, BeforeLBrace }

    let mut sstate = SState::Scan;
    let mut pending_id: Option<String> = None;
    let mut brace_depth: u32 = 0;
    let mut task_stack: Vec<(String, u32)> = Vec::new();

    // Deps-argument collection state.
    let mut in_deps = false;
    // After consuming a dep argument we require a comma before accepting the
    // next one.  Any other token (including a new-statement keyword) terminates
    // the dep list.  This prevents keywords on the next line from being
    // mistaken for dependency identifiers.
    let mut needs_comma = false;
    let mut bang_tokens: Vec<Token> = Vec::new();

    loop {
        let tok = lexer.next_token();
        if tok.kind == TokenKind::Eof {
            break;
        }
        if matches!(tok.kind, TokenKind::LineComment | TokenKind::BlockComment) {
            continue;
        }

        match tok.kind {
            // `{` ends any active depends list and updates scope depth.
            TokenKind::LBrace => {
                in_deps = false;
                needs_comma = false;
                bang_tokens.clear();
                brace_depth += 1;
                if sstate == SState::BeforeLBrace {
                    if let Some(id) = pending_id.take() {
                        task_stack.push((id, brace_depth));
                    }
                }
                pending_id = None;
                sstate = SState::Scan;
            }

            // `}` ends any active depends list and pops the innermost task scope.
            TokenKind::RBrace => {
                in_deps = false;
                needs_comma = false;
                bang_tokens.clear();
                task_stack.retain(|(_, d)| *d < brace_depth);
                if brace_depth > 0 {
                    brace_depth -= 1;
                }
                sstate = SState::Scan;
                pending_id = None;
            }

            TokenKind::Bang => {
                if in_deps {
                    if needs_comma {
                        // `!` after a dep arg without a preceding comma is
                        // unexpected — exit deps mode.
                        in_deps = false;
                        needs_comma = false;
                        bang_tokens.clear();
                    } else {
                        bang_tokens.push(tok);
                    }
                }
            }

            TokenKind::Comma => {
                if in_deps {
                    bang_tokens.clear();
                    needs_comma = false;
                }
            }

            TokenKind::Ident => {
                if in_deps {
                    if is_decl_keyword(&tok.text) || is_task_attr_keyword(&tok.text) {
                        // Any recognised statement keyword ends the deps list;
                        // fall through to scope-tracking below.
                        in_deps = false;
                        needs_comma = false;
                        bang_tokens.clear();
                    } else if tok.text == "depends" || tok.text == "precedes" {
                        // Back-to-back depends/precedes: restart argument collection.
                        bang_tokens.clear();
                        needs_comma = false;
                        // Stay in_deps = true.
                        continue;
                    } else if needs_comma {
                        // Ident after a dep arg without a preceding comma → the
                        // dep list has ended and this starts a new statement.
                        // Exit deps mode and fall through to scope tracking.
                        in_deps = false;
                        needs_comma = false;
                        bang_tokens.clear();
                    } else {
                        // Validate this identifier as a dependency reference.
                        let scope: Vec<String> =
                            task_stack.iter().map(|(id, _)| id.clone()).collect();
                        let bang_count = bang_tokens.len();
                        if !validate_ref_check(symbols, &scope, bang_count, &tok.text) {
                            let k = scope.len();
                            if bang_count > 0 && bang_count > k {
                                let bang_start =
                                    bang_tokens.first().map(|t| t.start).unwrap_or(tok.start);
                                let bang_end =
                                    bang_tokens.last().map(|t| t.end).unwrap_or(tok.start);
                                diagnostics.push(Diagnostic {
                                    range: Range {
                                        start: bang_start,
                                        end: bang_end,
                                    },
                                    severity: Some(DiagnosticSeverity::WARNING),
                                    message: "dependency reference escapes beyond project root"
                                        .into(),
                                    ..Default::default()
                                });
                            } else {
                                diagnostics.push(Diagnostic {
                                    range: Range {
                                        start: tok.start,
                                        end: tok.end,
                                    },
                                    severity: Some(DiagnosticSeverity::ERROR),
                                    message: format!("unresolved task: `{}`", tok.text),
                                    ..Default::default()
                                });
                            }
                        }
                        bang_tokens.clear();
                        needs_comma = true; // Require comma before next dep arg.
                        continue; // Don't do scope tracking for dep identifiers.
                    }
                }

                // Scope tracking for Ident tokens.
                if tok.text == "depends" || tok.text == "precedes" {
                    in_deps = true;
                    needs_comma = false;
                    bang_tokens.clear();
                    sstate = SState::Scan;
                } else if tok.text == "task" {
                    sstate = SState::ExpectId;
                    pending_id = None;
                } else if sstate == SState::ExpectId {
                    pending_id = Some(tok.text.clone());
                    sstate = SState::BeforeLBrace;
                }
                // In Scan or BeforeLBrace with a non-"task" keyword: no change.
            }

            _ => {
                if in_deps {
                    if needs_comma {
                        // Non-comma token after dep arg → end of dep list.
                        in_deps = false;
                        needs_comma = false;
                        bang_tokens.clear();
                    } else {
                        // Unexpected token (string, number, …) before any arg —
                        // just clear pending bangs and stay in deps mode.
                        bang_tokens.clear();
                    }
                }
                if sstate == SState::ExpectId {
                    sstate = SState::Scan;
                    pending_id = None;
                }
            }
        }
    }

    diagnostics
}

#[cfg(test)]
mod tests {
    use super::*;

    fn print_symbols(symbols: &[Symbol], indent: usize) {
        for sym in symbols {
            println!(
                "{}[{}] {} \"{}\"",
                "  ".repeat(indent),
                sym.detail,
                format!("{:?}", sym.kind).to_lowercase(),
                sym.name
            );
            print_symbols(&sym.children, indent + 1);
        }
    }

    #[test]
    fn test_tutorial() {
        let src = include_str!("../test/tutorial.tjp");
        let result = parse(src);
        println!("Diagnostics ({}):", result.diagnostics.len());
        for diag in &result.diagnostics {
            println!(
                "  line {} col {}: {}",
                diag.range.start.line + 1,
                diag.range.start.character + 1,
                diag.message
            );
        }
        println!("\nSymbols:");
        print_symbols(&result.symbols, 0);
    }

    #[test]
    fn valid_dep_no_diagnostic() {
        // Absolute reference (0 bangs) to an existing task — no diagnostic expected.
        let src = "task a \"A\" {}\ntask b \"B\" {\n  depends a\n}";
        let result = parse(src);
        let dep_diags: Vec<_> = result
            .diagnostics
            .iter()
            .filter(|d| {
                d.message.contains("unresolved task")
                    || d.message.contains("escapes beyond")
            })
            .collect();
        assert!(
            dep_diags.is_empty(),
            "valid absolute dep should have no diagnostic; got: {:?}",
            dep_diags
        );
    }

    #[test]
    fn invalid_dep_produces_error() {
        // Reference to a task id that doesn't exist.
        let src = "task a \"A\" {}\ntask b \"B\" {\n  depends nonexistent\n}";
        let result = parse(src);
        let errors: Vec<_> = result
            .diagnostics
            .iter()
            .filter(|d| {
                d.severity == Some(DiagnosticSeverity::ERROR)
                    && d.message.contains("unresolved task")
            })
            .collect();
        assert_eq!(
            errors.len(),
            1,
            "should have exactly one unresolved-dep error; got: {:?}",
            errors
        );
        assert!(errors[0].message.contains("nonexistent"));
    }

    #[test]
    fn too_many_bangs_warning() {
        // `a` is a top-level task (depth 1); `!!x` uses 2 bangs — one too many.
        let src = "task x \"X\" {}\ntask a \"A\" {\n  depends !!x\n}";
        let result = parse(src);
        let warnings: Vec<_> = result
            .diagnostics
            .iter()
            .filter(|d| {
                d.severity == Some(DiagnosticSeverity::WARNING)
                    && d.message.contains("escapes beyond")
            })
            .collect();
        assert!(
            !warnings.is_empty(),
            "!! inside single top-level task should produce a warning; diagnostics: {:?}",
            result.diagnostics
        );
    }

    #[test]
    fn tutorial_no_dep_diagnostics() {
        // All dependency references in tutorial.tjp should resolve cleanly.
        let src = include_str!("../test/tutorial.tjp");
        let result = parse(src);
        let dep_diags: Vec<_> = result
            .diagnostics
            .iter()
            .filter(|d| {
                d.message.contains("unresolved task")
                    || d.message.contains("escapes beyond")
            })
            .collect();
        assert!(
            dep_diags.is_empty(),
            "tutorial.tjp should have no dependency diagnostics; got: {:?}",
            dep_diags
        );
    }
}

pub fn parse(src: &str) -> ParseResult {
    let mut parser = Parser::new(src);
    let mut diagnostics = Vec::new();
    let mut symbols = Vec::new();

    // Loop to recover after unmatched `}` at the top level.
    loop {
        let new_syms = parser.parse_items(&mut diagnostics);
        symbols.extend(new_syms);

        parser.skip_trivia();
        if parser.at_eof() {
            break;
        }

        // Any `}` here is unmatched — report it and keep going.
        if parser.peek().kind == TokenKind::RBrace {
            let tok = parser.advance();
            diagnostics.push(Diagnostic {
                range: Range {
                    start: tok.start,
                    end: tok.end,
                },
                severity: Some(DiagnosticSeverity::ERROR),
                message: "unmatched `}`".into(),
                ..Default::default()
            });
        } else {
            parser.advance();
        }
    }

    let dep_diagnostics = validate_deps(src, &symbols);
    diagnostics.extend(dep_diagnostics);

    ParseResult {
        diagnostics,
        symbols,
    }
}
