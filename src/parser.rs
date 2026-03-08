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

#[derive(Debug)]
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
        // id is required
        if self.peek().kind != TokenKind::Ident {
            self.skip_item_tail();
            return None;
        }
        let id = self.advance();
        self.skip_trivia();

        // optional quoted display name
        let name = if self.peek().kind == TokenKind::Str {
            let s = self.advance().text.clone();
            self.skip_trivia();
            s
        } else {
            id.text.clone()
        };

        // skip remaining inline arguments
        self.skip_args();

        // optional body
        let mut range_end = id.end;
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
            detail: kw.text.clone(),
            kind: symbol_kind_for(&kw.text),
            range: Range {
                start: kw.start,
                end: range_end,
            },
            selection_range: Range {
                start: id.start,
                end: id.end,
            },
            children,
        })
    }

    /// Skip inline argument tokens: stop at `{`, `}`, EOF, or a declaration keyword.
    fn skip_args(&mut self) {
        loop {
            self.skip_trivia();
            match self.peek().kind {
                TokenKind::LBrace | TokenKind::RBrace | TokenKind::Eof => break,
                TokenKind::Ident if is_decl_keyword(&self.peek().text) => break,
                _ => {
                    self.advance();
                }
            }
        }
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

    ParseResult {
        diagnostics,
        symbols,
    }
}
