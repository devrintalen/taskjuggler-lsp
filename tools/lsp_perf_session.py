#!/usr/bin/env python3
"""
lsp_perf_session.py — Build a simulated LSP session from a TJP file.

Generates a JSON message sequence (compatible with lsp_bench.py) from a
TaskJuggler .tjp file, optionally running it directly against a server.

Usage:
  # Generate a session file for use with lsp_bench.py:
  lsp_perf_session.py project.tjp --output session.json
  python3 tools/lsp_bench.py ./taskjuggler-lsp session.json

  # Run directly against the server with timing output:
  lsp_perf_session.py project.tjp --run ./taskjuggler-lsp

  # Choose specific request types:
  lsp_perf_session.py project.tjp --requests semantic-tokens,document-symbol

  # Sample more cursor positions and repeat each request multiple times:
  lsp_perf_session.py project.tjp --positions 10 --repeat 3 --run ./taskjuggler-lsp

Request types (--requests):
  hover               textDocument/hover at sampled positions
  completion          textDocument/completion at sampled positions
  signature           textDocument/signatureHelp at sampled positions
  definition          textDocument/definition at sampled positions
  references          textDocument/references at sampled positions
  document-symbol     textDocument/documentSymbol (whole document)
  folding-range       textDocument/foldingRange (whole document)
  semantic-tokens     textDocument/semanticTokens/full (whole document)
  workspace-symbol    workspace/symbol with an empty query string
"""

import argparse
import json
import queue
import random
import statistics
import subprocess
import sys
import threading
import time


DEFAULT_REQUESTS = 'semantic-tokens,document-symbol,folding-range,hover,completion'

# Request types that require a cursor position in the document.
POSITION_REQUESTS = {'hover', 'completion', 'signature', 'definition', 'references'}

# All valid request type names.
ALL_REQUEST_TYPES = POSITION_REQUESTS | {
    'document-symbol', 'folding-range', 'semantic-tokens', 'workspace-symbol',
}


# ── LSP framing ───────────────────────────────────────────────────────────────

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


# ── Session builder ───────────────────────────────────────────────────────────

def sample_positions(text, count, rng):
    """Return up to `count` (line, 0) positions from non-empty, non-comment lines."""
    candidate_lines = [
        i for i, line in enumerate(text.splitlines())
        if line.strip() and not line.strip().startswith('/*') and not line.strip().startswith('*')
    ]
    if not candidate_lines:
        return [(0, 0)]
    sample_size = min(count, len(candidate_lines))
    return [(line_num, 0) for line_num in rng.sample(candidate_lines, sample_size)]


def build_session(tjp_text, uri, request_types, positions, repeat):
    """Return a list of LSP message dicts representing the full session.

    Args:
        tjp_text:      Full text content of the .tjp file.
        uri:           Document URI to use in LSP messages.
        request_types: Ordered list of request type names to include.
        positions:     List of (line, character) tuples for position-based requests.
        repeat:        Number of times to repeat each request.
    """
    messages = []
    next_id = [1]

    def req(method, params):
        msg = {'jsonrpc': '2.0', 'id': next_id[0], 'method': method, 'params': params}
        next_id[0] += 1
        return msg

    def notif(method, params):
        return {'jsonrpc': '2.0', 'method': method, 'params': params}

    # ── Lifecycle ──────────────────────────────────────────────────────────────
    messages.append(req('initialize', {
        'processId': None,
        'rootUri': None,
        'capabilities': {},
    }))
    messages.append(notif('initialized', {}))
    messages.append(notif('textDocument/didOpen', {
        'textDocument': {
            'uri': uri,
            'languageId': 'taskjuggler',
            'version': 1,
            'text': tjp_text,
        },
    }))

    # ── Requests ───────────────────────────────────────────────────────────────
    def text_doc_params():
        return {'textDocument': {'uri': uri}}

    def pos_params(line, character):
        return {'textDocument': {'uri': uri}, 'position': {'line': line, 'character': character}}

    for _rep in range(repeat):
        for request_type in request_types:
            if request_type in POSITION_REQUESTS:
                for line, character in positions:
                    method_map = {
                        'hover':      'textDocument/hover',
                        'completion': 'textDocument/completion',
                        'signature':  'textDocument/signatureHelp',
                        'definition': 'textDocument/definition',
                        'references': 'textDocument/references',
                    }
                    messages.append(req(method_map[request_type], pos_params(line, character)))
            elif request_type == 'document-symbol':
                messages.append(req('textDocument/documentSymbol', text_doc_params()))
            elif request_type == 'folding-range':
                messages.append(req('textDocument/foldingRange', text_doc_params()))
            elif request_type == 'semantic-tokens':
                messages.append(req('textDocument/semanticTokens/full', text_doc_params()))
            elif request_type == 'workspace-symbol':
                messages.append(req('workspace/symbol', {'query': ''}))

    # ── Shutdown ───────────────────────────────────────────────────────────────
    messages.append(req('shutdown', {}))

    return messages


