#!/usr/bin/env python3
"""
Benchmark an LSP server using a JSON message sequence.

The input is a JSON array of LSP message objects (the same format used by
lsp_test.py and produced by lsp_log_parse.py).  Messages are replayed in
order against a fresh server instance.  Notifications are fire-and-forget;
requests block until the matching response arrives so that each round-trip
can be timed individually.

Usage:
  lsp_bench.py <server> <input.json> [options]

Options:
  --iterations N   Number of measured runs (default: 5)
  --warmup N       Warmup runs whose timings are discarded (default: 1)
  --methods M,...  Comma-separated list of methods to include in the report
                   (default: all, excluding initialize)
  --perf FILE      Record a single run under `perf record`, writing perf.data
                   to FILE (default: perf.data).  Implies --warmup 1
                   --iterations 1.  View results with: perf report -i FILE
"""

import argparse
import json
import os
import statistics
import sys

# Allow importing from the same tools/ directory
sys.path.insert(0, os.path.dirname(__file__))
from lsp_framing import run_scenario  # noqa: F401 (re-exported for lsp_record_bench.py)


# ── Statistics ────────────────────────────────────────────────────────────────

def compute_stats(values):
    values = sorted(values)
    n = len(values)
    result = {
        'count': n,
        'min': values[0],
        'max': values[-1],
        'mean': statistics.mean(values),
        'median': statistics.median(values),
    }
    if n >= 20:
        result['p95'] = values[int(n * 0.95)]
    return result


# ── ANSI color helpers ────────────────────────────────────────────────────────

def _use_color():
    return sys.stdout.isatty()

def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _use_color() else text

def bold(t): return _c("1", t)
def dim(t):  return _c("2", t)
def cyan(t): return _c("36", t)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Benchmark an LSP server using a JSON message sequence.',
    )
    parser.add_argument('server', help='Path to the server binary')
    parser.add_argument('input', help='JSON message sequence file (input.json)')
    parser.add_argument(
        '--iterations', type=int, default=5,
        help='Number of measured runs (default: 5)',
    )
    parser.add_argument(
        '--warmup', type=int, default=1,
        help='Warmup runs whose timings are discarded (default: 1)',
    )
    parser.add_argument(
        '--methods',
        help='Comma-separated list of methods to include in the report '
             '(default: all except initialize)',
    )
    parser.add_argument(
        '--perf', metavar='FILE', nargs='?', const='perf.data',
        help='Record a single run under perf record, writing profile to FILE '
             '(default: perf.data).  View with: perf report -i FILE',
    )
    args = parser.parse_args()

    with open(args.input) as f:
        messages = json.load(f)

    if args.methods:
        report_methods = set(args.methods.split(','))
    else:
        report_methods = None  # all except initialize

    if args.perf:
        args.warmup = 1
        args.iterations = 1

    total_runs = args.warmup + args.iterations
    all_timings: dict[str, list[float]] = {}

    print(bold(f"\nBenchmarking: {args.server}"))
    print(dim(f"Input:        {args.input}  ({len(messages)} messages)"))
    print(dim(f"Runs:         {args.warmup} warmup + {args.iterations} measured\n"))

    for run_index in range(total_runs):
        is_warmup = run_index < args.warmup
        if is_warmup:
            label = dim(f"  warmup {run_index + 1}/{args.warmup}  ")
        else:
            label = f"  run    {run_index - args.warmup + 1}/{args.iterations}  "

        print(label, end='', flush=True)

        is_perf_run = args.perf and not is_warmup
        try:
            timings = run_scenario(
                args.server, messages,
                perf_output=args.perf if is_perf_run else None,
            )
        except (TimeoutError, EOFError) as exc:
            print(f"ERROR: {exc}")
            sys.exit(1)

        total_ms = sum(t for _, t in timings)
        req_count = len(timings)
        print(f"{total_ms:6.0f}ms  ({req_count} request{'' if req_count == 1 else 's'})")

        if not is_warmup:
            for method, elapsed_ms in timings:
                include = (
                    method != 'initialize'
                    if report_methods is None
                    else method in report_methods
                )
                if include:
                    all_timings.setdefault(method, []).append(elapsed_ms)

    if not all_timings:
        print("\nNo timing data collected.")
        return

    # Report
    col_method = 42
    print()
    print(bold(
        f"  {'Method':<{col_method}}  "
        f"{'n':>4}  {'mean':>8}  {'median':>8}  {'min':>7}  {'max':>7}"
        + ("  " + f"{'p95':>7}" if any(len(v) >= 20 for v in all_timings.values()) else "")
    ))
    print(dim("  " + "─" * (col_method + 46)))

    for method in sorted(all_timings):
        s = compute_stats(all_timings[method])
        p95_col = f"  {s['p95']:6.1f}ms" if 'p95' in s else ""
        padded_method = cyan(method) + " " * max(0, col_method - len(method))
        print(
            f"  {padded_method}  "
            f"{s['count']:>4}  "
            f"{s['mean']:>7.1f}ms  "
            f"{s['median']:>7.1f}ms  "
            f"{s['min']:>5.1f}ms  "
            f"{s['max']:>5.1f}ms"
            f"{p95_col}"
        )

    print()

    if args.perf:
        print(dim(f"  perf data written to: {args.perf}"))
        print(dim(f"  view with:  perf report -i {args.perf}"))
        print(dim(f"  flamegraph: perf script -i {args.perf} | stackcollapse-perf.pl | flamegraph.pl > flame.svg"))
        print()


if __name__ == '__main__':
    main()
