# taskjuggler-lsp

A [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) (LSP) server for [TaskJuggler v3](https://taskjuggler.org/), written in C.

This is an independent implementation that provides editor tooling for TaskJuggler's `.tjp`/`.tji` file format. It is not a modified version of TaskJuggler and does not include any TaskJuggler source code.

## Features

| Feature              | Method                                          | Status          | Notes                                                                                                 |
|----------------------|-------------------------------------------------|-----------------|-------------------------------------------------------------------------------------------------------|
| Lifecycle            | `initialize` / `shutdown` / `exit`              | Implemented     | Negotiates capabilities on init                                                                       |
| Document Sync        | `textDocument/didOpen`, `didChange`, `didClose` | Implemented     | Full-document sync; caches up to 64 open files                                                        |
| File Watching        | `workspace/didChangeWatchedFiles`               | Implemented     | Registers watchers for `**/*.tjp` and `**/*.tji`; re-parses on create/change, removes on delete      |
| Diagnostics          | `textDocument/publishDiagnostics`               | Implemented     | Reports unresolved `depends`/`precedes` targets as errors; out-of-scope relative refs as warnings; cross-file validation |
| Hover                | `textDocument/hover`                            | Implemented     | Markdown docs for 39 TaskJuggler keywords                                                             |
| Completion           | `textDocument/completion`                       | Implemented     | Context-aware keyword and identifier suggestions; supports hierarchical and relative (`!`) references |
| Signature Help       | `textDocument/signatureHelp`                    | Implemented     | Parameter descriptions for 39 keywords                                                                |
| Document Symbols     | `textDocument/documentSymbol`                   | Implemented     | Hierarchical symbol tree for projects, tasks, resources, accounts, shifts                             |
| Workspace Symbols    | `workspace/symbol`                              | Implemented     | Case-insensitive substring search across all open files                                               |
| Go to Definition     | `textDocument/definition`                       | Implemented     | Jumps to task declaration from `depends`/`precedes` reference; supports cross-file references         |
| Find References      | `textDocument/references`                       | Implemented     | Finds `depends`/`precedes` paths that reference a task; trigger from the task's declaration identifier; same-file only |
| Folding Ranges       | `textDocument/foldingRange`                     | Implemented     | Folds brace-delimited blocks (`{}`), macro bodies (`[]`), and multi-line block comments               |
| Semantic Tokens      | `textDocument/semanticTokens/full`              | Implemented     | Syntax highlighting for keywords, identifiers, strings, numbers, dates, and comments                  |
| Rename               | `textDocument/rename`                           | Not implemented |                                                                                                       |
| Code Actions         | `textDocument/codeAction`                       | Not implemented |                                                                                                       |
| Formatting           | `textDocument/formatting`                       | Not implemented |                                                                                                       |

## Dependencies

- [yyjson](https://github.com/ibireme/yyjson)
- [Flex](https://github.com/westes/flex)
- [Bison](https://www.gnu.org/software/bison/)
- [Python](https://www.python.org/) (for running unit tests)

On Debian/Ubuntu:

```sh
apt install libyyjson-dev flex bison
```

On Gentoo:

```sh
emerge -a dev-libs/yyjson sys-devel/flex sys-devel/bison
```

## Building

```sh
make
```

This produces the `taskjuggler-lsp` binary. To clean build artifacts:

```sh
make clean
```

## Testing

To build and run the standalone lexer test tool:

```sh
make lexer-test
./lexer-test test/tutorial.tjp
```

To run the LSP integration tests:

```sh
python3 tools/lsp_test.py --all test/cases ./taskjuggler-lsp
```

### Python tools

All tools live in `tools/`. They share a common session file format: a JSON array of LSP message objects that is replayed against the server. This format is produced by `lsp_perf_session.py` and `lsp_log_parse.py`, and consumed by `lsp_test.py`, `lsp_bench.py`, and `callgrind.py`.

#### Testing

**`lsp_test.py`** — Golden-file test harness.

Replays a JSON message sequence against the server and diffs the output against a recorded `expected.json`. Use `--record` to capture a new golden file.

```sh
# Run all test cases:
python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases

# Record a new golden file for a single case:
python3 tools/lsp_test.py ./taskjuggler-lsp --record test/cases/hover_keyword
```

Test cases live under `test/cases/`. Each is a directory containing `input.json` (the message sequence to send) and `expected.json` (the golden output).

#### Benchmarking

The benchmarking tools form a pipeline:

```
generate_large_tjp.py           # (one-time) create fixture .tjp files
    ↓
lsp_perf_session.py             # build session JSON from a .tjp file
    ↓
lsp_bench.py / lsp_record_bench.py   # replay session, collect timings
    ↓
lsp_check_perf.py               # compare records, flag regressions
```

**`lsp_perf_session.py`** — Build a session JSON file from a `.tjp` fixture.

Generates a realistic LSP message sequence (initialize, didOpen, a mix of requests, shutdown) with cursor positions sampled to exercise each handler's interesting code paths: hover lands on documented keyword tokens, completion samples a weighted mix of keyword/dep-ref/resource-ref/account-ref positions, definition and references land on positions that return non-null results.

```sh
# Build a session file for all default request types:
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --output test/session_flat.json

# Quick single-shot run with timing output (no file written):
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --run ./taskjuggler-lsp

# Profile a specific method with more positions:
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --requests completion --positions 10 --run ./taskjuggler-lsp
```

Available request types: `hover`, `completion`, `signature`, `definition`, `references`, `document-symbol`, `folding-range`, `semantic-tokens`, `workspace-symbol`, `didchange`.

**`lsp_bench.py`** — Replay a session file with multiple iterations and report statistics.

```sh
python3 tools/lsp_bench.py ./taskjuggler-lsp test/session_flat.json \
    --iterations 7 --warmup 2

# Profile with Linux perf:
python3 tools/lsp_bench.py ./taskjuggler-lsp test/session_flat.json \
    --perf perf.data
```

Reports `n`, mean, median, min, max, and p95 (when ≥20 samples) per method.

**`lsp_record_bench.py`** — Run all fixtures and append a timestamped record to `benchmarks.jsonl`.

Runs `lsp_bench.py`'s core against every `test/session_*.json` (and `test/session_*_warm.json` for warm-cache measurements), then appends a single JSON line containing the git commit hash, timestamp, hostname, and per-method stats to `benchmarks.jsonl`.

```sh
python3 tools/lsp_record_bench.py
python3 tools/lsp_record_bench.py --iterations 10 --warmup 3
python3 tools/lsp_record_bench.py --dry-run   # print without writing
```

**`lsp_check_perf.py`** — Compare the last two records in `benchmarks.jsonl` and flag regressions.

```sh
python3 tools/lsp_check_perf.py                        # default 10% threshold
python3 tools/lsp_check_perf.py --threshold 5          # stricter
python3 tools/lsp_check_perf.py --method documentSymbol --fixture flat
python3 tools/lsp_check_perf.py --threshold 999        # print table, never fail
```

Exits 0 if no regressions, 1 if any method/fixture combination regressed beyond the threshold.

**`generate_large_tjp.py`** — Generate large valid `.tjp` files for stress testing.

```sh
python3 tools/generate_large_tjp.py --tasks 10000 --depth 1 --deps-per-task 2 \
    --output test/perf_flat.tjp
python3 tools/generate_large_tjp.py --tasks 2000 --depth 3 --deps-per-task 8 \
    --output test/perf_highdeps.tjp
```

The committed fixture files under `test/perf_*.tjp` were generated with this tool and cover different structural stress profiles (flat, wide, deep, high-dependency-density).

#### Profiling with Callgrind

**`callgrind.py`** — Profile a scenario under Valgrind Callgrind, or analyse an existing output file.

Requires a debug build (`make debug`) so that function names resolve correctly.

```sh
# Generate a focused single-request scenario:
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --requests completion --positions 1 --repeat 1 \
    --output test/scenarios/flat_completion.json

# Profile it (slow — Callgrind runs ~20x slower than normal):
python3 tools/callgrind.py ./taskjuggler-lsp-debug \
    test/scenarios/flat_completion.json \
    -o test/callgrind/flat_completion/

# Analyse a previously recorded output file:
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out \
    --tree handle_completion
```

Profile mode writes `callgrind.out` and `calltree.txt` to the output directory, then prints a flat profile (top N functions by exclusive instruction count) and a call tree. Parse mode reads an existing `callgrind.out` directly and prints the same analysis. The output directory can also be opened in KCachegrind for interactive exploration.

#### Capturing real sessions

**`lsp_log_parse.py`** — Convert an Emacs `lsp-mode` trace log into a session JSON file.

Enable `lsp-log-io` in Emacs (`M-x lsp-workspace-set-log-level RET io`), reproduce the scenario, then export the log buffer and parse it:

```sh
python3 tools/lsp_log_parse.py /path/to/lsp-log.txt -o session.json
```

The output is compatible with `lsp_test.py`, `lsp_bench.py`, and `callgrind.py`.


## Usage

Configure your editor to launch `taskjuggler-lsp` as the language server for `.tjp` and `.tji` files. The server communicates over standard input/output using the LSP JSON-RPC protocol.

In Emacs, which I use with `lsp-mode`, this looks like this:

```emacs-lisp

  (use-package lsp-mode
    :init
    (setq lsp-keymap-prefix "C-c l")
    (setq lsp-semantic-tokens-enable t)
    (setq lsp-log-io t)
    :hook ((taskjuggler-mode . lsp))
    :commands lsp
    :config
    (setq lsp-completion-filter-on-incomplete nil))

  (use-package lsp-ui
    :hook (lsp-mode . lsp-ui-mode)
    :config
    (setq lsp-ui-doc-show-with-cursor t))

  (with-eval-after-load 'lsp-mode
    (lsp-register-client
     (make-lsp-client
      :new-connection (lsp-stdio-connection
  		     "/path/to/taskjuggler-lsp/taskjuggler-lsp")
      :major-modes '(taskjuggler-mode)
      :server-id 'taskjuggler-lsp)))

```

This initialization code depends on `taskjuggler.el`, which is an Emacs major mode for TaskJuggler that I am still working on and have not made public yet.

## Limitations

- **Find References** (`textDocument/references`) only searches the queried document's dependency links. If task `backend` is defined in `project.tjp` and referenced from `tasks.tji`, a references query on `backend` will not surface the cross-file usage. Go to Definition works cross-file; find references does not yet.
- **Completion** does not suggest task identifiers from other open files in `depends`/`precedes` clauses. Only identifiers visible in the current file's symbol tree are offered.

## License

Copyright (C) 2026 Devrin Talen

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; version 2 of the License. See [LICENSE](LICENSE) for the full license text.

### Relationship to TaskJuggler

[TaskJuggler](https://taskjuggler.org/) is copyright Chris Schlaeger and others, licensed under GPLv2. This project is a separate tool that supports the TaskJuggler file format; it is not a derivative work of the TaskJuggler source code.

The file `test/tutorial.tjp` is an example project from the TaskJuggler tutorial, copyright Chris Schlaeger, included here as a test fixture under the terms of the GPLv2.

