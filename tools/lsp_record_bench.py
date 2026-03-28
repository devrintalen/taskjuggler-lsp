#!/usr/bin/env python3
"""
Record a benchmark run and append a JSON line to benchmarks.jsonl.

Runs lsp_bench's run_scenario against every test/session_*.json fixture and
the corresponding test/session_*_warm.json warm-cache variants, collects p50,
p95, min, max per method, and writes one JSONL record containing the git
commit, timestamp, and all results.

Usage:
  lsp_record_bench.py [--server PATH] [--iterations N] [--warmup N] [--output FILE]

Options:
  --server PATH    Server binary to benchmark (default: ./taskjuggler-lsp)
  --iterations N   Measured runs per fixture (default: 7)
  --warmup N       Warmup runs per fixture, discarded (default: 2)
  --output FILE    JSONL file to append to (default: benchmarks.jsonl)
  --dry-run        Print what would be recorded without writing
"""

import argparse
import datetime
import json
import os
import socket
import subprocess
import sys
from pathlib import Path

# Allow importing from the same tools/ directory
sys.path.insert(0, os.path.dirname(__file__))
from lsp_bench import run_scenario, compute_stats


FIXTURES = ['flat', 'wide', 'balanced', 'highdeps', 'deep']
SKIP_METHODS = {'initialize', 'shutdown', 'exit'}

# Methods treated as whole-document (1 sample/run); all others are per-position.
WHOLE_DOC_METHODS = {
    'textDocument/semanticTokens/full',
    'textDocument/documentSymbol',
    'textDocument/foldingRange',
}


def git_info():
    """Return (commit_hash, is_dirty) for the current repo."""
    try:
        commit = subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        status = subprocess.check_output(
            ['git', 'status', '--porcelain'],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        return commit, bool(status)
    except Exception:
        return 'unknown', False


def run_fixture(server, session_file, iterations, warmup, label):
    """Run a session file and return {method: stats_dict}."""
    with open(session_file) as f:
        messages = json.load(f)

    all_timings: dict[str, list[float]] = {}
    total_runs = warmup + iterations

    print(f"  {label} ({iterations} runs + {warmup} warmup) ...", flush=True)

    for run_index in range(total_runs):
        is_warmup = run_index < warmup
        timings = run_scenario(server, messages)
        if not is_warmup:
            for method, ms in timings:
                if method not in SKIP_METHODS:
                    all_timings.setdefault(method, []).append(ms)

    return {method: compute_stats(vals) for method, vals in all_timings.items()}


def main():
    parser = argparse.ArgumentParser(
        description='Record a benchmark run into benchmarks.jsonl.',
    )
    parser.add_argument('--server', default='./taskjuggler-lsp',
                        help='Server binary (default: ./taskjuggler-lsp)')
    parser.add_argument('--iterations', type=int, default=7,
                        help='Measured runs per fixture (default: 7)')
    parser.add_argument('--warmup', type=int, default=2,
                        help='Warmup runs per fixture (default: 2)')
    parser.add_argument('--output', default='benchmarks.jsonl',
                        help='JSONL output file (default: benchmarks.jsonl)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Print the record without writing it')
    args = parser.parse_args()

    if not os.path.exists(args.server):
        print(f"ERROR: server not found: {args.server}", file=sys.stderr)
        sys.exit(1)

    commit, dirty = git_info()
    record = {
        'ts':         datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'commit':     commit,
        'dirty':      dirty,
        'host':       socket.gethostname(),
        'server':     args.server,
        'iterations': args.iterations,
        'warmup':     args.warmup,
        'results':    {},
    }

    test_dir = Path(__file__).parent.parent / 'test'

    for fixture in FIXTURES:
        session = test_dir / f'session_{fixture}.json'
        if not session.exists():
            print(f"  SKIP {fixture}: {session} not found")
            continue

        stats = run_fixture(args.server, session, args.iterations, args.warmup,
                            fixture)
        record['results'][fixture] = {
            method: {k: round(v, 3) for k, v in s.items()}
            for method, s in stats.items()
        }

        warm_session = test_dir / f'session_{fixture}_warm.json'
        if warm_session.exists():
            warm_stats = run_fixture(args.server, warm_session, args.iterations,
                                     args.warmup, f'{fixture}_warm')
            record['results'][f'{fixture}_warm'] = {
                method: {k: round(v, 3) for k, v in s.items()}
                for method, s in warm_stats.items()
            }

    line = json.dumps(record, separators=(',', ':'))

    if args.dry_run:
        print('\nWould write to', args.output, ':')
        print(json.dumps(record, indent=2))
        return

    with open(args.output, 'a') as f:
        f.write(line + '\n')

    print(f"\nAppended to {args.output}  (commit={commit}{'*' if dirty else ''})")
    _print_summary(record)


def _print_summary(record):
    """Print a compact table of median times from the record."""
    results = record['results']
    fixtures = [f for f in FIXTURES if f in results]
    if not fixtures:
        return

    # Collect all methods seen across fixtures
    all_methods: set[str] = set()
    for fixture in fixtures:
        all_methods.update(results[fixture].keys())
    all_methods -= SKIP_METHODS

    # Print cold-cache table
    print(f"\n  {'Method':<45} " + "  ".join(f"{f:>10}" for f in fixtures))
    print("  " + "-" * (45 + 13 * len(fixtures)))
    for method in sorted(all_methods):
        short = method.replace('textDocument/', '')
        row = f"  {short:<45}"
        for fixture in fixtures:
            s = results.get(fixture, {}).get(method)
            if s:
                row += f"  {s['median']:>9.1f}ms"
            else:
                row += f"  {'—':>10}"
        print(row)

    # Print warm-cache table if any warm results exist
    warm_fixtures = [f'{f}_warm' for f in fixtures if f'{f}_warm' in results]
    if warm_fixtures:
        print(f"\n  Warm-cache (documentSymbol):")
        for fixture in warm_fixtures:
            s = results[fixture].get('textDocument/documentSymbol')
            if s:
                label = fixture.replace('_warm', '')
                print(f"    {label:<10}  p50={s['median']:.1f}ms  "
                      f"p95={s.get('p95', float('nan')):.1f}ms  "
                      f"min={s['min']:.1f}ms")


if __name__ == '__main__':
    main()
