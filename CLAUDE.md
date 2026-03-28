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
- `lsp_record_bench.py` — runs all fixtures and appends a record to `benchmarks.jsonl`
- `lsp_check_perf.py` — compares the last two records in `benchmarks.jsonl` and flags regressions
- `callgrind.py` — profile mode: runs a scenario under Valgrind Callgrind; parse mode: reads an existing `callgrind.out` — both modes print flat top-N and a call tree
- `lsp_framing.py` — shared LSP transport helpers (not run directly; imported by bench/perf/callgrind tools)
- `lsp_log_parse.py` — parses Emacs lsp-mode trace logs into session JSON
- `lsp_test.py` — golden-file test harness

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

# Profile and analyse in one step:
python3 tools/callgrind.py ./taskjuggler-lsp-debug \
    test/scenarios/flat_completion.json \
    -o test/callgrind/flat_completion/

# Or analyse an existing output file:
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out \
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

### Callgrind findings (2026-03-27, second pass — intentional position sampling)

Re-profiled after the position sampler was rewritten to target meaningful cursor
positions per handler. Scenarios use the deepest positions in the file for
scan-sensitive handlers (completion).

Handler-inclusive instruction counts (excluding one-time parse cost on didOpen):

| Scenario | Handler Ir | Key costs |
|---|---|---|
| flat (10k tasks) × semantic-tokens | 12.2M | `build_semantic_tokens_json` self 11.6M |
| flat (10k tasks) × hover (line 7590) | 1.05M | `scan_kw_stack` 3,649 tokens |
| flat (10k tasks) × signatureHelp (line 7590) | 1.28M | `scan_kw_stack` 3,649 tokens |
| flat (10k tasks) × completion (line 31098) | 28.1M | `collect_ids` 7.0M; `scan_kw_stack` 5.2M |
| flat (10k tasks) × definition | 42.6K | O(depth) hash lookup |
| flat (10k tasks) × references | 341.5K | linear scan + `find_task_at` |
| flat (10k tasks) × documentSymbol | ~30.5M | 7.4M handler + 23.1M `yyjson_mut_write_opts` |
| flat (10k tasks) × foldingRange | 2.87M | `push_range` called 20,004× |
| highdeps (2k tasks, 8 deps) × semantic-tokens | 57.9M | dense token array; `build_semantic_tokens_json` self 57.6M |
| highdeps (2k tasks, 8 deps) × completion (line 30042) | 81.2M | `scan_kw_stack` 29.7M; `build_completions_json` self 47.1M |
| highdeps (2k tasks, 8 deps) × documentSymbol | 7.9M | 2k symbols × range_json |

**New bottleneck A — `documentSymbol` yyjson serialization**
`build_document_symbols_json` builds a full yyjson object tree: `doc_symbol_to_json`
calls `range_json` 40,004× (4 ranges × 10k symbols = 40k yyjson objects), then
`yyjson_mut_write_opts` traverses the entire tree — 23.1M Ir just for serialization,
dwarfing the 7.4M Ir tree-building cost. Total effective handler cost ≈ 30.5M Ir.
Fix: pre-serialize the symbol tree to a flat JSON string once and cache it;
invalidate on didChange. (Same pattern as Bottleneck 3 fix for semantic tokens.)

**Fixed (2026-03-28):** Replaced `doc_symbol_to_json` (yyjson tree) with a flat
`Buf`-based serializer in `document_symbol.c`. Key optimizations: hand-written
`write_uint()` (avoids `sprintf`), bulk `memcpy` runs in `buf_push_json_str` (avoids
per-byte `buf_push`), and compile-time `PUSH_LIT()` macro (avoids `strlen` for
literal strings). The resulting JSON string is cached in `Document.doc_symbols_json`
(freed on `doc_free` / `handle_didchange`). Subsequent requests skip the build step
entirely; `yyjson_mut_rawncpy` embeds the cached bytes verbatim.

| Scenario | Before | After (cold cache) | Δ |
|---|---|---|---|
| flat (10k) × documentSymbol | 63.5ms | 55.2ms | −13% |
| deep (500, d=5) × documentSymbol | 90.4ms | 84.6ms | −6.4% |
| `yyjson_mut_write_opts` self (flat) | 23.1M Ir | ~0 | −100% |
| `build_document_symbols_json` Ir (flat) | 7.4M Ir | 25.0M Ir (first call) | cache miss worse, but one-time |

**New bottleneck B — `scan_kw_stack` on dense dep files (completion.c)**
In `highdeps × completion`, `scan_kw_stack` costs 29.7M Ir (27.1M self) scanning
19,848 tokens — roughly 3× more tokens per task than the flat file due to 8 dep-ref
tokens per task. The backward linear scan is O(all_tokens_before_cursor), which scales
with both file size and dep density. `build_completions_json` self is also 47.1M Ir,
partly from string filtering across the collected identifier list.
Fix: build a sorted keyword-token index at parse time so `scan_kw_stack` can
binary-search to the first relevant token, skipping non-keyword tokens.

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

### Wall-clock baseline (2026-03-28, after documentSymbol optimization)

Same measurement methodology as 2026-03-27. Same session files (`test/session_*.json`)
and same `.tjp` fixtures. The benchmark restarts the server for each run (7 measured
runs per fixture), so `documentSymbol` timings reflect the cold-cache cost (first
request after didOpen).

| Method | flat (10k, d=1) | wide (5k, d=2) | balanced (5k, d=3) | highdeps (2k, 8dep) | deep (500, d=5) |
|---|---|---|---|---|---|
| semanticTokens/full | 55.5 | 84.6 | 97.8 | 307.8 | 162.4 |
| documentSymbol | 55.2 | 59.6 | 62.1 | 68.2 | 84.6 |
| foldingRange | 16.2 | 16.8 | 19.2 | 19.8 | 24.2 |
| completion (median) | 2.2 | 0.9 | 0.7 | 2.7 | 1.4 |
| completion (p95) | 4.9 | 5.6 | 6.2 | 17.1 | 7.7 |
| hover (median) | 0.8 | 1.0 | 1.0 | 1.9 | 1.0 |
| signatureHelp (median) | 1.1 | 2.0 | 1.9 | 2.2 | 2.1 |
| definition (median) | 0.1 | 0.2 | 0.1 | 0.2 | 0.1 |
| references (median) | 0.1 | 0.2 | 0.2 | 0.4 | 0.4 |

**Changes vs 2026-03-27:**
- `documentSymbol` improved 13% on flat (63.5→55.2ms) and 6.4% on deep (90.4→84.6ms).
  Remaining cost is dominated by I/O (writing ~2MB JSON per request).
- Warm-cache requests (after first call on a live server) are much faster: the build
  step is skipped, only `rawncpy` + response I/O remain (~few ms).
- All other handlers are essentially unchanged.

