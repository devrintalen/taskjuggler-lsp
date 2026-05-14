# tools/

Helper scripts for the language server's test harness, benchmark
pipeline, and profiling workflow.

| File                    | Purpose                                                                                  |
|-------------------------|------------------------------------------------------------------------------------------|
| `callgrind.py`          | Profile a scenario under Valgrind Callgrind, or analyse an existing `callgrind.out`      |
| `generate_large_tjp.py` | Generate large valid `.tjp` files for stress testing                                     |
| `lexer_test.c`          | Standalone Flex lexer driver (built by `make lexer-test`)                                |
| `lsp_bench.py`          | Replay a session JSON against the server with repeated iterations and p95 stats          |
| `lsp_check_perf.py`     | Compare the last two records in `benchmarks.jsonl` and flag regressions                  |
| `lsp_framing.py`        | Shared LSP transport helpers (imported by the bench / perf / callgrind tools)            |
| `lsp_log_parse.py`      | Parse Emacs `lsp-mode` trace logs into session JSON                                      |
| `lsp_perf_session.py`   | Build a JSON message sequence from a `.tjp` file; can also run it against the server     |
| `lsp_record_bench.py`   | Run all `test/session_*.json` fixtures and append a record to `benchmarks.jsonl`         |
| `lsp_test.py`           | Golden-file test harness                                                                 |

See `doc/performance.rst` for the full benchmark pipeline, fixture
 catalogue, and per-tool reference.

[perf-docs]: https://taskjuggler-lsp.readthedocs.io/en/latest/performance.html
