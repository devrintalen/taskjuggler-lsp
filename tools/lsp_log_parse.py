#!/usr/bin/env python3
"""
Parse an LSP trace log (Emacs lsp-mode format) into a JSON message sequence.

The output is a JSON array compatible with the input.json format used by
lsp_test.py and lsp_bench.py.  Only outgoing (client → server) messages
are included; server responses and server-initiated notifications are dropped.

Usage:
  lsp_log_parse.py <logfile> [-o output.json] [--keep-cancelled]
"""

import argparse
import json
import re
import sys


# Match the timestamp/action header line.
TRACE_RE = re.compile(r'\[Trace - \d+:\d+:\d+ [AP]M\] (.+)')

# "Sending request 'textDocument/hover - (7)'."
SEND_REQUEST_RE = re.compile(r"Sending request '(.+) - \((\d+)\)'\.")

# "Sending notification 'textDocument/didChange'."
SEND_NOTIF_RE = re.compile(r"Sending notification '(.+)'\.")


def parse_log(text):
    """Return a list of outgoing JSON-RPC message dicts, in log order."""
    lines = text.splitlines()
    messages = []
    i = 0

    while i < len(lines):
        m = TRACE_RE.match(lines[i])
        if not m:
            i += 1
            continue

        action = m.group(1)
        req_m = SEND_REQUEST_RE.match(action)
        notif_m = SEND_NOTIF_RE.match(action) if not req_m else None

        if not req_m and not notif_m:
            i += 1
            continue

        # Advance to the "Params:" line (should be immediately next, but skip
        # any blank lines just in case).
        i += 1
        while i < len(lines) and not lines[i].startswith('Params:'):
            if TRACE_RE.match(lines[i]):
                break  # missed params somehow; move on
            i += 1

        if i >= len(lines) or not lines[i].startswith('Params:'):
            continue

        # Collect JSON: everything after "Params: " on this line, then
        # subsequent non-blank lines.
        params_inline = lines[i][len('Params:'):].strip()
        json_lines = [params_inline] if params_inline else []
        i += 1
        while i < len(lines) and lines[i].strip():
            json_lines.append(lines[i])
            i += 1

        try:
            params = json.loads('\n'.join(json_lines))
        except json.JSONDecodeError as exc:
            print(f"Warning: skipping unparseable params near line {i}: {exc}",
                  file=sys.stderr)
            continue

        if req_m:
            method = req_m.group(1)
            msg_id = int(req_m.group(2))
            messages.append({
                "jsonrpc": "2.0",
                "id": msg_id,
                "method": method,
                "params": params,
            })
        else:
            method = notif_m.group(1)
            messages.append({
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
            })

    return messages


def find_cancelled_ids(messages):
    """Return the set of request IDs that were cancelled by a $/cancelRequest."""
    cancelled = set()
    for msg in messages:
        if msg.get('method') == '$/cancelRequest':
            req_id = msg.get('params', {}).get('id')
            if req_id is not None:
                cancelled.add(req_id)
    return cancelled


def strip_cancelled(messages):
    """Remove cancelled requests and their corresponding cancel notifications."""
    cancelled_ids = find_cancelled_ids(messages)
    if not cancelled_ids:
        return messages

    print(f"Stripping {len(cancelled_ids)} cancelled request(s): {sorted(cancelled_ids)}",
          file=sys.stderr)

    kept = []
    for msg in messages:
        msg_id = msg.get('id')
        is_cancelled_request = msg_id is not None and msg_id in cancelled_ids
        is_cancel_notif = (
            msg.get('method') == '$/cancelRequest'
            and msg.get('params', {}).get('id') in cancelled_ids
        )
        if not is_cancelled_request and not is_cancel_notif:
            kept.append(msg)
    return kept


def main():
    parser = argparse.ArgumentParser(
        description='Parse an LSP trace log into a JSON message sequence.',
    )
    parser.add_argument('log', help='Input log file (Emacs lsp-mode trace format)')
    parser.add_argument('-o', '--output', help='Output file (default: stdout)')
    parser.add_argument(
        '--keep-cancelled', action='store_true',
        help='Include cancelled requests and their cancel notifications (default: strip them)',
    )
    args = parser.parse_args()

    with open(args.log) as f:
        text = f.read()

    messages = parse_log(text)
    print(f"Parsed {len(messages)} outgoing message(s).", file=sys.stderr)

    if not args.keep_cancelled:
        messages = strip_cancelled(messages)

    output = json.dumps(messages, indent=2) + '\n'

    if args.output:
        with open(args.output, 'w') as f:
            f.write(output)
        print(f"Wrote {len(messages)} message(s) to {args.output}.", file=sys.stderr)
    else:
        sys.stdout.write(output)


if __name__ == '__main__':
    main()
