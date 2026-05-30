# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A Language Server Protocol (LSP) implementation for
[TaskJuggler](https://taskjuggler.org/), written in C.

The project supports TaskJuggler v3 and does not support earlier
versions.

Generated Doxygen API documentation is published at
<https://devrintalen.github.io/taskjuggler-lsp/index.html>.

## Dependencies

- yyjson (`-lyyjson`)
- Flex
- Bison
- Python 3 (test harness and benchmark tooling)
- Valgrind (optional, for callgrind profiling)

## Common Commands

Build / clean:

```sh
make           # produces ./taskjuggler-lsp
make debug     # produces ./taskjuggler-lsp-debug with -g, -no-pie (for perf/valgrind)
make clean
```

Run the full test suite:

```sh
python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases
```

Run / record a single golden test case:

```sh
python3 tools/lsp_test.py ./taskjuggler-lsp test/cases/hover_keyword
python3 tools/lsp_test.py ./taskjuggler-lsp --record test/cases/hover_keyword
```

Performance: see `tools/README.md` for the full pipeline. Quick reference:

```sh
python3 tools/lsp_record_bench.py     # all fixtures → append to benchmarks.jsonl
python3 tools/lsp_check_perf.py       # diff last two records, fail on regression
python3 tools/lsp_bench.py ./taskjuggler-lsp test/session_flat.json
```

Standalone lexer driver (helpful when debugging tokenization):

```sh
make lexer-test       # builds ./lexer-test, links only flex output
```

The build order matters: bison runs first to emit `src/grammar.tab.h`,
then flex consumes it. Every `.o` depends on `grammar.tab.h`, so
`make clean` followed by `make` is the safe way to recover from a
stale generated header.

## Architecture

### Process model

JSON-RPC over stdin/stdout. `src/main.c` reads `Content-Length`-framed
messages and hands each body to `server_process()` (`src/server.c`);
responses are written back via `lsp_send_message()` (serialized under
`stdout_mutex`).

Message handling uses a three-thread-class design (`src/threadpool.c`):

- **Reader** (`main.c`): parses Content-Length framing, calls
  `server_process()` to classify each message and push a `Job` onto
  `work_queue`.
- **Coordinator** (single thread): pops jobs from `work_queue` in
  arrival order.  Notifications and lifecycle requests (`initialize`,
  `shutdown`, `textDocument/semanticTokens/full[/delta]`) run **inline**
  under `docs_mutex` so subsequent messages observe their side effects.
  For regular query jobs the coordinator calls
  `server_snapshot_for_job()` — which briefly holds `docs_mutex` to copy
  the relevant slab pages and deep-copy the project tree — then pushes
  the job (with its attached snapshot) to `request_queue`.
- **Query workers** (`NUM_QUERY_WORKERS = 4`): pop from `request_queue`
  and call `server_dispatch_query()`.  Workers run **entirely lock-free**:
  they operate only on the private `workspace_snapshot` attached to the
  job and never touch `docs[]`, `projects[]`, `g_workspace_root`,
  `g_cc_*`, or the global flex/bison parser state.

#### Thread-safety invariant

A query worker handler MUST NOT read or write any of the following:

- `docs[]` / `num_docs` / the `Document` struct fields of any live slot
- `projects[]` / `num_projects`
- `g_workspace_root`, `g_cc_path`, `g_cc_mtime_*`, `g_cc_size`,
  `g_cc_attempted`
- The global lexer/parser build state (`g_build_root`, `yylval`,
  `yytext`, `yylineno`, `yycolumn`) — i.e. `parse()` must never be
  called from a worker
- Any non-reentrant libc function (`strtok`, `localtime`, `gmtime`,
  `strerror`, `rand`, …); use the `_r` variants where applicable

Workers may safely read: their job's `workspace_snapshot` (a private
mmap copy of the slab and a deep copy of the project tree), the immutable
`static const` keyword/signature tables in `completion.c`/`signature.c`,
and any per-call stack-local state.  stdout writes must go through
`lsp_send_message()`.

### Document store

`src/server.c` owns a static array of `Document` slots (URI + raw text
+ `ParseResult`). Editor content is authoritative while a file is
open; `didClose` re-reads the file from disk and keeps it as a
"background" entry so cross-file references stay valid.

`compile_commands.json` at the workspace root is the sole startup
populator of `docs[]`: every listed `.tjp` is loaded as `disk_only`,
and `follow_includes` cascades into the transitive `.tji` closure.
If the file is missing or malformed the server stays alive but loads
no documents and surfaces an Error-severity `window/showMessage`.
After startup, `workspace/didChangeWatchedFiles` events admit
individual files into background slots; watcher events are ignored
for files the editor already has open.

### Parse pipeline

