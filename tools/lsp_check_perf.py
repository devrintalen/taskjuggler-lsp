#!/usr/bin/env python3
"""
Compare the two most recent benchmark records in benchmarks.jsonl and flag
any regressions that exceed a threshold.

Exit code 0 = no regressions; 1 = at least one regression detected.

Usage:
  lsp_check_perf.py [--threshold PCT] [--baseline N] [FILE]

Options:
  FILE            JSONL file to read (default: benchmarks.jsonl)
  --threshold N   Regression threshold as a percentage (default: 10)
  --baseline N    Which record to use as baseline: 1=latest, 2=second-latest,
                  etc.  Default: compare record[-1] vs record[-2]
  --method M      Only check methods matching this substring
  --fixture F     Only check this fixture name (e.g. flat, highdeps)

Examples:
  # Check for >10% regressions between the last two records
  lsp_check_perf.py

  # Check only documentSymbol for >5% regressions
  lsp_check_perf.py --threshold 5 --method documentSymbol

  # Print a comparison table without failing
  lsp_check_perf.py --threshold 999
"""

import argparse
import json
import sys


def load_records(path):
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def iter_results(record, fixture_filter=None, method_filter=None):
    """Yield (fixture, method, stats_dict) tuples from a record."""
    for fixture, methods in record.get('results', {}).items():
        if fixture_filter and fixture_filter not in fixture:
            continue
        for method, stats in methods.items():
            if method_filter and method_filter not in method:
                continue
            yield fixture, method, stats


def main():
    parser = argparse.ArgumentParser(
        description='Check for performance regressions between benchmark records.',
    )
    parser.add_argument('file', nargs='?', default='benchmarks.jsonl',
                        help='JSONL benchmark file (default: benchmarks.jsonl)')
    parser.add_argument('--threshold', type=float, default=10.0,
                        help='Regression threshold %% (default: 10)')
    parser.add_argument('--method', default=None,
                        help='Filter: only check methods containing this string')
    parser.add_argument('--fixture', default=None,
                        help='Filter: only check this fixture (e.g. flat)')
    args = parser.parse_args()

    try:
        records = load_records(args.file)
    except FileNotFoundError:
        print(f"ERROR: {args.file} not found", file=sys.stderr)
        sys.exit(1)

    if len(records) < 2:
        print(f"Need at least 2 records in {args.file} to compare.")
        sys.exit(0)

    baseline = records[-2]
    current  = records[-1]

    b_commit = baseline.get('commit', '?')
    c_commit = current.get('commit', '?')
    b_dirty  = '*' if baseline.get('dirty') else ''
    c_dirty  = '*' if current.get('dirty') else ''

    print(f"Baseline: {b_commit}{b_dirty}  ({baseline.get('ts', '?')})")
    print(f"Current:  {c_commit}{c_dirty}  ({current.get('ts', '?')})")
    print()

    # Build lookup for baseline
    base_lookup: dict[tuple, dict] = {}
    for fixture, method, stats in iter_results(baseline, args.fixture, args.method):
        base_lookup[(fixture, method)] = stats

    regressions = []
    improvements = []
    unchanged = []

    for fixture, method, cur_stats in iter_results(current, args.fixture, args.method):
        key = (fixture, method)
        if key not in base_lookup:
            continue
        base_stats = base_lookup[key]

        base_p50 = base_stats.get('median', base_stats.get('p50'))
        cur_p50  = cur_stats.get('median', cur_stats.get('p50'))
        if base_p50 is None or cur_p50 is None or base_p50 == 0:
            continue

        pct_change = (cur_p50 - base_p50) / base_p50 * 100.0
        entry = (fixture, method, base_p50, cur_p50, pct_change)

        if pct_change > args.threshold:
            regressions.append(entry)
        elif pct_change < -args.threshold:
            improvements.append(entry)
        else:
            unchanged.append(entry)

    def fmt_entry(e):
        fixture, method, base, cur, pct = e
        short = method.replace('textDocument/', '')
        sign = '+' if pct > 0 else ''
        return (f"  {fixture:<15}  {short:<40}  "
                f"{base:>7.1f}ms → {cur:>7.1f}ms  "
                f"({sign}{pct:.1f}%)")

    if improvements:
        print(f"IMPROVEMENTS (>{args.threshold:.0f}%):")
        for e in sorted(improvements, key=lambda x: x[4]):
            print(fmt_entry(e))
        print()

    if regressions:
        print(f"REGRESSIONS (>{args.threshold:.0f}%):")
        for e in sorted(regressions, key=lambda x: -x[4]):
            print(fmt_entry(e))
        print()
        print(f"  {len(regressions)} regression(s) detected.")
        sys.exit(1)

    if not improvements and not regressions:
        total = len(unchanged)
        print(f"No regressions or improvements (>{args.threshold:.0f}%) across "
              f"{total} method/fixture combinations.")

    sys.exit(0)


if __name__ == '__main__':
    main()
