#!/usr/bin/env python3
"""
LSP message framing and server runner utilities.

Shared by lsp_bench.py, lsp_perf_session.py, and callgrind.py.
Not intended to be run directly.
"""

import json
import queue
import subprocess
import threading
import time


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


def _reader_thread(stdout, q):
    """Background thread: push every server message onto q, then push None at EOF."""
    while True:
        msg = read_message(stdout)
        q.put(msg)
        if msg is None:
            break


def run_scenario(server, messages, response_timeout=30.0, perf_output=None):
    """Start the server, replay messages, and return timing data.

    server        — path to the binary (str) or a full command list (list of str,
                    e.g. ['valgrind', '--tool=callgrind', ..., 'binary'])
    messages      — list of LSP message dicts to send
    perf_output   — if set, launch under `perf record -o <path>` (str)

    Returns a list of (method, elapsed_ms) for every request that received
    a response.
    """
    if isinstance(server, list):
        cmd = server
    elif perf_output:
        cmd = ['perf', 'record', '-g', '-F', '997', '-o', perf_output,
               '--', server]
    else:
        cmd = [server]

    process = subprocess.Popen(
        cmd,
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
