#!/usr/bin/env python3
"""Phase 5D: live RESP2 expire/pexpire without timing-sensitive sleeps."""
import contextlib
import re
import select
import signal
import socket
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: resp_expire_pexpire_test.py SERVER_EXECUTABLE")
SERVER = sys.argv[1]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def req(*parts):
    return b"*"+str(len(parts)).encode()+b"\r\n"+b"".join(
        b"$"+str(len(p)).encode()+b"\r\n"+p+b"\r\n" for p in parts)


def recv(sock, n):
    out=bytearray()
    while len(out)<n:
        b=sock.recv(n-len(out))
        check(b, "unexpected EOF")
        out.extend(b)
    return bytes(out)


def expect(sock, expected, label):
    actual=recv(sock,len(expected))
    check(actual==expected, f"{label}: {actual!r} != {expected!r}")


def bulk(p):
    return b"$"+str(len(p)).encode()+b"\r\n"+p+b"\r\n"


def connect(port):
    s=socket.create_connection(("127.0.0.1",port),timeout=8)
    s.settimeout(8)
    return s


@contextlib.contextmanager
def server():
    proc=subprocess.Popen([SERVER,"--resp2-server","--port","0","--max-clients","16"],
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    try:
        ready,_,_=select.select([proc.stdout],[],[],8)
        check(ready,"server readiness timeout")
        line=proc.stdout.readline()
        match=re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n",line)
        check(match is not None,f"unexpected readiness line: {line!r}")
        yield int(match.group(1))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            out,err=proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            out,err=proc.communicate(timeout=8)
        check(proc.returncode==0,f"server terminated unexpectedly: {err!r}")
        check("RESP_STOPPED" in out,"graceful shutdown marker missing")


def run(port):
    with connect(port) as a, connect(port) as b:
        k=b"binary\x00\xff"
        a.sendall(req(b"EXPIRE",k,b"5")+req(b"PEXPIRE",k,b"5")+
                  req(b"SET",k,b"value")+req(b"EXPIRE",k,b"600")+
                  req(b"PEXPIRE",k,b"500000"))
        expect(a,b":0\r\n:0\r\n+OK\r\n:1\r\n:1\r\n","initial deadline pipeline")
        b.sendall(req(b"GET",k)+req(b"TYPE",k))
        expect(b,bulk(b"value")+b"+string\r\n","other client sees live key")
        b.sendall(req(b"EXPIRE",k,b"0")+req(b"GET",k)+req(b"EXISTS",k))
        expect(b,b":1\r\n$-1\r\n:0\r\n","zero-seconds immediate delete")
        a.sendall(req(b"SET",k,b"2")+req(b"PEXPIRE",k,b"-2")+
                  req(b"MGET",k,b"missing"))
        expect(a,b"+OK\r\n:1\r\n*2\r\n$-1\r\n$-1\r\n","negative ms delete shared")
        a.sendall(req(b"SET",k,b"stable"))
        expect(a,b"+OK\r\n","reset for rejected expiry")
        bad=b"-ERR value is not an integer or out of range\r\n"
        overflow_s=b"-ERR invalid expire time in 'expire' command\r\n"
        overflow_ms=b"-ERR invalid expire time in 'pexpire' command\r\n"
        a.sendall(req(b"EXPIRE",k,b"01")+req(b"PEXPIRE",k,b"+1")+
                  req(b"EXPIRE",k,b"9223372036854775807")+
                  req(b"PEXPIRE",k,b"9223372036854775807")+
                  req(b"GET",k)+req(b"PING"))
        expect(a,bad+bad+overflow_s+overflow_ms+bulk(b"stable")+b"+PONG\r\n",
               "invalid expiry preserves stored value and connection")
        a.sendall(req(b"EXPIRE",k)+req(b"PEXPIRE",k,b"1",b"NX")+
                  req(b"PEXPIRE",b"absent",b"-5"))
        expect(a,b"-ERR wrong number of arguments for 'expire' command\r\n"+
               b"-ERR wrong number of arguments for 'pexpire' command\r\n:0\r\n",
               "arity and missing result")
        # FIFO replies from many sequential basic expiration operations.
        frames=bytearray()
        results=bytearray()
        for i in range(32):
            key=f"expire:{i:02d}".encode()
            frames.extend(req(b"SET",key,b"v"))
            results.extend(b"+OK\r\n")
            frames.extend(req(b"PEXPIRE",key,b"200000"))
            results.extend(b":1\r\n")
            frames.extend(req(b"EXPIRE",key,b"-1"))
            results.extend(b":1\r\n")
            frames.extend(req(b"TYPE",key))
            results.extend(b"+none\r\n")
        a.sendall(frames)
        expect(a,bytes(results),"128 FIFO expiry replies")
        b.sendall(req(b"GET",k)+req(b"PING"))
        expect(b,bulk(b"stable")+b"+PONG\r\n","independent key and client")


def main():
    with server() as port:
        run(port)
    print("PASS: Phase 5D live RESP2 two-client EXPIRE/PEXPIRE, binary, errors, 128 FIFO replies")

if __name__=="__main__":
    main()
