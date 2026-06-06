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

## Debug logging

`src/debug.{h,c}` provide compile-time, per-category debug logging.
Output goes to **stderr** — stdout is reserved for the LSP JSON-RPC
stream the client watches, so it must never be written to for
debugging. Most editors surface a server's stderr in a dedicated
output channel.

Each category is an integer verbosity ceiling that defaults to `0`
(off). Because the category expands to a compile-time constant, a
disabled call site is eliminated entirely by the optimizer — no branch,
no string formatting, not even argument evaluation — so logging has zero
cost in the default build. Levels are `LOG_INFO` (1), `LOG_VERBOSE` (2),
and `LOG_TRACE` (3). Categories: `DEBUG_LIFECYCLE`,
`DEBUG_COMPILE_COMMANDS`, `DEBUG_DOCSTORE`, `DEBUG_INCLUDES`,
`DEBUG_PARSER`, `DEBUG_REVALIDATE`, `DEBUG_TJ3`, `DEBUG_THREADS`,
`DEBUG_RPC`.

Emit lines with `DLOG(category, level, fmt, ...)`, passing the category
macro name itself (it is both compared as an integer and stringified for
the log tag):

```c
DLOG(DEBUG_LIFECYCLE, LOG_INFO,  "initialize: root=%s", root);
DLOG(DEBUG_TJ3,       LOG_TRACE, "token %d kind=%d", i, kind);
```

Raise a category by editing its default in `src/debug.h`, or override
at build time without touching the file via `CFLAGS_EXTRA` (wired into
both `make` and `make debug`):

```sh
make CFLAGS_EXTRA="-DDEBUG_TJ3=3 -DDEBUG_REVALIDATE=2"
```

The docs[] table dump (`dump_docs_to_stderr`) is gated behind
`DEBUG_REVALIDATE >= LOG_VERBOSE`, so it is compiled out of the default
build rather than printing on every revalidation.

When adding a feature, log its key events under the matching category
(or add a new category to `src/debug.h` if none fits) so the subsystem
can be traced without adding ad-hoc `fprintf`s.

## Architecture

### Process model

JSON-RPC over stdin/stdout. `src/main.c` reads `Content-Length`-framed
messages and hands each body to `server_process()` (`src/server.c`),
which enqueues a `Job` onto an arrival-ordered queue. A single
**coordinator** thread pops jobs in order (preserving the LSP "process
in arrival order" rule) and owns the live document store under
`docs_mutex`: it runs notifications and the inline lifecycle methods
(`initialize` / `shutdown`) itself. Every other request is a read-only
query — the coordinator pins the current immutable workspace snapshot
(an O(1) refcount bump, see "Cross-file revalidation") into the Job's
`query_context` and hands it to a pool of query **workers**
(`src/threadpool.c`, `NUM_QUERY_WORKERS`). Workers run their handler
lock-free against the pinned snapshot and write responses via
`lsp_send_message()` (serialized by `stdout_mutex`).

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
`revalidate_all_docs()`: it polls `compile_commands.json` for on-disk
changes, calls `build_workspace_snapshot()` to assemble a fresh
immutable `workspace_snapshot` from the current `docs[]`, atomically
swaps it in as the published `g_ws` (releasing the previous one), then
republishes (currently empty) diagnostics on every editor-managed
document.

Snapshots are refcounted and immutable, so a query that pinned the old
snapshot keeps reading a consistent revision until it releases its ref;
the old snapshot is freed only then. Two layers (`src/query_context.{h,c}`):

- `doc_snapshot` — one document's frozen parse output (`tj_node` tree,
  token spans, source text), created once per parse and **shared by
  ref**: editing document A produces a new `doc_snapshot` for A while
  every other document's snapshot is re-referenced unchanged. Each
  `Document` holds its current `snap` plus the immediately previous
  `prev_snap` (retained so `semanticTokens/delta` can diff against the
  version the client last held). A `doc_snapshot` also carries a
  write-once, compare-exchange–published memo for its semantic-token
  data; the `resultId` is the document's parse version (`doc_version`).
- `workspace_snapshot` — a `ws_doc` per parsed document (its `doc_snapshot`
  ref plus the include-prefixes in force this revision) and a
  `ws_project` per assembled project. Each `ws_project` owns one synthetic
  `ProjectNode` root over all kinds, populated by deep-copying every
  member document's top-level entries under the includer's prefix
  target. The include BFS stamps each `ws_doc.project_index`; handlers
  scope cross-file lookups to siblings sharing the requester's project,
  so two unrelated `.tjp`s in the same workspace stay independent.

Dependency edges resolve lazily: each `ProjectDep` memoizes its resolved
target write-once in an atomic `resolved` cell (`project_dep_resolve`),
so concurrent workers pinning the same snapshot resolve safely and
back-to-back queries on one revision warm the memo. `handle_definition`,
`handle_references`, and dependency hover resolve against the snapshot's
`ProjectNode` tree.

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

This applies to data types as well: new `struct` / `enum` / `typedef`
names use snake_case (e.g. `doc_snapshot`, `workspace_snapshot`,
`query_context`), not CamelCase. A number of older types predate this
rule and remain CamelCase (`Document`, `Job`, `ProjectNode`,
`TokenSpan`, `ParseOutput`, …); leave those as they are unless you are
already refactoring them, but do not introduce new CamelCase types.

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
