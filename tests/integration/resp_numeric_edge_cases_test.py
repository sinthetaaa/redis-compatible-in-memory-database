#!/usr/bin/env python3
"""Phase 4G: differential signed-64-bit arithmetic and RESP2 mutation safety."""

import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_numeric_edge_cases_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"
MIN = -(2**63)
MAX = 2**63 - 1
INVALID = b"-ERR value is not an integer or out of range\r\n"
OVERFLOW = b"-ERR increment or decrement would overflow\r\n"


def check(ok, why):
    if not ok:
        raise AssertionError(why)


def bulk(payload):
    return b"$" + str(len(payload)).encode() + b"\r\n" + payload + b"\r\n"


def request(*args):
    return b"*" + str(len(args)).encode() + b"\r\n" + b"".join(bulk(p) for p in args)


def integer(value):
    return b":" + str(value).encode() + b"\r\n"


def take(sock, length):
    result = bytearray()
    while len(result) < length:
        fragment = sock.recv(length - len(result))
        check(fragment, "unexpected TCP EOF")
        result.extend(fragment)
    return bytes(result)


def expect(sock, wanted, what):
    got = take(sock, len(wanted))
    check(got == wanted, f"{what}: {got!r} != {wanted!r}")


def client(port):
    conn = socket.create_connection((HOST, port), timeout=5)
    conn.settimeout(5)
    return conn


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 5)
        check(ready, "server not ready")
        hello = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", hello)
        check(match is not None, f"readiness line: {hello!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        check(proc.returncode == 0, f"server abnormal exit: {stderr!r}")
        check("RESP_STOPPED" in stdout, "server failed graceful shutdown")


def signed_boundary_matrix(port):
    key = b"matrix:\x00\xff"
    values = [MIN, MIN + 1, MIN + 2, -100, -2, -1, 0, 1, 2, 100, MAX - 2, MAX - 1, MAX]
    deltas = [MIN, MIN + 1, -100, -2, -1, 0, 1, 2, 100, MAX - 1, MAX]
    with client(port) as a, client(port) as b:
        for current in values:
            for operand in deltas:
                for cmd in (b"INCRBY", b"DECRBY"):
                    before = str(current).encode()
                    a.sendall(request(b"SET", key, before))
                    expect(a, b"+OK\r\n", "boundary setup")
                    result = current + operand if cmd == b"INCRBY" else current - operand
                    a.sendall(request(cmd, key, str(operand).encode()))
                    if result < MIN or result > MAX:
                        expect(a, OVERFLOW, "reference overflow not rejected")
                        expected_value = before
                    else:
                        expect(a, integer(result), "Python big-int differential mismatch")
                        expected_value = str(result).encode()
                    b.sendall(request(b"GET", key))
                    expect(b, bulk(expected_value), "cross-client write or failed-update mismatch")


def malformed_corpus(port):
    key = b"malformed\x00key"
    bad = [b"", b"-", b"+", b"+0", b"+1", b"-0", b"00", b"01", b"-00", b"-01",
           b" 0", b"0 ", b"\t1", b"1\n", b"1.0", b"1e2", b"0x1", b"--1",
           b"9223372036854775808", b"-9223372036854775809", b"1\x00", b"1\xff",
           b"\xef\xbc\x91", b"9" * 512]
    with client(port) as a, client(port) as b:
        for raw in bad:
            a.sendall(request(b"SET", key, raw))
            expect(a, b"+OK\r\n", "malformed seed")
            a.sendall(request(b"INCR", key) + request(b"DECRBY", key, b"1"))
            expect(a, INVALID * 2, "noncanonical stored integer accepted")
            b.sendall(request(b"GET", key))
            expect(b, bulk(raw), "invalid integer changed stored bytes")
            a.sendall(request(b"DEL", key))
            expect(a, integer(1), "delete malformed seed")
            a.sendall(request(b"INCRBY", key, raw) + request(b"DECRBY", key, raw))
            expect(a, INVALID * 2, "invalid operand accepted")
            b.sendall(request(b"TYPE", key))
            expect(b, b"+none\r\n", "invalid operand created key")


def exact_fifo_and_recovery(port):
    key = b"fifo-numeric"
    ops = [(b"INCRBY", 3), (b"DECRBY", 5), (b"INCRBY", 2)] * 32
    with client(port) as a, client(port) as b:
        a.sendall(request(b"SET", key, b"0"))
        expect(a, b"+OK\r\n", "FIFO setup")
        payload = bytearray()
        expected = bytearray()
        value = 0
        for verb, operand in ops:
            payload.extend(request(verb, key, str(operand).encode()))
            value = value + operand if verb == b"INCRBY" else value - operand
            expected.extend(integer(value))
        # The oracle is computed from the operations, not loop positions.
        # This avoids the 4F test-oracle mistake with resetting counters.
        a.sendall(payload)
        expect(a, bytes(expected), "96 pipelined numerical results out of order")
        check(value == 0, "test model expected neutral triplets")
        b.sendall(request(b"GET", key))
        expect(b, bulk(b"0"), "pipeline final value wrong")
        a.sendall(request(b"SET", key, str(MAX).encode()) + request(b"INCR", key)
                  + request(b"GET", key) + request(b"DECR", key))
        expect(a, b"+OK\r\n" + OVERFLOW + bulk(str(MAX).encode()) + integer(MAX - 1),
               "connection did not recover after overflow")
        b.sendall(request(b"GET", key) + request(b"PING"))
        expect(b, bulk(str(MAX - 1).encode()) + b"+PONG\r\n",
               "cross-client state after failure incorrect")


def main():
    with server() as port:
        signed_boundary_matrix(port)
        malformed_corpus(port)
        exact_fifo_and_recovery(port)
    print("PASS: 4G Python big-int 286 boundary cases, malformed data, 96 FIFO replies")


if __name__ == "__main__":
    main()
