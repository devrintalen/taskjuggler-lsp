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


def run_server(server_binary, input_messages):
    """Start the server, send all messages, and return the collected output."""
    process = subprocess.Popen(
        [server_binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
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


def diff_output(expected, actual):
    """Return a colored unified diff string, or empty string if equal."""
    expected_lines = format_json(expected).splitlines(keepends=True)
    actual_lines = format_json(actual).splitlines(keepends=True)
    diff = list(difflib.unified_diff(
        expected_lines, actual_lines,
        fromfile='expected.json',
        tofile='actual output',
    ))
    if not diff:
        return ''
    result = []
    for line in diff:
        if line.startswith('+') and not line.startswith('+++'):
            result.append(green(line))
        elif line.startswith('-') and not line.startswith('---'):
            result.append(red(line))
        elif line.startswith('@@'):
            result.append(yellow(line))
        else:
            result.append(dim(line))
    return ''.join(result)


def run_test_case(server_binary, case_dir, record):
    """Run a single test case. Returns True on pass (or record), False on fail."""
    input_path = os.path.join(case_dir, 'input.json')
    expected_path = os.path.join(case_dir, 'expected.json')
    case_name = os.path.basename(case_dir)

    if not os.path.isfile(input_path):
        print(f"  {yellow('SKIP')}  {case_name}  {dim('no input.json')}")
        return True

    with open(input_path, 'r') as input_file:
        input_messages = json.load(input_file)

    actual = run_server(server_binary, input_messages)

    if record:
        with open(expected_path, 'w') as expected_file:
            json.dump(actual, expected_file, indent=2)
            expected_file.write('\n')
        print(f"  {yellow('RECORDED')}  {case_name}  {dim(f'({len(actual)} message(s))')}")
        return True

    if not os.path.isfile(expected_path):
        print(f"  {red('FAIL')}  {case_name}  {dim('no expected.json (run with --record to create it)')}")
        return False

    with open(expected_path, 'r') as expected_file:
        expected = json.load(expected_file)

    diff = diff_output(expected, actual)
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
