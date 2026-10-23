#!/usr/bin/env python3
"""Phase 6C.2: seven RESP2 hash commands over real TCP with bounds and eight clients."""
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
    raise SystemExit("usage: resp_hash_commands_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"
WRONG = ("error", b"WRONGTYPE Operation against a key holding the wrong kind of value")


def check(ok, message):
    if not ok:
        raise AssertionError(message)


def req(*args):
    return b"*" + str(len(args)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(arg)).encode() + b"\r\n" + arg + b"\r\n" for arg in args
    )


def exact(sock, size):
    out = bytearray()
    while len(out) < size:
        part = sock.recv(size - len(out))
        check(bool(part), "unexpected EOF")
        out.extend(part)
    return bytes(out)


def line(sock):
    out = bytearray()
    while True:
        ch = exact(sock, 1)
        if ch == b"\r":
            check(exact(sock, 1) == b"\n", "bad RESP line ending")
            return bytes(out)
        check(len(out) < 8192, "oversize RESP line")
        out += ch


def reply(sock):
    kind = exact(sock, 1)
    if kind == b"+":
        return ("simple", line(sock))
    if kind == b"-":
        return ("error", line(sock))
    if kind == b":":
        return int(line(sock))
    if kind == b"$":
        n = int(line(sock))
        if n == -1:
            return None
        check(0 <= n <= 8192, "oversize bulk reply")
        content = exact(sock, n)
        check(exact(sock, 2) == b"\r\n", "bad bulk ending")
        return content
    if kind == b"*":
        n = int(line(sock))
        check(0 <= n <= 8192, "unexpected array count")
        return [reply(sock) for _ in range(n)]
    raise AssertionError(f"unknown RESP type: {kind!r}")


def send(sock, *args):
    sock.sendall(req(*args))
    return reply(sock)


def connect(port):
    sock = socket.create_connection((HOST, port), timeout=5)
    sock.settimeout(5)
    return sock


