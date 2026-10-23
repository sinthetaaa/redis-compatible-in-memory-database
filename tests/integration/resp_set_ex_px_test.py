#!/usr/bin/env python3
"""Phase 5F live SET EX/PX: two-client visibility, rejection and FIFO ordering."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_set_ex_px_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]


def check(value, message):
    if not value:
        raise AssertionError(message)


def request(*args):
    return b"*" + str(len(args)).encode() + b"\r\n" + b"".join(
        b"$" + str(len(a)).encode() + b"\r\n" + a + b"\r\n" for a in args
    )


def exact(sock, count):
    out = bytearray()
    while len(out) < count:
        block = sock.recv(count - len(out))
        check(block, "unexpected TCP EOF")
        out.extend(block)
    return bytes(out)


def line(sock):
    out = bytearray()
    while True:
        out.extend(exact(sock, 1))
        check(len(out) <= 160, "oversized RESP2 line")
        if out.endswith(b"\r\n"):
            return bytes(out[:-2])


def reply(sock):
    typ = exact(sock, 1)
    if typ == b":":
        return int(line(sock))
    if typ in (b"+", b"-"):
        return typ, line(sock)
    if typ == b"$":
        size = int(line(sock))
        if size == -1:
            return None
        check(0 <= size <= 8192, "unexpected bulk payload")
        result = exact(sock, size)
        check(exact(sock, 2) == b"\r\n", "invalid RESP2 terminator")
        return result
    raise AssertionError(f"unexpected RESP2 reply {typ!r}")


def command(sock, *args):
    sock.sendall(request(*args))
    return reply(sock)


def connect(port):
    conn = socket.create_connection(("127.0.0.1", port), timeout=8)
    conn.settimeout(8)
    return conn


@contextlib.contextmanager
def server():
    proc = subprocess.Popen(
        [SERVER, "--resp2-server", "--port", "0", "--max-clients", "16"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 8)
        check(ready, "server readiness timeout")
        message = proc.stdout.readline()
        match = re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n", message)
        check(match is not None, f"unexpected server readiness: {message!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            output, errors = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, errors = proc.communicate(timeout=8)
        check(proc.returncode == 0, f"abnormal server shutdown: {errors!r}")
        check("RESP_STOPPED" in output, "shutdown marker missing")


def test_live(port):
    key = b"b\x00\xff\r\n"
    ok = (b"+", b"OK")
    with connect(port) as a, connect(port) as b:
        check(command(a, b"SET", key, b"old", b"eX", b"600") == ok, "SET EX")
        check(command(b, b"GET", key) == b"old", "cross-client timed value")
        sec = command(b, b"TTL", key)
        check(isinstance(sec, int) and 590 <= sec <= 600, f"EX TTL out of range {sec!r}")
        check(command(b, b"SET", key, b"new", b"pX", b"600000") == ok, "SET PX")
        millis = command(a, b"PTTL", key)
        check(isinstance(millis, int) and 590000 <= millis <= 600000,
              f"PX PTTL out of range {millis!r}")
        check(command(a, b"GET", key) == b"new", "PX changed value incorrectly")
        check(command(a, b"INCRBY", b"other", b"3") == 3, "other key corrupted")
        check(command(a, b"SET", key, b"persistent") == ok, "plain SET after PX")
        check(command(b, b"TTL", key) == -1, "plain SET retained TTL")
        check(command(b, b"GET", key) == b"persistent", "plain SET value mismatch")
        check(command(a, b"SET", key, b"old", b"EX", b"600") == ok, "reject fixture")
        before = command(b, b"PTTL", key)
        error = (b"-", b"ERR invalid expire time in 'set' command")
        integer = (b"-", b"ERR value is not an integer or out of range")
        syntax = (b"-", b"ERR syntax error")
        arity = (b"-", b"ERR wrong number of arguments for 'set' command")
        a.sendall(request(b"SET", key, b"bad", b"EX", b"0") +
                  request(b"SET", key, b"bad", b"PX", b"-1") +
                  request(b"SET", key, b"bad", b"EX", b"01") +
                  request(b"SET", key, b"bad", b"NX", b"10") +
                  request(b"SET", key, b"bad", b"EX") +
                  request(b"SET", key, b"bad", b"EX", b"9223372036854775807") +
                  request(b"PING"))
        for expected in (error, error, integer, syntax, arity, error, (b"+", b"PONG")):
            check(reply(a) == expected, f"SET error or reply order mismatch {expected!r}")
        check(command(b, b"GET", key) == b"old", "rejected SET changed value")
        after = command(b, b"PTTL", key)
        check(0 <= after <= before <= 600000, "rejected SET changed deadline")

        # Six commands per group, 192 replies; all replies must preserve FIFO.
        frames = bytearray()
        for i in range(32):
            name = f"set-with-expiry:{i:02d}".encode()
            for args in ((b"SET", name, b"v", b"EX", b"600"),
                         (b"TTL", name), (b"GET", name),
                         (b"SET", name, b"w", b"PX", b"600000"),
                         (b"PTTL", name), (b"PERSIST", name)):
                frames.extend(request(*args))
        a.sendall(frames)
        for i in range(32):
            check(reply(a) == ok, f"pipeline SET EX {i}")
            lifetime = reply(a)
            check(isinstance(lifetime, int) and 590 <= lifetime <= 600,
                  f"pipeline TTL {i}: {lifetime!r}")
            check(reply(a) == b"v", f"pipeline GET {i}")
            check(reply(a) == ok, f"pipeline SET PX {i}")
            lifetime_ms = reply(a)
            check(isinstance(lifetime_ms, int) and 590000 <= lifetime_ms <= 600000,
                  f"pipeline PTTL {i}: {lifetime_ms!r}")
            check(reply(a) == 1, f"pipeline PERSIST {i}")
        check(command(b, b"GET", key) == b"old", "pipeline overwrote unrelated key")
        check(command(b, b"PING") == (b"+", b"PONG"), "second client broken")


def main():
    with server() as port:
        test_live(port)
    print("PASS: Phase 5F two-client SET EX/PX, binary values, 192 FIFO responses, "
          "error preservation and TTL visibility")


if __name__ == "__main__":
    main()
