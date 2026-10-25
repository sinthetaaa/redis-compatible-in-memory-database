"""Phase 6G.2A.1: bounded wrong-type/state and TTL differential tests.

Standalone additive module: uses accepted stdlib-only RESP2 and owned-child
helpers. Never starts/stops unrelated Redis services or sends admin commands.
Positive TTLs use separate-clock bounds, not exact response equality.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from curated_parity import ALLOWED, decode_request
from native_smoke import (
    REFERENCE_SHA256, REFERENCE_VERSION, HarnessFailure, _close_owned,
    _connect, _digest, _exchange, _start_custom, _start_reference,
)
from resp2_wire import RespProtocolError, RespReader, comparable, encode_command

# These are intentional NON-COMPARISONS; none counts toward eligible parity.
EXCLUSIONS = (
    ('HELLO', 'Redis protocol negotiation is not implemented in the RESP2 subset'),
    ('AUTH', 'Authentication commands are outside the approved subset'),
    ('FLUSHDB', 'Administrative keyspace deletion must never be used by this harness'),
    ('SET_NX_XX_GET_KEEPTTL', 'Advanced SET options are not implemented'),
    ('EXPIRE_NX_XX_GT_LT', 'Conditional expiry modifiers are not implemented'),
    ('LPOP_RPOP_COUNT', 'Count-form list pops are not implemented'),
    ('OVERSIZED_RESPONSE', 'The C++ RESP2 server has an intentional 8192-byte response bound'),
)
MAX_STEPS = 128
MAX_WAIT_MS = 1500


class ParityMismatch(HarnessFailure):
    pass


class TimingAmbiguous(HarnessFailure):
    pass


def negative_steps() -> list[dict]:
    """Wrong type must be returned without mutating the original key."""
    result: list[dict] = []

    def cmd(who: str, *args: bytes, policy: str = 'strict') -> None:
        result.append({'client': who, 'args': args, 'policy': policy})

    cmd('a', b'SET', b'neg:str\x00\xff', b'payload\x00')
    cmd('b', b'RPUSH', b'neg:list', b'l1', b'l2')
    cmd('a', b'HSET', b'neg:hash', b'field', b'value')
    cmd('b', b'SADD', b'neg:set', b'member')
    # Key state probes surround rejected mutators and readers.
    cmd('a', b'GET', b'neg:list', policy='wrongtype')
    cmd('b', b'APPEND', b'neg:list', b'x', policy='wrongtype')
    cmd('a', b'STRLEN', b'neg:list', policy='wrongtype')
    cmd('b', b'INCR', b'neg:list', policy='wrongtype')
    cmd('a', b'INCRBY', b'neg:list', b'3', policy='wrongtype')
    cmd('b', b'LRANGE', b'neg:list', b'0', b'-1')
    cmd('a', b'LLEN', b'neg:list')
    cmd('b', b'TYPE', b'neg:list')
    cmd('a', b'HGET', b'neg:list', b'field', policy='wrongtype')
    cmd('b', b'HLEN', b'neg:list', policy='wrongtype')
    cmd('a', b'HGETALL', b'neg:list', policy='wrongtype')
    cmd('b', b'LRANGE', b'neg:list', b'0', b'-1')
    cmd('a', b'RPUSH', b'neg:str\x00\xff', b'x', policy='wrongtype')
    cmd('b', b'HSET', b'neg:str\x00\xff', b'f', b'v', policy='wrongtype')
    cmd('a', b'SADD', b'neg:str\x00\xff', b'm', policy='wrongtype')
    cmd('b', b'GET', b'neg:str\x00\xff')
    cmd('a', b'GET', b'neg:hash', policy='wrongtype')
    cmd('b', b'LPUSH', b'neg:hash', b'x', policy='wrongtype')
    cmd('a', b'HMGET', b'neg:hash', b'field', b'absent')
    cmd('b', b'HEXISTS', b'neg:hash', b'field')
    cmd('a', b'HGETALL', b'neg:hash')
    cmd('b', b'GET', b'neg:set', policy='wrongtype')
    cmd('a', b'LPOP', b'neg:set', policy='wrongtype')
    cmd('b', b'HDEL', b'neg:set', b'member', policy='wrongtype')
    cmd('a', b'SCARD', b'neg:set')
    cmd('b', b'SMEMBERS', b'neg:set')
    cmd('a', b'SMISMEMBER', b'neg:set', b'member', b'absent')
    cmd('b', b'MGET', b'neg:list', b'neg:hash', b'neg:set', b'neg:str\x00\xff')
    cmd('a', b'TTL', b'neg:list')  # -1 remains intact
    cmd('b', b'PTTL', b'neg:set')  # -1 remains intact
    cmd('a', b'EXISTS', b'neg:list', b'neg:hash', b'neg:set', b'neg:str\x00\xff')
    return result


def ttl_steps() -> list[dict]:
    """Only stable state codes compared exactly; active TTL is time bounded."""
    result: list[dict] = []

    def cmd(who: str, *args: bytes, policy: str = 'strict', duration_ms: int = 0) -> None:
        result.append({'client': who, 'args': args, 'policy': policy,
                       'duration_ms': duration_ms})

    cmd('a', b'TTL', b'ttl:missing')
    cmd('b', b'PTTL', b'ttl:missing')
    cmd('a', b'PERSIST', b'ttl:missing')
    cmd('b', b'EXPIRE', b'ttl:missing', b'5')
    cmd('a', b'PEXPIRE', b'ttl:missing', b'5000')
    cmd('b', b'SET', b'ttl:main', b'v')
    cmd('a', b'TTL', b'ttl:main')
    cmd('b', b'PTTL', b'ttl:main')
    cmd('a', b'PERSIST', b'ttl:main')
    cmd('b', b'EXPIRE', b'ttl:main', b'20')
    cmd('a', b'TTL', b'ttl:main', policy='positive_ttl', duration_ms=20000)
    cmd('b', b'PTTL', b'ttl:main', policy='positive_pttl', duration_ms=20000)
    cmd('a', b'PTTL', b'ttl:main', policy='positive_pttl', duration_ms=20000)
    cmd('b', b'PERSIST', b'ttl:main')
    cmd('a', b'PTTL', b'ttl:main')
    cmd('b', b'PERSIST', b'ttl:main')
    cmd('a', b'SET', b'ttl:main', b'w', b'PX', b'15000')
    cmd('b', b'TTL', b'ttl:main', policy='positive_ttl', duration_ms=15000)
    cmd('a', b'PTTL', b'ttl:main', policy='positive_pttl', duration_ms=15000)
    cmd('b', b'PEXPIRE', b'ttl:main', b'0')
    cmd('a', b'GET', b'ttl:main')
    cmd('b', b'TTL', b'ttl:main')
    cmd('a', b'SET', b'ttl:delete', b'z')
    cmd('b', b'EXPIRE', b'ttl:delete', b'-1')
    cmd('a', b'EXISTS', b'ttl:delete')
    cmd('b', b'PTTL', b'ttl:delete')
    cmd('a', b'SET', b'ttl:short', b'x', b'PX', b'350')
    # The delay is deliberately much larger than 350ms to avoid a boundary read.
    result.append({'wait_ms': 1000})
    cmd('b', b'GET', b'ttl:short')
    cmd('a', b'PTTL', b'ttl:short')
    cmd('b', b'TYPE', b'ttl:short')
    cmd('a', b'SET', b'ttl:invalid', b'original')
    cmd('b', b'SET', b'ttl:invalid', b'new', b'EX', b'0', policy='invalid_set_ttl')
    cmd('a', b'GET', b'ttl:invalid')
    cmd('b', b'SET', b'ttl:invalid', b'new', b'PX', b'-1', policy='invalid_set_ttl')
    cmd('a', b'GET', b'ttl:invalid')
    cmd('b', b'EXPIRE', b'ttl:invalid', b'0')
    cmd('a', b'GET', b'ttl:invalid')
    cmd('b', b'SET', b'ttl:positive', b'v', b'EX', b'10')
    cmd('a', b'PTTL', b'ttl:positive', policy='positive_pttl', duration_ms=10000)
    cmd('b', b'PEXPIRE', b'ttl:positive', b'15000')
    cmd('a', b'PTTL', b'ttl:positive', policy='positive_pttl', duration_ms=15000)
    cmd('b', b'TTL', b'ttl:positive', policy='positive_ttl', duration_ms=15000)
    cmd('a', b'PERSIST', b'ttl:positive')
    cmd('b', b'TTL', b'ttl:positive')
    return result


def serialize_plan(steps: list[dict]) -> list[dict]:
    """Persist every raw request, policy and delay. No unsafe command eligible."""
    if not 1 <= len(steps) <= MAX_STEPS:
        raise ValueError('negative/TTL trace case count exceeded')
    serialized = []
    for step in steps:
        if 'wait_ms' in step:
            if set(step) != {'wait_ms'} or not isinstance(step['wait_ms'], int) or not 0 < step['wait_ms'] <= MAX_WAIT_MS:
                raise ValueError('invalid bounded wait')
            serialized.append({'wait_ms': step['wait_ms']})
            continue
        if step['client'] not in ('a', 'b') or not step['args']:
            raise ValueError('invalid case')
        args = step['args']
        command = args[0].decode('ascii', 'strict').upper()
        if command not in ALLOWED or command == 'QUIT':
            raise ValueError('command outside negative/TTL eligibility')
        if command == 'SET' and (len(args) not in (3, 5) or
                                 (len(args) == 5 and args[3].upper() not in (b'EX', b'PX'))):
            raise ValueError('unsupported SET form')
        if command in ('EXPIRE', 'PEXPIRE') and len(args) != 3:
            raise ValueError('unsupported expiry form')
        if command in ('TTL', 'PTTL', 'PERSIST', 'GET', 'TYPE') and len(args) != 2:
            raise ValueError('unsupported read form')
        if step['policy'] not in ('strict', 'wrongtype', 'positive_ttl', 'positive_pttl', 'invalid_set_ttl'):
            raise ValueError('unknown assertion policy')
        wire = encode_command(args)
        if len(wire) > 4096:
            raise ValueError('request exceeds bounded test input')
        serialized.append({'client': step['client'], 'request_hex': wire.hex(),
                           'policy': step['policy'], 'duration_ms': step.get('duration_ms', 0)})
    return serialized



def read_replay_plan(path: Path, suite: str) -> list[dict]:
    """Parse a persisted failure report without evaluating a new generator."""
    obj = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(obj, dict) or obj.get('format') != 'phase6g-negative-ttl-v1' or obj.get('suite') != suite:
        raise ValueError('replay format or suite mismatch')
    rows = obj.get('trace')
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_STEPS:
        raise ValueError('bad replay trace length')
    steps = []
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError('bad replay row')
        if 'wait_ms' in row:
            if set(row) != {'wait_ms'}:
                raise ValueError('bad replay delay fields')
            steps.append({'wait_ms': row['wait_ms']})
            continue
        if set(row) != {'client', 'request_hex', 'policy', 'duration_ms'} or not isinstance(row['request_hex'], str):
            raise ValueError('bad replay command fields')
        if not isinstance(row['duration_ms'], int) or row['duration_ms'] < 0 or row['duration_ms'] > 30000:
            raise ValueError('bad replay duration')
        args = decode_request(bytes.fromhex(row['request_hex']))
        steps.append({'client': row['client'], 'args': args, 'policy': row['policy'],
                      'duration_ms': row['duration_ms']})
    validated = serialize_plan(steps)
    if validated != rows:
        raise ValueError('replay trace changed during canonical validation')
    return validated


def compare_step(custom_reply, redis_reply, command: bytes, policy: str, *,
                 duration_ms: int = 0, prior: dict | None = None,
                 client_key: bytes | None = None, elapsed_since_lease_ms: float | None = None) -> dict:
    """Typed exact parity for ordinary cases; bounded cross-clock positive TTL."""
    if policy in ('wrongtype', 'invalid_set_ttl'):
        expected_prefix = b'WRONGTYPE ' if policy == 'wrongtype' else b'ERR invalid expire time in \'set\' command'
        if custom_reply.kind != 'error' or redis_reply.kind != 'error':
            raise ParityMismatch(f'expected {policy} error reply from both endpoints')
        if not custom_reply.value.startswith(expected_prefix) or not redis_reply.value.startswith(expected_prefix):
            raise ParityMismatch(f'unexpected {policy} error category')
        if custom_reply.value != redis_reply.value:
            raise ParityMismatch(f'{policy} error wording differs under approved strict policy')
        return {'policy': policy, 'match': True}
    if policy in ('positive_ttl', 'positive_pttl'):
        if custom_reply.kind != 'integer' or redis_reply.kind != 'integer':
            raise ParityMismatch('positive TTL must have RESP integer type')
        cv, rv = custom_reply.value, redis_reply.value
        unit = 1 if policy == 'positive_pttl' else 1000
        maxval = (duration_ms + unit - 1) // unit + (200 if unit == 1 else 1)
        # Stay far from expiry boundaries; server-clock exact equality is invalid.
        if cv <= 0 or rv <= 0:
            raise TimingAmbiguous('positive TTL crossed expiry or returned non-positive state')
        if cv > maxval or rv > maxval:
            raise ParityMismatch('TTL value exceeds known initial duration')
        if elapsed_since_lease_ms is not None:
            if not 0 <= elapsed_since_lease_ms <= 30000:
                raise TimingAmbiguous('TTL sample outside bounded timing window')
            # Use the command-send monotonic timestamp (before either server
            # updates its lease), so this lower bound is deliberately safe.
            slack_ms = 1100 if unit == 1000 else 250
            floor_ms = max(0.0, duration_ms - elapsed_since_lease_ms - slack_ms)
            if cv * unit < floor_ms or rv * unit < floor_ms:
                raise ParityMismatch('TTL fell below elapsed-time mathematical bound')
        difference = abs(cv - rv) * unit
        if difference > 2000:
            raise ParityMismatch('independent-server TTL readings differ by >2s')
        if prior is not None and client_key is not None:
            for name, n in (('custom', cv), ('reference', rv)):
                key = (name, client_key, policy)
                if key in prior and n > prior[key]:
                    raise ParityMismatch(f'{name} TTL increased without renewal')
                prior[key] = n
        return {'policy': policy, 'match': True, 'custom_ttl': cv, 'redis_ttl': rv,
                'allowed_delta_ms': 2000, 'duration_ms': duration_ms,
                'elapsed_since_lease_ms': elapsed_since_lease_ms}
    if comparable(custom_reply, command) != comparable(redis_reply, command):
        raise ParityMismatch('strict typed RESP2 parity mismatch')
    return {'policy': policy, 'match': True}


def _atomic_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    staged = None
    try:
        with tempfile.NamedTemporaryFile('w', dir=path.parent, prefix='.phase6g2a_',
                                         encoding='utf-8', delete=False) as stream:
            staged = Path(stream.name)
            json.dump(data, stream, sort_keys=True, indent=2)
            stream.write('\n')
        os.replace(staged, path)
    finally:
        if staged is not None and staged.exists():
            staged.unlink()


def run_suite(custom: Path, reference: Path, report_path: Path, kind: str, *,
              replay_path: Path | None = None) -> dict:
    if kind not in ('negative', 'ttl'):
        raise ValueError('unknown suite')
    plan = (read_replay_plan(replay_path, kind) if replay_path is not None else
            serialize_plan(negative_steps() if kind == 'negative' else ttl_steps()))
    report = {
        'phase': '6G.2A.1', 'format': 'phase6g-negative-ttl-v1', 'suite': kind,
        'scope': 'CURATED_NATIVE_ONLY_NOT_RANDOM_OR_LINUX',
        'replay_mode': replay_path is not None,
        'reference_version': REFERENCE_VERSION, 'reference_binary_sha256': REFERENCE_SHA256,
        'trace': plan, 'planned_commands': sum('request_hex' in p for p in plan),
        'classification': 'HARNESS_INFRA_FAILURE', 'failure_reason': None,
        'cases': [], 'cleanup': {'custom': {'started': False}, 'reference': {'started': False}},
        'exclusions': [{'form': name, 'reason': why, 'classification': 'EXPECTED_EXCLUSION'}
                       for name, why in EXCLUSIONS],
    }
    children = {'custom': None, 'reference': None}
    cleanup_errors = []
    failure: BaseException | None = None
    clock_history: dict = {}
    lease_epochs: dict[bytes, tuple[int, float]] = {}
    try:
        if not custom.is_file() or not os.access(custom, os.X_OK):
            raise HarnessFailure('custom binary not executable')
        if not reference.is_file() or not os.access(reference, os.X_OK) or _digest(reference) != REFERENCE_SHA256:
            raise HarnessFailure('reference binary SHA mismatch or not executable')
        version = subprocess.run([str(reference), '--version'], capture_output=True,
                                 text=True, timeout=4, check=True).stdout
        if f'v={REFERENCE_VERSION} ' not in version:
            raise HarnessFailure('reference version does not match pinned Redis 8.10.2')
        with tempfile.TemporaryDirectory(prefix='phase6g_2a_owned_') as tmp:
            root = Path(tmp)
            try:
                children['reference'], ref_port = _start_reference(reference, root)
                children['custom'], our_port = _start_custom(custom, root)
                with contextlib.ExitStack() as stack:
                    ca, ra = stack.enter_context(_connect(our_port)), stack.enter_context(_connect(ref_port))
                    cb, rb = stack.enter_context(_connect(our_port)), stack.enter_context(_connect(ref_port))
                    peers = {'a': (ca, RespReader(ca), ra, RespReader(ra)),
                             'b': (cb, RespReader(cb), rb, RespReader(rb))}
                    for i, step in enumerate(plan):
                        if 'wait_ms' in step:
                            time.sleep(step['wait_ms'] / 1000)
                            continue
                        args = decode_request(bytes.fromhex(step['request_hex']))
                        record = {'case': i, 'client': step['client'], 'command': args[0].decode('ascii').upper(),
                                  'request_hex': step['request_hex'], 'policy': step['policy']}
                        report['cases'].append(record)
                        p = peers[step['client']]
                        writer_started_at = time.monotonic()
                        got, raw = _exchange(p[0], p[1], *args)
                        record['custom_reply_hex'] = raw.hex()
                        expected, raw = _exchange(p[2], p[3], *args)
                        record['redis_reply_hex'] = raw.hex()
                        try:
                            observed_elapsed = None
                            if step['policy'] in ('positive_ttl', 'positive_pttl'):
                                lease = lease_epochs.get(args[1])
                                if lease is None or lease[0] != step['duration_ms']:
                                    raise HarnessFailure('positive TTL lacks matching observed lease origin')
                                observed_elapsed = (time.monotonic() - lease[1]) * 1000.0
                            info = compare_step(got, expected, args[0], step['policy'],
                                                duration_ms=step.get('duration_ms', 0),
                                                prior=clock_history, client_key=args[1] if len(args) > 1 else None,
                                                elapsed_since_lease_ms=observed_elapsed)
                            record.update(info)
                        except (ParityMismatch, TimingAmbiguous) as exc:
                            record['match'] = False
                            record['failure_reason'] = str(exc)
                            raise
                        # Record a real monotonic clock origin for each successful lease.
                        # An invalid SET TTL must NOT reset an existing epoch.
                        writer = args[0].upper()
                        if writer in (b'SET', b'EXPIRE', b'PEXPIRE', b'PERSIST') and len(args) > 1 and got.kind != 'error':
                            key = args[1]
                            for old_key in list(clock_history):
                                if old_key[1] == key:
                                    del clock_history[old_key]
                            if writer == b'SET':
                                if len(args) == 5:
                                    multiple = 1000 if args[3].upper() == b'EX' else 1
                                    lease_epochs[key] = (int(args[4]) * multiple, writer_started_at)
                                else:
                                    lease_epochs.pop(key, None)
                            elif writer in (b'EXPIRE', b'PEXPIRE'):
                                if got.kind == 'integer' and got.value == 1 and int(args[2]) > 0:
                                    multiple = 1000 if writer == b'EXPIRE' else 1
                                    lease_epochs[key] = (int(args[2]) * multiple, writer_started_at)
                                elif got.kind == 'integer' and got.value == 1:
                                    lease_epochs.pop(key, None)
                            elif writer == b'PERSIST' and got.kind == 'integer' and got.value == 1:
                                lease_epochs.pop(key, None)
                if children['custom'].poll() is not None or children['reference'].poll() is not None:
                    raise HarnessFailure('owned child unexpectedly exited')
            finally:
                for label, proc in children.items():
                    if proc is None:
                        continue
                    state = {'started': True, 'pid': proc.pid}
                    try:
                        _close_owned(proc)
                    except BaseException as exc:
                        cleanup_errors.append(f'{label}: {type(exc).__name__}: {exc}')
                    state['returncode'] = proc.poll()
                    state['exited'] = state['returncode'] is not None
                    report['cleanup'][label] = state
                    if not state['exited']:
                        cleanup_errors.append(f'{label}: child still running')
    except BaseException as exc:
        failure = exc
    finally:
        if cleanup_errors:
            report['classification'] = 'HARNESS_INFRA_FAILURE'
        elif failure is None:
            report['classification'] = 'PARITY_PASS'
        elif isinstance(failure, ParityMismatch):
            report['classification'] = 'PARITY_MISMATCH'
        elif isinstance(failure, TimingAmbiguous):
            report['classification'] = 'TIMING_AMBIGUOUS'
        elif isinstance(failure, RespProtocolError):
            report['classification'] = 'REFERENCE_OR_PROTOCOL_FAILURE'
        else:
            report['classification'] = 'HARNESS_INFRA_FAILURE'
        report['passed'] = sum(x.get('match') is True for x in report['cases'])
        report['failed'] = sum(x.get('match') is False for x in report['cases'])
        report['failure_reason'] = None if failure is None else f'{type(failure).__name__}: {failure}'
        report['cleanup_errors'] = cleanup_errors
        _atomic_json(report_path, report)
    if failure is not None:
        raise failure
    if cleanup_errors:
        raise HarnessFailure('owned process cleanup failed: ' + '; '.join(cleanup_errors))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description='Owned native 6G.2A negative and TTL differential tests')
    parser.add_argument('--custom', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--suite', choices=('negative', 'ttl'), required=True)
    parser.add_argument('--replay', type=Path, help='re-execute saved exact request bytes and delays')
    args = parser.parse_args()
    try:
        result = run_suite(args.custom, args.reference, args.report, args.suite, replay_path=args.replay)
        print(f'PHASE6G_2A1_{args.suite.upper()}=PASS cases={result["passed"]} exclusions={len(EXCLUSIONS)}')
        print(f'PHASE6G_2A1_EVIDENCE={args.report}')
        return 0
    except (HarnessFailure, OSError, ValueError, TimeoutError,
            subprocess.SubprocessError, RespProtocolError) as exc:
        print(f'PHASE6G_2A1_{args.suite.upper()}=FAIL {type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
