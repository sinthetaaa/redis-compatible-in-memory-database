"""Owned server processes: snapshot signals, generations, suffix replay and SIGKILL.

SIGKILL demonstrates process crashes, never power-loss behavior of storage devices.
"""
import os
from pathlib import Path
import resource
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from resp_aof_test import RunningServer, server, launch_args, OK, RespValue


def crc32c(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def selected(directory):
    return struct.unpack('>Q', (directory / 'CURRENT').read_bytes()[16:24])[0]


def request_snapshot(running, directory):
    previous = selected(directory)
    running.proc.send_signal(signal.SIGUSR1)
    assert select.select([running.proc.stdout], [], [], 10)[0], 'snapshot publication timeout'
    line = running.proc.stdout.readline()
    assert line.startswith(b'SNAPSHOT_PUBLISHED '), (line, running.proc.poll())
    generation, sequence = map(int, line.split()[1:])
    assert generation > previous and selected(directory) == generation, line
    assert (directory / f'generation-{generation}.snap').is_file()
    return generation, sequence


def failure(path, directory):
    before = {p: p.read_bytes() for p in (path, Path(str(path) + '.commit')) if p.is_file()}
    result = subprocess.run(launch_args(path, '--snapshot-dir', str(directory)),
                            capture_output=True, timeout=10)
    assert result.returncode == 1 and b'RESP_READY' not in result.stdout, result
    assert all(p.read_bytes() == data for p, data in before.items()), result
    return result.stderr


def test_types_suffix_and_cycles(root):
    for policy in ('always', 'everysec', 'no'):
        directory = root / policy
        directory.mkdir()
        path = root / (policy + '.aof')
        options = ('--snapshot-dir', str(directory), '--appendfsync', policy)
        binary = b'\x00\xff\r\n'
        running = RunningServer(path, *options)
        try:
            assert running.command(b'SET', binary, binary) == OK
            assert running.command(b'RPUSH', b'l', binary, b'') == RespValue('integer', 2)
            assert running.command(b'HSET', b'h', binary, binary) == RespValue('integer', 1)
            assert running.command(b'SADD', b's', binary) == RespValue('integer', 1)
            assert running.command(b'SET', b'n', b'9') == OK
            assert running.command(b'INCR', b'n') == RespValue('integer', 10)
            assert running.command(b'SET', b'future', b'v', b'PX', b'60000') == OK
            assert running.command(b'SET', b'expired', b'v', b'PX', b'700') == OK
            first, sequence = request_snapshot(running, directory)
            marker = Path(str(path) + '.commit').read_bytes()
            assert struct.unpack('>Q', marker[8:16])[0] == sequence, 'weaker policy not checkpointed'
            old = (directory / f'generation-{first}.snap').read_bytes()
            assert running.command(b'INCR', b'n') == RespValue('integer', 11)
            second, _ = request_snapshot(running, directory)
            assert second > first and (directory / f'generation-{first}.snap').read_bytes() == old
            assert running.command(b'APPEND', b'n', b'!') == RespValue('integer', 3)
            # Synchronize suffix even under weak policies, then test process death.
            running.stop(abrupt=policy == 'always')
        finally:
            if running.proc.poll() is None:
                running.stop()
        time.sleep(0.8)  # key expires while no server exists
        for cycle in range(3):
            with server(path, *options) as recovered:
                assert recovered.command(b'GET', binary) == RespValue('bulk', binary)
                assert recovered.command(b'GET', b'n') == RespValue('bulk', b'11!' + b'!' * cycle)
                assert recovered.command(b'LRANGE', b'l', b'0', b'-1') == RespValue(
                    'array', (RespValue('bulk', binary), RespValue('bulk', b'')))
                assert recovered.command(b'HGET', b'h', binary) == RespValue('bulk', binary)
                assert recovered.command(b'SMEMBERS', b's') == RespValue('array', (RespValue('bulk', binary),))
                assert recovered.command(b'PTTL', b'expired') == RespValue('integer', -2)
                assert 0 < recovered.command(b'PTTL', b'future').value < 60000
                assert recovered.command(b'APPEND', b'n', b'!').kind == 'integer'


def test_eviction_and_aof_only(root):
    path = root / 'memory.aof'
    directory = root / 'memory'
    directory.mkdir()
    memory = ('--maxmemory', '132', '--maxmemory-policy', 'allkeys-lru')
    # Existing AOF-only directory migrates without changing the journal format.
    with server(path, *memory) as running:
        assert running.command(b'MSET', b'a', b'x', b'b', b'y') == OK
    options = (*memory, '--snapshot-dir', str(directory))
    with server(path, *options) as running:
        assert running.command(b'GET', b'a') == RespValue('bulk', b'x')  # read-only touch
        request_snapshot(running, directory)
    with server(path, *options) as running:
        assert running.command(b'SET', b'c', b'z') == OK  # snapshot captured a newer than b
        assert running.command(b'GET', b'b') == RespValue('null_bulk', None)
        request_snapshot(running, directory)
    with server(path, *options) as running:
        assert running.command(b'MGET', b'a', b'c') == RespValue(
            'array', (RespValue('bulk', b'x'), RespValue('bulk', b'z')))
    result = subprocess.run(launch_args(path, '--snapshot-dir', str(directory), '--maxmemory', '1'),
                            capture_output=True, timeout=10)
    assert result.returncode == 1 and b'OOM' in result.stderr, result
    # Explicit AOF-only mode remains usable: snapshots do not alter or replace the full log.
    with server(path, *memory) as running:
        assert running.command(b'GET', b'c') == RespValue('bulk', b'z')


def test_invalid_recovery(root):
    base = root / 'invalid-base'
    base.mkdir()
    path = base / 'db.aof'
    directory = base / 'snapshots'
    directory.mkdir()
    with server(path, '--snapshot-dir', str(directory)) as running:
        assert running.command(b'SET', b'k', b'v') == OK
        request_snapshot(running, directory)
    mutations = ('corrupt', 'version', 'sequence', 'offset', 'prefix', 'truncated',
                 'missing', 'symlink', 'fifo', 'missing-current', 'corrupt-current',
                 'missing-aof', 'truncated-aof', 'missing-watermark', 'corrupt-prefix')
    for mutation in mutations:
        fixture = root / mutation
        shutil.copytree(base, fixture)
        aof, snapshots = fixture / 'db.aof', fixture / 'snapshots'
        snapshot = snapshots / 'generation-1.snap'
        data = bytearray(snapshot.read_bytes())
        if mutation in ('version', 'sequence', 'offset', 'prefix'):
            offset = {'version': 8, 'sequence': 24, 'offset': 32, 'prefix': 44}[mutation]
            data[offset] ^= 1
            data[76:80] = struct.pack('>I', crc32c(data[:76]))
            data[-12:-8] = struct.pack('>I', crc32c(data[:-20]))
            data[-8:-4] = struct.pack('>I', crc32c(data[-20:-8]))
            snapshot.write_bytes(data)
        elif mutation == 'corrupt':
            data[90] ^= 1
            snapshot.write_bytes(data)
        elif mutation == 'truncated':
            snapshot.write_bytes(data[:-1])
        elif mutation in ('missing', 'symlink', 'fifo'):
            snapshot.unlink()
            if mutation == 'symlink':
                snapshot.symlink_to(aof)
            elif mutation == 'fifo':
                os.mkfifo(snapshot)
        elif mutation == 'missing-current':
            (snapshots / 'CURRENT').unlink()
        elif mutation == 'corrupt-current':
            (snapshots / 'CURRENT').write_bytes(b'broken')
        elif mutation == 'missing-aof':
            aof.unlink()
        elif mutation == 'truncated-aof':
            aof.write_bytes(aof.read_bytes()[:-1])
        elif mutation == 'corrupt-prefix':
            log = bytearray(aof.read_bytes())
            log[30] ^= 1
            aof.write_bytes(log)
        elif mutation == 'missing-watermark':
            Path(str(aof) + '.commit').unlink()
        failure(aof, snapshots)
        if mutation == 'missing-aof':
            assert not aof.exists(), 'snapshot recovery recreated missing journal'
    for options in (('--snapshot-dir', str(directory)),
                    ('--appendonly', 'no', '--snapshot-dir', str(directory)),
                    ('--appendonly', 'yes', '--snapshot-dir', ''),
                    ('--appendonly', 'yes', '--snapshot-dir', str(directory), '--snapshot-dir', str(directory))):
        result = subprocess.run([sys.argv[1], '--resp2-server', *options], capture_output=True, timeout=5)
        assert result.returncode == 2, (options, result)


def test_publication_disk_failure(root):
    directory = root / 'disk-failure'
    directory.mkdir()
    path = root / 'disk-failure.aof'
    options = ('--snapshot-dir', str(directory))

    def limit_snapshot_file():
        resource.setrlimit(resource.RLIMIT_FSIZE, (800, 800))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)

    running = RunningServer(path, *options, preexec_fn=limit_snapshot_file)
    try:
        assert running.command(b'SET', b'n', b'1') == OK
        generation, _ = request_snapshot(running, directory)
        previous = (directory / f'generation-{generation}.snap').read_bytes()
        args = [b'MSET']
        for i in range(30):
            args.extend((f'k{i}'.encode(), b'v'))
        assert running.command(*args) == OK
        assert path.stat().st_size < 800
        running.proc.send_signal(signal.SIGUSR1)
        stdout, stderr = running.proc.communicate(timeout=10)
        assert running.proc.returncode == 1 and b'snapshot write' in stderr, (stdout, stderr)
        assert b'SNAPSHOT_PUBLISHED' not in stdout
        assert selected(directory) == generation
        assert (directory / f'generation-{generation}.snap').read_bytes() == previous
    finally:
        running.sock.close()
        if running.proc.poll() is None:
            running.stop(abrupt=True)
    with server(path, *options) as recovered:
        assert recovered.command(b'GET', b'n') == RespValue('bulk', b'1')
        assert recovered.command(b'GET', b'k29') == RespValue('bulk', b'v')
        request_snapshot(recovered, directory)


