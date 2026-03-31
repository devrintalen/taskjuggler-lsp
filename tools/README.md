# Performance Testing Tooling

All tools live in `tools/`. They share a common session file format: a
JSON array of LSP message objects that is replayed against the
server. This format is produced by `lsp_perf_session.py` and
`lsp_log_parse.py`, and consumed by `lsp_test.py`, `lsp_bench.py`, and
`callgrind.py`.

- `generate_large_tjp.py` — generates large `.tjp` files for stress testing
- `lsp_perf_session.py` — builds a JSON message sequence from a `.tjp` file; can run it directly against the server and print per-method timings
- `lsp_bench.py` — replays a session JSON against the server with repeated iterations and p95 stats
- `lsp_record_bench.py` — runs all `test/session_*.json` fixtures and appends a record to `benchmarks.jsonl`
- `lsp_check_perf.py` — compares the last two records in `benchmarks.jsonl` and flags regressions
- `callgrind.py` — profile mode: runs a scenario under Valgrind Callgrind; parse mode: reads an existing `callgrind.out` — both modes print flat top-N and a call tree
- `lsp_framing.py` — shared LSP transport helpers (not run directly; imported by bench/perf/callgrind tools)
- `lsp_log_parse.py` — parses Emacs lsp-mode trace logs into session JSON
- `lsp_test.py` — golden-file test harness

## Benchmark workflow

The benchmarking tools form a pipeline:

```
generate_large_tjp.py                # (one-time) create fixture .tjp files
    ↓
lsp_perf_session.py                  # build session JSON from a .tjp file
    ↓                                  (pre-built session files are committed to test/)
lsp_bench.py / lsp_record_bench.py   # replay session, collect timings
    ↓
lsp_check_perf.py                    # compare records, flag regressions
```

Quick overview of typical usage:

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

## TJP fixture files (`test/perf_*.tjp`)

**`generate_large_tjp.py`** — Generate large valid `.tjp` files for stress testing.

```sh
python3 tools/generate_large_tjp.py --tasks 10000 --depth 1 --deps-per-task 2 \
    --output test/perf_flat.tjp
python3 tools/generate_large_tjp.py --tasks 2000 --depth 3 --deps-per-task 8 \
    --output test/perf_highdeps.tjp
```

The committed fixture files under `test/perf_*.tjp` were generated
with this tool and cover different structural stress profiles (flat,
wide, deep, high-dependency-density).

| File                | Structure                         | Purpose                                   |
|---------------------|-----------------------------------|-------------------------------------------|
| `perf_flat.tjp`     | 10k tasks, depth=1, 2.0 deps/task | Widest file; exercises linear token scans |
| `perf_wide.tjp`     | 10k tasks, depth=2, 2.0 deps/task | Shallow group hierarchy                   |
| `perf_balanced.tjp` | 10k tasks, depth=3, 2.0 deps/task | Baseline balanced tree                    |
| `perf_highdeps.tjp` | 10k tasks, depth=3, 8.0 deps/task | Stresses dep-ref validation               |
| `perf_deep.tjp`     | 10k tasks, depth=6, 2.0 deps/task | Deeply nested; stresses `scan_kw_stack`   |


## Session files (`test/session_*.json`)

**`lsp_perf_session.py`** — Build a session JSON file from a `.tjp` fixture.

Generates a realistic LSP message sequence (initialize, didOpen, a mix
of requests, shutdown) with cursor positions sampled to exercise each
handler's interesting code paths: hover lands on documented keyword
tokens, completion samples a weighted mix of
keyword/dep-ref/resource-ref/account-ref positions, definition and
references land on positions that return non-null results.

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

Available request types: `hover`, `completion`, `signature`,
`definition`, `references`, `document-symbol`, `folding-range`,
`semantic-tokens`, `workspace-symbol`, `didchange`.

Pre-generated JSON message sequences used by
`lsp_record_bench.py`. Each fixture has a standard variant and a
`_warm` variant (includes a `didChange` notification to exercise
re-parse):

```
test/session_{flat,wide,balanced,highdeps,deep}.json
test/session_{flat,wide,balanced,highdeps,deep}_warm.json
```

To regenerate a session file from its TJP fixture:

```bash
python3 tools/lsp_perf_session.py test/perf_flat.tjp --output test/session_flat.json
```

**`lsp_log_parse.py`** — Convert an Emacs `lsp-mode` trace log into a session JSON file.

Enable `lsp-log-io` in Emacs (`M-x lsp-workspace-set-log-level RET
io`), reproduce the scenario, then export the log buffer and parse it:

```sh
python3 tools/lsp_log_parse.py /path/to/lsp-log.txt -o session.json
```

The output is compatible with `lsp_test.py`, `lsp_bench.py`, and `callgrind.py`.

## Benchmarking (`test/benchmarks.jsonl`)

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

Runs `lsp_bench.py`'s core against every `test/session_*.json` (and
`test/session_*_warm.json` for warm-cache measurements), then appends
a single JSON line containing the git commit hash, timestamp,
hostname, and per-method stats to `benchmarks.jsonl`.

```sh
python3 tools/lsp_record_bench.py
python3 tools/lsp_record_bench.py --iterations 10 --warmup 3
python3 tools/lsp_record_bench.py --dry-run   # print without writing
```

## Analysis

**`lsp_check_perf.py`** — Compare the last two records in `benchmarks.jsonl` and flag regressions.

```sh
python3 tools/lsp_check_perf.py                        # default 10% threshold
python3 tools/lsp_check_perf.py --threshold 5          # stricter
python3 tools/lsp_check_perf.py --method documentSymbol --fixture flat
python3 tools/lsp_check_perf.py --threshold 999        # print table, never fail
```

Exits 0 if no regressions, 1 if any method/fixture combination regressed beyond the threshold.

## Profiling with Callgrind

**`callgrind.py`** — Profile a scenario under Valgrind Callgrind, or analyse an existing output file.

Requires a debug build (`make debug`) so that function names resolve correctly.

```sh
make debug   # build taskjuggler-lsp-debug with -g symbols

# Generate a focused scenario (initialize + initialized + didOpen + one request):
python3 tools/lsp_perf_session.py test/perf_flat.tjp \
    --requests completion --positions 1 --repeat 1 \
    --output /tmp/flat_completion.json

# Profile it (slow — Callgrind runs ~20x slower than normal):
python3 tools/callgrind.py ./taskjuggler-lsp-debug \
    /tmp/flat_completion.json \
    -o test/callgrind/flat_completion/

# Analyse a previously recorded output file:
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out \
    --tree handle_completion
```

Profile mode writes `callgrind.out` and `calltree.txt` to the output
directory, then prints a flat profile (top N functions by exclusive
instruction count) and a call tree. Parse mode reads an existing
`callgrind.out` directly and prints the same analysis. The output
directory can also be opened in KCachegrind for interactive
exploration.
