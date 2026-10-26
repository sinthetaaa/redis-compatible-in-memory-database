"""Owned TCP server lifecycle for bounded native/container benchmarks."""
from __future__ import annotations

from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time

from docker_support import DockerResources, docker, inspect


def options(policy, data, *, container=False):
    result = ['--resp2-server', '--port', '6380' if container else '0']
    if container:
        result += ['--bind', '0.0.0.0']
    if policy != 'off':
        result += ['--appendonly', 'yes', '--aof-file', str(data / 'appendonly.aof'),
                   '--appendfsync', policy, '--snapshot-dir', str(data / 'snapshots')]
    return result


class NativeServer:
    def __init__(self, binary, policy):
        self.temporary = tempfile.TemporaryDirectory(prefix='redisdb-benchmark-')
        self.data = Path(self.temporary.name)
        (self.data / 'snapshots').mkdir()
        self.stderr = (self.data / 'stderr').open('wb')
        self.proc = subprocess.Popen([str(binary), *options(policy, self.data)],
                                     stdout=subprocess.PIPE, stderr=self.stderr)
        try:
            if not select.select([self.proc.stdout], [], [], 15)[0]:
                raise RuntimeError('native readiness timeout')
            line = self.proc.stdout.readline()
            if not line.startswith(b'RESP_READY 127.0.0.1:'):
                raise RuntimeError('native startup failed: ' + (self.data / 'stderr').read_text())
            self.port = int(line.rsplit(b':', 1)[1])
        except BaseException:
            self.abort()
            raise

    def aof_bytes(self):
        file = self.data / 'appendonly.aof'
        return file.stat().st_size if file.exists() else 0

    def peak_rss(self):
        status = Path(f'/proc/{self.proc.pid}/status')
        if status.is_file():
            for line in status.read_text().splitlines():
                if line.startswith('VmHWM:'):
                    return {'bytes': int(line.split()[1]) * 1024, 'method': 'Linux /proc PID VmHWM'}
        return {'bytes': None, 'method': 'not available reliably on this native platform'}

    def resources(self):
        # POSIX ps reports cumulative process CPU time and current RSS (KiB).
        # CPU resolution is platform dependent; this is not a peak-RSS sample.
        result = subprocess.run(['ps', '-o', 'time=,rss=', '-p', str(self.proc.pid)],
                                capture_output=True, text=True, timeout=5, check=True)
        cpu, rss = result.stdout.split()
        seconds = 0.0
        for part in cpu.split(':'):
            seconds = seconds * 60 + float(part)
        return {'cpu_seconds': seconds, 'rss_bytes': int(rss) * 1024,
                'method': 'ps cumulative CPU time/current RSS; boundary samples, not peak'}

    def snapshot(self):
        start = time.perf_counter()
        self.proc.send_signal(signal.SIGUSR1)
        if not select.select([self.proc.stdout], [], [], 20)[0]:
            raise RuntimeError('native snapshot timeout')
        if not self.proc.stdout.readline().startswith(b'SNAPSHOT_PUBLISHED '):
            raise RuntimeError('native snapshot failed')
        return time.perf_counter() - start

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            out, _ = self.proc.communicate(timeout=20)
            if self.proc.returncode != 0 or b'RESP_STOPPED' not in out:
                raise RuntimeError('native server did not shut down cleanly')
        finally:
            self.abort()

    def abort(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.communicate(timeout=5)
        self.stderr.close()
        self.temporary.cleanup()


class ContainerServer:
    def __init__(self, image, policy):
        self.owned = DockerResources()
        try:
            self.volume = self.owned.volume()
            self.container, self.port = self.owned.start(image, self.volume,
                options(policy, Path('/data'), container=True))
        except BaseException:
            self.owned.close()
            raise

    def aof_bytes(self):
        result = docker('exec', self.container, 'sh', '-ec',
                        'if test -f /data/appendonly.aof; then stat -c %s /data/appendonly.aof; else echo 0; fi')
        return int(result.stdout)

    def peak_rss(self):
        # Linux containers expose the actual server as PID 1. Docker stats is
        # cgroup memory, not process RSS, so do not mislabel it.
        status = docker('exec', self.container, 'cat', '/proc/1/status').stdout
        for line in status.splitlines():
            if line.startswith('VmHWM:'):
                return {'bytes': int(line.split()[1]) * 1024, 'method': 'Linux container /proc/1/status VmHWM'}
        return {'bytes': None, 'method': 'VmHWM unavailable'}

    def resources(self):
        return {'cpu_seconds': None, 'rss_bytes': None,
                'method': 'container CPU/current RSS sampling not implemented; see peak_rss'}

    def snapshot(self):
        before = docker('logs', self.container).stdout.count('SNAPSHOT_PUBLISHED ')
        start = time.perf_counter()
        docker('kill', '--signal', 'USR1', self.container)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if docker('logs', self.container).stdout.count('SNAPSHOT_PUBLISHED ') > before:
                return time.perf_counter() - start
            if not inspect(self.container)['State']['Running']:
                raise RuntimeError('container snapshot failed')
            time.sleep(0.02)
        raise RuntimeError('container snapshot timeout')

    def close(self):
        try:
            docker('stop', '--time', '20', self.container)
            if inspect(self.container)['State']['ExitCode'] != 0:
                raise RuntimeError('container did not shut down cleanly')
        finally:
            self.owned.close()