# ── Server runner (for --run mode) ────────────────────────────────────────────

def _reader_thread(stdout, q):
    """Background thread: push every server message onto q, then push None at EOF."""
    while True:
        msg = read_message(stdout)
        q.put(msg)
        if msg is None:
            break


def run_scenario(server_binary, messages, response_timeout=60.0):
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
                process.stdin.write(frame_message(msg))
                process.stdin.flush()
                continue

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


# ── Statistics & display ──────────────────────────────────────────────────────

def _use_color():
    return sys.stderr.isatty()


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _use_color() else text


def bold(t): return _c("1", t)
def dim(t):  return _c("2", t)
def cyan(t): return _c("36", t)


def print_timings(timings, tjp_path, request_types):
    """Print a per-method timing table to stderr."""
    by_method: dict[str, list[float]] = {}
    for method, elapsed_ms in timings:
        by_method.setdefault(method, []).append(elapsed_ms)

    # Exclude initialize and shutdown from the summary.
    for skip in ('initialize', 'shutdown'):
        by_method.pop(skip, None)

    print(file=sys.stderr)
    print(bold(f"  File:     {tjp_path}"), file=sys.stderr)
    print(dim(f"  Requests: {', '.join(request_types)}"), file=sys.stderr)
    print(file=sys.stderr)

    if not by_method:
        print("  No request timings collected.", file=sys.stderr)
        return

    col_method = 42
    header = (
        f"  {'Method':<{col_method}}  "
        f"{'n':>4}  {'mean':>8}  {'median':>8}  {'min':>7}  {'max':>7}"
    )
    print(bold(header), file=sys.stderr)
    print(dim("  " + "─" * (col_method + 46)), file=sys.stderr)

    for method in sorted(by_method):
        values = sorted(by_method[method])
        n = len(values)
        mean = statistics.mean(values)
        median = statistics.median(values)
        min_val = values[0]
        max_val = values[-1]
        padded = cyan(method) + " " * max(0, col_method - len(method))
        print(
            f"  {padded}  "
            f"{n:>4}  "
            f"{mean:>7.1f}ms  "
            f"{median:>7.1f}ms  "
            f"{min_val:>5.1f}ms  "
            f"{max_val:>5.1f}ms",
            file=sys.stderr,
        )

    print(file=sys.stderr)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Build a simulated LSP session from a TJP file.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Request types:
  hover               textDocument/hover at sampled positions
  completion          textDocument/completion at sampled positions
  signature           textDocument/signatureHelp at sampled positions
  definition          textDocument/definition at sampled positions
  references          textDocument/references at sampled positions
  document-symbol     textDocument/documentSymbol (whole document)
  folding-range       textDocument/foldingRange (whole document)
  semantic-tokens     textDocument/semanticTokens/full (whole document)
  workspace-symbol    workspace/symbol with an empty query string

