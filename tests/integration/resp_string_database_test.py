#!/usr/bin/env python3
"""Live, stateful RESP2 SET/GET tests across pipelined and concurrent TCP clients."""

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
    raise SystemExit("Usage: resp_string_database_test.py SERVER_EXECUTABLE")

SERVER = sys.argv[1]
HOST = "127.0.0.1"
LIMIT = 8176
LIMIT_ERROR = b"-ERR string value exceeds the 8176-byte reply-safe limit\r\n"


def check(ok, what):
    if not ok:
        raise AssertionError(what)


def bulk(payload):
    return b"$" + str(len(payload)).encode("ascii") + b"\r\n" + payload + b"\r\n"


def request(*parts):
    return b"*" + str(len(parts)).encode("ascii") + b"\r\n" + b"".join(
        bulk(p) for p in parts
    )


def read_exact(conn, count):
    result = bytearray()
    while len(result) < count:
        part = conn.recv(count - len(result))
        check(part, "socket closed while reading reply")
        result.extend(part)
    return bytes(result)


def expect(conn, expected, message):
    check(read_exact(conn, len(expected)) == expected, message)


def client(port):
    s = socket.create_connection((HOST, port), timeout=5)
    s.settimeout(5)
    return s


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
        check(bool(ready), "server readiness timed out")
        first = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", first)
        check(match is not None, f"bad readiness: {first!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        check(proc.returncode == 0, f"server exited abnormally: {stderr!r}")
        check("RESP_STOPPED" in stdout, "server did not shut down cleanly")


def test_basic_and_shared(port):
    with client(port) as a, client(port) as b:
        a.sendall(request(b"GET", b"missing"))
        expect(a, b"$-1\r\n", "missing key must return nil")
        a.sendall(request(b"sEt", b"user:1", b"Alice"))
        expect(a, b"+OK\r\n", "SET reply")
        b.sendall(request(b"GET", b"user:1"))
        expect(b, bulk(b"Alice"), "cross-client GET")
        b.sendall(request(b"SET", b"user:1", b"Bob"))
        expect(b, b"+OK\r\n", "overwrite reply")
        a.sendall(request(b"get", b"user:1"))
        expect(a, bulk(b"Bob"), "shared overwritten value")
        a.sendall(request(b"GET", b"USER:1"))
        expect(a, b"$-1\r\n", "case-sensitive key")
        a.sendall(request(b"SET", b"", b""))
        expect(a, b"+OK\r\n", "empty key-value SET")
        b.sendall(request(b"GET", b""))
        expect(b, bulk(b""), "empty string versus nil")
        key = b"K\x00\r\n\xff"
        value = b"V\x00\xff\r\n\x80" * 70
        b.sendall(request(b"SET", key, value))
        expect(b, b"+OK\r\n", "binary SET")
        a.sendall(request(b"GET", key))
        expect(a, bulk(value), "binary GET")

        a.sendall(request(b"SET", b"user:1", b"failed", b"NX"))
        expected = b"-ERR wrong number of arguments for 'set' command\r\n"
        expect(a, expected, "unsupported option response")
        b.sendall(request(b"GET", b"user:1"))
        expect(b, bulk(b"Bob"), "failed SET overwrote live key")
        a.sendall(request(b"GET", b"user:1", b"unexpected"))
        expect(a, b"-ERR wrong number of arguments for 'get' command\r\n",
               "GET arity error")
        a.sendall(request(b"PING"))
        expect(a, b"+PONG\r\n", "PING regression")


def test_fragmented_and_pipelined(port):
    with client(port) as a:
        frame = request(b"SET", b"fragmented", b"value-with-\x00-binary")
        a.sendall(frame[:3])
        a.sendall(frame[3:19])
        a.sendall(frame[19:])
        expect(a, b"+OK\r\n", "fragmented SET")
        payload = bytearray()
        output = bytearray()
        for i in range(40):
            value = b"pip" + str(i).encode("ascii")
            payload.extend(request(b"SET", b"piped", value))
            output.extend(b"+OK\r\n")
            payload.extend(request(b"GET", b"piped"))
            output.extend(bulk(value))
        a.sendall(payload)
        expect(a, bytes(output), "80 pipelined SET/GET replies must remain in FIFO order")
        a.sendall(request(b"GET", b"fragmented"))
        expect(a, bulk(b"value-with-\x00-binary"), "fragmented value lost")


def test_value_limit(port):
    with client(port) as a, client(port) as b:
        valid = (bytes(range(256)) * ((LIMIT + 255) // 256))[:LIMIT]
        check(len(valid) == LIMIT, "test boundary bad")
        a.sendall(request(b"SET", b"size", valid))
        expect(a, b"+OK\r\n", "boundary SET")
        b.sendall(request(b"GET", b"size"))
        expect(b, bulk(valid), "boundary GET")
        a.sendall(request(b"SET", b"size", valid + b"X"))
        expect(a, LIMIT_ERROR, "oversize overwrite error")
        b.sendall(request(b"GET", b"size"))
        expect(b, bulk(valid), "oversize must preserve old value")
        a.sendall(request(b"SET", b"new-oversize", valid + b"X"))
        expect(a, LIMIT_ERROR, "oversize new key error")
        b.sendall(request(b"GET", b"new-oversize"))
        expect(b, b"$-1\r\n", "oversize failure created a key")


def test_concurrent(port):
    def worker(i):
        with client(port) as a:
            k = b"concurrent:" + str(i).encode("ascii")
            for j in range(25):
                value = bytes([i]) * 256 + str(j).encode("ascii")
                a.sendall(request(b"SET", k, value) + request(b"GET", k))
                expect(a, b"+OK\r\n" + bulk(value), "cross-client data corruption")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(worker, range(8))), "concurrent worker failed")
    with client(port) as reader:
        for i in range(8):
            k = b"concurrent:" + str(i).encode("ascii")
            reader.sendall(request(b"GET", k))
            expect(reader, bulk(bytes([i]) * 256 + b"24"),
                   "completed worker value must be visible to a new client")


def test_quit_retains_shared_state(port):
    with client(port) as a:
        a.sendall(request(b"SET", b"survives-quit", b"yes") + request(b"QUIT")
                  + request(b"SET", b"must-not-run", b"bad"))
        expect(a, b"+OK\r\n+OK\r\n", "SET/QUIT FIFO or final OK lost")
        check(a.recv(1) == b"", "QUIT must close after reply")
    with client(port) as b:
        b.sendall(request(b"GET", b"survives-quit") + request(b"GET", b"must-not-run"))
        expect(b, bulk(b"yes") + b"$-1\r\n", "QUIT broke shared state")


def test_redis_cli(port):
    cli = shutil.which("redis-cli")
    if cli is None:
        print("SKIP: redis-cli not installed; raw RESP2 validation passed")
        return
    def call(*args):
        proc = subprocess.run(
            [cli, "-2", "--raw", "-h", HOST, "-p", str(port), *args],
            text=True,
            capture_output=True,
            timeout=6,
        )
        check(proc.returncode == 0, f"redis-cli failed: {proc.stderr!r}")
        return proc.stdout.rstrip("\n")
    check(call("SET", "redis-cli:key", "from-client") == "OK", "redis-cli SET")
    check(call("GET", "redis-cli:key") == "from-client", "redis-cli GET")
    check(call("SET", "redis-cli:key", "new") == "OK", "redis-cli overwrite")
    check(call("GET", "redis-cli:key") == "new", "redis-cli GET overwrite")
    print("PASS: actual redis-cli SET/GET across connections")


def main():
    with server() as port:
        test_basic_and_shared(port)
        test_fragmented_and_pipelined(port)
        test_value_limit(port)
        test_concurrent(port)
        test_quit_retains_shared_state(port)
        test_redis_cli(port)
    # Persistence is not implemented; process restart must start empty.
    with server() as port:
        with client(port) as c:
            c.sendall(request(b"GET", b"survives-quit"))
            expect(c, b"$-1\r\n", "4B must not imply persistence")
    print("PASS: TCP SET/GET, binary, arity, pipelining, 8 clients, bounds, restart")


if __name__ == "__main__":
    main()
