"""Bounded persistent client churn, slow readers, pending shutdown and V1 recovery."""
from concurrent.futures import ThreadPoolExecutor
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from resp_aof_test import RunningServer, OK
from resp2_wire import RespReader, RespValue, encode_command


def benchmark_reply_checks():
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'benchmarks'))
    from tcp_baseline import validate_get
    validate_get(RespValue('bulk', b'x' * 64), b'x' * 64)
    for reply in (RespValue('bulk', b'y' * 64), RespValue('bulk', b'x'),
                  RespValue('null_bulk', None)):
        try:
            validate_get(reply, b'x' * 64)
        except RuntimeError:
            continue
        raise AssertionError('benchmark accepted an incorrect GET reply')


def connect(port):
    sock = socket.create_connection(('127.0.0.1', port), timeout=5)
    sock.settimeout(5)
    return sock


def resource_sample(pid):
    # Linux descriptor count/current RSS; macOS reports RSS via ps, no FD claim.
    fds = Path(f'/proc/{pid}/fd')
    result = subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)],
                            text=True, capture_output=True, timeout=5, check=True)
    return {'rss_bytes': int(result.stdout.strip()) * 1024,
            'fds': len(list(fds.iterdir())) if fds.is_dir() else None}


def workload(running, rounds, clients=None):
    port = running.sock.getpeername()[1]
    def worker(client):
        key = f'worker:{client}'.encode()
        # Each reconnect exchanges a bounded pipeline. No shared-key reply race.
        for turn in range(rounds):
            with (contextlib.nullcontext(clients[client]) if clients is not None
                  else connect(port)) as sock:
                reader = RespReader(sock)
                commands = []
                expected = []
                for index in range(4):
                    value = f'{turn}:{index}'.encode()
                    commands += [(b'SET', key, value), (b'GET', key),
                                 (b'APPEND', key, b'!'), (b'GET', key)]
                    expected += [OK, RespValue('bulk', value),
                                 RespValue('integer', len(value)+1), RespValue('bulk', value+b'!')]
                sock.sendall(b''.join(encode_command(c) for c in commands))
                assert [reader.read_reply()[0] for _ in commands] == expected
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(worker, range(4)))
    return 4 * rounds * 16


