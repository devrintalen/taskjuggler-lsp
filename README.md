# taskjuggler-lsp

[![Documentation Status](https://readthedocs.org/projects/taskjuggler-lsp/badge/?version=latest)](https://taskjuggler-lsp.readthedocs.io/en/latest/?badge=latest)

A [Language Server Protocol][] (LSP) server for [TaskJuggler][], written in C.

![A video showing some of the basic features of taskjuggler-lsp in Emacs lsp-mode](screenshots/overview.gif)

A **language server** is a background program that gives editors
language-aware features — completion, hover docs, go-to-definition,
find-references, diagnostics, rename, etc. — without each editor
having to reimplement them.

The server talks to the editor over the [Language Server Protocol
(LSP)][LSP Specification]. The editor sends events like "user opened
this file" or "cursor is here"; the server parses the code, maintains
its own model of the workspace, and replies with structured results
the editor renders.

This program implements that for TaskJuggler.

## Features

- Diagnostics from three independent sources merged per file:
  - parser (syntax, unresolved local refs);
  - the real `tj3` scheduler run asynchronously per project (cross-file refs, scheduling errors);
  - server configuration warnings (e.g. missing `compile_commands.json`).
- Hover and signature help for 39 TaskJuggler keywords
- Context-aware completion of keywords and identifiers, including hierarchical and relative (`!`) references
- Document and workspace symbols across all open and background-loaded files
- Go to definition and find references for `depends`/`precedes`, cross-file
- Document highlight, folding ranges, and semantic-token syntax highlighting (with delta support)
- Cancellable requests (`$/cancelRequest`) and lock-free parallel query workers backed by immutable refcounted snapshots
- Incremental document sync, file watching for `**/*.tjp` and `**/*.tji`, and rename tracking
- Workspace loaded from a top-level `compile_commands.json`, with transitive `include` directives admitting background documents automatically

## Dependencies

Build-time:

- [yyjson](https://github.com/ibireme/yyjson)
- [Flex](https://github.com/westes/flex)
- [Bison](https://www.gnu.org/software/bison/)
- [Python](https://www.python.org/) (test harness and benchmark tooling)
- [Valgrind](https://valgrind.org/) (optional, for callgrind profiling)

Runtime:

- [TaskJuggler](https://taskjuggler.org/) (`tj3` 3.x, optional but strongly recommended)

If `tj3` is on `PATH` the server invokes it asynchronously per project
to surface real scheduler diagnostics (cross-file unresolved
references, scheduling conflicts, date-range errors). Without it the
server still parses and serves every LSP feature; only the `tj3`-sourced
diagnostics are missing.

On Debian/Ubuntu:

```sh
apt install libyyjson-dev flex bison taskjuggler
```

On Gentoo:

```sh
emerge -a dev-libs/yyjson sys-devel/flex sys-devel/bison dev-lang/ruby
gem install taskjuggler
```

TaskJuggler is not in the main Gentoo portage tree; the upstream
distribution is a Ruby gem, so install Ruby through portage and then
pull `taskjuggler` from rubygems.org.

## Building

```sh
make
```

This produces the `taskjuggler-lsp` binary. To clean build artifacts:

```sh
make clean
```

## Testing

**`lsp_test.py`** — Golden-file test harness.

Replays a JSON message sequence against the server and diffs the
output against a recorded `expected.json`. Use `--record` to capture a
new golden file.

```sh
# Run all test cases:
python3 tools/lsp_test.py ./taskjuggler-lsp --all test/cases

# Record a new golden file for a single case:
python3 tools/lsp_test.py ./taskjuggler-lsp --record test/cases/hover_keyword
```

Test cases live under `test/cases/`. Each is a directory containing
`input.json` (the message sequence to send) and `expected.json` (the
golden output).

## Documentation

The published site at
<https://taskjuggler-lsp.readthedocs.io/> is built with
[Sphinx][] from the sources under `doc/`, with the API reference
section pulled in from [Doxygen][] XML via [Breathe][]. Both layers
can be built locally.

### Doxygen API reference

`doxygen` reads `Doxyfile` at the repo root and writes HTML and XML
output under `doc/_doxygen/`. The XML tree (`doc/_doxygen/xml/`) is
also what the Sphinx build consumes.

```sh
make docs           # equivalent to: doxygen Doxyfile
make docs-clean     # remove doc/_doxygen/
```

Open `doc/_doxygen/html/index.html` to view the API reference on its
own.

### Sphinx site

The Sphinx configuration lives in `doc/conf.py`. Python dependencies
are pinned in `doc/requirements.txt`:

```sh
pip install -r doc/requirements.txt
```

The Sphinx build depends on the Doxygen XML output, so run `make docs`
first (or whenever the C sources change), then run `sphinx-build`:

```sh
make docs
sphinx-build doc doc/_build/html
```

Open `doc/_build/html/index.html` to view the full site.

## Usage

Configure your editor to launch `taskjuggler-lsp` as the language
server for `.tjp` and `.tji` files. The server communicates over
standard input/output using the LSP JSON-RPC protocol.

See `doc/usage.rst` for more details on integrating with specific
IDEs.

### Workspace configuration

Each workspace needs a `compile_commands.json` at its root listing
the top-level `.tjp` files. This is the sole source of truth for
which projects exist; without it the server stays alive but loads
no documents and surfaces the problem as a per-file diagnostic on
every open `.tjp` / `.tji`.

Minimal schema (only `file` and optional `directory` are read today;
`command` is preserved for future use):

```json
[
  { "directory": ".", "command": "tj3 main.tjp",  "file": "main.tjp" },
  { "directory": ".", "command": "tj3 other.tjp", "file": "other.tjp" }
]
```

Each listed `.tjp` becomes the root of one project; its transitive
`include` closure (every reachable `.tji` / `.tjp`) joins the same
project automatically and inherits the includer's prefix.

> **Placeholder — schema flags.** Today `command` is captured but
> not interpreted. A future revision is likely to honour `-D` style
> defines and scenario flags from the command string, at which point
> this section should document which flags are read.

### Architecture overview

For a deeper tour of the runtime — the coordinator/worker thread
model, the immutable refcounted `doc_snapshot` /
`workspace_snapshot` layers, the per-project tj3 workers, and how
the parse output flows to each LSP feature — see the [Module
guide][module-guide] in the published docs.

[module-guide]: https://taskjuggler-lsp.readthedocs.io/en/latest/modules/index.html

> **Placeholder — Status / roadmap.** This README does not currently
> track which LSP methods are implemented or planned. The published
> docs have an exhaustive table at
> [`doc/lsp_coverage.rst`][lsp-coverage]; if the project gains a
> public roadmap, a short "Status" section linking to it would
> belong here.

[lsp-coverage]: https://taskjuggler-lsp.readthedocs.io/en/latest/lsp_coverage.html

## License

Copyright (C) 2026 Devrin Talen

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License. See
[LICENSE](LICENSE) for the full license text.

### Relationship to TaskJuggler

[TaskJuggler][] is copyright Chris Schlaeger and others, licensed
under GPLv2. This project is an independent implementation that
provides editor tooling for TaskJuggler's `.tjp`/`.tji` file
format. It is not a modified version of TaskJuggler and does not
include any TaskJuggler source code.

The file `test/tutorial.tjp` is an example project from the
TaskJuggler tutorial, copyright Chris Schlaeger, included here as a
test fixture under the terms of the GPLv2.


[Language Server Protocol]: https://microsoft.github.io/language-server-protocol/
[TaskJuggler]: https://taskjuggler.org/
[`taskjuggler-mode.el`]: https://github.com/devrintalen/taskjuggler-mode.el
[Sphinx]: https://www.sphinx-doc.org/
[Doxygen]: https://www.doxygen.nl/
[Breathe]: https://breathe.readthedocs.io/
[LSP Specification]: https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
