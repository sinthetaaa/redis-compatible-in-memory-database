#!/usr/bin/env python3
"""Phase 4E binary-safe APPEND/STRLEN through the real RESP2 server."""

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
    raise SystemExit("usage: resp_append_strlen_test.py SERVER_EXECUTABLE")

SERVER = sys.argv[1]
HOST = "127.0.0.1"
LIMIT = 8176
LIMIT_ERROR = b"-ERR string value exceeds the 8176-byte reply-safe limit\r\n"


def check(value, description):
    if not value:
        raise AssertionError(description)


def bulk(blob):
    return b"$" + str(len(blob)).encode("ascii") + b"\r\n" + blob + b"\r\n"


def request(*elements):
    return b"*" + str(len(elements)).encode("ascii") + b"\r\n" + b"".join(
        bulk(element) for element in elements
    )


def integer(value):
    return b":" + str(value).encode("ascii") + b"\r\n"


def recv_exact(sock, length):
    output = bytearray()
    while len(output) < length:
        piece = sock.recv(length - len(output))
        check(piece, "early TCP EOF while reading command reply")
        output.extend(piece)
    return bytes(output)


def expect(sock, expected, explanation):
    actual = recv_exact(sock, len(expected))
    check(actual == expected, f"{explanation}: {actual!r} != {expected!r}")


