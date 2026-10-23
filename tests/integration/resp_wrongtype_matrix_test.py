#!/usr/bin/env python3
"""Phase 6E: 78-case cross-type WRONGTYPE matrix over real loopback RESP2 TCP."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_wrongtype_matrix_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]
HOST = "127.0.0.1"
WRONG = ("error", b"WRONGTYPE Operation against a key holding the wrong kind of value")


def check(condition, description):
    if not condition:
        raise AssertionError(description)


def request(*arguments):
    return b"*" + str(len(arguments)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(arg)).encode() + b"\r\n" + arg + b"\r\n" for arg in arguments
    )


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        check(bool(chunk), "unexpected EOF")
        data.extend(chunk)
    return bytes(data)


def line(sock):
    data = bytearray()
    while True:
        item = exact(sock, 1)
        if item == b"\r":
            check(exact(sock, 1) == b"\n", "malformed RESP line ending")
            return bytes(data)
        check(len(data) < 8192, "oversize RESP line")
        data.extend(item)


def reply(sock):
    prefix = exact(sock, 1)
    if prefix == b"+":
        return ("simple", line(sock))
    if prefix == b"-":
        return ("error", line(sock))
    if prefix == b":":
        return int(line(sock))
    if prefix == b"$":
        count = int(line(sock))
        if count == -1:
            return None
        check(0 <= count <= 8192, "oversize bulk reply")
        data = exact(sock, count)
        check(exact(sock, 2) == b"\r\n", "malformed bulk terminator")
        return data
    if prefix == b"*":
        count = int(line(sock))
        check(0 <= count <= 8192, "oversize array reply")
        return [reply(sock) for _ in range(count)]
    raise AssertionError(f"invalid RESP2 prefix: {prefix!r}")


def send(sock, *args):
    sock.sendall(request(*args))
    return reply(sock)


def connect(port):
    sock = socket.create_connection((HOST, port), timeout=5)
    sock.settimeout(5)
    return sock


@contextlib.contextmanager
def server():
    process = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([process.stdout], [], [], 5)
        check(bool(ready), "server startup timeout")
        banner = process.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", banner)
        check(match is not None, f"bad ready banner: {banner!r}")
        yield int(match.group(1))
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate(timeout=5)
        check(process.returncode == 0, f"server exit failed: {stderr!r}")
        check("RESP_STOPPED" in stdout, "server shutdown banner missing")


OPERATIONS = (
    ("string", b"GET", ()),
    ("string", b"APPEND", (b"suffix",)),
    ("string", b"STRLEN", ()),
    ("string", b"INCR", ()),
    ("string", b"DECR", ()),
    ("string", b"INCRBY", (b"3",)),
    ("string", b"DECRBY", (b"3",)),
    ("list", b"LPUSH", (b"insert",)),
    ("list", b"RPUSH", (b"insert",)),
    ("list", b"LLEN", ()),
    ("list", b"LPOP", ()),
    ("list", b"RPOP", ()),
    ("list", b"LRANGE", (b"0", b"-1")),
    ("hash", b"HSET", (b"field", b"replacement")),
    ("hash", b"HGET", (b"field",)),
    ("hash", b"HEXISTS", (b"field",)),
    ("hash", b"HLEN", ()),
    ("hash", b"HDEL", (b"field",)),
    ("hash", b"HMGET", (b"field",)),
    ("hash", b"HGETALL", ()),
    ("set", b"SADD", (b"added",)),
    ("set", b"SREM", (b"member",)),
    ("set", b"SISMEMBER", (b"member",)),
    ("set", b"SCARD", ()),
    ("set", b"SMEMBERS", ()),
    ("set", b"SMISMEMBER", (b"member", b"absent")),
)


def acceptance(port):
    with connect(port) as actor, connect(port) as observer:
        typed = (
            ("string", b"str\x00\xff", b"GET", ()),
            ("list", b"list\x00\xff", b"LRANGE", (b"0", b"-1")),
            ("hash", b"hash\x00\xff", b"HGETALL", ()),
            ("set", b"set\x00\xff", b"SMEMBERS", ()),
        )
        check(send(actor, b"SET", typed[0][1], b"19") == ("simple", b"OK"), "seed string")
        check(send(actor, b"RPUSH", typed[1][1], b"first", b"second") == 2, "seed list")
        check(send(actor, b"HSET", typed[2][1], b"field", b"value") == 1, "seed hash")
        check(send(actor, b"SADD", typed[3][1], b"member", b"other") == 2, "seed set")
        for kind, key, _, _ in typed:
            check(send(actor, b"PEXPIRE", key, b"180000") == 1, "set finite TTL")
            check(send(observer, b"TYPE", key) == ("simple", kind.encode()), "seed TYPE")

        count = 0
        for kind, key, read, tail in typed:
            original = send(observer, read, key, *tail)
            prior_ttl = send(observer, b"PTTL", key)
            check(isinstance(prior_ttl, int) and 0 < prior_ttl <= 180000, "valid TTL")
            for owner, name, args in OPERATIONS:
                if owner == kind:
                    continue
                result = send(actor, name, key, *args)
                check(result == WRONG, f"{name!r} on {kind} expected WRONGTYPE, got {result!r}")
                check(send(observer, b"TYPE", key) == ("simple", kind.encode()),
                      "foreign command changed TYPE")
                check(send(observer, read, key, *tail) == original,
                      "foreign command modified existing collection")
                current_ttl = send(observer, b"PTTL", key)
                check(isinstance(current_ttl, int) and 0 < current_ttl <= prior_ttl,
                      "foreign command changed TTL or expiry metadata")
                prior_ttl = current_ttl
                count += 1
        check(count == 78, "expected 26 commands against three incompatible kinds")
        check(send(observer, b"MGET", *(item[1] for item in typed), b"missing", typed[0][1]) ==
              [b"19", None, None, None, None, b"19"],
              "MGET preserves nil non-string and duplicates")
        check(send(actor, b"TYPE", b"missing") == ("simple", b"none"), "missing TYPE")
        check(send(actor, b"HLEN", b"missing") == 0, "missing HLEN")
        check(send(actor, b"SCARD", b"missing") == 0, "missing SCARD")
        check(send(actor, b"LRANGE", b"missing", b"0", b"-1") == [],
              "missing LRANGE")

        # Pipelined wrong-type results must not corrupt framing or command order.
        actor.sendall(
            request(b"GET", typed[3][1]) + request(b"HSET", typed[1][1], b"f", b"v") +
            request(b"SADD", typed[2][1], b"x") + request(b"LPUSH", typed[0][1], b"x") +
            request(b"PING")
        )
        for _ in range(4):
            check(reply(actor) == WRONG, "pipeline WRONGTYPE result")
        check(reply(actor) == ("simple", b"PONG"), "pipeline recovery after errors")
        check(send(observer, b"SCARD", typed[3][1]) == 2, "observer sees unmodified set")

        # SET and MSET are intentionally type-changing, not WRONGTYPE cases.
        check(send(actor, b"SET", typed[1][1], b"converted") == ("simple", b"OK"),
              "SET replaces list")
        check(send(observer, b"GET", typed[1][1]) == b"converted", "SET new value")
        check(send(observer, b"PTTL", typed[1][1]) == -1, "SET replacement clears TTL")
        check(send(actor, b"MSET", typed[2][1], b"hash-replaced", typed[3][1], b"set-replaced") ==
              ("simple", b"OK"), "MSET replaces hash and set")
        for key, expected in ((typed[2][1], b"hash-replaced"),
                              (typed[3][1], b"set-replaced")):
            check(send(observer, b"GET", key) == expected, "MSET new bytes")
            check(send(observer, b"PTTL", key) == -1, "MSET clears TTL")


def main():
    with server() as port:
        acceptance(port)
    print("PASS: Phase 6E live-TCP 78-case cross-type WRONGTYPE matrix, rollback and framing")


if __name__ == "__main__":
    main()
