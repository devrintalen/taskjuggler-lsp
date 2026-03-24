#!/usr/bin/env python3
"""
Parse a callgrind.out file and print a readable flat profile + call tree.

Usage:
  callgrind_parse.py <callgrind.out> [--top N] [--tree FUNCNAME]

--top N      Print the top N functions by self instruction count (default: 40)
--tree FUNC  Print the call tree rooted at FUNC (partial name match)
"""

import argparse
import os
import re
import sys
from collections import defaultdict


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


def func_label(fid, funcs, files, func_file=None):
    name = funcs.get(fid, f'<fn#{fid}>')
    if func_file and fid in func_file and func_file[fid] in files:
        short = os.path.basename(files[func_file[fid]])
        return f'{name}  [{short}]'
    return name


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


def main():
    parser = argparse.ArgumentParser(
        description='Parse callgrind.out and print flat profile + call tree.',
    )
    parser.add_argument('callgrind_out', help='callgrind.out file')
    parser.add_argument('--top', type=int, default=40,
                        help='Number of functions in flat profile (default: 40)')
    parser.add_argument('--tree', metavar='FUNC',
                        help='Print call tree rooted at this function (partial name match)')
    args = parser.parse_args()

    files, funcs, self_ir, call_ir, call_cnt = parse_callgrind(args.callgrind_out)

    total = sum(self_ir.values())
    print(f"\nTotal instructions: {total:,}\n")
    print(f"Flat profile — top {args.top} by self (exclusive) instruction count\n")
    print_flat(funcs, files, self_ir, call_ir, args.top)

    if args.tree:
        matches = find_func(args.tree, funcs)
        if not matches:
            print(f"No function matching '{args.tree}' found.", file=sys.stderr)
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
        # Default: show tree for the server_process entry point or top handler
        for target in ('server_process', 'handle_completion', 'handle_hover',
                       'handle_didchange', 'handle_didopen'):
            matches = find_func(target, funcs)
            if matches:
                root_id = matches[0][0]
                print(f"Call tree: {funcs[root_id]}\n")
                print_call_tree(root_id, funcs, self_ir, call_ir, call_cnt)
                break
    print()


if __name__ == '__main__':
    main()
