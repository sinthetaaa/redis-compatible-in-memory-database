"""Small owned-resource Docker adapter. Never inspects/mutates unowned data."""
from __future__ import annotations

import json
import socket
import subprocess
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABEL = 'io.redisdb.stage4a.owner'


def docker(*args, timeout=60, check=True):
    result = subprocess.run(['docker', *map(str, args)], capture_output=True, text=True,
                            timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f'docker {args[0]} failed: {result.stderr.strip()}')
    return result


def inspect(container):
    return json.loads(docker('inspect', container).stdout)[0]


def connect(port):
    sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    sock.settimeout(10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


class DockerResources:
    def __init__(self):
        version = json.loads(docker('version', '--format', '{{json .Server}}').stdout)
        if version.get('Os') != 'linux' or int(version['Version'].split('.')[0]) < 28:
            raise RuntimeError('requires Linux Docker Engine >=28 for localhost publishing isolation')
        self.version = version['Version']
        self.architecture = version['Arch']
        self.owner = uuid.uuid4().hex
        self.volumes = []
        self.containers = []
        self.network = None

    def volume(self):
        name = 'redisdb-stage4a-' + self.owner + '-' + str(len(self.volumes))
        docker('volume', 'create', '--label', LABEL + '=' + self.owner, name)
        self.volumes.append(name)
        return name

    def create(self, image, volume, args=(), *, port=6380, user=None, entrypoint=None,
               publish=True, extra_mounts=()):
        name = 'redisdb-stage4a-' + self.owner + '-' + str(len(self.containers))
        flags = ['create', '--name', name, '--label', LABEL + '=' + self.owner,
                 '--read-only', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
                 '--pids-limit', '64', '--memory', '512m',
                 '--mount', f'type=volume,src={volume},dst=/data']
        if publish:
            if self.network is None:
                name = 'redisdb-stage4a-' + self.owner
                docker('network', 'create', '--label', LABEL + '=' + self.owner, name)
                self.network = name
            flags += ['--network', self.network, '--publish', f'127.0.0.1::{port}']
        else:
            flags += ['--network', 'none']
        if user:
            flags += ['--user', user]
        if entrypoint:
            flags += ['--entrypoint', entrypoint]
        for mount in extra_mounts:
            flags += ['--mount', mount]
        container = docker(*flags, image, *args).stdout.strip()
        self.containers.append(container)
        return container

    def start(self, image, volume, args=(), *, port=6380, **kwargs):
        container = self.create(image, volume, args, port=port, **kwargs)
        docker('start', container)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = inspect(container)
            if not state['State']['Running']:
                raise RuntimeError('container exited before readiness: ' + docker('logs', container).stdout)
            bindings = state['NetworkSettings']['Ports'].get(f'{port}/tcp')
            if bindings:
                if len(bindings) != 1 or bindings[0]['HostIp'] != '127.0.0.1':
                    raise RuntimeError('unexpected host publication; refusing connection')
                host_port = int(bindings[0]['HostPort'])
                try:
                    with connect(host_port) as sock:
                        sock.sendall(b'*1\r\n$4\r\nPING\r\n')
                        if sock.recv(64) == b'+PONG\r\n':
                            return container, host_port
                except OSError:
                    pass
            time.sleep(0.05)
        raise RuntimeError('container readiness timed out')

    def helper(self, image, volume, script, *, extra_mounts=()):
        container = self.create(image, volume, ['-ec', script], user='10001:10001',
                                entrypoint='/bin/sh', publish=False, extra_mounts=extra_mounts)
        result = docker('start', '--attach', container)
        state = inspect(container)['State']
        if state['ExitCode'] != 0:
            raise RuntimeError('owned fixture helper failed: ' + result.stdout + result.stderr)
        return result.stdout

    def close(self):
        errors = []
        for container in reversed(self.containers):
            try:
                state = inspect(container)
                if state['Config']['Labels'].get(LABEL) != self.owner:
                    raise RuntimeError('ownership label mismatch')
                docker('rm', '--force', container)
            except Exception as exc:
                errors.append(str(exc))
        for volume in reversed(self.volumes):
            try:
                state = json.loads(docker('volume', 'inspect', volume).stdout)[0]
                if state['Labels'].get(LABEL) != self.owner:
                    raise RuntimeError('volume ownership label mismatch')
                docker('volume', 'rm', volume)
            except Exception as exc:
                errors.append(str(exc))
        if getattr(self, 'network', None):
            try:
                state = json.loads(docker('network', 'inspect', self.network).stdout)[0]
                if state['Labels'].get(LABEL) != self.owner:
                    raise RuntimeError('network ownership label mismatch')
                docker('network', 'rm', self.network)
            except Exception as exc:
                errors.append(str(exc))
        if errors:
            raise RuntimeError('owned Docker cleanup failed: ' + '; '.join(errors))

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()
