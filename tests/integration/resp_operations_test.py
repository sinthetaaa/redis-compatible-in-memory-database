"""Bounded operational TCP checks: 32 clients, 4096 commands, 5s socket deadlines.

No external service or network binding beyond loopback. CTest wall limit: 60s.
"""
from concurrent.futures import ThreadPoolExecutor
import contextlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from resp_aof_test import RunningServer, OK
from resp2_wire import RespReader, RespValue, encode_command
from resp_hardening_test import resource_sample

SERVER = sys.argv[1]
PONG = RespValue('simple', b'PONG')


class Server:
    def __init__(self, *options):
        self.events = ['startup', 'shutdown_started', 'shutdown_completed']
        self.proc = subprocess.Popen([SERVER, '--resp2-server', '--port', '0', *options],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert select.select([self.proc.stdout], [], [], 10)[0], 'readiness timeout'
            line = self.proc.stdout.readline()
            assert line.startswith(b'RESP_READY 127.0.0.1:'), (line, self.proc.poll())
            self.port = int(line.rsplit(b':', 1)[1])
        except BaseException:
            self.proc.kill()
            self.proc.communicate(timeout=5)
            raise

    def connect(self):
        sock = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        sock.settimeout(5)
        return sock

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            out, err = self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=5)
            raise AssertionError('shutdown timeout')
        assert self.proc.returncode == 0 and b'RESP_STOPPED\n' in out, (out, err)
        logs = [json.loads(line) for line in err.splitlines()]
        assert [x['event'] for x in logs] == self.events, logs
        assert b'secret-key' not in err and b'secret-value' not in err
        assert all(x['severity'] == ('warning' if x['event'] == 'shutdown_drain_timeout' else 'info') for x in logs)


@contextlib.contextmanager
def server(*options):
    running = Server(*options)
    try:
        yield running
    finally:
        running.stop()


def command(sock, reader, *args):
    sock.sendall(encode_command(args))
    return reader.read_reply()[0]


def info(sock, reader, section=b'stats'):
    value = command(sock, reader, b'INFO', section)
    assert value.kind == 'bulk' and value.value.endswith(b'\r\n'), value
    return dict(line.split(b':', 1) for line in value.value.split(b'\r\n')
                if line and not line.startswith(b'#'))


def operations():
    with server() as running, running.connect() as sock:
        reader = RespReader(sock)
        # Counter replies exclude the inspection command itself.
        sock.sendall(encode_command((b'INFO', b'stats')))
        first_reply, first_raw = reader.read_reply()
        first = dict(line.split(b':', 1) for line in first_reply.value.split(b'\r\n')
                     if line and not line.startswith(b'#'))
        assert int(first[b'total_commands_processed']) == 0, first
        sent = 0
        received = 0
        for args, expected in [((b'PING',), PONG), ((b'SET', b'secret-key', b'secret-value'), OK),
                               ((b'GET',), RespValue('error', b"ERR wrong number of arguments for 'get' command")),
                               ((b'INFO', b'unknown'), RespValue('bulk', b''))]:
            wire = encode_command(args)
            sock.sendall(wire)
            value, raw = reader.read_reply()
            assert value == expected, value
            sent += len(wire)
            received += len(raw)
        later = info(sock, reader)
        assert int(later[b'total_commands_processed']) == 5, later
        assert int(later[b'redisdb_commands_succeeded']) == 4, later
        assert int(later[b'redisdb_commands_rejected']) == 1, later
        assert int(later[b'total_net_input_bytes']) - int(first[b'total_net_input_bytes']) == sent + len(encode_command((b'INFO', b'stats')))
        assert int(later[b'total_net_output_bytes']) - int(first[b'total_net_output_bytes']) == received + len(first_raw)
        assert info(sock, reader, b'clients') == {b'connected_clients': b'1'}
        pairs = command(sock, reader, b'CONFIG', b'GET', b'*')
        assert pairs.kind == 'array' and len(pairs.value) == 18, pairs
        settings = dict((pairs.value[i].value, pairs.value[i+1].value) for i in range(0, 18, 2))
        assert settings == {b'port': str(running.port).encode(), b'bind': b'127.0.0.1',
                            b'maxclients': b'1024', b'maxmemory': b'0',
                            b'maxmemory-policy': b'noeviction', b'appendonly': b'no',
                            b'appendfsync': b'always', b'timeout': b'0', b'idle-timeout-ms': b'0'}
        assert command(sock, reader, b'CONFIG', b'GET', b'aof-file', b'snapshot-dir') == RespValue('array', ())
        assert command(sock, reader, b'CONFIG', b'SET', b'port', b'1') == RespValue(
            'error', b'ERR unsupported CONFIG subcommand; only GET is supported')
        assert command(sock, reader, b'CONFIG', b'GET') == RespValue(
            'error', b"ERR wrong number of arguments for 'config|get' command")
        assert command(sock, reader, b'INFO', b'a', b'b') == RespValue(
            'error', b"ERR wrong number of arguments for 'info' command")
        with running.connect() as bad:
            bad.sendall(b'!bad\r\n')
            assert bad.recv(1) == b''
        with running.connect() as invalid:
            invalid.sendall(b':1\r\n')
            assert RespReader(invalid).read_reply()[0] == RespValue('error', b'ERR expected RESP2 array of bulk strings')
        stats = info(sock, reader)
        assert int(stats[b'redisdb_parser_errors']) == 1
        assert int(stats[b'redisdb_connections_closed']) >= 1
        assert int(stats[b'total_connections_received']) == 3


