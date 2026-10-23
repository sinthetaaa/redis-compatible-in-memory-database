#!/usr/bin/env python3
"""Phase 5H acceptance: stateful RESP2 expiration behavior across two clients."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_expiration_acceptance_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def request(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(part)).encode() + b"\r\n" + part + b"\r\n" for part in parts
    )


def recv_exact(sock, n):
    result = bytearray()
    while len(result) != n:
        chunk = sock.recv(n - len(result))
        require(chunk, "early EOF")
        result.extend(chunk)
    return bytes(result)


def recv_line(sock):
    result = bytearray()
    while not result.endswith(b"\r\n"):
        require(len(result) < 200, "RESP line bound exceeded")
        result.extend(recv_exact(sock, 1))
    return bytes(result[:-2])


def reply(sock):
    prefix = recv_exact(sock, 1)
    if prefix == b":":
        return int(recv_line(sock))
    if prefix == b"+" or prefix == b"-":
        return prefix, recv_line(sock)
    if prefix == b"$":
        length = int(recv_line(sock))
        if length == -1:
            return None
        require(0 <= length <= 8192, "oversized bulk response")
        value = recv_exact(sock, length)
        require(recv_exact(sock, 2) == b"\r\n", "invalid bulk terminator")
        return value
    raise AssertionError(f"unexpected reply prefix {prefix!r}")


def call(sock, *parts):
    sock.sendall(request(*parts))
    return reply(sock)


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=8)
    sock.settimeout(8)
    return sock


@contextlib.contextmanager
def server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 8)
        require(ready, "server startup timed out")
        greeting = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", greeting)
        require(match, f"unexpected startup output: {greeting!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            output, stderr = process.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            output, stderr = process.communicate(timeout=8)
        require(process.returncode == 0, f"server stopped abnormally: {stderr!r}")
        require("RESP_STOPPED" in output, "graceful shutdown marker missing")


def run(port):
    with connect(port) as a, connect(port) as b:
        key = b"x\x00\xff\r\n"
        require(call(a, b"SET", key, b"old", b"PX", b"600000") == (b"+", b"OK"),
                "binary-key PX seed")
        require(call(b, b"GET", key) == b"old", "cross-client binary value")
        remaining = call(b, b"PTTL", key)
        require(isinstance(remaining, int) and 0 < remaining <= 600000,
                "PTTL out of expected bounds")
        require(call(a, b"SET", key, b"new", b"EX", b"600") == (b"+", b"OK"),
                "timed replacement")
        require(call(b, b"GET", key) == b"new", "replacement not visible")
        require(call(b, b"PERSIST", key) == 1, "PERSIST did not clear deadline")
        require(call(a, b"TTL", key) == -1, "persistent TTL response")
        require(call(b, b"PERSIST", key) == 0, "second PERSIST must be zero")
        require(call(a, b"SET", b"reject", b"original", b"EX", b"600") ==
                (b"+", b"OK"), "guard key seed")
        require(call(b, b"SET", b"reject", b"corrupt", b"PX", b"0") ==
                (b"-", b"ERR invalid expire time in 'set' command"), "rejected timeout")
        require(call(a, b"GET", b"reject") == b"original", "rejected SET damaged value")
        still_timed = call(b, b"PTTL", b"reject")
        require(isinstance(still_timed, int) and 0 < still_timed <= 600000,
                "rejected SET damaged deadline")
        require(call(a, b"EXPIRE", b"reject", b"0") == 1, "EXPIRE zero delete")
        require(call(b, b"TTL", b"reject") == -2, "deleted key must return TTL -2")

        # 48 independent, fixed-order cycles; 192 RESP2 replies in one stream.
        payload = bytearray()
        for i in range(48):
            item = f"matrix:{i:02d}".encode()
            payload.extend(request(b"SET", item, b"a", b"PX", b"600000"))
            payload.extend(request(b"PEXPIRE", item, b"600000"))
            payload.extend(request(b"PERSIST", item))
            payload.extend(request(b"PING"))
        a.sendall(payload)
        for i in range(48):
            require(reply(a) == (b"+", b"OK"), f"pipeline SET {i}")
            require(reply(a) == 1, f"pipeline PEXPIRE {i}")
            require(reply(a) == 1, f"pipeline PERSIST {i}")
            require(reply(a) == (b"+", b"PONG"), f"pipeline PING {i}")
        require(call(b, b"TTL", b"matrix:00") == -1, "pipelined persistence invisible")
        require(call(b, b"GET", b"matrix:47") == b"a", "last pipelined value missing")
        require(call(a, b"SET", key, b"plain") == (b"+", b"OK"), "plain SET failed")
        require(call(b, b"EXISTS", key) == 1, "binary key lost")
        require(call(b, b"TTL", key) == -1, "plain SET retained deadline")
        require(call(a, b"PING") == (b"+", b"PONG"), "client stalled after workload")


def main():
    with server() as port:
        run(port)
    print("PASS: Phase 5H TCP two-client EX/PX/PEXPIRE/PERSIST/TTL matrix and 192 FIFO replies")


if __name__ == "__main__":
    main()
