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

### Callgrind findings (2026-03-27)

Profiled 8 scenarios across the 5 fixture files. Handler-inclusive instruction counts
(excluding the one-time parse cost on didOpen):

| Scenario | Handler Ir |
|---|---|
| flat (10k tasks) × completion | 14.4M |
| flat (10k tasks) × hover | 7.5M |
| flat (10k tasks) × semantic-tokens | 13.9M |
| heavy_deps (2k tasks, 8 deps) × completion | 16.2M |
| heavy_deps (2k tasks, 8 deps) × references | ~51K |
| deep (500 tasks, depth=5) × completion | 1.56M |
| deep (500 tasks, depth=5) × hover | 604K |
| nodeps (5k tasks, 0 deps) × completion | 5.6M |

**Bottleneck 1 — `has_signature_help` / `kw_has_docs` called inside `scan_kw_stack` (hover.c)**
`scan_kw_stack` walks backwards through the token array on every hover/completion
request. It calls `has_signature_help` once per scanned token to classify the keyword
context. On flat 10k: 27,258 `has_signature_help` calls costing 3.56M Ir — ~40% of the
total completion handler cost. Fix: compute the signature-help flag once at the start
of the scan rather than re-checking on each token.

**Fixed (2026-03-27):** Added `KW_SIG_END` sentinel to `grammar.y` (placed immediately
before `KW_DOCS_END`). Changed `scan_kw_stack` to accept `int kind_max` instead of a
filter function pointer; callers pass `KW_SIG_END` (sig help) or `KW_DOCS_END` (hover).
Removed `has_signature_help()`/`sig_keywords[]` and `kw_has_docs()`/`doc_keywords[]`
entirely. Result: zero string comparisons in the scan hot path.

| Scenario | Before | After | Δ |
|---|---|---|---|
| flat (10k) × completion | 14.4M Ir | 10.9M Ir | −3.5M (−24%) |
| flat (10k) × hover | 7.5M Ir | 3.9M Ir | −3.6M (−48%) |

**Bottleneck 2 — `tok_span_at` linear scan (hover.c)**
`tok_span_at` finds the token at the cursor position with a linear walk over the token
array. Cost on flat 10k: 1.37M Ir; on heavy_deps: 2.01M Ir.
Fix: binary search (tokens are stored in source order).

**Fixed (2026-03-27):** Added `pos_cmp(LspPos a, LspPos b)` as a 3-way comparator to
`hover.c` (reusing the identical `static inline` already in `parser.h`; the local copy
was dropped as a duplicate). Rewrote `pos_in` as a one-liner wrapper around `pos_cmp`.
Replaced the O(n) forward scan in `tok_span_at` with a binary search: find the
rightmost token whose `start ≤ pos`, then verify containment with `pos_in`.
`tok_span_at` is only called from `partial_word` in `completion.c`; the hover handler
does not call it, so only completion benefits.

| Scenario | Before | After | Δ |
|---|---|---|---|
| flat (10k) × completion | 10.9M Ir | 9.5M Ir | −1.4M (−13%) |
| heavy_deps (2k, 8 deps) × completion | — | 13.5M Ir | — |
| `tok_span_at` self (flat) | 1.37M Ir | <1K Ir | −99.9% |
| `tok_span_at` self (heavy_deps) | 2.01M Ir | <1K Ir | −99.9% |

**Bottleneck 3 — Semantic tokens JSON serialization (semantic_tokens.c)**
`push_entry` (116,910 calls): 10.8M Ir. `yyjson_mut_write_opts` serializing the result:
14.0M Ir. Together that is ~80% of the semantic-tokens handler cost — the token
classification logic itself is cheap. Fix: build the LSP integer array directly into a
flat buffer rather than via a yyjson object tree, then emit it once.

**Fixed (2026-03-27):** Replaced the `push_entry` / yyjson array tree approach with a
flat `uint32_t` buffer. `build_semantic_tokens_json` now writes five integers per token
directly into a `realloc`-grown buffer, then passes the raw byte span to the JSON
encoder as a pre-formatted string. `push_entry` and the yyjson array mutation calls are
gone entirely; `yyjson_mut_write_opts` no longer traverses the token array.

| Scenario                     | Before (handler) | Before (serialization) | After (total) | Δ               |
|------------------------------|------------------|------------------------|---------------|-----------------|
| flat (10k) × semantic-tokens | 13.8M Ir         | +14.0M Ir (yyjson)     | 12.2M Ir      | −56% end-to-end |
| `push_entry` self            | 10.7M Ir         | —                      | 0             | −100%           |
| `yyjson_mut_write_opts` self | 14.0M Ir         | —                      | <1K Ir        | −99.9%          |
| wall-clock (flat, median)    | 54.2ms           | —                      | 49.2ms        | −9%             |

**Bottleneck 4 — `build_completions_json` dep-ref traversal (completion.c)**
With 0 deps: `build_completions_json` self cost is 1.3M Ir.
With 8 deps/task: self cost jumps to 7.76M Ir despite 2.5× fewer tasks.
The identifier-collection loop visits dep references every request.
Fix: cache the candidate identifier list per document; invalidate on didChange.

### Wall-clock baseline (2026-03-27)

Measured with `lsp_bench.py --iterations 7 --warmup 2` after all 2026-03-27
optimisations. Session files generated with the intentional per-handler position
sampler (hover on keyword tokens, completion weighted across all branches,
definition/references on positions that produce non-null results).

All times are **median ms** across 7 runs × 5 positions (35 samples) for
position-based handlers, or 7 runs × 1 for whole-document handlers.

| Method | flat (10k, d=1) | wide (5k, d=2) | balanced (5k, d=3) | highdeps (2k, 8dep) | deep (500, d=5) |
|---|---|---|---|---|---|
| semanticTokens/full | 53.6 | 79.1 | 101.5 | 295.9 | 164.8 |
| documentSymbol | 63.5 | 60.4 | 62.7 | 69.0 | 90.4 |
| foldingRange | 15.9 | 17.0 | 17.9 | 18.6 | 23.2 |
| completion (median) | 2.5 | 0.7 | 0.7 | 2.5 | 1.6 |
| completion (p95) | 11.1 | 4.4 | 7.6 | 30.3 | 15.0 |
| hover (median) | 0.8 | 0.8 | 1.0 | 1.7 | 0.9 |
| signatureHelp (median) | 1.2 | 1.4 | 1.9 | 2.3 | 2.2 |
| definition (median) | 0.1 | 0.1 | 0.1 | 0.2 | 0.1 |
| references (median) | 0.1 | 0.1 | 0.2 | 0.4 | 0.4 |

**Observations:**
- `semanticTokens/full` is the dominant cost for whole-document requests; scales
  steeply with dep count (highdeps 296ms vs. flat 54ms) due to token array size.
- `documentSymbol` is surprisingly expensive (63–90ms) relative to its O(n) tree
  walk; likely dominated by JSON serialization of the symbol tree.
- `completion` p95 on highdeps (30ms) confirms Bottleneck 4 is unresolved: the
  8-deps/task file causes the identifier-collection loop to visit far more nodes.
- Position-based handlers (hover, signature, definition, references) are all
  sub-5ms median across all fixtures; no action needed.

