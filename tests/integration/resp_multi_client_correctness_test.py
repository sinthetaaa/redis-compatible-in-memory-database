#!/usr/bin/env python3
"""Phase 4H: multi-client linearization, atomic snapshots, and isolation."""

import collections
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_multi_client_correctness_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def request(*items):
    return b"*" + str(len(items)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(item)).encode() + b"\r\n" + item + b"\r\n"
        for item in items
    )


def read_exact(sock, amount):
    data = bytearray()
    while len(data) < amount:
        piece = sock.recv(amount - len(data))
        check(piece, "unexpected EOF in RESP2 reply")
        data.extend(piece)
    return bytes(data)


def read_line(sock):
    data = bytearray()
    while True:
        char = read_exact(sock, 1)
        data.extend(char)
        check(len(data) <= 96, "oversized RESP2 header")
        if data.endswith(b"\r\n"):
            return bytes(data[:-2])


def reply(sock):
    prefix = read_exact(sock, 1)
    if prefix in (b"+", b"-"):
        return prefix, read_line(sock)
    if prefix == b":":
        return int(read_line(sock))
    if prefix == b"$":
        length = int(read_line(sock))
        if length == -1:
            return None
        check(0 <= length <= 8192, "invalid bulk reply size")
        data = read_exact(sock, length)
        check(read_exact(sock, 2) == b"\r\n", "missing bulk terminator")
        return data
    if prefix == b"*":
        count = int(read_line(sock))
        check(0 <= count <= 1024, "unexpected array reply size")
        return [reply(sock) for _ in range(count)]
    raise AssertionError(f"invalid RESP2 reply prefix: {prefix!r}")


def connect(port):
    conn = socket.create_connection((HOST, port), timeout=10)
    conn.settimeout(10)
    return conn


def command(sock, *args):
    sock.sendall(request(*args))
    return reply(sock)


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "24"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 10)
        check(ready, "RESP2 server startup timeout")
        line = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", line)
        check(match is not None, f"unexpected server startup: {line!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            output, errors = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, errors = proc.communicate(timeout=8)
        check(proc.returncode == 0, f"abnormal server exit: {errors!r}")
        check("RESP_STOPPED" in output, "graceful shutdown marker absent")


def concurrent_counters(port):
    with connect(port) as seed:
        check(command(seed, b"SET", b"shared:counter", b"0") == (b"+", b"OK"),
              "counter setup failed")
    start = threading.Barrier(11)

    def increment_worker():
        with connect(port) as sock:
            start.wait(timeout=12)
            observations = []
            for _ in range(80):
                observed = command(sock, b"INCR", b"shared:counter")
                check(isinstance(observed, int), "INCR response was not an integer")
                observations.append(observed)
            return observations

    def read_worker():
        with connect(port) as sock:
            start.wait(timeout=12)
            observations = []
            for _ in range(110):
                value = command(sock, b"GET", b"shared:counter")
                check(isinstance(value, bytes), "counter GET returned nonbulk reply")
                observations.append(int(value))
            check(observations == sorted(observations),
                  "single observer witnessed the counter decrease")
            return observations

    with ThreadPoolExecutor(max_workers=11) as executor:
        writers = [executor.submit(increment_worker) for _ in range(8)]
        readers = [executor.submit(read_worker) for _ in range(3)]
        results = [future.result() for future in writers]
        snapshots = [future.result() for future in readers]
    observed = [n for series in results for n in series]
    check(sorted(observed) == list(range(1, 641)),
          "counter increments lost, duplicated, or not linearized")
    check(all(0 <= n <= 640 for series in snapshots for n in series),
          "observer saw an impossible counter value")
    with connect(port) as sock:
        check(command(sock, b"GET", b"shared:counter") == b"640",
              "final counter missing increments")


def atomic_mset_snapshots(port):
    keys = (b"snapshot:left", b"snapshot:right", b"snapshot:\x00\xff")
    with connect(port) as seed:
        check(command(seed, b"MSET", keys[0], b"seed", keys[1], b"seed",
                      keys[2], b"seed") == (b"+", b"OK"), "snapshot setup")
    start = threading.Barrier(6)

    def writer(index):
        with connect(port) as sock:
            start.wait(timeout=12)
            for iteration in range(140):
                stamp = f"W{index}:{iteration:03d}".encode()
                outcome = command(sock, b"MSET", keys[0], stamp, keys[1], stamp,
                                  keys[2], stamp)
                check(outcome == (b"+", b"OK"), "MSET writer failed")
        return True

    def reader():
        with connect(port) as sock:
            start.wait(timeout=12)
            for _ in range(160):
                snapshot = command(sock, b"MGET", keys[2], keys[0], b"missing:\x00",
                                   keys[1], keys[0])
                check(isinstance(snapshot, list) and len(snapshot) == 5,
                      "unexpected MGET array")
                check(snapshot[0] == snapshot[1] == snapshot[3] == snapshot[4],
                      "partial MSET became visible within a single MGET")
                check(snapshot[2] is None, "MGET missing key not nil")
                check(snapshot[0] == b"seed" or re.fullmatch(rb"W[01]:[0-9]{3}", snapshot[0]),
                      "snapshot contains an impossible revision")
        return True

    with ThreadPoolExecutor(max_workers=6) as executor:
        futures = [executor.submit(writer, i) for i in range(2)]
        futures.extend(executor.submit(reader) for _ in range(4))
        check(all(future.result() for future in futures), "snapshot workers failed")


def concurrent_append(port):
    with connect(port) as seed:
        check(command(seed, b"SET", b"shared:append", b"") == (b"+", b"OK"),
              "append setup")
    start = threading.Barrier(8)

    def writer(index):
        parts = []
        lengths = []
        with connect(port) as sock:
            start.wait(timeout=12)
            for iteration in range(45):
                part = f"{index}:{iteration:03d}|".encode()
                parts.append(part)
                lengths.append(command(sock, b"APPEND", b"shared:append", part))
        return parts, lengths

    with ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(writer, range(8)))
    expected_parts = [part for parts, _ in results for part in parts]
    all_lengths = [length for _, lengths in results for length in lengths]
    check(sorted(all_lengths) == list(range(6, 2161, 6)),
          "atomic APPEND length sequence lost or duplicated")
    with connect(port) as sock:
        value = command(sock, b"GET", b"shared:append")
        check(isinstance(value, bytes) and len(value) == 2160,
              "final APPEND value truncated")
        tokens = value.split(b"|")
        check(tokens[-1] == b"", "APPEND delimiters corrupted")
        check(collections.Counter(token + b"|" for token in tokens[:-1]) ==
              collections.Counter(expected_parts), "APPEND tokens lost or corrupted")
        check(command(sock, b"STRLEN", b"shared:append") == 2160,
              "STRLEN disagrees with complete APPEND result")


