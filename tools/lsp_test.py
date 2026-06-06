#!/usr/bin/env python3
"""
LSP golden-file test harness for taskjuggler-lsp.

Each test case is a directory containing:
  input.json    - JSON array of LSP message objects to send to the server
  expected.json - JSON array of LSP messages received from the server (golden)

Usage:
  lsp_test.py [--record] <server> <case_dir> [<case_dir> ...]
  lsp_test.py [--record] <server> --all <cases_root>

Modes:
  (default)  Compare server output against expected.json; report pass/fail.
  --record   Run the server and write its output to expected.json.
"""

import argparse
import difflib
import json
import os
import shutil
import subprocess
import sys
import threading


# ── ANSI color helpers ────────────────────────────────────────────────────────

def _use_color():
    return sys.stdout.isatty()

def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _use_color() else text

def green(t):  return _c("32",   t)
def red(t):    return _c("31",   t)
def yellow(t): return _c("33",   t)
def bold(t):   return _c("1",    t)
def dim(t):    return _c("2",    t)


def frame_message(message):
    """Encode a dict as an LSP-framed message (bytes)."""
    body = json.dumps(message, separators=(',', ':')).encode('utf-8')
    header = f"Content-Length: {len(body)}\r\n\r\n".encode('ascii')
    return header + body


def read_message(stream):
    """Read one LSP-framed message from a binary stream.

    Returns the parsed JSON dict, or None on EOF/error.
    """
    content_length = None

    while True:
        raw_line = stream.readline()
        if not raw_line:
            return None
        line = raw_line.rstrip(b'\r\n')
        if not line:
            break  # blank line ends headers
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


def collect_server_output(stdout, output_list):
    """Background thread: read LSP messages from stdout until EOF."""
    while True:
        message = read_message(stdout)
        if message is None:
            break
        output_list.append(message)


def _wait_for_response_id(collected, expected_id, deadline_seconds=5.0):
    """Block until a response with @p expected_id appears in @p collected.

    The reader thread appends to the list as messages arrive.  Returns
    True on success, False on timeout.
    """
    import time
    deadline = time.monotonic() + deadline_seconds
    seen = 0
    while time.monotonic() < deadline:
        while seen < len(collected):
            message = collected[seen]
            seen += 1
            if _is_response(message) and message.get('id') == expected_id:
                return True
        time.sleep(0.01)
    return False


def _wait_quiescent(collected, quiet_seconds=0.5, max_wait=15.0):
    """Block until @p collected stops growing for @p quiet_seconds (or timeout).

    Used for tj3 cases, whose diagnostics arrive asynchronously from a worker
    thread after the triggering notification is processed.  We wait for the
    output stream to go quiet rather than guessing a fixed delay.
    """
    import time
    last_len = -1
    last_change = time.monotonic()
    deadline = last_change + max_wait
    while time.monotonic() < deadline:
        if len(collected) != last_len:
            last_len = len(collected)
            last_change = time.monotonic()
        elif time.monotonic() - last_change >= quiet_seconds:
            return
        time.sleep(0.02)


