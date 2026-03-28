#!/usr/bin/env python3
"""
Profile and analyze LSP requests using Valgrind Callgrind.

Two modes, selected by the input file extension:

  Profile mode (.json input):
    Run the server under Callgrind with a scenario file, write callgrind.out
    and calltree.txt to an output directory, then print a flat profile and
    call tree.

  Parse mode (.out input):
    Parse an existing callgrind.out file and print a flat profile and/or
    call tree.  No server execution.

Usage:
  # Profile a scenario and print analysis:
  callgrind.py <server> <scenario.json> [-o output_dir] [--top N] [--tree FUNC]

  # Analyse an existing callgrind output file:
  callgrind.py <callgrind.out> [--top N] [--tree FUNC]

Options (both modes):
  --top N       Print the top N functions by self instruction count (default: 40)
  --tree FUNC   Print the call tree rooted at this function (partial name match)

Profile-mode options:
  -o DIR        Directory for callgrind.out and calltree.txt (default: .)

Examples:
  # Generate a scenario then profile it:
  python3 tools/lsp_perf_session.py test/perf_flat.tjp \\
      --requests completion --positions 1 --repeat 1 \\
      --output test/scenarios/flat_completion.json

  python3 tools/callgrind.py ./taskjuggler-lsp-debug \\
      test/scenarios/flat_completion.json \\
      -o test/callgrind/flat_completion/

  # Analyse a previously recorded file:
  python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
  python3 tools/callgrind.py test/callgrind/flat_completion/callgrind.out \\
      --tree handle_completion
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

# Allow importing from the same tools/ directory
sys.path.insert(0, os.path.dirname(__file__))
from lsp_framing import run_scenario


# ── Callgrind output parser ───────────────────────────────────────────────────

def parse_callgrind(path):
    """
    Parse a callgrind data file.

    Callgrind format summary:
      fl=(N) /path         define file label N
      fn=(N) name          define function label N, start its section
      cfn=(N) [name]       callee function label
      calls=COUNT lineno   next * line is the inclusive cost of this call site
      * Ir Dr Dw ...       aggregate (inclusive) cost of a call
      lineno Ir Dr Dw ...  self cost at this line (no leading *)

    Returns:
      files    : {id: path}
      funcs    : {id: name}
      self_ir  : {func_id: int}   exclusive instruction count
      call_ir  : {(caller_id, callee_id): int}   total inclusive Ir across all sites
      call_cnt : {(caller_id, callee_id): int}   total call count
    """
    files    = {}
    funcs    = {}
    self_ir  = defaultdict(int)
    call_ir  = defaultdict(int)
    call_cnt = defaultdict(int)

    current_func   = None
    pending_callee = None   # set after seeing cfn=, cleared after seeing * line
    pending_calls  = 0

    with open(path) as f:
        lines = f.readlines()

    for line in lines:
        line = line.rstrip('\n')

        # ── File label ────────────────────────────────────────────────────
        m = re.match(r'^(?:fl|fe|cfi|cob)=\((\d+)\)\s*(.*)', line)
        if m:
            label = int(m.group(1))
            p = m.group(2).strip()
            if p:
                files[label] = p
            continue

        # ── Function label ────────────────────────────────────────────────
        m = re.match(r'^fn=\((\d+)\)\s*(.*)', line)
        if m:
            label = int(m.group(1))
            name = m.group(2).strip()
            if name:
                funcs[label] = name
            current_func   = label
            pending_callee = None
            continue

        # ── Callee function label ─────────────────────────────────────────
        m = re.match(r'^cfn=\((\d+)\)\s*(.*)', line)
        if m:
            label = int(m.group(1))
            name = m.group(2).strip()
            if name:
                funcs[label] = name
            pending_callee = label
            pending_calls  = 0
            continue

        # ── calls=N lineno ────────────────────────────────────────────────
        m = re.match(r'^calls=(\d+)', line)
        if m:
            pending_calls = int(m.group(1))
            continue

        # ── Aggregate call cost: * Ir Dr Dw ... ──────────────────────────
        if line.startswith('* ') or line == '*':
            if current_func is not None and pending_callee is not None:
                parts = line[2:].split()
                ir = int(parts[0]) if parts else 0
                key = (current_func, pending_callee)
                call_ir[key]  += ir
                call_cnt[key] += pending_calls
            # Do NOT clear pending_callee yet — multiple * lines can follow
            # one calls= entry (e.g. for inlined call sites).
            continue

        # ── Self cost line: lineno|+offset|-offset Ir Dr Dw ... ──────────
        # A self cost line resets the pending_callee.
        m = re.match(r'^[+\-]?\d+\s+(\d)', line)
        if m and current_func is not None:
            parts = line.split()
            try:
                ir = int(parts[1])
                self_ir[current_func] += ir
            except (ValueError, IndexError):
                pass
            pending_callee = None
            continue

    return files, funcs, self_ir, call_ir, call_cnt


def compute_inclusive(func_ids, self_ir, call_ir):
    """
    Compute inclusive cost for every function using the call graph.
    For each function: inclusive = self + sum(call_ir for all outgoing edges).
    (call_ir already stores the inclusive cost of each callee sub-tree as
    reported by callgrind, so we don't recurse.)
    """
    inclusive = dict(self_ir)
    for (caller, callee), ir in call_ir.items():
        inclusive[caller] = inclusive.get(caller, 0) + ir
    return inclusive


def print_flat(funcs, files, self_ir, call_ir, top_n):
    inclusive = compute_inclusive(set(self_ir) | {c for c, _ in call_ir},
                                  self_ir, call_ir)
    total = sum(self_ir.values())

    all_funcs = set(self_ir) | {c for _, c in call_ir} | {c for c, _ in call_ir}
    ranked = sorted(all_funcs,
                    key=lambda f: self_ir.get(f, 0),
                    reverse=True)

    print(f"  {'Self Ir':>12}  {'%':>6}  {'Inclusive Ir':>14}  Function")
    print("  " + "─" * 76)
    for fid in ranked[:top_n]:
        s = self_ir.get(fid, 0)
        inc = inclusive.get(fid, 0)
        pct = 100.0 * s / total if total else 0
        label = funcs.get(fid, f'<fn#{fid}>')
        print(f"  {s:>12,}  {pct:>5.1f}%  {inc:>14,}  {label}")
    print()


def print_call_tree(root_id, funcs, self_ir, call_ir, call_cnt,
                    depth=0, max_depth=6, visited=None, prefix=''):
    if visited is None:
        visited = set()
    self_cost = self_ir.get(root_id, 0)
    name = funcs.get(root_id, f'<fn#{root_id}>')
    print(f"{prefix}{name}  (self: {self_cost:,})")
    if root_id in visited or depth >= max_depth:
        if root_id in visited:
            print(f"{prefix}  [recursive — skipped]")
        return
    visited = visited | {root_id}

    callees = [(callee, call_ir[(root_id, callee)], call_cnt[(root_id, callee)])
               for callee in {c for (r, c) in call_ir if r == root_id}]
    callees.sort(key=lambda x: x[1], reverse=True)

    for i, (callee, ir, cnt) in enumerate(callees):
        is_last = (i == len(callees) - 1)
        branch = '└─' if is_last else '├─'
        child_prefix = prefix + ('   ' if is_last else '│  ')
        callee_name = funcs.get(callee, f'<fn#{callee}>')
        print(f"{prefix}{branch} [{ir:,} Ir, {cnt}×]  {callee_name}")
        print_call_tree(callee, funcs, self_ir, call_ir, call_cnt,
                        depth + 1, max_depth, visited, child_prefix)


def find_func(name_fragment, funcs):
    frag = name_fragment.lower()
    return [(fid, name) for fid, name in funcs.items() if frag in name.lower()]


def analyse(callgrind_out, top_n, tree_func):
    """Parse callgrind_out and print flat profile and/or call tree."""
    import json as _json  # only used for total formatting
    files, funcs, self_ir, call_ir, call_cnt = parse_callgrind(callgrind_out)

    total = sum(self_ir.values())
    print(f"\nTotal instructions: {total:,}\n")
    print(f"Flat profile — top {top_n} by self (exclusive) instruction count\n")
    print_flat(funcs, files, self_ir, call_ir, top_n)

    if tree_func:
        matches = find_func(tree_func, funcs)
        if not matches:
            print(f"No function matching '{tree_func}' found.", file=sys.stderr)
            print("Named functions:", file=sys.stderr)
            for fid, name in sorted(funcs.items(), key=lambda x: x[1])[:40]:
                if not name.startswith('0x'):
                    print(f"  {name}", file=sys.stderr)
            sys.exit(1)
        matches.sort(key=lambda x: self_ir.get(x[0], 0), reverse=True)
        root_id, root_name = matches[0]
        print(f"Call tree: {root_name}\n")
        print_call_tree(root_id, funcs, self_ir, call_ir, call_cnt)
    else:
        # Default: show tree for the top server handler
        for target in ('server_process', 'handle_completion', 'handle_hover',
                       'handle_didchange', 'handle_didopen'):
            matches = find_func(target, funcs)
            if matches:
                root_id = matches[0][0]
                print(f"Call tree: {funcs[root_id]}\n")
                print_call_tree(root_id, funcs, self_ir, call_ir, call_cnt)
                break
    print()


# ── Profile mode ──────────────────────────────────────────────────────────────

def profile(server, scenario_json, output_dir, top_n, tree_func):
    """Run the server under Callgrind, write output files, then analyse."""
    import json

    os.makedirs(output_dir, exist_ok=True)
    callgrind_out = os.path.join(output_dir, 'callgrind.out')
    calltree_out  = os.path.join(output_dir, 'calltree.txt')

    with open(scenario_json) as f:
        messages = json.load(f)

    method = next(
        (m.get('method') for m in messages if 'id' in m and m.get('method') != 'initialize'),
        'unknown'
    )
    print(f"Profiling: {method}")
    print(f"Scenario:  {scenario_json} ({len(messages)} messages)")
    print(f"Output:    {output_dir}/")
    print()

    callgrind_cmd = [
        'valgrind',
        '--tool=callgrind',
        f'--callgrind-out-file={callgrind_out}',
        '--cache-sim=yes',
        '--branch-sim=yes',
        '--collect-atstart=yes',
        '--instr-atstart=yes',
        server,
    ]

    print("Running callgrind (this will be slow — ~20x slower than normal)...")
    try:
        run_scenario(callgrind_cmd, messages, response_timeout=600.0)
    except (TimeoutError, EOFError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
    print(f"Callgrind data written to: {callgrind_out}")
    print()

    # Also run callgrind_annotate to produce a human-readable call tree file.
    print("Generating annotated call tree...")
    annotate_cmd = [
        'callgrind_annotate',
        '--tree=both',
        '--inclusive=yes',
        '--threshold=0',
        '--auto=yes',
        callgrind_out,
    ]
    result = subprocess.run(annotate_cmd, capture_output=True, text=True)
    if result.returncode == 0:
        with open(calltree_out, 'w') as f:
            f.write(result.stdout)
        print(f"Annotated tree written to:  {calltree_out}")
    else:
        print(f"callgrind_annotate failed (non-fatal):\n{result.stderr}", file=sys.stderr)
    print()
    print("To browse interactively:  kcachegrind " + callgrind_out)
    print()

    analyse(callgrind_out, top_n, tree_func)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Profile and analyze LSP requests using Valgrind Callgrind.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Profile a scenario:
  callgrind.py ./taskjuggler-lsp-debug test/scenarios/flat_completion.json \\
      -o test/callgrind/flat_completion/ --top 30 --tree handle_completion

  # Analyse an existing output:
  callgrind.py test/callgrind/flat_completion/callgrind.out --top 30
  callgrind.py test/callgrind/flat_completion/callgrind.out --tree handle_completion
""",
    )

    # Positional args vary by mode; detect by checking the first non-option arg.
    parser.add_argument('args', nargs='+',
                        help='[server scenario.json] OR [callgrind.out]')
    parser.add_argument('-o', '--output-dir', default='.',
                        help='Output directory for profile mode (default: .)')
    parser.add_argument('--top', type=int, default=40,
                        help='Number of functions in flat profile (default: 40)')
    parser.add_argument('--tree', metavar='FUNC',
                        help='Print call tree rooted at this function (partial name match)')

    args = parser.parse_args()

    positional = args.args

    # Detect mode: parse mode if single .out file, profile mode if server + .json
    if len(positional) == 1 and positional[0].endswith('.out'):
        analyse(positional[0], args.top, args.tree)
    elif len(positional) >= 2 and positional[1].endswith('.json'):
        profile(positional[0], positional[1], args.output_dir, args.top, args.tree)
    else:
        parser.error(
            "Provide either:\n"
            "  callgrind.py <server> <scenario.json> [-o dir]\n"
            "  callgrind.py <callgrind.out>"
        )


if __name__ == '__main__':
    main()