@contextlib.contextmanager
def running_server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "server did not start")
        banner = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", banner)
        check(match is not None, f"invalid ready banner {banner!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            out, err = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate(timeout=5)
        check(process.returncode == 0, f"server exit failed: {err!r}")
        check("RESP_STOPPED" in out, "server shutdown banner absent")


def acceptance(port):
    with connect(port) as a, connect(port) as b:
        key = b"key\x00\xff"
        field = b"f\x00\x80"
        payload = b"v\x00\xff\r\n"
        check(send(a, b"HGET", key, field) is None, "missing HGET")
        check(send(a, b"HEXISTS", key, field) == 0, "missing HEXISTS")
        check(send(a, b"HLEN", key) == 0, "missing HLEN")
        check(send(a, b"HDEL", key, field) == 0, "missing HDEL")
        check(send(a, b"HMGET", key, field, b"x") == [None, None], "missing HMGET")
        check(send(a, b"HGETALL", key) == [], "missing HGETALL")
        check(send(a, b"hSeT", key, field, b"old", b"other", b"", field, payload) == 2,
              "HSET duplicate semantics")
        check(send(b, b"TYPE", key) == ("simple", b"hash"), "TYPE hash on other client")
        check(send(b, b"HGET", key, field) == payload, "binary HGET across clients")
        check(send(b, b"HEXISTS", key, field) == 1, "HEXISTS true")
        check(send(b, b"HMGET", key, b"other", field, b"absent", field) ==
              [b"", payload, None, payload], "HMGET ordering and repeated fields")
        raw = send(b, b"HGETALL", key)
        check(len(raw) == 4 and dict(zip(raw[::2], raw[1::2])) ==
              {field: payload, b"other": b""}, "unordered paired HGETALL")
        check(send(a, b"HSET", key, field, b"latest", b"third", b"3") == 1,
              "HSET updated and new count")
        check(send(a, b"HLEN", key) == 3, "HLEN across clients")
        check(send(a, b"MGET", key) == [None], "MGET hash nil")
        check(send(a, b"GET", key) == WRONG, "GET hash wrongtype")
        check(send(a, b"LLEN", key) == WRONG, "LLEN hash wrongtype")
        check(send(a, b"PEXPIRE", key, b"5000") == 1, "set hash TTL")
        check(send(a, b"HSET", key, b"fourth", b"4") == 1, "HSET existing TTL")
        ttl = send(a, b"PTTL", key)
        check(isinstance(ttl, int) and 0 < ttl <= 5000, "HSET lost expiration")
        check(send(a, b"HDEL", key, field, b"third", b"other") == 3, "HDEL multiple")
        check(send(a, b"HDEL", key, b"fourth") == 1, "HDEL last field")
        check(send(b, b"TYPE", key) == ("simple", b"none"), "HDEL last deletes key")
        check(send(b, b"PTTL", key) == -2, "HDEL last cleans expiry")
        check(send(a, b"SET", b"string", b"s") == ("simple", b"OK"), "string seed")
        check(send(a, b"HSET", b"string", b"f", b"v") == WRONG, "HSET string wrongtype")
        check(send(a, b"HGETALL", b"string") == WRONG, "HGETALL wrongtype")
        check(send(a, b"RPUSH", b"list", b"x") == 1, "list seed")
        check(send(a, b"HMGET", b"list", b"f") == WRONG, "HMGET list wrongtype")
        check(send(a, b"HSET", b"replace", b"f", b"v") == 1, "hash seed for SET")
        check(send(a, b"SET", b"replace", b"after") == ("simple", b"OK"), "SET hash replacement")
        check(send(b, b"HGET", b"replace", b"f") == WRONG, "HGET after SET")
        check(send(a, b"HSET", b"mset", b"f", b"v") == 1, "hash seed for MSET")
        check(send(a, b"MSET", b"mset", b"new") == ("simple", b"OK"), "MSET replaces hash")
        check(send(b, b"GET", b"mset") == b"new", "MSET hash replacement")
        check(send(a, b"HSET", b"wide", b"a", b"a" * 4090) == 1, "large HSET 1")
        check(send(a, b"HSET", b"wide", b"b", b"b" * 4090) == 1, "large HSET 2")
        limit = ("error", b"ERR HGETALL response exceeds the 8192-byte limit")
        check(send(a, b"HGETALL", b"wide") == limit, "bounded HGETALL")
        check(send(a, b"HMGET", b"wide", b"a", b"b") ==
              ("error", b"ERR HMGET response exceeds the 8192-byte limit"), "bounded HMGET")
        check(send(a, b"HGET", b"wide", b"a") == b"a" * 4090, "HGET still works")
        check(send(a, b"HLEN", b"wide") == 2, "budget failure is nonmutating")
        frame = req(b"HSET", b"fragmented", b"a", b"1", b"b", b"2")
        for offset in (0, 3, 11):
            if offset == 0:
                a.sendall(frame[:3])
            elif offset == 3:
                a.sendall(frame[3:11])
            else:
                a.sendall(frame[11:])
        check(reply(a) == 2, "fragmented HSET")
        a.sendall(req(b"HMGET", b"fragmented", b"a", b"b") + req(b"PING"))
        check(reply(a) == [b"1", b"2"] and reply(a) == ("simple", b"PONG"),
              "pipeline ordering after fragmentation")
        check(send(a, b"HSET", b"odd", b"f") ==
              ("error", b"ERR wrong number of arguments for 'hset' command"), "HSET arity")
        # Values above 8176 bytes cannot fit inside the bounded 8192-byte
        # incoming RESP frame with a full HSET command. The direct unit test
        # verifies the handler's own reject-without-mutation policy instead.


def multiple_clients(port):
    def task(i):
        key = f"hashclient:{i}".encode()
        with connect(port) as sock:
            for j in range(12):
                field = f"f{j}".encode()
                val = f"v{j}".encode()
                sock.sendall(req(b"HSET", key, field, val) + req(b"HLEN", key))
                check(reply(sock) == 1 and reply(sock) == j + 1, "multiclient HSET/HLEN")
            check(send(sock, b"HMGET", key, b"f0", b"f11", b"absent") ==
                  [b"v0", b"v11", None], "multiclient HMGET")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(task, range(8))), "concurrent clients failed")


def redis_cli(port):
    cli = shutil.which("redis-cli")
    if cli is None:
        print("SKIP: redis-cli unavailable; raw TCP hash acceptance passed")
        return
    def run(*args):
        outcome = subprocess.run(
            [cli, "-2", "--raw", "-h", HOST, "-p", str(port), *args],
            capture_output=True, text=True, timeout=6,
        )
        check(outcome.returncode == 0, f"redis-cli error: {outcome.stderr!r}")
        return outcome.stdout.rstrip("\n")
    check(run("HSET", "cli:hash", "x", "1", "y", "2") == "2", "redis-cli HSET")
    check(run("HGET", "cli:hash", "x") == "1", "redis-cli HGET")
    check(run("HEXISTS", "cli:hash", "x") == "1", "redis-cli HEXISTS")
    check(run("HLEN", "cli:hash") == "2", "redis-cli HLEN")
    check(run("HMGET", "cli:hash", "x", "y") == "1\n2", "redis-cli HMGET")
    check(run("HDEL", "cli:hash", "x") == "1", "redis-cli HDEL")
    print("PASS: actual redis-cli hash commands")


def main():
    with running_server() as port:
        acceptance(port)
        multiple_clients(port)
        redis_cli(port)
    print("PASS: Phase 6C.2 real TCP hash commands, binary, TTL, wrongtype, bounds, 8 clients")


if __name__ == "__main__":
    main()
