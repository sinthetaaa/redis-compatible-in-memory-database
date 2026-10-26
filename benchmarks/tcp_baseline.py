"""Bounded closed-loop TCP measurements; stdlib only, no unowned server mode."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests/differential')]
from docker_support import connect, docker
from server_runtime import NativeServer, ContainerServer
from resp2_wire import RespReader, RespValue, encode_command

MIXES = {'get': 0.0, 'set': 1.0, 'read80': 0.2, 'write80': 0.8}
VALUE_BYTES = 64
MAX_OPS_PER_CLIENT = 1000000


def percentile(values, percentile):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * percentile) - 1)] / 1e6


def exchange(sock, reader, command):
    sock.sendall(encode_command(command))
    return reader.read_reply()[0]


def seed(port, count):
    with connect(port) as sock:
        reader = RespReader(sock)
        for first in range(0, count, 16):
            command = [b'MSET']
            for index in range(first, min(count, first + 16)):
                command += [f'k{index:08}'.encode(), b'z' * VALUE_BYTES]
            if exchange(sock, reader, command) != RespValue('simple', b'OK'):
                raise RuntimeError('dataset preload failed')


def phase(port, *, clients, depth, keys, workload, seconds, phase_id, collect):
    timing = {}
    def start_clock():
        timing['start'] = time.perf_counter()
        timing['end'] = timing['start'] + seconds
    ready = threading.Barrier(clients + 1, action=start_clock)

    def worker(client):
        rng = random.Random(20261026 + client)
        latencies = []
        writes = request_bytes = operations = 0
        with connect(port) as sock:
            reader = RespReader(sock)
            ready.wait(timeout=15)
            while time.perf_counter() < timing['end'] and operations < MAX_OPS_PER_CLIENT:
                batch = []
                for offset in range(min(depth, MAX_OPS_PER_CLIENT - operations)):
                    key = f'k{rng.randrange(keys):08}'.encode()
                    write = rng.random() < MIXES[workload]
                    # Every SET changes its value, including across warmup and workers.
                    value = f'{phase_id:02x}{client:02x}{operations + offset:012x}'.encode().ljust(VALUE_BYTES, b'x')
                    command = (b'SET', key, value) if write else (b'GET', key)
                    encoded = encode_command(command)
                    batch.append((write, encoded))
                    request_bytes += len(encoded)
                start_ns = time.perf_counter_ns()
                sock.sendall(b''.join(encoded for _, encoded in batch))
                for write, _ in batch:
                    reply = reader.read_reply()[0]
                    if write:
                        if reply != RespValue('simple', b'OK'):
                            raise RuntimeError('SET failed during benchmark')
                        writes += 1
                    elif reply.kind != 'bulk' or len(reply.value) != VALUE_BYTES:
                        raise RuntimeError('GET lost a preloaded value')
                    if collect:
                        latencies.append(time.perf_counter_ns() - start_ns)
                    operations += 1
        return {'operations': operations, 'writes': writes, 'request_bytes': request_bytes,
                'latencies': latencies, 'finished': time.perf_counter(),
                'hit_operation_cap': operations == MAX_OPS_PER_CLIENT}

    with ThreadPoolExecutor(max_workers=clients) as pool:
        futures = [pool.submit(worker, client) for client in range(clients)]
        ready.wait(timeout=15)
        results = [future.result(timeout=seconds + 20) for future in futures]
    elapsed = max(result['finished'] for result in results) - timing['start']
    latencies = [item for result in results for item in result['latencies']]
    operations = sum(result['operations'] for result in results)
    if operations == 0:
        raise RuntimeError('benchmark made no progress')
    result = {'operations': operations, 'writes': sum(r['writes'] for r in results),
              'request_bytes': sum(r['request_bytes'] for r in results),
              'elapsed_seconds': elapsed, 'throughput_ops_sec': operations / elapsed,
              'operation_cap_hit': any(r['hit_operation_cap'] for r in results)}
    if collect:
        result['latency_ms'] = {name: percentile(latencies, fraction)
                               for name, fraction in (('p50', .5), ('p95', .95), ('p99', .99))}
    return result


def scenarios(smoke):
    # A baseline workload/client/pipeline matrix plus an independent persistence
    # scaling matrix, rather than an unbounded Cartesian product.
    result = set()
    for workload in ('get', 'set', 'read80', 'write80'):
        for clients in (1, 4):
            for depth in (1, 16):
                result.add(('off', 1000, workload, clients, depth))
    for policy in ('off', 'always', 'everysec', 'no'):
        for keys in (100, 1000):
            for workload in ('get', 'write80'):
                result.add((policy, keys, workload, 1, 1))
    if smoke:
        return [(p, 100, 'write80', 1, 4) for p in ('off', 'always', 'everysec', 'no')]
    return sorted(result)


def metadata(args):
    def git(*command):
        return subprocess.check_output(['git', '-C', str(ROOT), *command], text=True).strip()
    source_files = sorted(set(git('ls-files').splitlines() + git('ls-files', '--others', '--exclude-standard').splitlines()))
    fingerprint = hashlib.sha256()
    for name in source_files:
        path = ROOT / name
        if path.is_file() and not path.is_symlink():
            fingerprint.update(name.encode() + b'\0' + path.read_bytes() + b'\0')
    cpu = platform.processor()
    if platform.system() == 'Darwin':
        cpu = subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
    elif Path('/proc/cpuinfo').is_file():
        cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                    if line.startswith('model name')), cpu)
    data = {'git_commit': git('rev-parse', 'HEAD'), 'dirty_worktree': bool(git('status', '--porcelain')),
            'source_files_sha256': fingerprint.hexdigest(), 'host': platform.node(),
            'os': platform.platform(), 'architecture': platform.machine(), 'cpu': cpu,
            'logical_cpus': os.cpu_count(), 'python': sys.version, 'value_bytes': VALUE_BYTES,
            'key_bytes': 9,
            'request_sizes_bytes': {'GET': len(encode_command((b'GET', b'k00000000'))),
                                    'SET': len(encode_command((b'SET', b'k00000000', b'x' * VALUE_BYTES)))},
            'key_distribution': 'uniform deterministic PRNG',
            'warmup_seconds': args.warmup, 'requested_seconds': args.seconds,
            'repeats': args.repeats, 'selected_policies': args.policies, 'max_operations_per_client': MAX_OPS_PER_CLIENT,
            'mode': 'container' if args.image else 'native',
            'method': 'closed-loop, bounded pipeline batches; nearest-rank per-operation completion latency',
            'limitations': ['Python driver and reply parsing consume CPU; not server-only capacity',
                'Pipelined operations share batch-send timestamp; includes queue and preceding reply decode time',
                'Closed-loop load omits open-loop coordinated-omission correction',
                'Snapshot time is signal-to-publication observation, including polling/CLI overhead',
                'No Redis throughput comparison or power-loss inference; short shared-host runs are noisy']}
    if args.image:
        image = json.loads(docker('image', 'inspect', args.image).stdout)[0]
        labels = image['Config'].get('Labels') or {}
        if (labels.get('io.redisdb.build.configuration') != 'Release' or
            labels.get('org.opencontainers.image.source') !=
                'https://github.com/sinthetaaa/redis-compatible-in-memory-database'):
            raise ValueError("container baseline requires this project's labelled Release image")
        if labels.get('org.opencontainers.image.revision') != data['git_commit']:
            raise ValueError('container image revision must match benchmark Git HEAD')
        data.update({'image_id': image['Id'], 'image_architecture': image['Architecture'],
                     'image_labels': image['Config']['Labels'], 'build_configuration': 'Dockerfile Release'})
    else:
        cache = args.server.parent / 'CMakeCache.txt'
        if not cache.is_file():
            raise ValueError('native benchmark requires adjacent CMakeCache.txt to verify build configuration')
        entries = dict(line.split('=', 1) for line in cache.read_text().splitlines()
                       if '=' in line and not line.startswith(('#', '//')))
        if entries.get('CMAKE_BUILD_TYPE:STRING') != 'Release':
            raise ValueError('native baselines require a verified Release build')
        data.update({'binary_sha256': hashlib.sha256(args.server.read_bytes()).hexdigest(),
                     'build_configuration': 'Release', 'cmake_compiler': entries.get('CMAKE_CXX_COMPILER:FILEPATH'),
                     'cmake_cxx_flags': entries.get('CMAKE_CXX_FLAGS:STRING'),
                     'cmake_release_flags': entries.get('CMAKE_CXX_FLAGS_RELEASE:STRING')})
    return data


def main():
    parser = argparse.ArgumentParser()
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--server', type=Path)
    choice.add_argument('--image')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', type=float, default=2.0)
    parser.add_argument('--warmup', type=float, default=.15)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--policies', nargs='+', choices=('off', 'always', 'everysec', 'no'),
                        default=['off', 'always', 'everysec', 'no'])
    parser.add_argument('--smoke', action='store_true', help='four bounded functional/performance profiles only')
    args = parser.parse_args()
    if not (.1 <= args.seconds <= 5 and .05 <= args.warmup <= 2 and 1 <= args.repeats <= 5):
        parser.error('duration/warmup/repeat bounds exceeded')
    if 'everysec' in args.policies and args.seconds < 2 and not args.smoke:
        parser.error('everysec baselines require >=2 seconds; short smoke results are explicitly limited')
    if args.server:
        args.server = args.server.resolve(strict=True)
    if args.output.exists():
        parser.error('output already exists; use a new path')
    report = {'metadata': metadata(args), 'profiles': [], 'complete': False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        for policy, keys, workload, clients, depth in scenarios(args.smoke):
            if policy not in args.policies:
                continue
            profile = {'policy': policy, 'keys': keys, 'dataset_value_bytes': keys * VALUE_BYTES,
                       'dataset_logical_bytes': keys * (64 + 9 + VALUE_BYTES),
                       'workload': workload, 'clients': clients, 'pipeline': depth, 'runs': []}
            report['profiles'].append(profile)
            for repeat in range(args.repeats):
                server = ContainerServer(args.image, policy) if args.image else NativeServer(args.server, policy)
                try:
                    seed(server.port, keys)
                    phase(server.port, clients=clients, depth=depth, keys=keys, workload=workload,
                          seconds=args.warmup, phase_id=0, collect=False)
                    before = server.aof_bytes()
                    result = phase(server.port, clients=clients, depth=depth, keys=keys, workload=workload,
                                   seconds=args.seconds, phase_id=1, collect=True)
                    result['aof_growth_bytes'] = server.aof_bytes() - before
                    result['peak_rss'] = server.peak_rss()
                    result['snapshot_observed_seconds'] = server.snapshot() if policy != 'off' else None
                    result['repeat'] = repeat
                    profile['runs'].append(result)
                finally:
                    server.close()
            profile['median_ops_sec'] = statistics.median(r['throughput_ops_sec'] for r in profile['runs'])
            profile['median_p99_ms'] = statistics.median(r['latency_ms']['p99'] for r in profile['runs'])
            print(f'{policy:8} keys={keys:4} {workload:7} c={clients} p={depth:2} '
                  f'{profile["median_ops_sec"]:10.0f} ops/s p99={profile["median_p99_ms"]:.3f} ms', flush=True)
        report['complete'] = True
    finally:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS {len(report["profiles"])} benchmark profiles -> {args.output}')


if __name__ == '__main__':
    main()
