#!/usr/bin/env python3
"""Phase 5E: live TTL/PTTL/PERSIST commands through two real RESP2 TCP clients."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_ttl_pttl_persist_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def request(*parts):
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(p)).encode() + b"\r\n" + p + b"\r\n" for p in parts
    )


def exact(sock, count):
    out = bytearray()
    while len(out) < count:
        part = sock.recv(count - len(out))
        check(part, "unexpected TCP EOF")
        out.extend(part)
    return bytes(out)


def line(sock):
    buf = bytearray()
    while True:
        buf.extend(exact(sock, 1))
        check(len(buf) <= 128, "oversized RESP2 line")
        if buf.endswith(b"\r\n"):
            return bytes(buf[:-2])


def reply(sock):
    code = exact(sock, 1)
    if code == b":":
        return int(line(sock))
    if code in (b"+", b"-"):
        return code, line(sock)
    if code == b"$":
        size = int(line(sock))
        if size == -1:
            return None
        check(0 <= size <= 8192, "invalid bulk length")
        value = exact(sock, size)
        check(exact(sock, 2) == b"\r\n", "malformed bulk terminator")
        return value
    raise AssertionError(f"unexpected reply prefix: {code!r}")


def command(sock, *args):
    sock.sendall(request(*args))
    return reply(sock)


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=8)
    sock.settimeout(8)
    return sock


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 8)
        check(ready, "RESP2 readiness timeout")
        startup = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", startup)
        check(match is not None, f"unexpected readiness line: {startup!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            out, err = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate(timeout=8)
        check(proc.returncode == 0, f"server exited abnormally: {err!r}")
        check("RESP_STOPPED" in out, "missing graceful stop marker")


def test_live(port):
    key = b"ttl\x00\xff\r\n"
    with connect(port) as a, connect(port) as b:
        check(command(a, b"TTL", key) == -2, "missing binary TTL")
        check(command(b, b"PTTL", key) == -2, "missing binary PTTL")
        check(command(b, b"PERSIST", key) == 0, "missing PERSIST")
        check(command(a, b"SET", key, b"value") == (b"+", b"OK"), "SET seed")
        check(command(b, b"tTl", key) == -1, "persistent TTL")
        check(command(b, b"pTtL", key) == -1, "persistent PTTL")
        check(command(a, b"PERSIST", key) == 0, "already-persistent PERSIST")
        check(command(a, b"EXPIRE", key, b"600") == 1, "EXPIRE fixture")
        ttl = command(b, b"TTL", key)
        pttl = command(b, b"PTTL", key)
        check(isinstance(ttl, int) and 590 <= ttl <= 600, f"unexpected TTL: {ttl}")
        check(isinstance(pttl, int) and 590000 <= pttl <= 600000, f"unexpected PTTL: {pttl}")
        check(command(b, b"pErSiSt", key) == 1, "cross-client PERSIST")
        check(command(a, b"TTL", key) == -1, "cross-client persistence invisible")
        check(command(a, b"GET", key) == b"value", "PERSIST changed bytes")
        check(command(b, b"PERSIST", key) == 0, "PERSIST second call")
        check(command(a, b"PEXPIRE", key, b"600000") == 1, "PEXPIRE fixture")
        check(command(a, b"SET", key, b"replacement") == (b"+", b"OK"), "SET overwrite")
        check(command(b, b"PTTL", key) == -1, "SET did not clear expiration")
        check(command(a, b"EXPIRE", key, b"0") == 1, "zero expiration")
        check(command(b, b"TTL", key) == -2, "immediately deleted TTL")
        check(command(a, b"PERSIST", key) == 0, "deleted key PERSIST")
        check(command(a, b"SET", b"", b"empty") == (b"+", b"OK"), "empty-key seed")
        check(command(b, b"TTL", b"") == -1, "empty-key TTL")
        check(command(b, b"EXPIRE", b"", b"600") == 1, "empty-key EXPIRE")
        check(command(a, b"PERSIST", b"") == 1, "empty-key PERSIST")
        check(command(a, b"GET", b"") == b"empty", "empty-key value lost")

        invalid = [
            request(b"TTL"), request(b"PTTL", key, b"extra"),
            request(b"PERSIST"), request(b"PERSIST", key, b"extra"),
            request(b"PING"),
        ]
        a.sendall(b"".join(invalid))
        for name in (b"ttl", b"pttl", b"persist", b"persist"):
            check(reply(a) == (b"-", b"ERR wrong number of arguments for '" + name + b"' command"),
                  f"{name!r} arity or FIFO error")
        check(reply(a) == (b"+", b"PONG"), "PING after errors")

        # 32 groups of six commands; expired/persistent replies stay in FIFO order.
        frames = bytearray()
        for i in range(32):
            key_i = f"ttl:{i:02d}".encode()
            for parts in ((b"SET", key_i, b"v"), (b"TTL", key_i),
                          (b"PEXPIRE", key_i, b"600000"), (b"PTTL", key_i),
                          (b"PERSIST", key_i), (b"TTL", key_i)):
                frames.extend(request(*parts))
        a.sendall(frames)
        for i in range(32):
            check(reply(a) == (b"+", b"OK"), f"FIFO SET {i}")
            check(reply(a) == -1, f"FIFO initial TTL {i}")
            check(reply(a) == 1, f"FIFO PEXPIRE {i}")
            remaining = reply(a)
            check(isinstance(remaining, int) and 590000 <= remaining <= 600000,
                  f"FIFO PTTL {i}: {remaining!r}")
            check(reply(a) == 1, f"FIFO PERSIST {i}")
            check(reply(a) == -1, f"FIFO restored TTL {i}")
        check(command(b, b"PING") == (b"+", b"PONG"), "second client broken by pipeline")


def main():
    with server() as port:
        test_live(port)
    print("PASS: Phase 5E live RESP2 TTL/PTTL/PERSIST, two clients, binary and empty keys, "
          "192 pipelined replies")


if __name__ == "__main__":
    main()
