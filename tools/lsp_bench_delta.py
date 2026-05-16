#!/usr/bin/env python3
"""
Ad-hoc measurement of semanticTokens/full vs semanticTokens/full/delta
response bandwidth.

Loads a large .tjp fixture, asks for the full token set, applies a small
edit, then asks for the delta — reporting the raw response body size of
each.  Repeats for a few edit shapes to show how the savings scale.
"""

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lsp_framing


SERVER = "./taskjuggler-lsp"
FIXTURE = "test/perf_balanced.tjp"


def measure(fixture_text, edits):
    """Run one scenario: full → didChange → delta → full-after.

    Returns dict with byte sizes of each response body.
    """
    uri = "file:///bench/sample.tjp"

    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"processId": None, "rootUri": None, "capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {
             "uri": uri, "languageId": "taskjuggler",
             "version": 1, "text": fixture_text}}},
        # First semanticTokens/full — establishes the cache + resultId.
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full",
         "params": {"textDocument": {"uri": uri}}},
        # Apply edits.
        {"jsonrpc": "2.0", "method": "textDocument/didChange",
         "params": {"textDocument": {"uri": uri, "version": 2},
                    "contentChanges": edits}},
        # Delta against the cached resultId="1".
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/semanticTokens/full/delta",
         "params": {"textDocument": {"uri": uri}, "previousResultId": "1"}},
        # Full again for comparison.
        {"jsonrpc": "2.0", "id": 4, "method": "textDocument/semanticTokens/full",
         "params": {"textDocument": {"uri": uri}}},
        {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": {}},
    ]

    # We need response bodies, not timings — duplicate the bits of
    # run_scenario that capture responses, but record their serialized size.
    import subprocess
    import queue
    import threading

    proc = subprocess.Popen([SERVER],
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE)
    q = queue.Queue()

    def reader():
        while True:
            msg = lsp_framing.read_message(proc.stdout)
            q.put(msg)
            if msg is None:
                break

    threading.Thread(target=reader, daemon=True).start()

    responses_by_id = {}
    elapsed_by_id = {}

    try:
        for m in messages:
            is_request = "id" in m
            if not is_request:
                proc.stdin.write(lsp_framing.frame_message(m))
                proc.stdin.flush()
                continue
            start = time.perf_counter()
            proc.stdin.write(lsp_framing.frame_message(m))
            proc.stdin.flush()
            while True:
                resp = q.get(timeout=120)
                if resp is None:
                    raise EOFError("server died")
                if resp.get("id") == m["id"]:
                    elapsed_by_id[m["id"]] = (time.perf_counter() - start) * 1000
                    responses_by_id[m["id"]] = resp
                    break
    finally:
        try: proc.stdin.close()
        except OSError: pass
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()

    def body_size(resp):
        return len(json.dumps(resp, separators=(',', ':')).encode('utf-8'))

    full_initial = responses_by_id[2]
    delta_resp   = responses_by_id[3]
    full_after   = responses_by_id[4]

    return {
        "full_initial_bytes": body_size(full_initial),
        "full_initial_ms":    elapsed_by_id[2],
        "delta_bytes":        body_size(delta_resp),
        "delta_ms":           elapsed_by_id[3],
        "full_after_bytes":   body_size(full_after),
        "full_after_ms":      elapsed_by_id[4],
        "delta_edit_count":   len(delta_resp["result"].get("edits", []))
                              if "edits" in delta_resp.get("result", {}) else None,
        "delta_is_full":      "data" in delta_resp.get("result", {}),
    }


def find_line_col(text, target_substr):
    """Locate target_substr in text and return its (line, col) start position."""
    idx = text.find(target_substr)
    if idx < 0:
        raise ValueError(f"substring not found: {target_substr!r}")
    line = text.count("\n", 0, idx)
    last_nl = text.rfind("\n", 0, idx)
    col = idx - (last_nl + 1) if last_nl >= 0 else idx
    return line, col


def main():
    fixture_path = Path(FIXTURE)
    fixture_text = fixture_path.read_text()
    print(f"Fixture: {fixture_path}  ({len(fixture_text):,} bytes, "
          f"{fixture_text.count(chr(10)):,} lines)")
    print()

    # Locate a stable identifier deep in the file for our edit anchor.
    line, col = find_line_col(fixture_text, 'task t4728 "Task t4728"')
    print(f"Edit anchor at line {line}, col {col}")
    print()

    # Another large fixture used to stress the worst-case path: every token
    # differs but the buffers are of comparable size.
    other_fixture = Path("test/perf_wide.tjp").read_text()

    # A completely different valid TJP — exercises the D_BOUND fallback
    # path in the Myers diff (middle slice is too large to diff).
    replacement_text = (
        'project tiny "Tiny" 2024-01-01 +1y {\n'
        '  timezone "UTC"\n'
        '}\n'
        'task only "Only" {\n'
        '  start 2024-01-01\n'
        '  duration 1d\n'
        '}\n'
    )

    scenarios = [
        ("no-op (insert empty string)",
         [{"range": {"start": {"line": line, "character": col + 5},
                     "end":   {"line": line, "character": col + 5}},
           "text": ""}]),
        ("tiny edit (rename t4728 -> t47280, 1 char inserted)",
         [{"range": {"start": {"line": line, "character": col + 9},
                     "end":   {"line": line, "character": col + 9}},
           "text": "0"}]),
        ("small block insert (new sibling task above)",
         [{"range": {"start": {"line": line, "character": 0},
                     "end":   {"line": line, "character": 0}},
           "text": '        task new_t "Inserted" {\n        }\n'}]),
        ("replace entire 1MB doc with 7-line file",
         [{"text": replacement_text}]),
        ("replace entire doc with similar-size different fixture (perf_wide)",
         [{"text": other_fixture}]),
    ]

    rows = []
    for label, edits in scenarios:
        result = measure(fixture_text, edits)
        rows.append((label, result))

    # Print summary table.  "Full (post)" is the size of a /full request
    # against the *post-edit* document and is the fair comparison for
    # "what would the client have received without delta support".
    header = ("scenario", "full (post)", "delta", "ratio", "edits", "full ms", "delta ms")
    print(f"{header[0]:<60}  {header[1]:>12}  {header[2]:>12}  "
          f"{header[3]:>7}  {header[4]:>6}  {header[5]:>9}  {header[6]:>9}")
    print("-" * 135)
    for label, r in rows:
        ratio = r["delta_bytes"] / r["full_after_bytes"]
        edits = ("FULL FALLBACK" if r["delta_is_full"]
                 else f"{r['delta_edit_count']}")
        print(f"{label:<60}  {r['full_after_bytes']:>12,}  {r['delta_bytes']:>12,}  "
              f"{ratio:>6.1%}  {edits:>6}  {r['full_after_ms']:>8.1f}  {r['delta_ms']:>8.1f}")


if __name__ == "__main__":
    if not os.path.exists(SERVER):
        sys.exit(f"Build the server first: make")
    main()