Examples:
  # Write session JSON for use with lsp_bench.py:
  lsp_perf_session.py project.tjp --output session.json
  python3 tools/lsp_bench.py ./taskjuggler-lsp session.json --iterations 10

  # Quick single-shot run with timing output:
  lsp_perf_session.py project.tjp --run ./taskjuggler-lsp

  # Stress-test specific methods with many repetitions:
  lsp_perf_session.py project.tjp --requests hover,completion --positions 20 --repeat 5 --run ./taskjuggler-lsp
""",
    )
    parser.add_argument('tjp_file', help='Path to the .tjp file to load')
    parser.add_argument(
        '--requests', default=DEFAULT_REQUESTS,
        metavar='LIST',
        help=(
            'Comma-separated list of request types to include '
            f'(default: {DEFAULT_REQUESTS})'
        ),
    )
    parser.add_argument(
        '--positions', type=int, default=5,
        metavar='N',
        help='Number of random cursor positions to sample for position-based requests (default: 5)',
    )
    parser.add_argument(
        '--repeat', type=int, default=1,
        metavar='N',
        help='Repeat each request N times (default: 1)',
    )
    parser.add_argument(
        '--uri', default='file:///perf/test.tjp',
        help='Document URI to use in LSP messages (default: file:///perf/test.tjp)',
    )
    parser.add_argument(
        '--seed', type=int, default=42,
        help='RNG seed for position sampling (default: 42)',
    )
    parser.add_argument(
        '--output', metavar='FILE',
        help='Write JSON message sequence to FILE (default: stdout)',
    )
    parser.add_argument(
        '--run', metavar='SERVER',
        help='Run the session against SERVER and print per-method timings',
    )
    args = parser.parse_args()

    # ── Validate request types ────────────────────────────────────────────────
    request_types = [r.strip() for r in args.requests.split(',') if r.strip()]
    unknown = [r for r in request_types if r not in ALL_REQUEST_TYPES]
    if unknown:
        parser.error(
            f"Unknown request type(s): {', '.join(unknown)}\n"
            f"Valid types: {', '.join(sorted(ALL_REQUEST_TYPES))}"
        )

    if args.positions < 1:
        parser.error("--positions must be >= 1")
    if args.repeat < 1:
        parser.error("--repeat must be >= 1")

    # ── Read TJP file ─────────────────────────────────────────────────────────
    try:
        with open(args.tjp_file) as f:
            tjp_text = f.read()
    except OSError as exc:
        print(f"Error reading {args.tjp_file}: {exc}", file=sys.stderr)
        sys.exit(1)

    # ── Sample positions ──────────────────────────────────────────────────────
    rng = random.Random(args.seed)
    positions = sample_positions(tjp_text, args.positions, rng)

    # ── Build session ─────────────────────────────────────────────────────────
    messages = build_session(
        tjp_text=tjp_text,
        uri=args.uri,
        request_types=request_types,
        positions=positions,
        repeat=args.repeat,
    )

    # ── Output JSON ───────────────────────────────────────────────────────────
    json_output = json.dumps(messages, indent=2)

    if args.output:
        try:
            with open(args.output, 'w') as f:
                f.write(json_output)
                f.write('\n')
        except OSError as exc:
            print(f"Error writing {args.output}: {exc}", file=sys.stderr)
            sys.exit(1)
        print(
            dim(f"  Wrote {len(messages)} messages to {args.output}"),
            file=sys.stderr,
        )
    elif not args.run:
        # Default: print JSON to stdout so the output can be piped or redirected.
        print(json_output)

    # ── Run against server ────────────────────────────────────────────────────
    if args.run:
        print(
            dim(f"  Running {len(messages)} messages against {args.run} ..."),
            file=sys.stderr,
        )
        try:
            timings = run_scenario(args.run, messages)
        except (TimeoutError, EOFError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            sys.exit(1)
        print_timings(timings, args.tjp_file, request_types)


if __name__ == '__main__':
    main()
