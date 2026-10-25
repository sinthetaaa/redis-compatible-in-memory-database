"""Owned-server coverage of memory CLI options, OOM replies and pipeline recovery."""
import contextlib
import select
import socket
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'differential'))
from resp2_wire import RespReader, RespValue, encode_command

SERVER = sys.argv[1]
OOM = RespValue('error', b"OOM command not allowed when used memory > 'maxmemory'.")
OK = RespValue('simple', b'OK')


@contextlib.contextmanager
def server(*options):
    proc = subprocess.Popen([SERVER, '--resp2-server', '--port', '0', *options],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([proc.stdout], [], [], 8)[0], 'readiness timeout'
        ready = proc.stdout.readline()
        assert ready.startswith(b'RESP_READY 127.0.0.1:'), ready
        port = int(ready.split(b':')[-1])
        with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
            yield sock, RespReader(sock)
    finally:
        proc.terminate()
        try:
            _, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate(timeout=5)
            raise AssertionError('server did not terminate')
        assert proc.returncode == 0, stderr


def command(peer, *args):
    sock, reader = peer
    sock.sendall(encode_command(args))
    return reader.read_reply()[0]


def test_noeviction():
    with server('--maxmemory', '66') as peer:
        assert command(peer, b'SET', b'a', b'x') == OK
        for args in ((b'SET', b'b', b'y'), (b'APPEND', b'a', b'y'),
                     (b'MSET', b'a', b'y', b'b', b'z'),
                     (b'LPUSH', b'l', b'x'), (b'HSET', b'h', b'f', b'v'),
                     (b'SADD', b's', b'm'), (b'EXPIRE', b'a', b'10'),
                     (b'PEXPIRE', b'a', b'10000'),
                     (b'SET', b'a', b'y', b'PX', b'10000')):
            assert command(peer, *args) == OOM, args
            assert command(peer, b'GET', b'a') == RespValue('bulk', b'x'), args
        assert command(peer, b'PTTL', b'a') == RespValue('integer', -1)
        sock, reader = peer
        sock.sendall(b''.join(encode_command(args) for args in
                             ((b'SET', b'b', b'y'), (b'GET', b'a'), (b'PING',))))
        assert [reader.read_reply()[0] for _ in range(3)] == [
            OOM, RespValue('bulk', b'x'), RespValue('simple', b'PONG')]
        assert command(peer, b'DEL', b'a') == RespValue('integer', 1)
        assert command(peer, b'SET', b'b', b'9') == OK
        assert command(peer, b'INCR', b'b') == OOM
        assert command(peer, b'GET', b'b') == RespValue('bulk', b'9')
        assert command(peer, b'EXPIRE', b'b', b'0') == RespValue('integer', 1)


def test_lru_and_binary():
    with server('--maxmemory', '132', '--maxmemory-policy', 'allkeys-lru') as peer:
        assert command(peer, b'SET', b'a', b'x') == OK
        assert command(peer, b'SET', b'b', b'y') == OK
        assert command(peer, b'GET', b'a') == RespValue('bulk', b'x')
        assert command(peer, b'SET', b'c', b'z') == OK
        assert command(peer, b'EXISTS', b'b') == RespValue('integer', 0)
        assert command(peer, b'SET', b'huge', b'x' * 200) == OOM
        assert command(peer, b'MGET', b'a', b'c') == RespValue(
            'array', (RespValue('bulk', b'x'), RespValue('bulk', b'z')))
        assert command(peer, b'SET', b'\x00\xff\r\n', b'\x00\xff') == OK
        assert command(peer, b'GET', b'\x00\xff\r\n') == RespValue('bulk', b'\x00\xff')


def test_configuration():
    for options in (('--maxmemory', '-1'), ('--maxmemory', 'invalid'),
                    ('--maxmemory', '18446744073709551616'),
                    ('--maxmemory-policy', 'random'),
                    ('--maxmemory', '1', '--maxmemory', '2')):
        result = subprocess.run([SERVER, '--resp2-server', *options],
                                capture_output=True, timeout=3)
        assert result.returncode == 2, options
    result = subprocess.run([SERVER, '--smoke-echo', '--maxmemory', '100'],
                            capture_output=True, timeout=3)
    assert result.returncode == 2
    with server('--maxmemory', '0') as peer:
        assert command(peer, b'SET', b'large', b'x' * 1000) == OK


if __name__ == '__main__':
    test_configuration()
    test_noeviction()
    test_lru_and_binary()
    print('PASS memory configuration, RESP OOM, atomic writes, pipeline recovery and LRU')
