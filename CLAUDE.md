# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A Language Server Protocol (LSP) implementation for [TaskJuggler](https://taskjuggler.org/), written in C.

The project supports TaskJuggler v3 and does not support earlier versions.

## Dependencies

Depends on:
- yyjson
- Flex
- Bison

## Architecture

To be documented as the codebase develops. Expected structure:
- LSP server handling JSON-RPC communication
- TaskJuggler `.tjp`/`.tji` file parser
- Language features: diagnostics, completion, hover, go-to-definition

## Code Style Conventions

Use snake_case rather than camelCase for multi-word identifiers.

Use K&R C style for code.

Use spaces instead of tabs, and use four spaces per indent.

Prefer full words rather than abbreviations for naming. For example, "token_end_line" rather than "tok_el".

## TaskJuggler Reference

Use the command "tj3man" to get definitive syntax and usage information for TaskJuggler. The command "tj3man <keyword>" will return detailed information on keywords and concepts. "tj3man" without any arguments will return a full list of available pages.

## Performance Testing

Tools in `tools/`:
- `generate_large_tjp.py` — generates large `.tjp` files for stress testing
- `lsp_perf_session.py` — builds a JSON message sequence from a `.tjp` file; can run it directly against the server and print per-method timings
- `lsp_bench.py` — replays a session JSON against the server with repeated iterations and p95 stats
- `lsp_record_bench.py` — runs all `test/session_*.json` fixtures and appends a record to `benchmarks.jsonl`
- `lsp_check_perf.py` — compares the last two records in `benchmarks.jsonl` and flags regressions
- `callgrind.py` — profile mode: runs a scenario under Valgrind Callgrind; parse mode: reads an existing `callgrind.out` — both modes print flat top-N and a call tree
- `lsp_framing.py` — shared LSP transport helpers (not run directly; imported by bench/perf/callgrind tools)
- `lsp_log_parse.py` — parses Emacs lsp-mode trace logs into session JSON
- `lsp_test.py` — golden-file test harness

### TJP fixture files (`test/perf_*.tjp`)

| File                | Structure                         | Purpose                                   |
|---------------------|-----------------------------------|-------------------------------------------|
| `perf_flat.tjp`     | 10k tasks, depth=1, 2.0 deps/task | Widest file; exercises linear token scans |
| `perf_wide.tjp`     | 10k tasks, depth=2, 2.0 deps/task | Shallow group hierarchy                   |
| `perf_balanced.tjp` | 10k tasks, depth=3, 2.0 deps/task | Baseline balanced tree                    |
| `perf_highdeps.tjp` | 10k tasks, depth=3, 8.0 deps/task | Stresses dep-ref validation               |
| `perf_deep.tjp`     | 10k tasks, depth=6, 2.0 deps/task | Deeply nested; stresses `scan_kw_stack`   |

### Session files (`test/session_*.json`)

Pre-generated JSON message sequences used by `lsp_record_bench.py`. Each fixture has a standard variant and a `_warm` variant (includes a `didChange` notification to exercise re-parse):

```
test/session_{flat,wide,balanced,highdeps,deep}.json
test/session_{flat,wide,balanced,highdeps,deep}_warm.json
```

To regenerate a session file from its TJP fixture:

```bash
python3 tools/lsp_perf_session.py test/perf_flat.tjp --output test/session_flat.json
```

### Benchmark workflow

```bash
# Record a full benchmark run across all fixtures (appends to benchmarks.jsonl):
python3 tools/lsp_record_bench.py

# Compare the last two records and flag regressions:
python3 tools/lsp_check_perf.py

# Run a single fixture manually with timing output:
python3 tools/lsp_bench.py ./taskjuggler-lsp test/session_flat.json

# Quick ad-hoc timing run against a TJP file:
python3 tools/lsp_perf_session.py test/perf_flat.tjp --run ./taskjuggler-lsp
```

### Callgrind workflow

```bash
make debug   # build taskjuggler-lsp-debug with -g symbols

# Generate a focused scenario (initialize + initialized + didOpen + one request):
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --requests completion --positions 1 --repeat 1 \
    --output /tmp/flat_completion.json

# Profile and analyse in one step:
python3 tools/callgrind.py ./taskjuggler-lsp-debug \
    /tmp/flat_completion.json \
    -o test/callgrind/flat_completion/

# Or analyse an existing output file:
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out \
    --tree handle_completion
```
