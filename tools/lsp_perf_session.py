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
  hover               textDocument/hover — cursor placed ON documented keywords
  completion          textDocument/completion — mix of keyword, dep-ref, and
                        resource-ref positions to exercise all completion branches
  signature           textDocument/signatureHelp — cursor at end of keyword
                        argument lines to exercise argument-index tracking
  definition          textDocument/definition — cursor on dep-ref paths inside
                        depends/precedes (the only positions that return results)
  references          textDocument/references — cursor on task declaration
                        identifiers (the only positions that return results)
  didchange           textDocument/didChange (notification) — fires once after
                        didOpen to exercise the re-parse / revalidate path
  document-symbol     textDocument/documentSymbol (whole document)
  folding-range       textDocument/foldingRange (whole document)
  semantic-tokens     textDocument/semanticTokens/full (whole document)
  workspace-symbol    workspace/symbol with an empty query string
"""

import argparse
import json
import queue
import random
import re
import statistics
import subprocess
import sys
import threading
import time


DEFAULT_REQUESTS = (
    'semantic-tokens,document-symbol,folding-range,'
    'hover,completion,signature,definition,references,didchange'
)

# Request types that require per-request cursor positions.
POSITION_REQUESTS = {'hover', 'completion', 'signature', 'definition', 'references'}

# All valid request type names.
ALL_REQUEST_TYPES = POSITION_REQUESTS | {
    'document-symbol', 'folding-range', 'semantic-tokens',
    'workspace-symbol', 'didchange',
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


# ── Position samplers ─────────────────────────────────────────────────────────

# Keywords covered by keyword_docs() in hover.c — hover returns docs only for
# these.  Sorted longest-first so the alternation matches greedily.
_HOVER_KW_RE = re.compile(
    r'\b(timingresolution|workinghours|responsible|supplement|'
    r'milestone|timeformat|scheduled|efficiency|precedes|scenario|'
    r'vacation|complete|currency|includes|timezone|maxstart|maxend|'
    r'minstart|minend|priority|duration|allocate|resource|booking|'
    r'project|include|account|depends|length|leaves|effort|shifts|'
    r'shift|flags|macro|start|rate|task|note|end|now)\b'
)

# Lines beginning with a dep-ref keyword (task-ID argument context).
_DEP_KW_RE = re.compile(r'^\s*(depends|precedes)\s+')

# Lines beginning with a resource-ID keyword.
_RESOURCE_KW_RE = re.compile(r'^\s*(allocate|responsible|managers)\s+')

# Lines beginning with an account-ID keyword.
_ACCOUNT_KW_RE = re.compile(r'^\s*(chargeset|balance)\s+')

# Task declaration — capture group 1 is the identifier.
_TASK_DECL_RE = re.compile(r'^\s*task\s+(\w+)')

# Declaration keywords that suppress completion (completion.c line 561-567).
_DECL_SUPPRESSED = frozenset({
    'task', 'project', 'resource', 'account',
    'shift', 'scenario', 'macro', 'include', 'supplement',
})


def _is_comment(line):
    s = line.strip()
    return s.startswith('#') or s.startswith('/*') or s.startswith('*') or s.startswith('//')


def _sample_generic(lines, count, rng):
    """Fallback: end-of-line on non-empty, non-comment lines."""
    candidates = [
        i for i, line in enumerate(lines)
        if line.strip() and not _is_comment(line)
    ]
    if not candidates:
        return [(0, 0)]
    sample_size = min(count, len(candidates))
    return [(ln, len(lines[ln].rstrip())) for ln in rng.sample(candidates, sample_size)]


def sample_positions_hover(lines, count, rng):
    """Positions ON documented keyword tokens.

    hover.c returns documentation only when the cursor sits on a token whose
    token_kind < KW_DOCS_END and whose brace depth matches the cursor depth.
    Placing the cursor at the start of a matched keyword guarantees it lands
    on the token and exercises the doc-returning path.  Lines with no
    documented keyword fall back to the generic sampler.
    """
    candidates = []
    for i, line in enumerate(lines):
        if _is_comment(line):
            continue
        m = _HOVER_KW_RE.search(line)
        if m:
            candidates.append((i, m.start()))
    if not candidates:
        return _sample_generic(lines, count, rng)
    return rng.sample(candidates, min(count, len(candidates)))


def sample_positions_completion(lines, count, rng):
    """Mix of positions covering all four completion branches.

    completion.c has five behaviorally distinct cases at the cursor:
      - Suppressed: line starts with a declaration keyword → returns null
      - Keyword completion: statement start inside a block → keyword list
      - Task-ID completion: inside depends/precedes context → task IDs
      - Resource-ID completion: inside allocate/responsible/managers → resource IDs
      - Account-ID completion: inside chargeset/balance → account IDs

    Positions are sampled proportionally from each non-empty bucket so that
    all branches are exercised, with keyword completion weighted most heavily
    since it is the most common real-world case.

    Bucket weights: keyword 50%, dep-ref 25%, resource-ref 15%, account-ref 10%.
    Lines that trigger completion suppression are excluded.
    """
    kw_lines  = []  # keyword-completion: end-of-line on a non-suppressed line
    dep_lines = []  # task-ID completion: end-of-line on a depends/precedes line
    res_lines = []  # resource-ID completion: end-of-line on allocate/etc. line
    acc_lines = []  # account-ID completion: end-of-line on chargeset/balance line

    for i, line in enumerate(lines):
        if not line.strip() or _is_comment(line):
            continue
        eol = (i, len(line.rstrip()))
        m = _DEP_KW_RE.match(line)
        if m:
            dep_lines.append(eol)
            continue
        m = _RESOURCE_KW_RE.match(line)
        if m:
            res_lines.append(eol)
            continue
        m = _ACCOUNT_KW_RE.match(line)
        if m:
            acc_lines.append(eol)
            continue
        # Exclude declaration-header lines (suppression branch) and bare braces.
        first_word = line.strip().split()[0] if line.strip().split() else ''
        if first_word not in _DECL_SUPPRESSED and first_word not in ('{', '}'):
            kw_lines.append(eol)

    buckets = [
        (kw_lines,  0.50),
        (dep_lines, 0.25),
        (res_lines, 0.15),
        (acc_lines, 0.10),
    ]
    non_empty = [(b, w) for b, w in buckets if b]
    if not non_empty:
        return _sample_generic(lines, count, rng)

    total_w = sum(w for _, w in non_empty)
    result = []
    remaining = count
    for idx, (bucket, weight) in enumerate(non_empty):
        if idx == len(non_empty) - 1:
            n = remaining
        else:
            n = max(1, round(count * weight / total_w))
        n = min(n, len(bucket), remaining)
        if n > 0:
            result.extend(rng.sample(bucket, n))
            remaining -= n
        if remaining <= 0:
            break

    rng.shuffle(result)
    return result if result else _sample_generic(lines, count, rng)


def sample_positions_signature(lines, count, rng):
    """Positions at end-of-line for lines containing a signature-documented keyword.

    signature.c / scan_kw_stack counts argument tokens before the cursor
    (track_argc=1) to determine the active parameter.  Placing the cursor at
    end-of-line ensures all argument tokens on the line are counted, exercising
    the argc > 0 path and parameter highlighting in the response.

    The same 39-keyword set as hover is used since build_signature_help_json()
    and keyword_docs() cover identical keywords.
    """
    candidates = []
    for i, line in enumerate(lines):
        if _is_comment(line):
            continue
        m = _HOVER_KW_RE.search(line)
        if m:
            candidates.append((i, len(line.rstrip())))
    if not candidates:
        return _sample_generic(lines, count, rng)
    return rng.sample(candidates, min(count, len(candidates)))


def sample_positions_definition(lines, count, rng):
    """Positions ON dependency reference paths inside depends/precedes lines.

    handle_definition returns a location only when the cursor falls inside a
    precomputed DefinitionLink source range — i.e., on a dep-ref identifier.
    The cursor is placed at the start of the identifier (after any leading '!'
    bang tokens) to land inside the link's source range.

    Positions on any other line produce a null response and are excluded.
    """
    candidates = []
    for i, line in enumerate(lines):
        m = _DEP_KW_RE.match(line)
        if m:
            rest = line[m.end():]
            # Advance past leading bangs to land on the identifier token.
            bang_count = len(rest) - len(rest.lstrip('!'))
            candidates.append((i, m.end() + bang_count))
    if not candidates:
        return _sample_generic(lines, count, rng)
    return rng.sample(candidates, min(count, len(candidates)))


def sample_positions_references(lines, count, rng):
    """Positions ON task declaration identifiers.

    handle_references returns results only when the cursor falls inside a task
    symbol's selection_range — the identifier token that immediately follows the
    `task` keyword.  The cursor is placed at the first character of that
    identifier so it lands within the selection_range and find_task_at()
    returns a match.

    Positions on any other line produce a null response and are excluded.
    """
    candidates = []
    for i, line in enumerate(lines):
        m = _TASK_DECL_RE.match(line)
        if m:
            candidates.append((i, m.start(1)))
    if not candidates:
        return _sample_generic(lines, count, rng)
    return rng.sample(candidates, min(count, len(candidates)))


def compute_position_sets(tjp_text, request_types, count, rng):
    """Return a dict mapping each position-based request type to its position list."""
    lines = tjp_text.splitlines()
    samplers = {
        'hover':      sample_positions_hover,
        'completion': sample_positions_completion,
        'signature':  sample_positions_signature,
        'definition': sample_positions_definition,
        'references': sample_positions_references,
    }
    return {
        rt: samplers[rt](lines, count, rng)
        for rt in request_types
        if rt in samplers
    }


# ── Session builder ───────────────────────────────────────────────────────────

def build_session(tjp_text, uri, request_types, position_sets, repeat):
    """Return a list of LSP message dicts representing the full session.

    Args:
        tjp_text:      Full text content of the .tjp file.
        uri:           Document URI to use in LSP messages.
        request_types: Ordered list of request type names to include.
        position_sets: Dict mapping each position-based request type to its
                       list of (line, character) tuples.
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

    def text_doc_params():
        return {'textDocument': {'uri': uri}}

    def pos_params(line, character):
        return {'textDocument': {'uri': uri}, 'position': {'line': line, 'character': character}}

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

    # ── didChange warmup (optional, fires once before timed requests) ──────────
    # Exercises handle_didchange: re-parse + revalidate_all_docs.  Sent as a
    # full-sync change with identical content so parse output is reproducible.
    if 'didchange' in request_types:
        messages.append(notif('textDocument/didChange', {
            'textDocument': {'uri': uri, 'version': 2},
            'contentChanges': [{'text': tjp_text}],
        }))

    # ── Timed requests ─────────────────────────────────────────────────────────
    method_map = {
        'hover':      'textDocument/hover',
        'completion': 'textDocument/completion',
        'signature':  'textDocument/signatureHelp',
        'definition': 'textDocument/definition',
        'references': 'textDocument/references',
    }

    for _rep in range(repeat):
        for request_type in request_types:
            if request_type == 'didchange':
                continue  # handled above as a one-time warmup notification
            elif request_type in POSITION_REQUESTS:
                positions = position_sets.get(request_type, [(0, 0)])
                for line, character in positions:
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
  hover               textDocument/hover — cursor ON documented keyword tokens
  completion          textDocument/completion — weighted mix of positions covering
                        keyword, dep-ref, resource-ref, and account-ref branches
  signature           textDocument/signatureHelp — cursor at end of keyword arg lines
  definition          textDocument/definition — cursor on dep-ref paths in depends/precedes
  references          textDocument/references — cursor on task declaration identifiers
  didchange           textDocument/didChange notification (once, before timed requests)
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
        help='Number of cursor positions to sample per position-based request type (default: 5)',
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

    # ── Sample positions (per handler) ────────────────────────────────────────
    rng = random.Random(args.seed)
    position_sets = compute_position_sets(tjp_text, request_types, args.positions, rng)

    # ── Build session ─────────────────────────────────────────────────────────
    messages = build_session(
        tjp_text=tjp_text,
        uri=args.uri,
        request_types=request_types,
        position_sets=position_sets,
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
