#!/usr/bin/env python3
"""
Ad-hoc measurement of semanticTokens/full vs semanticTokens/full/delta
response bandwidth AND latency on a large .tjp fixture.

For each edit shape we run a loop where the document is held in the
"post-edit" state for both the /full and /full/delta calls — they are
the apples-to-apples comparison for "what would the server return when
the editor asks again after a keystroke".  Each request is sampled
ITERATIONS times and the median latency is reported.

Response sizes are deterministic so N=1 is enough for the byte columns;
they come from the first iteration of each scenario.
"""

import json
import os
import queue
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lsp_framing


SERVER = "./taskjuggler-lsp"
FIXTURE = "test/perf_balanced.tjp"
ITERATIONS = 10  # samples per scenario after warmup
WARMUP = 2       # discarded leading samples


class ServerSession:
    """Minimal LSP client: tracks request ids and captures both response
    bodies and per-request latencies."""

    def __init__(self, server_path):
        self.proc = subprocess.Popen([server_path],
                                     stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self._next_id = 1

    def _reader(self):
        while True:
            msg = lsp_framing.read_message(self.proc.stdout)
            self.q.put(msg)
            if msg is None:
                return

    def notify(self, method, params):
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        self.proc.stdin.write(lsp_framing.frame_message(msg))
        self.proc.stdin.flush()

    def request(self, method, params):
        """Send a request, block for the response.  Returns (response_dict,
        elapsed_ms)."""
        req_id = self._next_id
        self._next_id += 1
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params}
        start = time.perf_counter()
        self.proc.stdin.write(lsp_framing.frame_message(msg))
        self.proc.stdin.flush()
        while True:
            resp = self.q.get(timeout=120)
            if resp is None:
                raise EOFError("server died")
            if resp.get("id") == req_id:
                elapsed_ms = (time.perf_counter() - start) * 1000
                return resp, elapsed_ms

    def close(self):
        try:
            self.notify("exit", {})
        except Exception:
            pass
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def body_size(resp):
    return len(json.dumps(resp, separators=(',', ':')).encode("utf-8"))


def find_line_col(text, target_substr):
    idx = text.find(target_substr)
    if idx < 0:
        raise ValueError(f"substring not found: {target_substr!r}")
    line = text.count("\n", 0, idx)
    last_nl = text.rfind("\n", 0, idx)
    col = idx - (last_nl + 1) if last_nl >= 0 else idx
    return line, col


def measure_scenario(fixture_text, edits, iterations=ITERATIONS, warmup=WARMUP):
    """Drive the server through `iterations` rounds of apply-edit → measure
    delta + full → revert → reset-cache, returning bandwidth (one-shot) and
    latency (median, p95) statistics for each request kind."""
    uri = "file:///bench/sample.tjp"
    sess = ServerSession(SERVER)
    try:
        # Setup.
        sess.request("initialize", {
            "processId": None, "rootUri": None, "capabilities": {}})
        sess.notify("initialized", {})
        sess.notify("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "taskjuggler",
                             "version": 1, "text": fixture_text}})

        # Establish cache with pre-edit data; remember its resultId.
        full_resp, _ = sess.request("textDocument/semanticTokens/full",
                                    {"textDocument": {"uri": uri}})
        pre_result_id = full_resp["result"]["resultId"]

        version = 2
        delta_times = []
        full_times = []
        sample_sizes = None  # captured on the first measured iteration

        for i in range(iterations + warmup):
            # Apply edit.
            sess.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": edits})
            version += 1

            # Sync barrier: didChange is a notification, so its parse cost
            # would otherwise leak into whichever request follows.  Issue an
            # untimed request that does not touch the semantic-tokens cache
            # but does block until the parse completes.
            sess.request("textDocument/documentSymbol",
                         {"textDocument": {"uri": uri}})

            # Measure delta against the pre-edit cache.
            delta_resp, delta_ms = sess.request(
                "textDocument/semanticTokens/full/delta",
                {"textDocument": {"uri": uri},
                 "previousResultId": pre_result_id})

            # Measure full on the post-edit document.
            full_post_resp, full_ms = sess.request(
                "textDocument/semanticTokens/full",
                {"textDocument": {"uri": uri}})

            if i >= warmup:
                delta_times.append(delta_ms)
                full_times.append(full_ms)
                if sample_sizes is None:
                    result = delta_resp.get("result", {})
                    sample_sizes = {
                        "full_post_bytes": body_size(full_post_resp),
                        "delta_bytes":     body_size(delta_resp),
                        "delta_edit_count": len(result["edits"])
                                            if "edits" in result else None,
                        "delta_is_full":    "data" in result,
                    }

            # Revert edit: replace whole text back to fixture.
            sess.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": fixture_text}]})
            version += 1

            # Sync the revert parse before re-seeding the cache.
            sess.request("textDocument/documentSymbol",
                         {"textDocument": {"uri": uri}})

            # Re-seed the cache with pre-edit data so the next iteration's
            # delta has something meaningful to diff against.
            reseed_resp, _ = sess.request(
                "textDocument/semanticTokens/full",
                {"textDocument": {"uri": uri}})
            pre_result_id = reseed_resp["result"]["resultId"]

        sess.request("shutdown", {})
    finally:
        sess.close()

    def stats(samples):
        s = sorted(samples)
        return {
            "median": statistics.median(s),
            "p95":    s[int(len(s) * 0.95)] if len(s) > 1 else s[0],
        }

    return {**sample_sizes,
            "delta_lat": stats(delta_times),
            "full_lat":  stats(full_times)}


