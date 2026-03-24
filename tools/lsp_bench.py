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
"""

import argparse
import json
import queue
import statistics
import subprocess
import sys
import threading
import time


# ── LSP framing ──────────────────────────────────────────────────────────────

def frame_message(message):
    """Encode a dict as a Content-Length-framed LSP message (bytes)."""
    body = json.dumps(message, separators=(',', ':')).encode('utf-8')
    header = f"Content-Length: {len(body)}\r\n\r\n".encode('ascii')
    return header + body


def read_message(stream):
    """Read one LSP-framed message from a binary stream.

    Returns the parsed dict, or None on EOF/error.
    """
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


# ── Server runner ─────────────────────────────────────────────────────────────

def _reader_thread(stdout, q):
    """Background thread: push every server message onto q, then push None at EOF."""
    while True:
        msg = read_message(stdout)
        q.put(msg)
        if msg is None:
            break


def run_scenario(server_binary, messages, response_timeout=30.0):
    """Start the server, replay messages, and return timing data.

    Returns a list of (method, elapsed_ms) for every request that received
    a response.
    """
    process = subprocess.Popen(
        [server_binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )

    response_queue = queue.Queue()
    reader = threading.Thread(
        target=_reader_thread,
        args=(process.stdout, response_queue),
        daemon=True,
    )
    reader.start()

    timings = []

    try:
        for msg in messages:
            is_request = 'id' in msg

            if not is_request:
                # Notification: fire and forget.
                process.stdin.write(frame_message(msg))
                process.stdin.flush()
                continue

            # Request: send, then drain the response queue until we see the
            # matching response.  Server-push notifications (e.g.
            # publishDiagnostics) that arrive in between are silently skipped.
            msg_id = msg['id']
            method = msg['method']

            start = time.perf_counter()
            process.stdin.write(frame_message(msg))
            process.stdin.flush()

            deadline = time.monotonic() + response_timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        f"No response for '{method}' (id={msg_id}) "
                        f"within {response_timeout}s"
                    )
                try:
                    response = response_queue.get(timeout=max(0.001, remaining))
                except queue.Empty:
                    raise TimeoutError(
                        f"No response for '{method}' (id={msg_id}) "
                        f"within {response_timeout}s"
                    )
                if response is None:
                    raise EOFError(
                        f"Server closed connection while waiting for "
                        f"'{method}' (id={msg_id})"
                    )
                if response.get('id') == msg_id:
                    elapsed_ms = (time.perf_counter() - start) * 1000
                    timings.append((method, elapsed_ms))
                    break
                # Otherwise it's a server-initiated notification or a response
                # for a different id; keep reading.

    finally:
        try:
            process.stdin.close()
        except OSError:
            pass
        reader.join(timeout=5.0)
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()

    return timings


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
    args = parser.parse_args()

    with open(args.input) as f:
        messages = json.load(f)

    if args.methods:
        report_methods = set(args.methods.split(','))
    else:
        report_methods = None  # all except initialize

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

        try:
            timings = run_scenario(args.server, messages)
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


if __name__ == '__main__':
    main()
