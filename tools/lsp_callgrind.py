#!/usr/bin/env python3
"""
Profile a single LSP request using Valgrind Callgrind.

Runs the server under callgrind with a minimal scenario (initialize + didOpen
+ one request), then invokes callgrind_annotate to produce a full call tree
showing every function and its inclusive/exclusive instruction counts.

Usage:
  lsp_callgrind.py <server> <scenario.json> [-o output_dir]

The scenario.json should be a small JSON message sequence (use lsp_log_parse.py
and extract a 3-4 message file: initialize, initialized, didOpen, <one request>).

Output:
  <output_dir>/callgrind.out   - raw callgrind data
  <output_dir>/calltree.txt    - annotated call tree (inclusive costs, callers
                                 and callees for every function)
"""

import argparse
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading


# ── LSP framing (duplicated from lsp_bench.py to keep this script standalone) ─

def frame_message(message):
    body = json.dumps(message, separators=(',', ':')).encode('utf-8')
    header = f"Content-Length: {len(body)}\r\n\r\n".encode('ascii')
    return header + body


def read_message(stream):
    content_length = None
    while True:
        raw_line = stream.readline()
        if not raw_line:
            return None
        line = raw_line.rstrip(b'\r\n')
        if not line:
            break
        if line.lower().startswith(b'content-length:'):
            content_length = int(line.split(b':', 1)[1].strip())
    if content_length is None or content_length <= 0:
        return None
    body = b''
    remaining = content_length
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            return None
        body += chunk
        remaining -= len(chunk)
    return json.loads(body)


def _reader_thread(stdout, q):
    while True:
        msg = read_message(stdout)
        q.put(msg)
        if msg is None:
            break


def run_scenario(server_cmd, messages, response_timeout=60.0):
    """Run the server (given as a list of args) and replay messages."""
    process = subprocess.Popen(
        server_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )
    rq = queue.Queue()
    t = threading.Thread(target=_reader_thread, args=(process.stdout, rq), daemon=True)
    t.start()

    try:
        for msg in messages:
            is_request = 'id' in msg
            process.stdin.write(frame_message(msg))
            process.stdin.flush()
            if not is_request:
                continue
            msg_id = msg['id']
            method = msg['method']
            import time
            deadline = time.monotonic() + response_timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"No response for '{method}' (id={msg_id})")
                try:
                    response = rq.get(timeout=max(0.001, remaining))
                except queue.Empty:
                    raise TimeoutError(f"No response for '{method}' (id={msg_id})")
                if response is None:
                    raise EOFError(f"Server closed while waiting for '{method}'")
                if response.get('id') == msg_id:
                    break
    finally:
        try:
            process.stdin.close()
        except OSError:
            pass
        t.join(timeout=5.0)
        try:
            process.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            process.kill()


def main():
    parser = argparse.ArgumentParser(
        description='Profile a single LSP request with Valgrind Callgrind.',
    )
    parser.add_argument('server', help='Path to the server binary (debug build recommended)')
    parser.add_argument('scenario', help='JSON message sequence (4-message single-request file)')
    parser.add_argument('-o', '--output-dir', default='.',
                        help='Directory for callgrind.out and calltree.txt (default: .)')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    callgrind_out = os.path.join(args.output_dir, 'callgrind.out')
    calltree_out  = os.path.join(args.output_dir, 'calltree.txt')

    with open(args.scenario) as f:
        messages = json.load(f)

    method = next(
        (m.get('method') for m in messages if 'id' in m and m.get('method') != 'initialize'),
        'unknown'
    )
    print(f"Profiling: {method}")
    print(f"Scenario:  {args.scenario} ({len(messages)} messages)")
    print(f"Output:    {args.output_dir}/")
    print()

    # Callgrind options:
    #   --callgrind-out-file   where to write the data
    #   --cache-sim=yes        also collect cache simulation data
    #   --branch-sim=yes       also collect branch prediction data
    #   --collect-atstart=yes  profile everything (no need to toggle)
    #   --fn-skip             skip trivial wrappers to keep tree readable
    callgrind_cmd = [
        'valgrind',
        '--tool=callgrind',
        f'--callgrind-out-file={callgrind_out}',
        '--cache-sim=yes',
        '--branch-sim=yes',
        '--collect-atstart=yes',
        '--instr-atstart=yes',
        args.server,
    ]

    print("Running callgrind (this will be slow — ~20x slower than normal)...")
    try:
        run_scenario(callgrind_cmd, messages)
    except (TimeoutError, EOFError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
    print(f"Callgrind data written to: {callgrind_out}")
    print()

    # Annotate: produce a full call tree with inclusive costs.
    # --tree=both shows callers and callees for each function.
    # --inclusive=yes shows inclusive (subtree) costs.
    # --threshold=0   show all functions, not just top N%.
    print("Generating call tree...")
    annotate_cmd = [
        'callgrind_annotate',
        '--tree=both',
        '--inclusive=yes',
        '--threshold=0',
        '--auto=yes',
        callgrind_out,
    ]
    result = subprocess.run(annotate_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"callgrind_annotate failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    with open(calltree_out, 'w') as f:
        f.write(result.stdout)
    print(f"Call tree written to:       {calltree_out}")
    print()
    print("To browse interactively:  kcachegrind " + callgrind_out)
    print()

    # Print a concise flat summary (top functions by self cost) to stdout.
    _print_flat_summary(result.stdout, method)


def _print_flat_summary(annotate_output, method):
    """Parse callgrind_annotate output and print the top-20 functions by Ir."""
    import re

    # The flat profile section looks like:
    #   Ir        file:function
    # We parse lines matching:  <number> <path>:<funcname>
    flat_re = re.compile(
        r'^\s*([\d,]+)\s+(?:\(\s*[\d.]+%\)\s+)?(\S+):(\S+)\s*$'
    )
    entries = []
    in_flat = False
    for line in annotate_output.splitlines():
        if 'Ir' in line and 'file:function' in line:
            in_flat = True
            continue
        if in_flat:
            m = flat_re.match(line)
            if m:
                ir = int(m.group(1).replace(',', ''))
                filename = os.path.basename(m.group(2))
                func = m.group(3)
                entries.append((ir, filename, func))
            elif line.strip() == '' and entries:
                break  # end of flat section

    if not entries:
        print("(Could not parse flat summary — see calltree.txt for full data)")
        return

    total_ir = entries[0][0] if entries else 1
    print(f"Top functions by instruction count  [{method}]")
    print(f"{'Ir':>14}  {'% of total':>10}  {'file':>20}  function")
    print("─" * 72)
    for ir, fname, func in entries[:25]:
        pct = 100.0 * ir / total_ir if total_ir else 0
        print(f"{ir:>14,}  {pct:>9.1f}%  {fname:>20}  {func}")
    print()


if __name__ == '__main__':
    main()