def main():
    fixture_path = Path(FIXTURE)
    fixture_text = fixture_path.read_text()
    print(f"Fixture: {fixture_path}  ({len(fixture_text):,} bytes, "
          f"{fixture_text.count(chr(10)):,} lines)")
    print(f"Samples per scenario: {ITERATIONS} (after {WARMUP} warmup)")
    print()

    line, col = find_line_col(fixture_text, 'task t4728 "Task t4728"')
    other_fixture = Path("test/perf_wide.tjp").read_text()
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
        ("tiny edit (rename t4728 -> t47280)",
         [{"range": {"start": {"line": line, "character": col + 9},
                     "end":   {"line": line, "character": col + 9}},
           "text": "0"}]),
        ("small block insert (new sibling task above)",
         [{"range": {"start": {"line": line, "character": 0},
                     "end":   {"line": line, "character": 0}},
           "text": '        task new_t "Inserted" {\n        }\n'}]),
        ("replace whole doc with 7-line file",
         [{"text": replacement_text}]),
        ("replace whole doc with similar-size other fixture (perf_wide)",
         [{"text": other_fixture}]),
    ]

    rows = []
    for label, edits in scenarios:
        print(f"  running: {label} ...", flush=True)
        rows.append((label, measure_scenario(fixture_text, edits)))

    print()
    # Bandwidth table.
    print("Bandwidth (response body bytes)")
    print(f"{'scenario':<60}  {'full':>12}  {'delta':>12}  "
          f"{'ratio':>7}  {'edits':>6}")
    print("-" * 105)
    for label, r in rows:
        edits = ("FALLBACK" if r["delta_is_full"] else f"{r['delta_edit_count']}")
        ratio = r["delta_bytes"] / r["full_post_bytes"]
        print(f"{label:<60}  {r['full_post_bytes']:>12,}  {r['delta_bytes']:>12,}  "
              f"{ratio:>6.1%}  {edits:>6}")
    print()

    # Latency table.
    print("Latency (request → response, ms; median / p95 over N samples)")
    print(f"{'scenario':<60}  {'full p50':>9}  {'full p95':>9}  "
          f"{'delta p50':>10}  {'delta p95':>10}  {'speedup':>8}")
    print("-" * 122)
    for label, r in rows:
        f50, f95 = r["full_lat"]["median"], r["full_lat"]["p95"]
        d50, d95 = r["delta_lat"]["median"], r["delta_lat"]["p95"]
        speedup = f"{f50/d50:.2f}x" if d50 > 0 else "n/a"
        print(f"{label:<60}  {f50:>8.1f}   {f95:>8.1f}   "
              f"{d50:>9.1f}   {d95:>9.1f}   {speedup:>8}")


if __name__ == "__main__":
    if not os.path.exists(SERVER):
        sys.exit("Build the server first: make")
    main()
