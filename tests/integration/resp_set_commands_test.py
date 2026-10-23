#!/usr/bin/env python3
"""Phase 6D.2 six RESP2 set commands over real TCP, bounded replies, eight clients."""
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
    raise SystemExit("usage: resp_set_commands_test.py SERVER_EXECUTABLE")
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

def exact(sock, count):
    buf = bytearray()
    while len(buf) < count:
        part = sock.recv(count - len(buf))
        check(bool(part), "unexpected EOF")
        buf.extend(part)
    return bytes(buf)

def line(sock):
    out = bytearray()
    while True:
        ch = exact(sock, 1)
        if ch == b"\r":
            check(exact(sock, 1) == b"\n", "bad RESP line ending")
            return bytes(out)
        check(len(out) < 8192, "oversize line")
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
        check(0 <= n <= 8192, "oversize RESP2 bulk")
        value = exact(sock, n)
        check(exact(sock, 2) == b"\r\n", "bad bulk ending")
        return value
    if kind == b"*":
        n = int(line(sock))
        check(0 <= n <= 8192, "oversize array")
        return [reply(sock) for _ in range(n)]
    raise AssertionError(f"unrecognized RESP kind: {kind!r}")

def send(sock, *args):
    sock.sendall(req(*args))
    return reply(sock)

def connect(port):
    s = socket.create_connection((HOST, port), timeout=5)
    s.settimeout(5)
    return s

@contextlib.contextmanager
def running_server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "server startup timeout")
        banner = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", banner)
        check(match is not None, f"bad ready banner {banner!r}")
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
        check("RESP_STOPPED" in out, "server stop banner absent")

def acceptance(port):
    with connect(port) as a, connect(port) as b:
        key = b"set\x00\xff"
        binary = b"m\x00\xfe\r\n"
        check(send(a, b"SCARD", key) == 0, "missing SCARD")
        check(send(a, b"SISMEMBER", key, binary) == 0, "missing SISMEMBER")
        check(send(a, b"SREM", key, binary) == 0, "missing SREM")
        check(send(a, b"SMEMBERS", key) == [], "missing SMEMBERS")
        check(send(a, b"SMISMEMBER", key, binary, b"no") == [0, 0], "missing SMISMEMBER")
        check(send(a, b"sAdD", key, binary, b"", b"abc", binary, b"ABC") == 4, "SADD unique")
        check(send(b, b"TYPE", key) == ("simple", b"set"), "TYPE set")
        check(send(b, b"SCARD", key) == 4, "SCARD across clients")
        check(send(b, b"SISMEMBER", key, binary) == 1, "binary member")
        check(send(b, b"SMISMEMBER", key, b"ABC", b"never", binary, b"", binary) ==
              [1, 0, 1, 1, 1], "SMISMEMBER ordering, duplicates")
        check(set(send(b, b"SMEMBERS", key)) == {binary, b"", b"abc", b"ABC"},
              "SMEMBERS unordered bytes")
        check(send(a, b"SADD", key, b"abc", binary) == 0, "SADD existing no-op")
        check(send(a, b"MGET", key) == [None], "MGET set nil")
        check(send(a, b"GET", key) == WRONG, "GET set wrongtype")
        check(send(a, b"LLEN", key) == WRONG, "LLEN set wrongtype")
        check(send(a, b"HLEN", key) == WRONG, "HLEN set wrongtype")
        check(send(a, b"PEXPIRE", key, b"5000") == 1, "set TTL")
        check(send(a, b"SADD", key, b"fresh") == 1, "SADD retains TTL")
        remaining = send(a, b"PTTL", key)
        check(isinstance(remaining, int) and 0 < remaining <= 5000, "SADD lost TTL")
        check(send(a, b"SREM", key, binary, b"abc", b"ABC", b"") == 4, "SREM repeated")
        check(send(b, b"TYPE", key) == ("simple", b"set"), "nonempty set persists")
        check(send(a, b"SREM", key, b"fresh") == 1, "SREM last")
        check(send(b, b"PTTL", key) == -2, "SREM last deletes TTL/index")
        check(send(a, b"SET", b"str", b"x") == ("simple", b"OK"), "string setup")
        check(send(a, b"RPUSH", b"list", b"x") == 1, "list setup")
        check(send(a, b"HSET", b"hash", b"field", b"value") == 1, "hash setup")
        for typed in (b"str", b"list", b"hash"):
            for command, tail in ((b"SADD", (b"x",)), (b"SREM", (b"x",)),
                                  (b"SISMEMBER", (b"x",)), (b"SCARD", ()),
                                  (b"SMEMBERS", ()), (b"SMISMEMBER", (b"x",))):
                check(send(a, command, typed, *tail) == WRONG, "wrongtype "+str(command))
        check(send(a, b"SADD", b"convert", b"original") == 1, "new set")
        check(send(a, b"SET", b"convert", b"converted") == ("simple", b"OK"), "SET replacement")
        check(send(b, b"SISMEMBER", b"convert", b"original") == WRONG, "SET new type")
        check(send(a, b"SADD", b"mset", b"original") == 1, "MSET setup")
        check(send(a, b"MSET", b"mset", b"converted") == ("simple", b"OK"), "MSET replaces")
        check(send(b, b"GET", b"mset") == b"converted", "MSET result")
        check(send(a, b"SADD", b"wide", b"a"*4090) == 1, "large SADD a")
        check(send(a, b"SADD", b"wide", b"b"*4090) == 1, "large SADD b")
        check(send(a, b"SMEMBERS", b"wide") ==
              ("error", b"ERR SMEMBERS response exceeds the 8192-byte limit"), "SMEMBERS cap")
        check(send(a, b"SCARD", b"wide") == 2, "cap must not mutate")
        fragmented = req(b"SADD", b"pieces", b"a", b"b")
        a.sendall(fragmented[:3])
        a.sendall(fragmented[3:11])
        a.sendall(fragmented[11:])
        check(reply(a) == 2, "fragmented SADD")
        a.sendall(req(b"SMISMEMBER", b"pieces", b"b", b"x", b"a") + req(b"PING"))
        check(reply(a) == [1, 0, 1] and reply(a) == ("simple", b"PONG"),
              "pipelined SMISMEMBER and PING")
        check(send(a, b"SADD", b"bad") ==
              ("error", b"ERR wrong number of arguments for 'sadd' command"), "bad arity")

