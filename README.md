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

- **`tools/lsp_test.py`** — Golden-file test harness. Replays a JSON array of LSP messages against the server and compares output to `expected.json`. Pass `--record` to write a new golden file instead of comparing.
- **`tools/lsp_bench.py`** — Benchmarks the server by replaying a message sequence and reporting per-method round-trip times. Accepts `--iterations`, `--warmup`, and `--methods` options; supports `perf record` via `--perf`.
- **`tools/lsp_callgrind.py`** — Profiles a single LSP request under Valgrind Callgrind and produces an annotated call tree with inclusive/exclusive instruction counts.
- **`tools/lsp_log_parse.py`** — Converts an Emacs `lsp-mode` trace log into a JSON message sequence compatible with `lsp_test.py` and `lsp_bench.py`.
- **`tools/callgrind_parse.py`** — Parses a raw `callgrind.out` file and prints a flat profile and call tree without requiring `callgrind_annotate`.


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

