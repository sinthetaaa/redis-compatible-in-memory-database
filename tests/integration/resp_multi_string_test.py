#!/usr/bin/env python3
"""Phase 4D external-process RESP2 MSET/MGET acceptance suite."""
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
    raise SystemExit("usage: resp_multi_string_test.py SERVER_EXECUTABLE")
SERVER=sys.argv[1]
HOST="127.0.0.1"
LIMIT=8176

def check(ok, reason):
    if not ok: raise AssertionError(reason)

def bulk(value):
    return b"$"+str(len(value)).encode()+b"\r\n"+value+b"\r\n"

def request(*pieces):
    return b"*"+str(len(pieces)).encode()+b"\r\n"+b"".join(bulk(x) for x in pieces)

def array(*elements):
    return b"*"+str(len(elements)).encode()+b"\r\n"+b"".join(
        b"$-1\r\n" if x is None else bulk(x) for x in elements)

def read_exact(sock, n):
    result=bytearray()
    while len(result)<n:
        b=sock.recv(n-len(result))
        check(b, "EOF in reply")
        result.extend(b)
    return bytes(result)

def expect(sock, wanted, name):
    got=read_exact(sock,len(wanted))
    check(got==wanted, f"{name}: {got!r} != {wanted!r}")

def connect(port):
    s=socket.create_connection((HOST,port),timeout=5)
    s.settimeout(5)
    return s

@contextlib.contextmanager
def server():
    p=subprocess.Popen([SERVER,"--resp2-server","--port","0","--max-clients","16"],
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    try:
        ready,_,_=select.select([p.stdout],[],[],5)
        check(bool(ready),"server not ready")
        line=p.stdout.readline()
        match=re.fullmatch(r"RESP_READY 127[.]0[.]0[.]1:([0-9]+)\n",line)
        check(match is not None,f"readiness: {line!r}")
        yield int(match.group(1))
    finally:
        if p.poll() is None: p.send_signal(signal.SIGTERM)
        try: out,err=p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill();out,err=p.communicate(timeout=5)
        check(p.returncode==0,f"abnormal server exit: {err!r}")
        check("RESP_STOPPED" in out,"not gracefully stopped")

def test_order_and_atomic_errors(port):
    with connect(port) as a, connect(port) as b:
        a.sendall(request(b"MGET",b"absent",b"absent"))
        expect(a,array(None,None),"missing MGET")
        a.sendall(request(b"mSeT",b"a",b"1",b"b",b"2",b"a",b"3"))
        expect(a,b"+OK\r\n","last-write-wins MSET")
        b.sendall(request(b"mGeT",b"b",b"a",b"absent",b"a"))
        expect(b,array(b"2",b"3",None,b"3"),"ordered cross-client MGET")
        k=b"k\x00\xff\r\n"
        v=b"v\x00\xfe\r\n"*40
        a.sendall(request(b"MSET",k,v,b"",b""))
        expect(a,b"+OK\r\n","binary/empty MSET")
        b.sendall(request(b"MGET",k,b"",b"K"))
        expect(b,array(v,b"",None),"binary/empty/case MGET")
        a.sendall(request(b"MSET",b"a",b"should-not-change",b"bad"))
        expect(a,b"-ERR wrong number of arguments for 'mset' command\r\n","odd MSET")
        b.sendall(request(b"MGET",b"a",b"bad"))
        expect(b,array(b"3",None),"odd MSET was not atomic")
        a.sendall(request(b"MGET"))
        expect(a,b"-ERR wrong number of arguments for 'mget' command\r\n","zero-arity MGET")
        very_large=b"z"*LIMIT
        a.sendall(request(b"MSET",b"good",b"bad",b"too-large",very_large+b"!"))
        expect(a,b"-ERR string value exceeds the 8176-byte reply-safe limit\r\n",
               "late oversize MSET")
        b.sendall(request(b"MGET",b"good",b"too-large"))
        expect(b,array(None,None),"failed MSET partially committed")
        a.sendall(request(b"MSET",b"max",very_large))
        expect(a,b"+OK\r\n","single max MSET")
        b.sendall(request(b"MGET",b"max"))
        expect(b,array(very_large),"single max MGET")
        b.sendall(request(b"MGET",b"max",b"max"))
        expect(b,b"-ERR MGET response exceeds the 8192-byte limit\r\n",
               "oversize response should error, not close")
        b.sendall(request(b"PING")+request(b"GET",b"a"))
        expect(b,b"+PONG\r\n"+bulk(b"3"),"connection survives oversize MGET")

def test_pipeline_and_clients(port):
    with connect(port) as a:
        frame=request(b"MSET",b"fragment",b"yes")
        a.sendall(frame[:3]);a.sendall(frame[3:15]);a.sendall(frame[15:])
        expect(a,b"+OK\r\n","fragmented MSET")
        payload=bytearray();expected=bytearray()
        for i in range(40):
            val=str(i).encode()
            payload.extend(request(b"MSET",b"p",val,b"q",b"x"))
            expected.extend(b"+OK\r\n")
            payload.extend(request(b"MGET",b"q",b"p",b"missing"))
            expected.extend(array(b"x",val,None))
        a.sendall(payload)
        expect(a,bytes(expected),"80 mixed pipelined MSET/MGET must be FIFO")
        a.sendall(request(b"MGET",b"fragment"))
        expect(a,array(b"yes"),"pipeline corrupted earlier key")

    def worker(i):
        with connect(port) as c:
            key=("client:"+str(i)).encode()
            for j in range(22):
                value=str(j).encode()
                c.sendall(request(b"MSET",key,value,key+b":other",b"ok")+
                          request(b"MGET",key,key+b":other"))
                expect(c,b"+OK\r\n"+array(value,b"ok"),"client value mismatch")
        return True
    with ThreadPoolExecutor(max_workers=8) as pool:
        check(all(pool.map(worker,range(8))),"concurrent client failure")
    with connect(port) as c:
        for i in range(8):
            key=("client:"+str(i)).encode()
            c.sendall(request(b"MGET",key,key+b":other"))
            expect(c,array(b"21",b"ok"),"cross-client final state")

def test_redis_cli(port):
    cli=shutil.which("redis-cli")
    if not cli:
        print("SKIP: redis-cli not installed; raw RESP2 tests passed")
        return
    def run(*args):
        p=subprocess.run([cli,"-2","--raw","-h",HOST,"-p",str(port),*args],
                         capture_output=True,text=True,timeout=6)
        check(p.returncode==0,f"redis-cli error: {p.stderr!r}")
        return p.stdout.rstrip("\n")
    check(run("MSET","cli:a","apple","cli:b","banana")=="OK","redis-cli MSET")
    check(run("MGET","cli:b","cli:a")=="banana\napple","redis-cli MGET")
    check(run("GET","cli:b")=="banana","redis-cli GET after MSET")
    print("PASS: actual redis-cli MSET and MGET")

def main():
    with server() as port:
        test_order_and_atomic_errors(port)
        test_pipeline_and_clients(port)
        test_redis_cli(port)
    print("PASS: live MSET/MGET atomicity, binary, limits, 8 clients, pipelines, redis-cli")

if __name__ == "__main__": main()