def multiple_clients(port):
    def task(i):
        key=f"setclient:{i}".encode()
        with connect(port) as sock:
            for j in range(10):
                member=f"member:{j}".encode()
                sock.sendall(req(b"SADD", key, member)+req(b"SCARD", key))
                check(reply(sock) == 1 and reply(sock) == j+1, "client SADD/SCARD")
            check(send(sock,b"SMISMEMBER",key,b"member:0",b"none",b"member:9") ==
                  [1,0,1], "client SMISMEMBER")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(task,range(8))), "eight-client acceptance")

def redis_cli(port):
    exe=shutil.which("redis-cli")
    if not exe:
        print("SKIP: redis-cli unavailable; raw TCP set commands passed")
        return
    def run(*args):
        result=subprocess.run([exe,"-2","--raw","-h",HOST,"-p",str(port),*args],
                              capture_output=True,text=True,timeout=6)
        check(result.returncode==0, f"redis-cli failure: {result.stderr!r}")
        return result.stdout.rstrip("\n")
    check(run("SADD","cli:set","a","b") == "2", "redis-cli SADD")
    check(run("SISMEMBER","cli:set","a") == "1", "redis-cli SISMEMBER")
    check(run("SCARD","cli:set") == "2", "redis-cli SCARD")
    check(set(run("SMEMBERS","cli:set").split("\n")) == {"a","b"}, "redis-cli SMEMBERS")
    check(run("SMISMEMBER","cli:set","a","z","b") == "1\n0\n1", "redis-cli SMISMEMBER")
    check(run("SREM","cli:set","a","b") == "2", "redis-cli SREM")
    print("PASS: redis-cli six set command subset")

def main():
    with running_server() as port:
        acceptance(port)
        multiple_clients(port)
        redis_cli(port)
    print("PASS: Phase 6D.2 live TCP six set routes, binary, TTL, bounds, eight clients")

if __name__ == "__main__":
    main()
