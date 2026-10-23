#!/usr/bin/env python3
"""Phase 6B.2: real TCP RESP2 list semantics, binary data, limits, TTL and clients."""
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
    raise SystemExit("usage: resp_list_commands_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"
WRONG = b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"

def check(ok, reason):
    if not ok:
        raise AssertionError(reason)

def bulk(data):
    return b"$" + str(len(data)).encode() + b"\r\n" + data + b"\r\n"

def req(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(bulk(x) for x in parts)

def arr(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(bulk(x) for x in parts)

def integer(n):
    return b":" + str(n).encode() + b"\r\n"

def exact(sock, n):
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        check(chunk, "unexpected EOF reading response")
        out.extend(chunk)
    return bytes(out)

def expect(sock, reply, label):
    got = exact(sock, len(reply))
    check(got == reply, f"{label}: got {got!r}, expected {reply!r}")

def connect(port):
    sock = socket.create_connection((HOST, port), timeout=5)
    sock.settimeout(5)
    return sock

@contextlib.contextmanager
def running_server():
    process = subprocess.Popen([SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, bufsize=1)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "server did not become ready")
        line = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", line)
        check(match is not None, f"bad ready banner: {line!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            out, err = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate(timeout=5)
        check(process.returncode == 0, f"unexpected server exit: {err!r}")
        check("RESP_STOPPED" in out, "server did not stop cleanly")

def acceptance(port):
    with connect(port) as a, connect(port) as b:
        key = b"key\x00\xff"
        blob = b"a\x00\xfe\r\n"
        a.sendall(req(b"LLEN", key) + req(b"LPOP", key) + req(b"LRANGE", key, b"0", b"-1"))
        expect(a, b":0\r\n$-1\r\n*0\r\n", "missing list responses")
        a.sendall(req(b"rPuSh", key, b"one", blob, b"one") + req(b"LPUSH", key, b"l1", b"l2"))
        expect(a, integer(3) + integer(5), "push order and counts")
        b.sendall(req(b"TYPE", key) + req(b"LRANGE", key, b"0", b"-1"))
        expect(b, b"+list\r\n" + arr(b"l2", b"l1", b"one", blob, b"one"), "live list cross client")
        b.sendall(req(b"LRANGE", key, b"1", b"-2") + req(b"LRANGE", key, b"-2", b"-1"))
        expect(b, arr(b"l1", b"one", blob) + arr(blob, b"one"), "negative range semantics")
        a.sendall(req(b"MGET", key) + req(b"GET", key) + req(b"STRLEN", key))
        expect(a, b"*1\r\n$-1\r\n" + WRONG + WRONG, "cross-type string handling")
        b.sendall(req(b"RPOP", key) + req(b"LPOP", key) + req(b"LLEN", key))
        expect(b, bulk(b"one") + bulk(b"l2") + integer(3), "pop order")
        a.sendall(req(b"PEXPIRE", key, b"5000") + req(b"RPUSH", key, b"tail") + req(b"PTTL", key))
        expect(a, integer(1) + integer(4), "TTL mutation setup")
        # Read integer PTTL without assuming exact wall clock rounding.
        check(exact(a, 1) == b":", "PTTL wire prefix")
        digits = bytearray()
        while True:
            ch = exact(a, 1)
            if ch == b"\r":
                check(exact(a, 1) == b"\n", "PTTL line ending")
                break
            digits.extend(ch)
        ttl = int(digits)
        check(0 < ttl <= 5000, f"TTL changed or expired unexpectedly: {ttl}")
        a.sendall(req(b"DEL", key) + req(b"TYPE", key))
        expect(a, integer(1) + b"+none\r\n", "list deletion and type")

        a.sendall(req(b"SET", b"string", b"abc") + req(b"LPUSH", b"string", b"x"))
        expect(a, b"+OK\r\n" + WRONG, "push on string")
        b.sendall(req(b"LRANGE", b"string", b"0", b"-1") + req(b"LLEN", b"string"))
        expect(b, WRONG + WRONG, "list reads on string")
        a.sendall(req(b"LPUSH", b"convert", b"first") + req(b"SET", b"convert", b"new"))
        expect(a, integer(1) + b"+OK\r\n", "SET replaces list")
        b.sendall(req(b"GET", b"convert") + req(b"RPUSH", b"convert", b"should-fail"))
        expect(b, bulk(b"new") + WRONG, "SET replacement and wrongtype")

        bad = b"-ERR value is not an integer or out of range\r\n"
        a.sendall(req(b"LRANGE", b"convert", b"not-number", b"-1") +
                  req(b"LRANGE", b"convert", b"0", b"9223372036854775808") + req(b"PING"))
        expect(a, bad + bad + b"+PONG\r\n", "bad indices preserve connection")
        oversize = b"-ERR LRANGE response exceeds the 8192-byte limit\r\n"
        # Two separate frames keep each request beneath the parser input cap.
        a.sendall(req(b"RPUSH", b"huge", b"a"*4090) +
                  req(b"RPUSH", b"huge", b"b"*4090) +
                  req(b"LRANGE", b"huge", b"0", b"-1") + req(b"LLEN", b"huge"))
        expect(a, integer(1) + integer(2) + oversize + integer(2), "bounded reply, no mutation")
        # Fragmented list command and pipelining on a persistent connection.
        frame = req(b"RPUSH", b"fragment", b"x", b"y")
        a.sendall(frame[:4]); a.sendall(frame[4:13]); a.sendall(frame[13:])
        expect(a, integer(2), "fragmented RPUSH")
        a.sendall(req(b"LRANGE", b"fragment", b"0", b"-1") + req(b"PING"))
        expect(a, arr(b"x", b"y") + b"+PONG\r\n", "pipeline remains ordered")

def concurrent_clients(port):
    def task(i):
        key = ("listclient:" + str(i)).encode()
        with connect(port) as sock:
            for j in range(15):
                elem = str(j).encode()
                sock.sendall(req(b"RPUSH", key, elem) + req(b"LLEN", key))
                expect(sock, integer(j+1) + integer(j+1), "multi-client append")
            sock.sendall(req(b"LRANGE", key, b"0", b"-1"))
            expect(sock, arr(*(str(j).encode() for j in range(15))), "multi-client range")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(task, range(8))), "concurrent clients failed")

def redis_cli(port):
    cli = shutil.which("redis-cli")
    if not cli:
        print("SKIP: redis-cli unavailable; raw TCP tests passed")
        return
    def run(*args):
        outcome = subprocess.run([cli, "-2", "--raw", "-h", HOST, "-p", str(port), *args],
                                 capture_output=True, text=True, timeout=6)
        check(outcome.returncode == 0, f"redis-cli error: {outcome.stderr!r}")
        return outcome.stdout.rstrip("\n")
    check(run("RPUSH", "cli:list", "a", "b") == "2", "redis-cli RPUSH")
    check(run("LPUSH", "cli:list", "head") == "3", "redis-cli LPUSH")
    check(run("LRANGE", "cli:list", "0", "-1") == "head\na\nb", "redis-cli LRANGE")
    check(run("LPOP", "cli:list") == "head", "redis-cli LPOP")
    check(run("LLEN", "cli:list") == "2", "redis-cli LLEN")
    print("PASS: actual redis-cli list commands")

def main():
    with running_server() as port:
        acceptance(port)
        concurrent_clients(port)
        redis_cli(port)
    print("PASS: live Phase 6B.2 list commands, binary, wrongtype, TTL, limits, 8 clients, redis-cli")

if __name__ == "__main__":
    main()