def idle_and_admission():
    with server('--idle-timeout-ms', '250', '--max-clients', '2') as running:
        with running.connect() as active, running.connect() as idle:
            reader = RespReader(active)
            assert command(active, reader, b'PING') == PONG
            # This third socket is queued in the OS backlog, never admitted above 2.
            with running.connect() as queued:
                qr = RespReader(queued)
                queued.sendall(encode_command((b'PING',)))
                queued.settimeout(.05)
                try:
                    queued.recv(1)
                    raise AssertionError('connection admitted over capacity')
                except socket.timeout:
                    pass
                for _ in range(8):
                    assert command(active, reader, b'PING') == PONG
                    time.sleep(.04)
                assert idle.recv(1) == b'', 'idle connection was not reaped'
                queued.settimeout(5)
                assert qr.read_reply()[0] == PONG, 'admission did not recover'
                assert command(active, reader, b'CONFIG', b'GET', b'idle-timeout-ms', b'timeout') == RespValue(
                    'array', (RespValue('bulk', b'timeout'), RespValue('bulk', b'0'),
                              RespValue('bulk', b'idle-timeout-ms'), RespValue('bulk', b'250')))


def concurrent_soak():
    with server('--max-clients', '40', '--idle-timeout-ms', '2000') as running:
        before = resource_sample(running.proc.pid)
        def worker(index):
            with running.connect() as sock:
                reader = RespReader(sock)
                for turn in range(8):
                    key = f'client:{index}'.encode()
                    value = f'{turn}'.encode()
                    commands = [(b'SET', key, value), (b'GET', key)] * 8
                    sock.sendall(b''.join(encode_command(c) for c in commands))
                    assert [reader.read_reply()[0] for _ in commands] == [OK, RespValue('bulk', value)] * 8, (index, turn)
        with ThreadPoolExecutor(max_workers=32) as pool:
            list(pool.map(worker, range(32)))
        after = resource_sample(running.proc.pid)
        envelope = (512 if os.environ.get('REDISDB_TEST_ASAN') == '1' else 32) * 1024 * 1024
        assert after['rss_bytes'] - before['rss_bytes'] < envelope, (before, after, envelope)
        print(f'32 clients, 4096 commands; RSS samples {before} -> {after}; envelope {envelope}')


def slow_pipeline_shutdown():
    with server('--idle-timeout-ms', '100') as running, running.connect() as slow, running.connect() as healthy:
        sr, hr = RespReader(slow), RespReader(healthy)
        assert command(healthy, hr, b'SET', b'large', b'x' * 4096) == OK
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        slow.sendall(encode_command((b'GET', b'large')) * 1024)
        for _ in range(6):
            assert command(healthy, hr, b'PING') == PONG
            time.sleep(.03)
        # Active output/backpressured buffered requests survive the idle deadline.
        for _ in range(1024):
            assert sr.read_reply()[0] == RespValue('bulk', b'x' * 4096)
        # Verify successful shutdown draining with a normal receive window.
        # A deliberately slow reader is allowed to exceed the one-second drain;
        # shutdown_deadline separately verifies that bounded truncation path.
        with running.connect() as draining:
            dr = RespReader(draining)
            draining.sendall(encode_command((b'GET', b'large')) * 128)
            assert dr.read_reply()[0] == RespValue('bulk', b'x' * 4096)
            running.proc.send_signal(signal.SIGTERM)
            for _ in range(127):
                assert dr.read_reply()[0] == RespValue('bulk', b'x' * 4096)


