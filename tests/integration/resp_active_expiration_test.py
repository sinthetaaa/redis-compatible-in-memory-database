#!/usr/bin/env python3
"""Phase 5G live RESP2 smoke: expiration-index churn and FIFO fairness."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_active_expiration_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def req(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(p)).encode() + b"\r\n" + p + b"\r\n" for p in parts
    )


def readn(sock, count):
    out = bytearray()
    while len(out) < count:
        part = sock.recv(count - len(out))
        check(part, "unexpected EOF")
        out.extend(part)
    return bytes(out)


def line(sock):
    out = bytearray()
    while not out.endswith(b"\r\n"):
        check(len(out) < 128, "RESP line too long")
        out += readn(sock, 1)
    return bytes(out[:-2])


def reply(sock):
    prefix = readn(sock, 1)
    if prefix == b":":
        return int(line(sock))
    if prefix in (b"+", b"-"):
        return prefix, line(sock)
    if prefix == b"$":
        count = int(line(sock))
        if count == -1:
            return None
        check(0 <= count <= 8192, "unbounded bulk reply")
        payload = readn(sock, count)
        check(readn(sock, 2) == b"\r\n", "malformed bulk reply")
        return payload
    raise AssertionError(f"unexpected RESP type: {prefix!r}")


def command(sock, *parts):
    sock.sendall(req(*parts))
    return reply(sock)


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
    s.settimeout(8)
    return s


@contextlib.contextmanager
def server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 8)
        check(ready, "startup timed out")
        greeting = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", greeting)
        check(match is not None, f"unexpected greeting: {greeting!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            output, error = process.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            output, error = process.communicate(timeout=8)
        check(process.returncode == 0, f"server exited abnormally: {error!r}")
        check("RESP_STOPPED" in output, "graceful shutdown absent")


def run(port):
    with connect(port) as first, connect(port) as second:
        check(command(first, b"SET", b"binary\0k", b"v", b"PX", b"600000") ==
              (b"+", b"OK"), "binary timed SET failed")
        check(command(second, b"PERSIST", b"binary\0k") == 1,
              "PERSIST failed across connections")
        check(command(first, b"TTL", b"binary\0k") == -1,
              "PERSIST not visible")
        # A large pipelined turnover exercises indexed INSERT, RESCHEDULE,
        # DELETE and plain SET without long real-time sleeps.
        outgoing = bytearray()
        for i in range(32):
            key = f"active:{i:02d}".encode()
            for args in ((b"SET", key, b"A", b"PX", b"600000"),
                         (b"EXPIRE", key, b"600"),
                         (b"PERSIST", key),
                         (b"SET", key, b"B", b"PX", b"600000"),
                         (b"EXPIRE", key, b"0"),
                         (b"PING",)):
                outgoing.extend(req(*args))
        first.sendall(outgoing)
        for i in range(32):
            check(reply(first) == (b"+", b"OK"), f"FIFO SET {i}")
            check(reply(first) == 1, f"FIFO EXPIRE {i}")
            check(reply(first) == 1, f"FIFO PERSIST {i}")
            check(reply(first) == (b"+", b"OK"), f"FIFO SET replacement {i}")
            check(reply(first) == 1, f"FIFO immediate delete {i}")
            check(reply(first) == (b"+", b"PONG"), f"FIFO PING {i}")
        check(command(second, b"GET", b"active:07") is None,
              "deleted key revived after churn")
        check(command(second, b"PING") == (b"+", b"PONG"),
              "second client stalled after maintenance workload")
        check(command(first, b"GET", b"binary\0k") == b"v",
              "persistent binary key corrupted")


def main():
    with server() as port:
        run(port)
    print("PASS: Phase 5G TCP multi-client indexed TTL churn and 192 pipelined replies")


if __name__ == "__main__":
    main()
