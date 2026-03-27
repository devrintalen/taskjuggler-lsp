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
- `lsp_callgrind.py` — profiles a single LSP request under Valgrind Callgrind; outputs `callgrind.out` + `calltree.txt`
- `callgrind_parse.py` — parses callgrind output; prints flat top-N by instruction count or a call tree rooted at a named function

### TJP fixture files (`test/perf_*.tjp`)

| File | Structure | Purpose |
|------|-----------|---------|
| `perf_flat.tjp` | 10k tasks, depth=1, 2.0 deps/task | Largest file; exercises linear token scans |
| `perf_wide.tjp` | 5k tasks, depth=2, 2.0 deps/task | Shallow group hierarchy |
| `perf_nodeps.tjp` | 5k tasks, depth=3, 0 deps | Baseline: parsing + structure only |
| `perf_heavy_deps.tjp` | 2k tasks, depth=3, 8.0 deps/task | Stresses dep-ref validation |
| `perf_deep.tjp` | 500 tasks, depth=5, 2.0 deps/task | Deeply nested braces; stresses `scan_kw_stack` |

### Callgrind workflow

```bash
make debug   # build taskjuggler-lsp-debug with -g symbols

# Generate a 4-message scenario (initialize + initialized + didOpen + one request):
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --requests completion --positions 1 --repeat 1 \
    --output test/scenarios/flat_completion.json

# Profile it:
python3 tools/lsp_callgrind.py ./taskjuggler-lsp-debug \
    test/scenarios/flat_completion.json \
    -o test/callgrind/flat_completion/

# Analyse results:
python3 tools/callgrind_parse.py test/callgrind/flat_completion/callgrind.out --top 30
python3 tools/callgrind_parse.py test/callgrind/flat_completion/callgrind.out \
    --tree handle_completion
```