def sustained_and_shutdown(root):
    for policy in ('always', 'everysec', 'no'):
        path = root / (policy + '.aof')
        running = RunningServer(path, '--appendfsync', policy, '--max-clients', '32')
        port = running.sock.getpeername()[1]
        try:
            assert running.command(b'SET', b'large', b'x' * 4096) == OK
            for command in ((b'MSET', b'incomplete'), (b'SET', b'large', b'new', b'EX', b'0'),
                            (b'INCRBY', b'large', b'bad'), (b'LPUSH', b'large', b'wrongtype'),
                            (b'APPEND', b'large', b'x' * 4096), (b'UNSUPPORTED',)):
                before_file = path.read_bytes()
                assert running.command(*command).kind == 'error', command[0]
                assert path.read_bytes() == before_file, 'failed command journaled effects'
                assert running.command(b'GET', b'large') == RespValue('bulk', b'x' * 4096)
            with connect(port) as malformed:
                malformed.sendall(b'!invalid\r\n')
                assert malformed.recv(64) == b'', 'malformed client kept alive'
            assert running.command(b'PING') == RespValue('simple', b'PONG')
            workload(running, 8) # warm caches and allocator arenas before sampling
            before = resource_sample(running.proc.pid)
            samples = [before]
            total = 0
            # A non-reading client creates backpressure while healthy clients mutate.
            with contextlib.ExitStack() as stack:
                slow = stack.enter_context(connect(port))
                clients = [stack.enter_context(connect(port)) for _ in range(4)]
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                slow.sendall(encode_command((b'GET', b'large')) * 128)
                deadline = time.monotonic() + 5
                rounds = 0
                while rounds < 4 or time.monotonic() < deadline:
                    # Bound churn to 512 reconnects per policy. Thereafter reuse
                    # four clients: unconstrained churn exhausts host TIME_WAIT
                    # ports and measures client-host limits rather than leaks.
                    total += workload(running, 32, None if rounds < 4 else clients)
                    samples.append(resource_sample(running.proc.pid))
                    rounds += 1
                assert running.command(b'PING') == RespValue('simple', b'PONG')
            # ASan retains freed allocations in quarantine and adds metadata.
            # CMake marks actual address-instrumented builds; keep the native
            # 32 MiB growth check and a separate 512 MiB sanitizer envelope.
            # ASan/LSan still check real memory errors/leaks at normal shutdown.
            # Neither short RSS check proves absence of long-term growth.
            rss_limit = (512 if os.environ.get('REDISDB_TEST_ASAN') == '1' else 32) * 1024 * 1024
            assert max(s['rss_bytes'] for s in samples) - before['rss_bytes'] < rss_limit, samples
            if before['fds'] is not None:
                assert max(s['fds'] for s in samples) <= before['fds'] + 9, samples
            # Pipeline weaker-policy mutations then signal immediately after replies:
            # finish() must synchronize pending AOF work before RESP_STOPPED.
            running.sock.sendall(b''.join(encode_command((b'SET', f'pending:{i}'.encode(), b'last'))
                                         for i in range(16)))
            assert [running.reader.read_reply()[0] for _ in range(16)] == [OK] * 16
            running.stop()
            print(f'{policy}: {total} mixed operations, {min(rounds,4)*128} reconnects; '
                  f'RSS start/end/max={before["rss_bytes"]}/{samples[-1]["rss_bytes"]}/'
                  f'{max(s["rss_bytes"] for s in samples)}; '
                  f'RSS growth limit={rss_limit}; '
                  f'FD start/end={before["fds"]}/{samples[-1]["fds"]}')
        finally:
            if running.proc.poll() is None:
                running.stop()
        for cycle in range(3):
            recovered = RunningServer(path, '--appendfsync', policy)
            try:
                assert recovered.command(b'GET', b'pending:15') == RespValue('bulk', b'last')
                assert recovered.command(b'GET', b'worker:0') == RespValue('bulk', b'31:3!')
                assert recovered.command(b'INCR', b'cycle') == RespValue('integer', cycle+1)
                # An acknowledged always-policy write survives process death.
                recovered.stop(abrupt=(policy == 'always'))
            finally:
                if recovered.proc.poll() is None:
                    recovered.stop()


def previous_version(root):
    fixture_path = Path(__file__).resolve().parents[1] / 'fixtures/stage4a-persistence-v1.json'
    fixture = json.loads(fixture_path.read_text())
    assert fixture['source_commit'] == '5e3b3cceaf1fd9c9522567b0a4807f2f7cf604cc'
    target = root / 'previous'; target.mkdir()
    for name, content in fixture['files'].items():
        relative = Path(name)
        assert not relative.is_absolute() and '..' not in relative.parts
        data = bytes.fromhex(content['hex'])
        assert hashlib.sha256(data).hexdigest() == content['sha256']
        path = target / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    for cycle in range(3):
        running = RunningServer(target/'db.aof', '--snapshot-dir', str(target/'snapshots'))
        try:
            assert running.command(b'GET', b'key\0\xff') == RespValue('bulk', b'value\0\xff!')
            assert running.command(b'LRANGE', b'l', b'0', b'-1') == RespValue('array', (RespValue('bulk', b'a'),RespValue('bulk', b'\0\xff')))
            assert running.command(b'HGET', b'h', b'f') == RespValue('bulk', b'\0\xff')
            assert running.command(b'SISMEMBER', b's', b'\0\xff') == RespValue('integer', 1)
            assert running.command(b'GET', b'gone') == RespValue('null_bulk', None)
            assert running.command(b'GET', b'n') == RespValue('bulk', str(10+cycle).encode())
            assert running.command(b'INCR', b'n') == RespValue('integer', 11+cycle)
            running.stop(abrupt=True)
        finally:
            if running.proc.poll() is None: running.stop()
    print('PASS accepted Stage 4A V1 snapshot + AOF fixture and three SIGKILL/restart cycles')


if __name__ == '__main__':
    benchmark_reply_checks()
    with tempfile.TemporaryDirectory(prefix='redisdb-hardening-') as temporary:
        root = Path(temporary)
        sustained_and_shutdown(root)
        previous_version(root)
    print('PASS persistent mixed-client, bounded pipelines, slow reader, pending SIGTERM and V1 recovery')
