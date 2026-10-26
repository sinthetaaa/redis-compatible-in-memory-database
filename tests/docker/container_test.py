"""Actual Linux containers, localhost ports and disposable labelled persistence volumes."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests/differential')]
from docker_support import DockerResources, connect, docker, inspect
from resp2_wire import RespReader, RespValue, encode_command


def exchange(port, *args):
    with connect(port) as sock:
        sock.sendall(encode_command(args))
        return RespReader(sock).read_reply()[0]


def snapshot(container):
    before = docker('logs', container).stdout.count('SNAPSHOT_PUBLISHED ')
    started = time.monotonic()
    docker('kill', '--signal', 'USR1', container)
    deadline = started + 15
    while time.monotonic() < deadline:
        if docker('logs', container).stdout.count('SNAPSHOT_PUBLISHED ') > before:
            return time.monotonic() - started
        if not inspect(container)['State']['Running']:
            raise AssertionError('snapshot failed: ' + docker('logs', container).stderr)
        time.sleep(0.02)
    raise AssertionError('snapshot timed out')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    assert shutil.which('redis-cli'), 'install redis-tools on the test host'
    details = {'image': args.image, 'cases': [], 'power_loss_tested': False}
    with DockerResources() as owned:
        volume = owned.volume()
        container, port = owned.start(args.image, volume)
        state = inspect(container)
        assert state['Config']['User'] == '10001:10001'
        assert state['Config']['Entrypoint'] == ['/usr/local/bin/redisdb-server']
        assert state['HostConfig']['ReadonlyRootfs'] and not state['HostConfig']['Privileged']
        assert docker('exec', container, 'id', '-u').stdout.strip() == '10001'
        assert docker('exec', container, 'id', '-g').stdout.strip() == '10001'
        status = docker('exec', container, 'cat', '/proc/1/status').stdout
        assert next(line.split()[1:] for line in status.splitlines()
                    if line.startswith('Uid:')) == ['10001'] * 4
        assert next(line.split()[1:] for line in status.splitlines()
                    if line.startswith('Gid:')) == ['10001'] * 4
        docker('exec', container, 'sh', '-ec',
               'test ! -e /src; test ! -e /build; ! command -v g++; ! command -v cmake; ! command -v ninja')
        details['image_bytes'] = json.loads(docker('image', 'inspect', args.image).stdout)[0]['Size']
        cli = subprocess.run(['redis-cli', '-h', '127.0.0.1', '-p', str(port), 'PING'],
                             capture_output=True, timeout=10, check=True)
        assert cli.stdout == b'PONG\n'
        binary = b'\0\xff\r\n'
        assert exchange(port, b'SET', binary, binary) == RespValue('simple', b'OK')
        assert exchange(port, b'GET', binary) == RespValue('bulk', binary)
        assert exchange(port, b'RPUSH', b'l', binary, b'') == RespValue('integer', 2)
        assert exchange(port, b'HSET', b'h', binary, binary) == RespValue('integer', 1)
        assert exchange(port, b'SADD', b's', binary) == RespValue('integer', 1)
        assert exchange(port, b'SET', b'n', b'1') == RespValue('simple', b'OK')
        details['snapshot_observed_seconds'] = snapshot(container)
        assert exchange(port, b'INCR', b'n') == RespValue('integer', 2)
        docker('stop', '--time', '20', container)
        assert inspect(container)['State']['ExitCode'] == 0
        assert 'RESP_STOPPED' in docker('logs', container).stdout
        details['cases'] += ['runtime hygiene/nonroot', 'redis-cli published localhost',
                             'types and binary', 'snapshot signal', 'graceful SIGTERM']
        container, port = owned.start(args.image, volume)
        assert exchange(port, b'GET', binary) == RespValue('bulk', binary)
        assert exchange(port, b'GET', b'n') == RespValue('bulk', b'2')
        assert exchange(port, b'LRANGE', b'l', b'0', b'-1') == RespValue(
            'array', (RespValue('bulk', binary), RespValue('bulk', b'')))
        assert exchange(port, b'HGET', b'h', binary) == RespValue('bulk', binary)
        assert exchange(port, b'SMEMBERS', b's') == RespValue('array', (RespValue('bulk', binary),))
        assert exchange(port, b'INCR', b'n') == RespValue('integer', 3)
        docker('kill', '--signal', 'KILL', container)
        assert inspect(container)['State']['ExitCode'] == 137
        for _ in range(2):
            container, port = owned.start(args.image, volume)
            assert exchange(port, b'GET', b'n') == RespValue('bulk', b'3')
            docker('stop', '--time', '20', container)
        details['cases'] += ['recreation with same volume', 'snapshot plus AOF suffix',
                             'SIGKILL always-policy acknowledgment', 'repeat recovery']
        fresh, fresh_port = owned.start(args.image, owned.volume())
        assert exchange(fresh_port, b'GET', binary) == RespValue('null_bulk', None)
        docker('stop', fresh)
        details['cases'].append('independent empty volume')
        # Copies of this run's stopped fixture only. No user volumes or running files.
        mutations = {
            'missing snapshot': 'mv /data/snapshots/generation-1.snap /data/snapshot.fixture-backup',
            'corrupt snapshot': "printf bad > /data/snapshots/generation-1.snap",
            'missing journal': 'mv /data/appendonly.aof /data/journal.fixture-backup',
            'missing watermark': 'mv /data/appendonly.aof.commit /data/watermark.fixture-backup',
            'incomplete journal': "printf bad >> /data/appendonly.aof",
        }
        for label, script in mutations.items():
            fixture = owned.volume()
            owned.helper(args.image, fixture, 'cp -a /source/. /data/; ' + script,
                         extra_mounts=[f'type=volume,src={volume},dst=/source,readonly'])
            failed = owned.create(args.image, fixture)
            docker('start', failed)
            result = docker('wait', failed, timeout=20)
            assert result.stdout.strip() == '1', (label, result)
            assert 'RESP_READY' not in docker('logs', failed).stdout, label
            details['cases'].append(label + ' rejected')
        details['docker_version'] = owned.version
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(details, indent=2) + '\n')
    print(f'PASS {len(details["cases"])} container cases; image_bytes={details["image_bytes"]}')


if __name__ == '__main__':
    main()