def shutdown_deadline():
    with server() as running, running.connect() as slow:
        reader = RespReader(slow)
        assert command(slow, reader, b'SET', b'large', b'x' * 4096) == OK
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        # 16 MiB of potential replies, bounded to 4096 requests and 64 KiB of
        # server-side application output; never allocate the full reply stream.
        slow.sendall(encode_command((b'GET', b'large')) * 4096)
        assert reader.read_reply()[0] == RespValue('bulk', b'x' * 4096)
        running.events.insert(2, 'shutdown_drain_timeout')
        start = time.monotonic()
        running.proc.terminate()
        running.proc.wait(timeout=3)
        assert time.monotonic() - start < 3, 'slow reader prevented bounded shutdown'


def persistence(root):
    (root / 'snapshots').mkdir()
    path = root / 'db.aof'
    running = RunningServer(path, '--snapshot-dir', str(root / 'snapshots'))
    try:
        assert running.command(b'SET', b'key', b'value') == OK
        before = path.read_bytes()
        sequence = running.command(b'INFO', b'persistence')
        for _ in range(3):
            assert running.command(b'INFO', b'persistence') == sequence
            assert running.command(b'CONFIG', b'GET', b'appendonly') == RespValue(
                'array', (RespValue('bulk', b'appendonly'), RespValue('bulk', b'yes')))
        assert path.read_bytes() == before, 'inspection wrote AOF'
    finally:
        running.stop()
    recovered = RunningServer(path)
    try:
        assert recovered.command(b'GET', b'key') == RespValue('bulk', b'value')
        stats = recovered.command(b'INFO', b'stats').value
        assert b'total_commands_processed:1\r\n' in stats, stats
    finally:
        recovered.stop()


def snapshot_logging(root):
    directory = root / 'log-snapshots'
    directory.mkdir()
    with server('--appendonly', 'yes', '--aof-file', str(root / 'log.aof'),
                '--snapshot-dir', str(directory)) as running, running.connect() as sock:
        reader = RespReader(sock)
        assert command(sock, reader, b'SET', b'secret-key', b'secret-value') == OK
        running.proc.send_signal(signal.SIGUSR1)
        assert select.select([running.proc.stdout], [], [], 5)[0], 'snapshot timeout'
        assert running.proc.stdout.readline().startswith(b'SNAPSHOT_PUBLISHED ')
        running.events.insert(1, 'snapshot_published')


def cli_validation():
    for value in ('-1', '+1', 'x', '', '31536000001', '18446744073709551616'):
        result = subprocess.run([SERVER, '--resp2-server', '--idle-timeout-ms', value],
                                capture_output=True, timeout=3)
        assert result.returncode == 2 and json.loads(result.stderr)['event'] == 'invalid_arguments'
    result = subprocess.run([SERVER, '--resp2-server', '--idle-timeout-ms', '1', '--idle-timeout-ms', '2'],
                            capture_output=True, timeout=3)
    assert result.returncode == 2
    # A closed diagnostic pipe must return the documented code, not SIGPIPE.
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        broken = subprocess.Popen([SERVER, '--bad-option'], stderr=write_fd, stdout=subprocess.PIPE)
    finally:
        os.close(write_fd)
    broken.communicate(timeout=3)
    assert broken.returncode == 74, broken.returncode
    # A broken diagnostic sink fails predictably without recursive reporting.
    if Path('/dev/full').exists():
        with open('/dev/full', 'wb') as sink:
            result = subprocess.run([SERVER, '--bad-option'], stderr=sink, stdout=subprocess.PIPE, timeout=3)
            assert result.returncode == 74


if __name__ == '__main__':
    cli_validation()
    operations()
    idle_and_admission()
    concurrent_soak()
    slow_pipeline_shutdown()
    shutdown_deadline()
    with tempfile.TemporaryDirectory(prefix='redisdb-operations-') as temporary:
        persistence(Path(temporary))
        snapshot_logging(Path(temporary))
    print('PASS operations, counters, JSON logs, idle/admission, bounded soak, slow pipeline/SIGTERM and AOF')
