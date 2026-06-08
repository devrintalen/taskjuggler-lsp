#!/usr/bin/env python3
"""Measure didChange processing cost in isolation.

Sends initialize/initialized/didOpen for a fixture, then repeatedly:
  - one didChange (a tiny incremental edit at the top of the file)
  - one cheap hover request (blocks behind the didChange in the coordinator)
The hover round-trip time ~= didChange (parse + revalidate) cost, since the
coordinator processes notifications in arrival order before the query worker
sees the following request.
"""
import json, os, sys, time, statistics
sys.path.insert(0, os.path.dirname(__file__))
from lsp_framing import frame_message, read_message
import subprocess, threading, queue


def run(server, fixture, rounds):
    text = open(fixture).read()
    uri = "file://" + os.path.abspath(fixture)

    proc = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    q = queue.Queue()

    def reader():
        while True:
            m = read_message(proc.stdout)
            q.put(m)
            if m is None:
                break
    threading.Thread(target=reader, daemon=True).start()

    def send(m):
        proc.stdin.write(frame_message(m))
        proc.stdin.flush()

    def request(m):
        send(m)
        while True:
            r = q.get(timeout=60)
            if r is None:
                raise EOFError("server closed")
            if r.get("id") == m["id"]:
                return

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"rootUri": "file://" + os.path.dirname(os.path.abspath(fixture)),
                     "capabilities": {}}})
    # drain initialize response
    while True:
        r = q.get(timeout=60)
        if r and r.get("id") == 1:
            break
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
          "params": {"textDocument": {"uri": uri, "languageId": "tjp",
                                       "version": 1, "text": text}}})

    times = []
    rid = 100
    for i in range(rounds):
        # tiny incremental edit: insert and remove a space at line 0 col 0
        send({"jsonrpc": "2.0", "method": "textDocument/didChange",
              "params": {"textDocument": {"uri": uri, "version": i + 2},
                         "contentChanges": [
                             {"range": {"start": {"line": 0, "character": 0},
                                        "end": {"line": 0, "character": 0}},
                              "text": " "}]}})
        rid += 1
        start = time.perf_counter()
        request({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover",
                 "params": {"textDocument": {"uri": uri},
                            "position": {"line": 5, "character": 4}}})
        elapsed = (time.perf_counter() - start) * 1000
        # undo the edit so the file does not grow unbounded
        send({"jsonrpc": "2.0", "method": "textDocument/didChange",
              "params": {"textDocument": {"uri": uri, "version": i + 1000},
                         "contentChanges": [
                             {"range": {"start": {"line": 0, "character": 0},
                                        "end": {"line": 0, "character": 1}},
                              "text": ""}]}})
        rid += 1
        request({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover",
                 "params": {"textDocument": {"uri": uri},
                            "position": {"line": 5, "character": 4}}})
        if i > 0:  # discard first (warmup)
            times.append(elapsed)

    times.sort()
    print(f"{os.path.basename(fixture):24s} n={len(times):2d} "
          f"min={times[0]:7.1f}ms  median={statistics.median(times):7.1f}ms  "
          f"mean={statistics.mean(times):7.1f}ms  max={times[-1]:7.1f}ms")

    try:
        proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
    return statistics.median(times)


if __name__ == "__main__":
    server = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    fixtures = sys.argv[3:] or [
        "test/perf_flat.tjp", "test/perf_balanced.tjp", "test/perf_deep.tjp",
        "test/perf_wide.tjp", "test/perf_highdeps.tjp"]
    for fx in fixtures:
        run(server, fx, rounds)
