# TaskJuggler LSP Memory

## Project
C LSP server for TaskJuggler v3 `.tjp`/`.tji` files.
Depends on cJSON (`-lcjson`).

## Build
```
make          # builds taskjuggler-lsp binary
make clean
```
CFLAGS include `-D_DEFAULT_SOURCE` (needed for `strdup` under `-std=c11`).

## Key Files
- `Makefile` — build system
- `src/main.c` — entry point; reads Content-Length framed JSON-RPC from stdin
- `src/server.c` — document store (array of 64 docs) + LSP method dispatch + publishDiagnostics
- `src/parser.h/c` — Lexer + Parser → ParseResult (symbols + diagnostics); includes dep validation
- `src/hover.h/c` — `token_at()`, `keyword_docs()` (static markdown strings)
- `src/signature.h/c` — `active_context()` (scans to cursor), `build_signature_help_json()`
- `src/completion.h/c` — `completions_json()` with block_stack, scope-aware dep/allocate IDs
- `src/semantic_tokens.h/c` — scissors string (`-8<-..->8-`) semantic tokens

## Architecture
- Single-threaded; synchronous stdin/stdout JSON-RPC
- Document store: static array of 64 `Document` structs (uri + text + ParseResult)
- On didOpen/didChange: parse text, store, publish diagnostics notification
- All heap strings use `strdup`; dynamic arrays use `realloc`

## LSP Capabilities
- textDocumentSync: Full
- documentSymbol, hover, signatureHelp (trigger: " ")
- completion (triggers: ",", " ", "!")
- semanticTokens/full (legend: ["string"] types)

## Token Types
TK_IDENT, TK_STR, TK_INTEGER, TK_FLOAT, TK_DATE, TK_LBRACE, TK_RBRACE,
TK_LBRACKET, TK_RBRACKET, TK_BANG, TK_PLUS, TK_MINUS, TK_COLON, TK_COMMA,
TK_LINE_COMMENT, TK_BLOCK_COMMENT, TK_MULTI_LINE_STR, TK_EOF, TK_ERROR

## Known Issues / Notes
- `signature.c`: SIG0() macro for zero-param keywords (milestone, scheduled)
- Hex escape sequences in hover.c followed by decimal digits need string concat split
- `active_context` in signature.c calls `build_signature_help_json` to test if keyword
  has a signature — this causes redundant allocation; could be optimized with `has_sig()`
