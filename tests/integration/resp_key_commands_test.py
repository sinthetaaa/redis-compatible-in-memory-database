#!/usr/bin/env python3
"""Phase 4C live RESP2 key commands, shared state, pipelines and redis-cli."""

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
    raise SystemExit("Usage: resp_key_commands_test.py SERVER_EXECUTABLE")

SERVER = sys.argv[1]
HOST = "127.0.0.1"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def bulk(value):
    return b"$" + str(len(value)).encode("ascii") + b"\r\n" + value + b"\r\n"


def request(*parts):
    return b"*" + str(len(parts)).encode("ascii") + b"\r\n" + b"".join(
        bulk(p) for p in parts
    )


def integer(value):
    return b":" + str(value).encode("ascii") + b"\r\n"


def recv_exact(sock, size):
    output = bytearray()
    while len(output) < size:
        part = sock.recv(size - len(output))
        check(bool(part), "premature TCP EOF")
        output.extend(part)
    return bytes(output)


def expect(sock, wanted, label):
    got = recv_exact(sock, len(wanted))
    check(got == wanted, f"{label}: got {got!r}; wanted {wanted!r}")


def connect(port):
    sock = socket.create_connection((HOST, port), timeout=5)
    sock.settimeout(5)
    return sock


@contextlib.contextmanager
def server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "server readiness timeout")
        header = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", header)
        check(match is not None, f"bad server readiness: {header!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate(timeout=5)
        check(process.returncode == 0, f"server exited abnormally: {stderr!r}")
        check("RESP_STOPPED" in stdout, "missing graceful stop")


def test_multi_client_and_binary(port):
    with connect(port) as a, connect(port) as b:
        a.sendall(request(b"TYPE", b"missing") + request(b"EXISTS", b"missing")
                  + request(b"DEL", b"missing"))
        expect(a, b"+none\r\n:0\r\n:0\r\n", "missing semantics")
        a.sendall(request(b"SET", b"a", b"first") + request(b"SET", b"b", b"second"))
        expect(a, b"+OK\r\n+OK\r\n", "seed keys")
        b.sendall(request(b"eXiStS", b"a", b"a", b"b", b"absent", b"b"))
        expect(b, integer(4), "EXISTS repeats counted")
        b.sendall(request(b"TYPE", b"a"))
        expect(b, b"+string\r\n", "TYPE string")
        b.sendall(request(b"dEl", b"a", b"missing", b"a", b"b", b"b"))
        expect(b, integer(2), "DEL repeated keys count once")
        a.sendall(request(b"GET", b"a") + request(b"TYPE", b"b"))
        expect(a, b"$-1\r\n+none\r\n", "DEL shared visibility")

        key = b"K\x00\r\n\xff"
        value = b"V\x00\r\n\xfe"
        a.sendall(request(b"SET", key, value))
        expect(a, b"+OK\r\n", "binary SET")
        b.sendall(request(b"EXISTS", key, b"K", key) + request(b"TYPE", key))
        expect(b, integer(2) + b"+string\r\n", "binary EXISTS/TYPE")
        b.sendall(request(b"DEL", b"K", key, key))
        expect(b, integer(1), "binary DEL")
        a.sendall(request(b"GET", key))
        expect(a, b"$-1\r\n", "binary deletion visible")
        a.sendall(request(b"SET", b"", b""))
        expect(a, b"+OK\r\n", "empty-key seed")
        b.sendall(request(b"EXISTS", b"", b"") + request(b"TYPE", b""))
        expect(b, integer(2) + b"+string\r\n", "empty key")
        b.sendall(request(b"DEL", b""))
        expect(b, integer(1), "empty key DEL")


def test_errors_and_pipeline(port):
    with connect(port) as sock:
        requests = (
            request(b"DEL") + request(b"EXISTS") + request(b"TYPE")
            + request(b"TYPE", b"a", b"b") + request(b"PING")
        )
        expected = (
            b"-ERR wrong number of arguments for 'del' command\r\n"
            b"-ERR wrong number of arguments for 'exists' command\r\n"
            b"-ERR wrong number of arguments for 'type' command\r\n"
            b"-ERR wrong number of arguments for 'type' command\r\n"
            b"+PONG\r\n"
        )
        sock.sendall(requests)
        expect(sock, expected, "arity errors must preserve connection and FIFO")

        frame = request(b"SET", b"fragment-key", b"v")
        sock.sendall(frame[:5])
        sock.sendall(frame[5:12])
        sock.sendall(frame[12:])
        expect(sock, b"+OK\r\n", "fragmented setup SET")

        data = bytearray()
        replies = bytearray()
        for i in range(40):
            key = b"multi:" + str(i).encode("ascii")
            data.extend(request(b"SET", key, b"v"))
            replies.extend(b"+OK\r\n")
            data.extend(request(b"EXISTS", key, key))
            replies.extend(integer(2))
            data.extend(request(b"TYPE", key))
            replies.extend(b"+string\r\n")
            data.extend(request(b"DEL", key, key))
            replies.extend(integer(1))
            data.extend(request(b"GET", key))
            replies.extend(b"$-1\r\n")
        sock.sendall(data)
        expect(sock, bytes(replies), "200 pipelined mixed replies")
        sock.sendall(request(b"GET", b"fragment-key"))
        expect(sock, bulk(b"v"), "pipeline corrupted earlier entry")


def test_multi_client_operations(port):
    def worker(index):
        with connect(port) as sock:
            key = b"worker:" + str(index).encode("ascii")
            for j in range(30):
                value = str(j).encode("ascii")
                sock.sendall(request(b"SET", key, value) + request(b"EXISTS", key)
                             + request(b"TYPE", key) + request(b"GET", key))
                expect(sock, b"+OK\r\n:1\r\n+string\r\n" + bulk(value),
                       "client data visibility")
            sock.sendall(request(b"DEL", key) + request(b"TYPE", key))
            expect(sock, b":1\r\n+none\r\n", "worker deletion")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(worker, range(8))), "concurrent client failure")