def connect(port):
    sock = socket.create_connection((HOST, port), timeout=5)
    sock.settimeout(5)
    return sock


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 5)
        check(ready, "server readiness timeout")
        first = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", first)
        check(match is not None, f"invalid server readiness: {first!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        check(proc.returncode == 0, f"abnormal server exit: {stderr!r}")
        check("RESP_STOPPED" in stdout, "missing graceful shutdown")


def test_basic_shared_and_binary(port):
    with connect(port) as a, connect(port) as b:
        a.sendall(request(b"STRLEN", b"missing") + request(b"TYPE", b"missing"))
        expect(a, b":0\r\n+none\r\n", "missing STRLEN must not create key")
        a.sendall(request(b"APPEND", b"greeting", b"Hello"))
        expect(a, integer(5), "missing APPEND creates key")
        b.sendall(request(b"append", b"greeting", b" World"))
        expect(b, integer(11), "APPEND existing length")
        a.sendall(request(b"sTrLeN", b"greeting") + request(b"GET", b"greeting"))
        expect(a, integer(11) + bulk(b"Hello World"), "cross-client append visibility")
        b.sendall(request(b"APPEND", b"greeting", b""))
        expect(b, integer(11), "empty APPEND preserves length")
        a.sendall(request(b"APPEND", b"", b"") + request(b"TYPE", b""))
        expect(a, integer(0) + b"+string\r\n", "empty append creates existing empty key")
        b.sendall(request(b"STRLEN", b"") + request(b"GET", b""))
        expect(b, integer(0) + bulk(b""), "empty value differs from missing")
        key = b"K\x00\xff\r\n"
        piece = b"A\x00\xff\r\n\x80"
        b.sendall(request(b"APPEND", key, piece) + request(b"APPEND", key, piece))
        expect(b, integer(6) + integer(12), "binary APPEND lengths")
        a.sendall(request(b"STRLEN", key) + request(b"GET", key) + request(b"STRLEN", b"k"))
        expect(a, integer(12) + bulk(piece * 2) + integer(0), "binary roundtrip and case")
        a.sendall(request(b"STRLEN") + request(b"STRLEN", b"a", b"b") +
                  request(b"APPEND") + request(b"APPEND", b"onlykey") +
                  request(b"APPEND", b"a", b"b", b"c") + request(b"PING"))
        expected = (
            b"-ERR wrong number of arguments for 'strlen' command\r\n" * 2
            + b"-ERR wrong number of arguments for 'append' command\r\n" * 3
            + b"+PONG\r\n"
        )
        expect(a, expected, "invalid arity or client connection regression")
        b.sendall(request(b"EXISTS", b"onlykey"))
        expect(b, integer(0), "invalid APPEND created a key")


def test_boundaries_and_atomic_rejection(port):
    with connect(port) as a, connect(port) as b:
        prefix = b"x" * (LIMIT - 16)
        a.sendall(request(b"SET", b"bound", prefix))
        expect(a, b"+OK\r\n", "boundary setup")
        a.sendall(request(b"APPEND", b"bound", b"y" * 16))
        expect(a, integer(LIMIT), "boundary APPEND must reach exactly limit")
        b.sendall(request(b"STRLEN", b"bound") + request(b"GET", b"bound"))
        expect(b, integer(LIMIT) + bulk(prefix + b"y" * 16), "limit GET/STRLEN")
        a.sendall(request(b"APPEND", b"bound", b""))
        expect(a, integer(LIMIT), "empty append at limit")
        a.sendall(request(b"APPEND", b"bound", b"z"))
        expect(a, LIMIT_ERROR, "oversize existing value should be rejected")
        b.sendall(request(b"GET", b"bound"))
        expect(b, bulk(prefix + b"y" * 16), "oversize changed previous bytes")
        a.sendall(request(b"APPEND", b"absent-oversize", b"!" * (LIMIT + 1)))
        expect(a, LIMIT_ERROR, "oversize missing value should be rejected")
        b.sendall(request(b"TYPE", b"absent-oversize"))
        expect(b, b"+none\r\n", "oversize created missing key")
        a.sendall(request(b"PING"))
        expect(a, b"+PONG\r\n", "connection lost after limit error")


def test_fragmented_pipeline_and_concurrent_clients(port):
    with connect(port) as a:
        frame = request(b"APPEND", b"fragment", b"binary\x00suffix")
        a.sendall(frame[:2])
        a.sendall(frame[2:17])
        a.sendall(frame[17:])
        expect(a, integer(len(b"binary\x00suffix")), "fragmented APPEND")
        payload = bytearray()
        output = bytearray()
        for i in range(45):
            part = bytes([i])
            payload.extend(request(b"APPEND", b"pipeline", part))
            output.extend(integer(i + 1))
            payload.extend(request(b"STRLEN", b"pipeline"))
            output.extend(integer(i + 1))
            payload.extend(request(b"GET", b"pipeline"))
            output.extend(bulk(bytes(range(i + 1))))
        a.sendall(payload)
        expect(a, bytes(output), "135 FIFO pipelined commands")
        a.sendall(request(b"STRLEN", b"fragment"))
        expect(a, integer(len(b"binary\x00suffix")), "pipeline damaged other key")

    def worker(index):
        with connect(port) as sock:
            key = b"append-client:" + str(index).encode("ascii")
            for j in range(35):
                payload = bytes([index, j])
                sock.sendall(request(b"APPEND", key, payload) + request(b"STRLEN", key))
                expect(sock, integer((j + 1) * 2) + integer((j + 1) * 2),
                       "client's two commands did not observe consistent bytes")
        return True

    with ThreadPoolExecutor(max_workers=8) as executor:
        check(all(executor.map(worker, range(8))), "concurrent client failed")

    with connect(port) as reader:
        for index in range(8):
            key = b"append-client:" + str(index).encode("ascii")
            reader.sendall(request(b"STRLEN", key))
            expect(reader, integer(70), "finished worker length not globally visible")


def test_quit_and_redis_cli(port):
    with connect(port) as first:
        first.sendall(request(b"APPEND", b"keep", b"kept") + request(b"QUIT") +
                      request(b"APPEND", b"keep", b"ignored"))
        expect(first, integer(4) + b"+OK\r\n", "APPEND/QUIT ordering")
        check(first.recv(1) == b"", "QUIT must close after final reply")
    with connect(port) as second:
        second.sendall(request(b"GET", b"keep"))
        expect(second, bulk(b"kept"), "APPEND after QUIT should not execute")

    cli = shutil.which("redis-cli")
    if cli is None:
        print("SKIP: redis-cli not installed; raw RESP2 integration passed")
        return

    def cli_call(*arguments):
        completed = subprocess.run(
            [cli, "-2", "--raw", "-h", HOST, "-p", str(port), *arguments],
            text=True, capture_output=True, timeout=6,
        )
        check(completed.returncode == 0, f"redis-cli failed: {completed.stderr!r}")
        return completed.stdout.rstrip("\n")

    check(cli_call("APPEND", "cli:a", "hello") == "5", "redis-cli first APPEND")
    check(cli_call("APPEND", "cli:a", " world") == "11", "redis-cli second APPEND")
    check(cli_call("STRLEN", "cli:a") == "11", "redis-cli STRLEN")
    check(cli_call("GET", "cli:a") == "hello world", "redis-cli GET after APPEND")
    print("PASS: actual redis-cli APPEND and STRLEN")


def main():
    with server() as port:
        test_basic_shared_and_binary(port)
        test_boundaries_and_atomic_rejection(port)
        test_fragmented_pipeline_and_concurrent_clients(port)
        test_quit_and_redis_cli(port)
    with server() as port:
        with connect(port) as sock:
            sock.sendall(request(b"STRLEN", b"keep") + request(b"GET", b"keep"))
            expect(sock, integer(0) + b"$-1\r\n", "restart must start with empty keyspace")
    print("PASS: TCP APPEND/STRLEN binary, boundaries, 8 clients, pipelining, redis-cli")


if __name__ == "__main__":
    main()