def test_kill_during_and_after_capture(root):
    directory = root / 'kill'
    directory.mkdir()
    path = root / 'kill.aof'
    options = ('--snapshot-dir', str(directory), '--appendfsync', 'no')
    running = RunningServer(path, *options)
    try:
        assert running.command(b'SET', b'n', b'1') == OK
        first, _ = request_snapshot(running, directory)
        # Enough real disk construction work to observe the unselected temp file.
        # The unit test additionally kills deterministically at every publication boundary.
        value = b'x' * 3000
        for i in range(700):
            assert running.command(b'SET', f'k{i}'.encode(), value) == OK
        assert running.command(b'INCR', b'n') == RespValue('integer', 2)
        # Explicit snapshot checkpoint synchronizes all AOF writes before staging starts.
        running.proc.send_signal(signal.SIGUSR1)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if any(directory.glob('snapshot.tmp.*')):
                running.proc.send_signal(signal.SIGSTOP)
                # Pausing after seeing a temp may race with completion; inspect selector.
                os.waitpid(running.proc.pid, os.WUNTRACED)
                assert selected(directory) == first, 'capture completed before SIGSTOP'
                running.stop(abrupt=True)
                break
            assert running.proc.poll() is None, 'server failed during snapshot'
        else:
            raise AssertionError('did not observe real snapshot construction')
    finally:
        if running.proc.poll() is None:
            running.stop(abrupt=True)
    with server(path, *options) as recovered:
        assert recovered.command(b'GET', b'n') == RespValue('bulk', b'2')
        assert recovered.command(b'GET', b'k699') == RespValue('bulk', value)
        assert selected(directory) == first
        generation, _ = request_snapshot(recovered, directory)
        assert generation > first
    running = RunningServer(path, *options)
    try:
        assert running.command(b'INCR', b'n') == RespValue('integer', 3)
        request_snapshot(running, directory)
    finally:
        running.stop(abrupt=True)
    for _ in range(2):
        with server(path, *options) as recovered:
            assert recovered.command(b'GET', b'n') == RespValue('bulk', b'3')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='redisdb-snapshot-tcp-') as temporary:
        root = Path(temporary)
        test_types_suffix_and_cycles(root)
        test_eviction_and_aof_only(root)
        test_invalid_recovery(root)
        test_publication_disk_failure(root)
        test_kill_during_and_after_capture(root)
    print('PASS snapshot TCP: three policies, types/binary/TTL, generations, suffix replay, '
          'eviction, invalid recovery, SIGKILL during/after publication')
