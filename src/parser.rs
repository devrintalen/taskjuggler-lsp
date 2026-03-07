use tower_lsp::lsp_types::{Diagnostic, DiagnosticSeverity, Position, Range};

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

pub fn parse(src: &str) -> Vec<Diagnostic> {
    let mut diagnostics = Vec::new();
    let mut lexer = Lexer::new(src);
    let mut brace_depth: i32 = 0;
    let mut brace_stack: Vec<Position> = Vec::new();

    loop {
        let tok = lexer.next_token();
        match tok.kind {
            TokenKind::Eof => break,
            TokenKind::Error => {
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
            TokenKind::LBrace => {
                brace_depth += 1;
                brace_stack.push(tok.start);
            }
            TokenKind::RBrace => {
                if brace_depth == 0 {
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
                    brace_depth -= 1;
                    brace_stack.pop();
                }
            }
            _ => {}
        }
    }

    // Any unclosed braces
    for open_pos in brace_stack {
        diagnostics.push(Diagnostic {
            range: Range {
                start: open_pos,
                end: Position {
                    line: open_pos.line,
                    character: open_pos.character + 1,
                },
            },
            severity: Some(DiagnosticSeverity::ERROR),
            message: "unclosed `{`".into(),
            ..Default::default()
        });
    }

    diagnostics
}