def test_quit_and_redis_cli(port):
    with connect(port) as sock:
        sock.sendall(request(b"SET", b"qkey", b"v") + request(b"QUIT")
                     + request(b"DEL", b"qkey"))
        expect(sock, b"+OK\r\n+OK\r\n", "QUIT drains and ignores following commands")
        check(sock.recv(1) == b"", "QUIT did not close")
    with connect(port) as sock:
        sock.sendall(request(b"EXISTS", b"qkey"))
        expect(sock, integer(1), "QUIT must not delete prior state")

    cli = shutil.which("redis-cli")
    if cli is None:
        print("SKIP: redis-cli absent; raw RESP2 TCP coverage passed")
        return

    def invoke(*argv):
        process = subprocess.run(
            [cli, "-2", "--raw", "-h", HOST, "-p", str(port), *argv],
            capture_output=True, text=True, timeout=6,
        )
        check(process.returncode == 0, f"redis-cli failed: {process.stderr!r}")
        return process.stdout.rstrip("\n")

    check(invoke("SET", "cli-key", "x") == "OK", "redis-cli SET")
    check(invoke("EXISTS", "cli-key", "cli-key", "missing") == "2", "redis-cli EXISTS")
    check(invoke("TYPE", "cli-key") == "string", "redis-cli TYPE")
    check(invoke("DEL", "cli-key", "cli-key") == "1", "redis-cli DEL")
    check(invoke("TYPE", "cli-key") == "none", "redis-cli missing TYPE")
    print("PASS: actual redis-cli DEL, EXISTS, TYPE")


def main():
    with server() as port:
        test_multi_client_and_binary(port)
        test_errors_and_pipeline(port)
        test_multi_client_operations(port)
        test_quit_and_redis_cli(port)
    print("PASS: Phase 4C RESP2 DEL/EXISTS/TYPE, pipeline, 8 clients, binary, redis-cli")


if __name__ == "__main__":
    main()
