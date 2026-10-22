#!/usr/bin/env python3
"""External-process RESP2 command-serving validation on macOS/Linux."""

import contextlib
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

if len(sys.argv) != 2:
    raise SystemExit("Usage: resp_command_server_test.py SERVER_EXECUTABLE")

SERVER = sys.argv[1]
HOST = "127.0.0.1"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def recv_exact(conn, count):
    received = bytearray()
    while len(received) < count:
        data = conn.recv(count - len(received))
        check(bool(data), "TCP EOF before complete RESP2 reply")
        received.extend(data)
    return bytes(received)


def bulk(value):
    return b"$" + str(len(value)).encode("ascii") + b"\r\n" + value + b"\r\n"


def command(*parts):
    result = b"*" + str(len(parts)).encode("ascii") + b"\r\n"
    return result + b"".join(bulk(p) for p in parts)


def connect(port):
    conn = socket.create_connection((HOST, port), timeout=4)
    conn.settimeout(4)
    return conn


@contextlib.contextmanager
def server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "RESP2 server did not become ready")
        line = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", line)
        check(match is not None, f"unexpected readiness line: {line!r}")
        port = int(match.group(1))
        check(1 <= port <= 65535, "invalid bound port")
        check(process.poll() is None, "server exited during startup")
        yield process, port
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)


def test_basic(port):
    with connect(port) as conn:
        conn.sendall(command(b"PING"))
        check(recv_exact(conn, 7) == b"+PONG\r\n", "PING reply mismatch")
        conn.sendall(command(b"pInG", b"payload"))
        check(recv_exact(conn, 13) == bulk(b"payload"), "PING argument mismatch")
        conn.sendall(command(b"ECHO", b""))
        check(recv_exact(conn, 6) == bulk(b""), "empty ECHO mismatch")
        binary = b"A\x00\r\n\x80\xff" * 100
        conn.sendall(command(b"ECHO", binary))
        check(recv_exact(conn, len(bulk(binary))) == bulk(binary), "binary ECHO mismatch")
        conn.sendall(command(b"PING", b"1", b"2"))
        message = b"-ERR wrong number of arguments for 'ping' command\r\n"
        check(recv_exact(conn, len(message)) == message, "arity error mismatch")
        conn.sendall(command(b"PING"))
        check(recv_exact(conn, 7) == b"+PONG\r\n", "client lost after arity error")


def test_fragment_and_pipeline(port):
    with connect(port) as conn:
        frame = command(b"ECHO", b"fragmented")
        conn.sendall(frame[:8])
        conn.sendall(frame[8:19])
        conn.sendall(frame[19:])
        expected = bulk(b"fragmented")
        check(recv_exact(conn, len(expected)) == expected, "fragmented ECHO failed")

        payload = command(b"PING") * 40
        conn.sendall(payload)
        check(recv_exact(conn, 7 * 40) == b"+PONG\r\n" * 40,
              "deferred FIFO pipeline replies missing or reordered")


def test_quit(port):
    with connect(port) as conn:
        request = command(b"PING") + command(b"QUIT") + command(b"PING")
        conn.sendall(request)
        check(recv_exact(conn, 12) == b"+PONG\r\n+OK\r\n",
              "QUIT lost preceding reply or +OK")
        check(conn.recv(1) == b"", "QUIT did not close socket after final reply")

    with connect(port) as conn:
        conn.sendall(command(b"QUIT", b"unexpected"))
        expected = b"-ERR wrong number of arguments for 'quit' command\r\n"
        check(recv_exact(conn, len(expected)) == expected,
              "invalid QUIT should send error without closing")
        conn.sendall(command(b"PING"))
        check(recv_exact(conn, 7) == b"+PONG\r\n", "invalid QUIT closed connection")


def test_errors_isolation(port):
    with connect(port) as conn:
        conn.sendall(command(b"NO_SUCH_COMMAND"))
        expected = b"-ERR unknown command\r\n"
        check(recv_exact(conn, len(expected)) == expected,
              "unknown-command placeholder lost")
        conn.sendall(b"*1\r\n:7\r\n")
        expected = b"-ERR expected RESP2 array of bulk strings\r\n"
        check(recv_exact(conn, len(expected)) == expected,
              "structurally invalid command response mismatch")

    with connect(port) as invalid, connect(port) as healthy:
        invalid.sendall(b"*01\r\n")
        check(invalid.recv(1) == b"", "malformed RESP2 peer not closed")
        healthy.sendall(command(b"PING"))
        check(recv_exact(healthy, 7) == b"+PONG\r\n",
              "malformed peer contaminated healthy client")


def test_concurrency(port):
    def worker(index):
        with connect(port) as conn:
            value = bytes([index]) * 4096
            conn.sendall(command(b"ECHO", value))
            return recv_exact(conn, len(bulk(value))) == bulk(value)
    with ThreadPoolExecutor(max_workers=8) as pool:
        outcomes = list(pool.map(worker, range(8)))
    check(all(outcomes), "concurrent binary clients failed")


def test_halfclose(port):
    with connect(port) as conn:
        conn.sendall(command(b"PING") * 7)
        conn.shutdown(socket.SHUT_WR)
        check(recv_exact(conn, 49) == b"+PONG\r\n" * 7,
              "half-closed peer lost buffered responses")
        check(conn.recv(1) == b"", "half-closed peer was not retired")


def test_redis_cli(port):
    tool = shutil.which("redis-cli")
    if tool is None:
        print("SKIP: redis-cli not installed (raw RESP2 integration still passed)")
        return
    for arguments, expected in [(["PING"], "PONG"),
                                (["ECHO", "redis-cli-works"], "redis-cli-works")]:
        completed = subprocess.run(
            [tool, "-2", "--raw", "-h", HOST, "-p", str(port), *arguments],
            capture_output=True, text=True, timeout=5,
        )
        check(completed.returncode == 0,
              f"redis-cli invocation failed: {completed.stderr!r}")
        check(completed.stdout.strip() == expected,
              f"redis-cli mismatch for {arguments}: {completed.stdout!r}")
    print("PASS: actual redis-cli RESP2 PING and ECHO")


def main():
    with server() as (process, port):
        test_basic(port)
        test_fragment_and_pipeline(port)
        test_quit(port)
        test_errors_isolation(port)
        test_concurrency(port)
        test_halfclose(port)
        test_redis_cli(port)
        check(process.poll() is None, "command server terminated unexpectedly")
    print("PASS: RESP2 server TCP, pipelining, QUIT, errors and client isolation")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise
