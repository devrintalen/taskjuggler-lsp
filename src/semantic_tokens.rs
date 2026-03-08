use tower_lsp::lsp_types::{
    SemanticToken, SemanticTokenModifier, SemanticTokenType, SemanticTokens,
    SemanticTokensLegend,
};

use crate::parser::{Lexer, TokenKind};

/// Token-type index for multi-line scissors strings.
const TOKEN_TYPE_STRING: u32 = 0;

pub fn legend() -> SemanticTokensLegend {
    SemanticTokensLegend {
        token_types: vec![SemanticTokenType::STRING],
        token_modifiers: vec![SemanticTokenModifier::new("declaration")],
    }
}

/// Build SemanticTokens for all `-8<-` ... `->8-` regions in `src`.
///
/// LSP requires that multi-line tokens be split into per-line segments, so
/// each line of a scissors string is emitted as a separate span.
pub fn build_semantic_tokens(src: &str) -> SemanticTokens {
    let mut lexer = Lexer::new(src);
    // Collect (line, start_char, length) for every line segment.
    let mut spans: Vec<(u32, u32, u32)> = Vec::new();

    loop {
        let tok = lexer.next_token();
        if tok.kind == TokenKind::Eof {
            break;
        }
        if tok.kind != TokenKind::MultiLineStr {
            continue;
        }

        // Split the token text on newlines and emit one span per line.
        let mut line = tok.start.line;
        let mut col = tok.start.character;
        for chunk in tok.text.split('\n') {
            // Trim \r for Windows line-endings before measuring length.
            let len = chunk.trim_end_matches('\r').len() as u32;
            if len > 0 {
                spans.push((line, col, len));
            }
            line += 1;
            col = 0; // continuation lines start at column 0
        }
    }

    // Encode spans as delta-relative SemanticToken entries.
    let mut data: Vec<SemanticToken> = Vec::with_capacity(spans.len());
    let mut prev_line = 0u32;
    let mut prev_start = 0u32;
    for (line, start_char, length) in spans {
        let delta_line = line - prev_line;
        let delta_start = if delta_line == 0 {
            start_char - prev_start
        } else {
            start_char
        };
        data.push(SemanticToken {
            delta_line,
            delta_start,
            length,
            token_type: TOKEN_TYPE_STRING,
            token_modifiers_bitset: 0,
        });
        prev_line = line;
        prev_start = start_char;
    }

    SemanticTokens {
        result_id: None,
        data,
    }
}
