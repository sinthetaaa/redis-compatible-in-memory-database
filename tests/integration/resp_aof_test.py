"""Owned-process AOF restart, abrupt termination, corruption and disk failure tests."""
import contextlib
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'differential'))
from resp2_wire import RespReader, RespValue, encode_command

SERVER = sys.argv[1]
OK = RespValue('simple', b'OK')


def launch_args(path, *options):
    return [SERVER, '--resp2-server', '--port', '0', '--appendonly', 'yes',
            '--aof-file', str(path), *options]


class RunningServer:
    def __init__(self, path, *options, preexec_fn=None):
        self.proc = subprocess.Popen(launch_args(path, *options), stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, preexec_fn=preexec_fn)
        self.sock = None
        try:
            assert select.select([self.proc.stdout], [], [], 10)[0], 'readiness timeout'
            line = self.proc.stdout.readline()
            assert line.startswith(b'RESP_READY 127.0.0.1:'), (line, self.proc.stderr.read())
            port = int(line.split(b':')[-1])
            self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
            self.reader = RespReader(self.sock)
        except BaseException:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.communicate(timeout=5)
            raise

    def command(self, *args):
        self.sock.sendall(encode_command(args))
        return self.reader.read_reply()[0]

    def stop(self, abrupt=False):
        if self.sock is not None:
            self.sock.close()
            self.sock = None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if abrupt else signal.SIGTERM)
        try:
            stdout, stderr = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=5)
            raise AssertionError('server did not stop')
        assert self.proc.returncode == (-signal.SIGKILL if abrupt else 0), stderr
        if not abrupt:
            assert b'RESP_STOPPED' in stdout


@contextlib.contextmanager
def server(path, *options):
    running = RunningServer(path, *options)
    try:
        yield running
    finally:
        running.stop()


def expect_startup_failure(path, *options):
    before = path.read_bytes()
    marker = Path(str(path) + '.commit').read_bytes()
    result = subprocess.run(launch_args(path, *options), capture_output=True, timeout=10)
    assert result.returncode == 1 and b'RESP_READY' not in result.stdout, result
    assert path.read_bytes() == before and Path(str(path) + '.commit').read_bytes() == marker
    return result.stderr


def test_types_and_restarts(root):
    path = root / 'types.aof'
    binary = b'\x00\xff\r\n'
    running = RunningServer(path)
    try:
        assert running.command(b'SET', b'key' + binary, binary) == OK
        assert running.command(b'RPUSH', b'l', b'one', binary, b'') == RespValue('integer', 3)
        assert running.command(b'LPOP', b'l') == RespValue('bulk', b'one')
        assert running.command(b'HSET', b'h', b'f', binary, b'delete', b'v') == RespValue('integer', 2)
        assert running.command(b'HDEL', b'h', b'delete') == RespValue('integer', 1)
        assert running.command(b'SADD', b's', binary, b'gone') == RespValue('integer', 2)
        assert running.command(b'SREM', b's', b'gone') == RespValue('integer', 1)
        assert running.command(b'SET', b'n', b'9') == OK
        assert running.command(b'INCR', b'n') == RespValue('integer', 10)
        assert running.command(b'DECRBY', b'n', b'3') == RespValue('integer', 7)
        assert running.command(b'MSET', b'x', b'first', b'y', b'second', b'x', b'last') == OK
        assert running.command(b'SET', b'expired', b'v', b'PX', b'150') == OK
        assert running.command(b'SET', b'future', b'v', b'PX', b'60000') == OK
        assert running.command(b'SET', b'persist', b'v', b'EX', b'60') == OK
        assert running.command(b'PERSIST', b'persist') == RespValue('integer', 1)
        assert running.command(b'SET', b'del', b'v') == OK
        assert running.command(b'DEL', b'del', b'y') == RespValue('integer', 2)
        assert running.command(b'SADD', b'last', b'm') == RespValue('integer', 1)
        assert running.command(b'SREM', b'last', b'm') == RespValue('integer', 1)
        assert running.command(b'LPUSH', b'overwrite', b'l') == RespValue('integer', 1)
        assert running.command(b'SET', b'overwrite', b'string') == OK
    finally:
        running.stop(abrupt=True) # all preceding acknowledgments used default always
    time.sleep(0.3) # expiry during downtime, without any maintenance process
    for cycle in range(3):
        with server(path) as recovered:
            assert recovered.command(b'GET', b'key' + binary) == RespValue('bulk', binary)
            assert recovered.command(b'LRANGE', b'l', b'0', b'-1') == RespValue(
                'array', (RespValue('bulk', binary), RespValue('bulk', b'')))
            assert recovered.command(b'HGET', b'h', b'f') == RespValue('bulk', binary)
            assert recovered.command(b'HLEN', b'h') == RespValue('integer', 1)
            assert recovered.command(b'SMEMBERS', b's') == RespValue('array', (RespValue('bulk', binary),))
            assert recovered.command(b'GET', b'n') == RespValue('bulk', b'7')
            assert recovered.command(b'GET', b'x') == RespValue('bulk', b'last' + b'!' * cycle)
            assert recovered.command(b'EXISTS', b'y', b'del', b'last', b'expired') == RespValue('integer', 0)
            assert recovered.command(b'PTTL', b'expired') == RespValue('integer', -2)
            ttl = recovered.command(b'PTTL', b'future')
            assert ttl.kind == 'integer' and 0 < ttl.value < 60000, ttl
            assert recovered.command(b'PTTL', b'persist') == RespValue('integer', -1)
            assert recovered.command(b'GET', b'overwrite') == RespValue('bulk', b'string')
            assert recovered.command(b'APPEND', b'x', b'!').kind == 'integer'