def run_server(server_binary, input_messages, use_tj3=False, wait_async=False):
    """Start the server, send all messages, and return the collected output.

    Honors LSP spec ordering for `initialize`: per the spec, the client
    must wait for the response to `initialize` before sending any other
    message.  After sending an `initialize` message with an id, the
    harness blocks until that response arrives before sending the next
    message.  Other requests are sent without waiting; the server may
    answer them in any order, and diff_output matches responses by id.

    By default the asynchronous tj3 diagnostics workers are suppressed so the
    golden output stays deterministic and independent of whether tj3 is
    installed.  Cases that opt in via `uses_tj3` run with tj3 enabled; cases
    that opt in via either `uses_tj3` or `uses_async_diag` set @p wait_async
    and block until the async diagnostics settle before closing (e.g. the
    "Missing compile_commands.json" warnings, which a worker emits even with
    tj3 disabled).
    """
    env = dict(os.environ)
    if not use_tj3:
        env["TASKJUGGLER_LSP_DISABLE_TJ3"] = "1"

    process = subprocess.Popen(
        [server_binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        env=env,
    )

    collected = []
    reader = threading.Thread(
        target=collect_server_output,
        args=(process.stdout, collected),
        daemon=True,
    )
    reader.start()

    for message in input_messages:
        process.stdin.write(frame_message(message))
        process.stdin.flush()
        if isinstance(message, dict) \
                and message.get('method') == 'initialize' \
                and 'id' in message:
            _wait_for_response_id(collected, message['id'])

    # Worker-emitted diagnostics (tj3 results and "Missing compile_commands.json"
    # warnings) are published asynchronously; let them settle so the captured
    # stream is complete before we close stdin.
    if wait_async:
        _wait_quiescent(collected)

    process.stdin.close()
    reader.join(timeout=10.0)
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()

    return collected


def format_json(obj):
    """Return a consistently formatted JSON string for diffing."""
    return json.dumps(obj, indent=2, sort_keys=True)


def _colorize_diff(diff_lines):
    result = []
    for line in diff_lines:
        if line.startswith('+') and not line.startswith('+++'):
            result.append(green(line))
        elif line.startswith('-') and not line.startswith('---'):
            result.append(red(line))
        elif line.startswith('@@'):
            result.append(yellow(line))
        else:
            result.append(dim(line))
    return ''.join(result)


def _is_response(message):
    """A JSON-RPC response has an `id` and no `method` field.

    Notifications have `method` and no `id`. Server-initiated requests
    have both `id` and `method` and are treated as notifications for
    ordering purposes (they are server-emitted, not replies).
    """
    return isinstance(message, dict) \
        and 'id' in message \
        and 'method' not in message


def _partition(messages):
    """Split a message stream into (responses_by_id, ordered_notifications).

    Async dispatch may reorder responses relative to one another and
    relative to notifications, but the order of notifications among
    themselves is semantically meaningful (e.g. diagnostics for file A
    before file B). Compare responses by id, notifications by position.
    """
    responses = {}
    notifications = []
    duplicate_ids = []
    for message in messages:
        if _is_response(message):
            mid = message['id']
            key = json.dumps(mid, sort_keys=True)
            if key in responses:
                duplicate_ids.append(mid)
            responses[key] = message
        else:
            notifications.append(message)
    return responses, notifications, duplicate_ids


def _collapse_diagnostics(notifications):
    """Reduce publishDiagnostics to the final state per URI, sorted by URI.

    Only used for cases that opt into asynchronous diagnostics.  Those markers
    are emitted by per-project worker threads that interleave nondeterministically
    with the coordinator's synchronous empty baselines, so neither the order nor
    the count of intermediate publishes is reproducible.  What *is* deterministic
    — and all the client ultimately renders — is the last diagnostics array
    published for each URI.  We keep every non-publishDiagnostics notification in
    order and replace the publishes with one entry per URI (its final state),
    ordered by URI.
    """
    pub = 'textDocument/publishDiagnostics'
    others = [m for m in notifications
              if not (isinstance(m, dict) and m.get('method') == pub)]
    final = {}
    for m in notifications:
        if isinstance(m, dict) and m.get('method') == pub:
            final[m['params']['uri']] = m
    pubs = [final[uri] for uri in sorted(final)]
    return others + pubs


def diff_output(expected, actual, collapse_diagnostics=False):
    """Return a colored diff string, or empty string if equal.

    Responses are matched by id (order-insensitive); notifications and
    server-initiated requests are diffed in order.  When @p collapse_diagnostics
    is set (asynchronous-diagnostics cases), publishDiagnostics are compared by
    final-state-per-URI instead of positionally (see _collapse_diagnostics).
    """
    exp_responses, exp_notifications, exp_dups = _partition(expected)
    act_responses, act_notifications, act_dups = _partition(actual)
    if collapse_diagnostics:
        exp_notifications = _collapse_diagnostics(exp_notifications)
        act_notifications = _collapse_diagnostics(act_notifications)

    sections = []

    if act_dups:
        sections.append(red(
            f"duplicate response ids in actual output: {act_dups}\n"
        ))
    if exp_dups:
        sections.append(red(
            f"duplicate response ids in expected output: {exp_dups}\n"
        ))

    missing_ids = sorted(set(exp_responses) - set(act_responses))
    extra_ids = sorted(set(act_responses) - set(exp_responses))
    if missing_ids:
        sections.append(red(f"missing response ids: {missing_ids}\n"))
    if extra_ids:
        sections.append(red(f"unexpected response ids: {extra_ids}\n"))

    for key in sorted(set(exp_responses) & set(act_responses)):
        if exp_responses[key] != act_responses[key]:
            exp_lines = format_json(exp_responses[key]).splitlines(keepends=True)
            act_lines = format_json(act_responses[key]).splitlines(keepends=True)
            diff = list(difflib.unified_diff(
                exp_lines, act_lines,
                fromfile=f'expected response id={key}',
                tofile=f'actual response id={key}',
            ))
            if diff:
                sections.append(_colorize_diff(diff))

    if exp_notifications != act_notifications:
        exp_lines = format_json(exp_notifications).splitlines(keepends=True)
        act_lines = format_json(act_notifications).splitlines(keepends=True)
        diff = list(difflib.unified_diff(
            exp_lines, act_lines,
            fromfile='expected notifications',
            tofile='actual notifications',
        ))
        if diff:
            sections.append(_colorize_diff(diff))

    return ''.join(sections)


def apply_substitutions(obj, case_dir):
    """Recursively replace ${CASE_DIR} in all string values with case_dir."""
    if isinstance(obj, str):
        return obj.replace('${CASE_DIR}', case_dir)
    if isinstance(obj, list):
        return [apply_substitutions(item, case_dir) for item in obj]
    if isinstance(obj, dict):
        return {k: apply_substitutions(v, case_dir) for k, v in obj.items()}
    return obj


def apply_redactions(obj, case_dir):
    """Recursively replace case_dir with ${CASE_DIR} in all string values.

    Used when recording expected output so that expected.json stays portable
    across machines.
    """
    if isinstance(obj, str):
        return obj.replace(case_dir, '${CASE_DIR}')
    if isinstance(obj, list):
        return [apply_redactions(item, case_dir) for item in obj]
    if isinstance(obj, dict):
        return {k: apply_redactions(v, case_dir) for k, v in obj.items()}
    return obj


def run_test_case(server_binary, case_dir, record):
    """Run a single test case. Returns True on pass (or record), False on fail."""
    input_path = os.path.join(case_dir, 'input.json')
    expected_path = os.path.join(case_dir, 'expected.json')
    case_name = os.path.basename(case_dir)

    if not os.path.isfile(input_path):
        print(f"  {yellow('SKIP')}  {case_name}  {dim('no input.json')}")
        return True

    # A `uses_tj3` marker opts the case into running the real tj3 compiler.
    # Such cases need tj3 on PATH; skip them (rather than fail) where it is
    # absent, so the rest of the suite stays runnable without tj3 installed.
    use_tj3 = os.path.isfile(os.path.join(case_dir, 'uses_tj3'))
    if use_tj3 and shutil.which('tj3') is None:
        print(f"  {yellow('SKIP')}  {case_name}  {dim('tj3 not on PATH')}")
        return True

    # A `uses_async_diag` marker opts the case into waiting for worker-emitted
    # diagnostics to settle (without enabling tj3) — needed for the "Missing
    # compile_commands.json" warnings, which a worker publishes asynchronously.
    uses_async_diag = os.path.isfile(os.path.join(case_dir, 'uses_async_diag'))
    wait_async = use_tj3 or uses_async_diag

    abs_case_dir = os.path.abspath(case_dir)
    with open(input_path, 'r') as input_file:
        input_messages = apply_substitutions(json.load(input_file), abs_case_dir)

    actual = run_server(server_binary, input_messages,
                        use_tj3=use_tj3, wait_async=wait_async)

    if record:
        # For async-diagnostics cases, store the canonical final-state-per-URI
        # form so the golden file does not capture a racy intermediate ordering.
        to_write = _collapse_diagnostics(actual) if wait_async else actual
        redacted = apply_redactions(to_write, abs_case_dir)
        with open(expected_path, 'w') as expected_file:
            json.dump(redacted, expected_file, indent=2)
            expected_file.write('\n')
        print(f"  {yellow('RECORDED')}  {case_name}  {dim(f'({len(actual)} message(s))')}")
        return True

    if not os.path.isfile(expected_path):
        print(f"  {red('FAIL')}  {case_name}  {dim('no expected.json (run with --record to create it)')}")
        return False

    with open(expected_path, 'r') as expected_file:
        expected = apply_substitutions(json.load(expected_file), abs_case_dir)

    diff = diff_output(expected, actual, collapse_diagnostics=wait_async)
    if not diff:
        print(f"  {green('✓')}  {case_name}")
        return True

    print(f"  {red('✗')}  {bold(case_name)}")
    print(diff)
    return False


def find_case_dirs(cases_root):
    """Return sorted list of test case directories under cases_root."""
    entries = sorted(os.listdir(cases_root))
    return [
        os.path.join(cases_root, entry)
        for entry in entries
        if os.path.isdir(os.path.join(cases_root, entry))
    ]


def main():
    parser = argparse.ArgumentParser(
        description='LSP golden-file test harness for taskjuggler-lsp.',
    )
    parser.add_argument('server', help='Path to the server binary')
    parser.add_argument(
        'cases', nargs='*',
        help='Test case directories to run',
    )
    parser.add_argument(
        '--all', metavar='DIR',
        help='Run all test cases found under DIR',
    )
    parser.add_argument(
        '--record', action='store_true',
        help='Record server output as expected.json instead of comparing',
    )
    args = parser.parse_args()

    if args.all:
        case_dirs = find_case_dirs(args.all)
    elif args.cases:
        case_dirs = args.cases
    else:
        parser.error('Provide test case directories or use --all <dir>')

    if not case_dirs:
        print('No test cases found.')
        sys.exit(0)

    passed = 0
    failed = 0
    for case_dir in case_dirs:
        if run_test_case(args.server, case_dir, args.record):
            passed += 1
        else:
            failed += 1

    total = passed + failed
    if args.record:
        print(f"\n{dim('─' * 40)}")
        print(f"  Recorded {bold(str(total))} test case(s).")
    else:
        print(f"\n{dim('─' * 40)}")
        if failed:
            summary = f"  {green(str(passed))} passed · {red(str(failed))} failed"
        else:
            summary = f"  {green(bold(str(passed)))} passed"
        print(summary)
        if failed:
            sys.exit(1)


if __name__ == '__main__':
    main()