`parse(text)` in `src/parser.c` is the single entry point that drives
flex (`src/lexer.l`) and bison (`src/grammar.y`) together and returns
a `ParseResult` containing:

- `tok_spans[]` — every token in source order, each with an `owner`
  pointer to its enclosing `DocSymbol`. This is the array that
  cursor-position queries (hover, completion, signature help, semantic
  tokens, folding ranges, document highlight) scan or binary-search.
- `doc_symbols[]` — hierarchical tree of named declarations
  (project / task / resource / account / shift). Used by
  document/workspace symbol, completion ID lists, definition,
  references, brace-block folding.
- `diagnostics[]` — split into two regions by `dep_diag_start`. Indices
  `[0..dep_diag_start)` are permanent syntax / in-file dep errors
  produced by the parse; indices `[dep_diag_start..]` are cross-file
  dep diagnostics that are cleared and rebuilt every revalidation
  cycle.
- `included_files[]` — unquoted filenames from `include` directives,
  used to drive transitive workspace loading.
- `cross_file_deps[]` — `RawDepRef`s for 0-bang references that did
  not resolve in-file; retried each cycle against other documents.

Resolved references are stored as `DefinitionLink` (outgoing, on the
declaring symbol) and `ReferenceLink` (incoming, on the target). When
either link's `target_uri` / `source_uri` is non-NULL it is a
cross-file edge.

### Cross-file revalidation

After any document-changing notification, `server.c` runs
`revalidate_all_docs()`. This polls `compile_commands.json` for
on-disk changes, runs `rebuild_all_projects()` to recompute project
membership and per-`Project` `tj_node` trees from the
`compile_commands.json` closure, then republishes (currently empty)
diagnostics on every editor-managed document.

Each `Project` owns four synthetic per-kind `tj_node` roots (`tasks`,
`accounts`, `reports`, `resources`) populated by deep-copying every
member document's top-level entries under the includer's prefix
target. Each `Document.primary_project` points at the project that
claimed it during BFS. Handlers (`handle_completion` today) scope
cross-file lookups to other documents sharing the requester's
`primary_project`, so two unrelated `.tjp`s in the same workspace
stay independent.

The richer dep-link machinery (`DefinitionLink` / `ReferenceLink` /
`resolve_cross_file_deps`) was removed in the tj_node refactor and is
not yet restored; `handle_definition` and `handle_references` return
`null` until it is.

### Feature dispatch

`server_process()` in `src/server.c` parses the JSON-RPC envelope and
dispatches to a per-method `handle_*` function. Each LSP feature lives
in its own `.c` / `.h` pair under `src/`, and the mapping from
`ParseResult` field to consumer is documented in the comment block at
the top of `src/server.c` ("QUERY DISPATCH"). When adding a feature,
follow the same pattern: a header declaring a `build_*_json()` entry
point that returns a `yyjson_mut_val *`, a corresponding `.c` with the
implementation, a new entry in the `Makefile`'s `SRC` list, and a
`handle_*` dispatch arm in `server.c`.

## Test harness

Golden-file tests under `test/cases/`. Each case is a directory with
`input.json` (LSP message sequence to send) and `expected.json`
(captured server output). `tools/lsp_test.py` replays input against
the built binary and diffs the response stream. Use `--record` to
regenerate `expected.json` after an intentional behavior change. The
harness is also what CI runs (`.github/workflows/build-and-test.yml`).

`test/perf_*.tjp` and `test/session_*.json` are large generated
fixtures used by the benchmark tooling, not the correctness tests.
`test/tutorial.tjp` is the upstream TaskJuggler tutorial included
under GPLv2 as a realistic fixture.

## Code Style Conventions

Use snake_case rather than camelCase for multi-word identifiers.

Use K&R C style for code.

Use spaces instead of tabs, and use four spaces per indent.

Prefer full words rather than abbreviations for naming. For example,
"token_end_line" rather than "tok_el".

## TaskJuggler Reference

Use the command "tj3man" to get definitive syntax and usage
information for TaskJuggler. The command "tj3man <keyword>" will
return detailed information on keywords and concepts. "tj3man" without
any arguments will return a full list of available pages.

## Release Checklist

When cutting a new release, perform every step below:

1. Bump `VERSION_MAJOR` / `VERSION_MINOR` / `VERSION_PATCH` and
   `VERSION_STRING` in `src/version.h`.
2. Bump `VERSION` in the `Makefile` to match.
3. Rebuild (`make`) and run the full test suite:
   `python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases`.
   The `initialize` response embeds `VERSION_STRING` in `serverInfo`,
   so every `expected.json` that captures an initialize reply must be
   updated to the new version. Use `--record` to regenerate them, or
   update the `"version": "X.Y.Z"` string in place.
4. Commit the version bump together with the updated test snapshots.
