#!/usr/bin/env python3
"""Phase 4F external-process signed integer command integration tests."""
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
    raise SystemExit("usage: resp_integer_commands_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"
INVALID = b"-ERR value is not an integer or out of range\r\n"
OVERFLOW = b"-ERR increment or decrement would overflow\r\n"
MIN = -(2**63)
MAX = 2**63 - 1


def check(ok, msg):
    if not ok:
        raise AssertionError(msg)


def bulk(data):
    return b"$" + str(len(data)).encode() + b"\r\n" + data + b"\r\n"


def request(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(bulk(p) for p in parts)


def integer(n):
    return b":" + str(n).encode() + b"\r\n"


def recv_exact(sock, n):
    out = bytearray()
    while len(out) < n:
        piece = sock.recv(n - len(out))
        check(piece, "EOF while reading RESP2 reply")
        out.extend(piece)
    return bytes(out)


def expect(sock, wanted, msg):
    result = recv_exact(sock, len(wanted))
    check(result == wanted, f"{msg}: {result!r} != {wanted!r}")


def integer_reply(sock):
    prefix = recv_exact(sock, 1)
    check(prefix == b":", f"not an integer reply: {prefix!r}")
    digits = bytearray()
    while True:
        ch = recv_exact(sock, 1)
        digits.extend(ch)
        check(len(digits) <= 24, "unbounded integer reply")
        if digits.endswith(b"\r\n"):
            return int(digits[:-2])


def connect(port):
    c = socket.create_connection((HOST, port), timeout=5)
    c.settimeout(5)
    return c


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 5)
        check(ready, "server readiness timeout")
        greeting = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", greeting)
        check(match is not None, f"bad server ready line: {greeting!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        check(proc.returncode == 0, f"server crashed: {stderr!r}")
        check("RESP_STOPPED" in stdout, "server shutdown not graceful")


def basic_and_failures(port):
    with connect(port) as a, connect(port) as b:
        a.sendall(request(b"INCR", b"x") + request(b"INCR", b"x") +
                  request(b"DECR", b"x") + request(b"INCRBY", b"x", b"12") +
                  request(b"DECRBY", b"x", b"5") + request(b"INCRBY", b"x", b"-10") +
                  request(b"DECRBY", b"x", b"-5"))
        expect(a, b"".join(integer(n) for n in [1, 2, 1, 13, 8, -2, 3]),
               "basic integer arithmetic")
        b.sendall(request(b"GET", b"x"))
        expect(b, bulk(b"3"), "shared state after arithmetic")
        binary = b"key\x00\xff\r\n"
        b.sendall(request(b"INCR", binary) + request(b"DECRBY", binary, b"3") +
                  request(b"GET", binary))
        expect(b, integer(1) + integer(-2) + bulk(b"-2"), "binary key arithmetic")
        a.sendall(request(b"INCR", b""))
        expect(a, integer(1), "empty key")
        a.sendall(request(b"INCRBY", b"zero", b"0"))
        expect(a, integer(0), "zero operand")
        b.sendall(request(b"TYPE", b"zero") + request(b"STRLEN", b"zero"))
        expect(b, b"+string\r\n" + integer(1), "zero creates stored decimal bytes")

        a.sendall(request(b"SET", b"bad", b"not-a-number"))
        expect(a, b"+OK\r\n", "bad setup")
        a.sendall(request(b"INCR", b"bad") + request(b"INCRBY", b"new", b"01") +
                  request(b"DECRBY", b"new", b"+1") + request(b"PING"))
        expect(a, INVALID * 3 + b"+PONG\r\n", "invalid integer errors or connection")
        b.sendall(request(b"GET", b"bad") + request(b"TYPE", b"new"))
        expect(b, bulk(b"not-a-number") + b"+none\r\n", "error mutated keyspace")

        expected_arity = b"".join(
            b"-ERR wrong number of arguments for '" + verb + b"' command\r\n"
            for verb in (b"incr", b"decr", b"incrby", b"decrby")
        )
        a.sendall(request(b"INCR") + request(b"DECR", b"x", b"extra") +
                  request(b"INCRBY", b"x") + request(b"DECRBY", b"x", b"1", b"too-many"))
        expect(a, expected_arity, "arity errors")

        a.sendall(request(b"SET", b"max", str(MAX).encode()) +
                  request(b"SET", b"min", str(MIN).encode()))
        expect(a, b"+OK\r\n+OK\r\n", "boundary setup")
        a.sendall(request(b"INCR", b"max") + request(b"DECR", b"min") +
                  request(b"DECRBY", b"min", b"1") +
                  request(b"DECRBY", b"new-overflow", str(MIN).encode()))
        expect(a, OVERFLOW * 4, "integer overflow must not mutate")
        b.sendall(request(b"GET", b"max") + request(b"GET", b"min") +
                  request(b"TYPE", b"new-overflow"))
        expect(b, bulk(str(MAX).encode()) + bulk(str(MIN).encode()) + b"+none\r\n",
               "overflow state corruption")
        a.sendall(request(b"INCRBY", b"negative-min", str(MIN).encode()) +
                  request(b"DECRBY", b"negative-min", str(MIN).encode()))
        expect(a, integer(MIN) + integer(0), "INT64_MIN subtraction avoids UB")
        a.sendall(request(b"SET", b"near", b"-1") + request(b"DECRBY", b"near", str(MIN).encode()))
        expect(a, b"+OK\r\n" + integer(MAX), "-1 - INT64_MIN == INT64_MAX")


def pipeline_and_concurrent(port):
    with connect(port) as a:
        frame = request(b"INCRBY", b"fragmented", b"12")
        a.sendall(frame[:2]); a.sendall(frame[2:11]); a.sendall(frame[11:])
        expect(a, integer(12), "fragmented numeric command")
        requests = bytearray()
        wanted = bytearray()
        # Each pair starts and ends at zero: INCR -> 1, DECRBY 1 -> 0.
        # Keep all 120 requests in one TCP write to exercise FIFO scheduling.
        for _ in range(60):
            requests.extend(request(b"INCR", b"pipeline"))
            wanted.extend(integer(1))
            requests.extend(request(b"DECRBY", b"pipeline", b"1"))
            wanted.extend(integer(0))
        a.sendall(requests)
        expect(a, bytes(wanted), "120 ordered pipelined arithmetic operations")
        a.sendall(request(b"GET", b"pipeline"))
        expect(a, bulk(b"0"), "pipeline final value must be zero")

    with connect(port) as a:
        a.sendall(request(b"SET", b"shared-counter", b"0"))
        expect(a, b"+OK\r\n", "counter seed")

    def worker(index):
        seen = []
        with connect(port) as c:
            for _ in range(30):
                c.sendall(request(b"INCR", b"shared-counter"))
                seen.append(integer_reply(c))
        return seen

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(worker, range(8)))
    observed = [n for group in results for n in group]
    check(sorted(observed) == list(range(1, 241)),
          "lost update / repeated counter value among eight concurrent TCP clients")
    with connect(port) as c:
        c.sendall(request(b"GET", b"shared-counter"))
        expect(c, bulk(b"240"), "final shared counter wrong")


def cli_compatibility(port):
    cli = shutil.which("redis-cli")
    if not cli:
        print("SKIP: redis-cli not installed; raw RESP2 tests passed")
        return
    def call(*args):
        p = subprocess.run([cli, "-2", "--raw", "-h", HOST, "-p", str(port), *args],
                           capture_output=True, text=True, timeout=6)
        check(p.returncode == 0, f"redis-cli failure: {p.stderr!r}")
        return p.stdout.rstrip("\n")
    check(call("INCR", "cli-count") == "1", "redis-cli INCR")
    check(call("INCRBY", "cli-count", "20") == "21", "redis-cli INCRBY")
    check(call("DECR", "cli-count") == "20", "redis-cli DECR")
    check(call("DECRBY", "cli-count", "3") == "17", "redis-cli DECRBY")
    check(call("GET", "cli-count") == "17", "redis-cli GET after arithmetic")
    print("PASS: actual redis-cli INCR/DECR/INCRBY/DECRBY")


def main():
    with server() as port:
        basic_and_failures(port)
        pipeline_and_concurrent(port)
        cli_compatibility(port)
    with server() as port:
        with connect(port) as c:
            c.sendall(request(b"GET", b"shared-counter"))
            expect(c, b"$-1\r\n", "process restart should clear in-memory values")
    print("PASS: integer RESP2, 8 clients, atomic counters, limits, redis-cli, restart")


if __name__ == "__main__":
    main()
