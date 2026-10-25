"""Bounded native mixed-command parity using the existing runner and raw replay."""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import random
import subprocess
import sys
import tempfile
from pathlib import Path

from curated_parity import ALLOWED, _pack_trace, _replay_trace, run
from native_smoke import HarnessFailure
from resp2_wire import RespProtocolError

GENERATOR_VERSION = 'phase6g-mixed-v1'
SEEDS = (0, 1, 7, 42, 99, 123, 256, 1024, 2026, 65535)
COMMANDS_PER_SEED = 100
ROUTES = tuple(sorted(ALLOWED - {'QUIT'}))
EXCLUSIONS = {
    'QUIT': 'Dedicated connection-termination coverage in curated_parity.py.',
    'POSITIVE_TTL': 'Clock bounds and natural expiry are covered by negative_ttl.py; '
                    'mixed traces use plain SET and nonpositive EXPIRE/PEXPIRE only.',
    'REDIS_ONLY_COMMANDS': 'Only the frozen 40-command matrix is eligible.',
    'ADVANCED_OPTIONS': 'No advanced SET options, conditional expiry or count-form pops.',
    'NUMERIC_EDGE_ERRORS': 'Canonical small integers on a dedicated numeric key; '
                           'unapproved error-text differences are outside this campaign.',
    'RESOURCE_AND_PROTOCOL_LIMITS': 'No oversized data, malformed framing or concurrent races.',
}


def mixed_trace(seed: int) -> list[tuple[str, tuple[bytes, ...]]]:
    """8 setup + 60 mixed + 32 final state probes = exactly 100 requests.

    At most 60 two-element pushes of <=8 bytes give a full-list RESP reply
    below 1800 bytes; strings stay below 500 bytes, hashes/sets have at most
    five fields/members. Every request/reply stays below 4096 bytes.
    """
    rng = random.Random(seed)
    keys = tuple(b'mix:' + str(i).encode() + b'\x00\xff' for i in range(7))
    number = b'mix:number'
    values = (b'', b'v', b'\x00\xff\r\n', b'123', b'abcdefgh')
    fields = (b'', b'f', b'\x00\xff', b'\r\n')
    steps = []

    def add(*args: bytes) -> None:
        steps.append(('a' if len(steps) % 2 == 0 else 'b', args))

    add(b'SET', number, b'0')
    add(b'SET', keys[0], b'initial')
    add(b'RPUSH', keys[1], b'l', b'l')
    add(b'HSET', keys[2], b'f', b'h')
    add(b'SADD', keys[3], b's', b's')
    add(b'SET', keys[4], b'')
    add(b'SET', keys[5], b'\x00\xff\r\n')
    add(b'DEL', keys[6])

    # Every seed visits all 39 midstream routes, then 21 additional choices.
    routes = list(ROUTES)
    rng.shuffle(routes)
    routes += [rng.choice(ROUTES) for _ in range(60 - len(routes))]
    for name in routes:
        key, other = rng.choice(keys), rng.choice(keys)
        value, field = rng.choice(values), rng.choice(fields)
        command = name.encode()
        if name == 'PING':
            args = (command,)
        elif name == 'ECHO':
            args = (command, value)
        elif name == 'SET':
            args = (command, key, value)
        elif name == 'MSET':
            args = (command, key, value, other, rng.choice(values))
        elif name in ('MGET', 'DEL', 'EXISTS'):
            args = (command, key, other, key)  # duplicates are intentional
        elif name == 'APPEND':
            args = (command, key, value)
        elif name in ('INCR', 'DECR'):
            args = (command, number)
        elif name in ('INCRBY', 'DECRBY'):
            args = (command, number, str(rng.randint(-9, 9)).encode())
        elif name in ('EXPIRE', 'PEXPIRE'):
            args = (command, key, rng.choice((b'0', b'-1')))
        elif name in ('LPUSH', 'RPUSH'):
            args = (command, key, value, value)
        elif name == 'LRANGE':
            args = (command, key, rng.choice((b'0', b'-3', b'1')), b'-1')
        elif name == 'HSET':
            args = (command, key, field, value, field, rng.choice(values))
        elif name in ('HGET', 'HEXISTS'):
            args = (command, key, field)
        elif name in ('HDEL', 'HMGET'):
            args = (command, key, field, rng.choice(fields), field)
        elif name in ('SADD', 'SREM', 'SMISMEMBER'):
            args = (command, key, field, rng.choice(fields), field)
        elif name == 'SISMEMBER':
            args = (command, key, field)
        else:
            args = (command, key)
        if rng.randrange(4) == 0:
            args = (args[0].lower(), *args[1:])
        add(*args)

    # Inspect complete final contents of every key, including rejected mutations.
    for key in (*keys, number):
        add(b'GET', key)
        add(b'LRANGE', key, b'0', b'-1')
        add(b'HGETALL', key)
        add(b'SMEMBERS', key)
    assert len(steps) == COMMANDS_PER_SEED
    return steps


def run_seed(custom: Path, reference: Path, report_path: Path, seed: int, *,
             replay_path: Path | None = None) -> dict:
    if replay_path is not None and replay_path.resolve() == report_path.resolve():
        raise ValueError('replay output must preserve the original report')
    if report_path.exists():
        raise ValueError('report already exists; choose a fresh output path')
    source = json.loads(replay_path.read_text()) if replay_path is not None else None
    trace = _replay_trace(replay_path) if replay_path is not None else _pack_trace(mixed_trace(seed))
    metadata = {
        'generator_version': source.get('generator_version') if source else GENERATOR_VERSION,
        'seed': source.get('seed') if source else seed,
        'scope': 'BOUNDED_MIXED_NATIVE_ONLY_NOT_LINUX',
        'platform': platform.platform(),
        'target_git_sha': subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=Path(__file__).resolve().parents[2],
            capture_output=True, text=True, check=True).stdout.strip(),
        'trace_sha256': hashlib.sha256(json.dumps(trace, sort_keys=True).encode()).hexdigest(),
        'exclusions': EXCLUSIONS,
    }
    try:
        return run(custom, reference, report_path, trace, replay=replay_path is not None)
    finally:
        # The existing runner saves the full trace and cleanup state on failures too.
        if report_path.exists():
            report = json.loads(report_path.read_text())
            report.update(metadata)
            with tempfile.NamedTemporaryFile('w', dir=report_path.parent, delete=False) as stream:
                staged = Path(stream.name)
                json.dump(report, stream, indent=2, sort_keys=True)
                stream.write('\n')
            staged.replace(report_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--custom', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report-dir', type=Path, required=True, help='fresh campaign output directory')
    parser.add_argument('--replay', type=Path, help='one saved seed report; generator is bypassed')
    args = parser.parse_args()
    try:
        args.report_dir.mkdir(parents=True, exist_ok=False)
        seeds = (0,) if args.replay else SEEDS
        for seed in seeds:
            path = args.report_dir / ('replay.json' if args.replay else f'seed-{seed}.json')
            result = run_seed(args.custom, args.reference, path, seed, replay_path=args.replay)
            print(f'MIXED_PARITY=PASS seed={seed if not args.replay else "replay"} '
                  f'commands={result["passed"]} report={path}', flush=True)
        print(f'MIXED_CAMPAIGN=PASS traces={len(seeds)} commands={len(seeds) * COMMANDS_PER_SEED}'
              if not args.replay else 'MIXED_REPLAY=PASS', flush=True)
        return 0
    except (HarnessFailure, RespProtocolError, OSError, ValueError, TypeError,
            subprocess.SubprocessError) as exc:
        print(f'MIXED_CAMPAIGN=FAIL {type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