def test_memory_rejections_and_ownership(root):
    path = root / 'memory.aof'
    options = ('--maxmemory', '132', '--maxmemory-policy', 'allkeys-lru')
    with server(path, *options) as running:
        assert running.command(b'MSET', b'a', b'x', b'b', b'y') == OK
        for args in ((b'MSET', b'a', b'xx', b'b', b'yy'),
                     (b'SET', b'huge', b'x' * 200), (b'LPUSH', b'a', b'x'),
                     (b'SET', b'a', b'x', b'EX', b'0'), (b'INCRBY', b'a', b'bad'),
                     (b'MSET', b'a'), (b'UNSUPPORTED',)):
            before = path.read_bytes()
            assert running.command(*args).kind == 'error', args
            assert path.read_bytes() == before, ('rejection journaled', args)
        second = subprocess.run(launch_args(path), capture_output=True, timeout=5)
        assert second.returncode == 1 and b'exclusive writer lock' in second.stderr
        assert running.command(b'GET', b'a') == RespValue('bulk', b'x')
        assert running.command(b'SET', b'c', b'z') == OK # evicts b
    with server(path, *options) as recovered:
        assert recovered.command(b'GET', b'b') == RespValue('null_bulk', None)
        assert recovered.command(b'MGET', b'a', b'c') == RespValue(
            'array', (RespValue('bulk', b'x'), RespValue('bulk', b'z')))
    assert b'maxmemory' in expect_startup_failure(path, '--maxmemory', '1')


def test_policies_and_corruption(root):
    for policy in ('always', 'everysec', 'no'):
        path = root / (policy + '.aof')
        with server(path, '--appendfsync', policy) as running:
            assert running.command(b'SET', b'k', policy.encode()) == OK
        with server(path, '--appendfsync', policy) as recovered:
            assert recovered.command(b'GET', b'k') == RespValue('bulk', policy.encode())
        before = path.read_bytes()
        path.write_bytes(before[:-1])
        expect_startup_failure(path)
        path.write_bytes(before)
        damaged = bytearray(before)
        damaged[40] ^= 1
        path.write_bytes(damaged)
        assert b'AOF' in expect_startup_failure(path)
    for args in (('--appendfsync', 'bad'), ('--appendonly', 'maybe'), ('--aof-file', 'unused')):
        result = subprocess.run([SERVER, '--resp2-server', *args], capture_output=True, timeout=3)
        assert result.returncode == 2, args


def test_real_disk_failure(root):
    path = root / 'disk-full.aof'

    def limit_output_file():
        resource.setrlimit(resource.RLIMIT_FSIZE, (128, 128))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN) # exercise EFBIG handling, not signal death

    running = RunningServer(path, preexec_fn=limit_output_file)
    try:
        running.sock.sendall(encode_command((b'SET', b'a', b'x' * 512)) + encode_command((b'PING',)))
        try:
            data = running.sock.recv(100)
        except (ConnectionResetError, BrokenPipeError):
            data = b''
        assert data == b'', ('persistence failure acknowledged or kept serving', data)
        _, stderr = running.proc.communicate(timeout=10)
        assert running.proc.returncode == 1 and b'AOF write' in stderr, stderr
    finally:
        running.sock.close()
        if running.proc.poll() is None:
            running.proc.kill()
            running.proc.communicate(timeout=5)
    expect_startup_failure(path)


def test_nonregular_paths_fail_without_blocking(root):
    path = root / 'fifo-marker.aof'
    with server(path) as running:
        assert running.command(b'SET', b'k', b'v') == OK
    before = path.read_bytes()
    marker = Path(str(path) + '.commit')
    marker.unlink() # replace only this test's owned fixture
    os.mkfifo(marker)
    result = subprocess.run(launch_args(path), capture_output=True, timeout=5)
    assert result.returncode == 1 and b'commit watermark' in result.stderr, result
    assert path.read_bytes() == before
    fifo = root / 'fifo-journal.aof'
    os.mkfifo(fifo)
    result = subprocess.run(launch_args(fifo), capture_output=True, timeout=5)
    assert result.returncode == 1 and b'regular file' in result.stderr, result


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='redisdb-aof-tcp-') as temporary:
        root = Path(temporary)
        test_types_and_restarts(root)
        test_memory_rejections_and_ownership(root)
        test_policies_and_corruption(root)
        test_real_disk_failure(root)
        test_nonregular_paths_fail_without_blocking(root)
    print('PASS AOF TCP restart, SIGKILL, binary/types/TTL, eviction, rejection, corruption and disk failure')