def pipelining_and_isolation(port):
    with connect(port) as a, connect(port) as b:
        payload = bytearray()
        for _ in range(72):
            payload.extend(request(b"APPEND", b"ordered:pipe", b"Q"))
            payload.extend(request(b"STRLEN", b"ordered:pipe"))
        a.sendall(payload)
        for i in range(1, 73):
            check(reply(a) == i and reply(a) == i, "FIFO pipeline response mismatch")
        check(command(b, b"GET", b"ordered:pipe") == b"Q" * 72,
              "pipeline side effects were reordered")
        check(command(a, b"SET", b"bad:numeric", b"text") == (b"+", b"OK"),
              "invalid numeric setup")
        failed = command(a, b"INCR", b"bad:numeric")
        check(isinstance(failed, tuple) and failed[0] == b"-",
              "invalid numeric mutation was not rejected")
        check(command(b, b"GET", b"bad:numeric") == b"text",
              "invalid operation changed shared state")
        check(command(b, b"PING") == (b"+", b"PONG"),
              "one client's error affected another client")
        a.sendall(request(b"APPEND", b"quit:key", b"kept") + request(b"QUIT") +
                  request(b"APPEND", b"quit:key", b"ignored"))
        check(reply(a) == 4, "pre-QUIT APPEND failed")
        check(reply(a) == (b"+", b"OK"), "QUIT acknowledgment wrong")
        check(a.recv(1) == b"", "QUIT did not close the client")
        check(command(b, b"GET", b"quit:key") == b"kept",
              "commands executed after QUIT")

    for _ in range(45):
        with connect(port) as churn:
            check(command(churn, b"PING") == (b"+", b"PONG"),
                  "connection churn damaged liveness")
    with connect(port) as final:
        check(command(final, b"GET", b"shared:counter") == b"640",
              "connection churn corrupted shared database")


def main():
    with server() as port:
        concurrent_counters(port)
        atomic_mset_snapshots(port)
        concurrent_append(port)
        pipelining_and_isolation(port)
    with server() as port:
        with connect(port) as fresh:
            check(command(fresh, b"GET", b"shared:counter") is None,
                  "process restart must reset the in-memory keyspace")
    print("PASS: 4H counters 640, 2 writers/4 readers atomic MSET, "
          "360 APPENDs, 144 FIFO commands, QUIT/churn/restart")


if __name__ == "__main__":
    main()
