"""Phase 6G.1B.1: deterministic 40-route native Redis 8.10.2 parity and raw replay.

Only the existing owned-process helpers are used. No global Redis services,
admin commands or unowned process cleanup are permitted. Optional candidate AOF
files stay inside the owned temporary directory; the reference remains nonpersistent.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from native_smoke import (
    REFERENCE_SHA256, REFERENCE_VERSION, HarnessFailure, _close_owned,
    _connect, _digest, _exchange, _start_custom, _start_reference,
)
from resp2_wire import RespProtocolError, RespReader, comparable, encode_command

ALLOWED = frozenset('''PING ECHO QUIT SET GET MSET MGET APPEND STRLEN INCR DECR INCRBY
DECRBY DEL EXISTS TYPE EXPIRE PEXPIRE TTL PTTL PERSIST LPUSH RPUSH LLEN LPOP
RPOP LRANGE HSET HGET HEXISTS HLEN HDEL HMGET HGETALL SADD SREM SISMEMBER
SCARD SMEMBERS SMISMEMBER'''.split())
MAX_CASES = 512


def curated_trace() -> list[tuple[str, tuple[bytes, ...]]]:
    """Deterministic, sequential two-client trace with all shared normal routes."""
    steps: list[tuple[str, tuple[bytes, ...]]] = []

    def add(client: str, *args: bytes) -> None:
        steps.append((client, args))

    kb = b'curated:binary\x00\xff'
    add('a', b'PING')
    add('b', b'ECHO', b'\x00\xff\r\n')
    add('a', b'sEt', kb, b'hello\x00\xff\r\n')
    add('b', b'GET', kb)
    add('a', b'APPEND', kb, b'!')
    add('b', b'STRLEN', kb)
    add('a', b'MSET', b'curated:m1', b'v1', b'curated:m2', b'v2')
    add('b', b'MGET', b'curated:m2', b'curated:absent', b'curated:m1')
    add('a', b'EXISTS', b'curated:m1', b'curated:m1', b'curated:absent')
    add('b', b'TYPE', kb)
    add('a', b'SET', b'curated:number', b'10')
    add('b', b'INCR', b'curated:number')
    add('a', b'DECR', b'curated:number')
    add('b', b'INCRBY', b'curated:number', b'9')
    add('a', b'DECRBY', b'curated:number', b'3')
    add('b', b'GET', b'curated:number')
    add('a', b'TTL', b'curated:number')  # stable -1
    add('b', b'PTTL', b'curated:absent')  # stable -2
    add('a', b'SET', b'curated:expire', b'v', b'PX', b'600000')
    add('b', b'PERSIST', b'curated:expire')
    add('a', b'PTTL', b'curated:expire')  # stable -1
    add('b', b'EXPIRE', b'curated:expire', b'0')
    add('a', b'EXISTS', b'curated:expire')
    add('b', b'SET', b'curated:expire2', b'v', b'EX', b'600')
    add('a', b'PEXPIRE', b'curated:expire2', b'-1')
    add('b', b'TTL', b'curated:expire2')  # stable -2
    add('a', b'RPUSH', b'curated:list', b'one', b'two')
    add('b', b'LPUSH', b'curated:list', b'zero')
    add('a', b'LLEN', b'curated:list')
    add('b', b'LRANGE', b'curated:list', b'0', b'-1')
    add('a', b'LPOP', b'curated:list')
    add('b', b'RPOP', b'curated:list')
    add('a', b'LRANGE', b'curated:list', b'0', b'-1')
    add('b', b'GET', b'curated:list')  # shared WRONGTYPE
    add('a', b'HSET', b'curated:hash', b'f\x00\xff', b'v\r\n', b'k', b'q')
    add('b', b'HGET', b'curated:hash', b'f\x00\xff')
    add('a', b'HEXISTS', b'curated:hash', b'k')
    add('b', b'HLEN', b'curated:hash')
    add('a', b'HMGET', b'curated:hash', b'absent', b'k', b'f\x00\xff')
    add('b', b'HGETALL', b'curated:hash')
    add('a', b'HDEL', b'curated:hash', b'k', b'k')
    add('b', b'HGETALL', b'curated:hash')
    add('a', b'SADD', b'curated:set', b'a', b'b', b'a', b'\x00\xff')
    add('b', b'SCARD', b'curated:set')
    add('a', b'SISMEMBER', b'curated:set', b'\x00\xff')
    add('b', b'SMISMEMBER', b'curated:set', b'b', b'absent', b'\x00\xff', b'b')
    add('a', b'SMEMBERS', b'curated:set')
    add('b', b'SREM', b'curated:set', b'b', b'b')
    add('a', b'SMEMBERS', b'curated:set')
    add('b', b'MGET', b'curated:set', b'curated:hash', kb)
    add('a', b'DEL', b'curated:m1', b'curated:m1', b'curated:m2')
    add('b', b'EXISTS', b'curated:m1', b'curated:m2')
    add('a', b'QUIT')  # dedicated final command for a; never reuse its socket
    add('b', b'QUIT')  # dedicated final command for b
    return steps


def decode_request(raw: bytes) -> tuple[bytes, ...]:
    """Strict canonical request decoding, then re-encoding, for standalone replay."""
    if len(raw) > 16384 or not raw.startswith(b'*'):
        raise ValueError('replay request must be a bounded RESP2 array')
    pos = 0

    def line() -> bytes:
        nonlocal pos
        end = raw.find(b'\r\n', pos)
        if end < 0 or end - pos > 24:
            raise ValueError('noncanonical replay framing')
        value = raw[pos:end]
        pos = end + 2
        return value

    def positive_int(data: bytes, *, allow_zero: bool) -> int:
        try:
            number = int(data)
        except ValueError as exc:
            raise ValueError('malformed replay length') from exc
        if str(number).encode('ascii') != data or number < (0 if allow_zero else 1):
            raise ValueError('noncanonical replay length')
        return number

    if raw[pos:pos+1] != b'*':
        raise ValueError('not RESP2 array')
    pos += 1
    n = positive_int(line(), allow_zero=False)
    if n > 512:
        raise ValueError('replay arity exceeds cap')
    parts = []
    for _ in range(n):
        if raw[pos:pos+1] != b'$':
            raise ValueError('replay must use bulk arguments')
        pos += 1
        length = positive_int(line(), allow_zero=True)
        if length > 8176 or raw[pos+length:pos+length+2] != b'\r\n':
            raise ValueError('replay bulk over limit / missing CRLF')
        parts.append(raw[pos:pos+length])
        pos += length + 2
    parsed = tuple(parts)
    if pos != len(raw) or encode_command(parsed) != raw:
        raise ValueError('replay has trailing data or noncanonical framing')
    return parsed


def _pack_trace(steps: list[tuple[str, tuple[bytes, ...]]]) -> list[dict[str, str]]:
    if not 1 <= len(steps) <= MAX_CASES:
        raise ValueError('trace case count outside bounds')
    closed = set()
    result = []
    for client, parts in steps:
        if client not in ('a', 'b') or client in closed:
            raise ValueError('trace client already closed or invalid')
        if not parts or parts[0].upper().decode('ascii', 'strict') not in ALLOWED:
            raise ValueError('trace contains an unsupported command')
        wire = encode_command(parts)
        if len(wire) > 4096:
            raise ValueError('trace command exceeds 4096 bytes')
        result.append({'client': client, 'request_hex': wire.hex()})
        if parts[0].upper() == b'QUIT':
            closed.add(client)
    return result


def _replay_trace(path: Path) -> list[dict[str, str]]:
    obj = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(obj, dict) or obj.get('format') != 'phase6g-curated-v1':
        raise ValueError('unsupported replay artifact format')
    rows = obj.get('trace')
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_CASES:
        raise ValueError('invalid replay trace length')
    decoded = []
    for row in rows:
        if not isinstance(row, dict) or set(row) != {'client', 'request_hex'}:
            raise ValueError('invalid replay row shape')
        raw = bytes.fromhex(row['request_hex'])
        decoded.append((row['client'], decode_request(raw)))
    return _pack_trace(decoded)


def _assert_coverage(steps: list[tuple[str, tuple[bytes, ...]]], matrix: Path) -> None:
    with matrix.open(newline='', encoding='utf-8') as stream:
        names = [row['command'] for row in csv.DictReader(stream)]
    if len(names) != 40 or len(set(names)) != 40 or set(names) != ALLOWED:
        raise HarnessFailure('frozen command matrix differs from the 40-command contract')
    missing = ALLOWED - {args[0].decode('ascii').upper() for _, args in steps}
    if missing:
        raise HarnessFailure(f'curated trace missing routes: {sorted(missing)}')


def run(custom: Path, reference: Path, report_path: Path, trace: list[dict[str, str]], *,
        replay: bool, appendfsync: str = 'off') -> dict:
    report = {
        'format': 'phase6g-curated-v1', 'phase': '6G.1B.1',
        'scope': 'CURATED_ONLY_NOT_RANDOM_OR_LINUX',
        'reference_version': REFERENCE_VERSION, 'reference_binary_sha256': REFERENCE_SHA256,
        'trace': trace, 'planned_cases': len(trace), 'cases': [],
        'classification': 'HARNESS_INFRA_FAILURE', 'failure_reason': None,
        'cleanup': {'custom': {'started': False}, 'reference': {'started': False}},
        'replay_mode': replay,
        'candidate_persistence': appendfsync,
    }
    procs = {'custom': None, 'reference': None}
    cleanup_errors = []
    failure = None
    try:
        if not custom.is_file() or not os.access(custom, os.X_OK):
            raise HarnessFailure('custom executable missing / not executable')
        if not reference.is_file() or not os.access(reference, os.X_OK) or _digest(reference) != REFERENCE_SHA256:
            raise HarnessFailure('native reference Redis binary fingerprint mismatch')
        version = subprocess.run([str(reference), '--version'], text=True, capture_output=True,
                                 check=True, timeout=4).stdout
        if f'v={REFERENCE_VERSION} ' not in version:
            raise HarnessFailure('native reference Redis version mismatch')
        with tempfile.TemporaryDirectory(prefix='phase6g_1b_owned_') as tmp:
            root = Path(tmp)
            try:
                procs['reference'], rp = _start_reference(reference, root)
                procs['custom'], cp = _start_custom(custom, root, appendfsync=appendfsync)
                with _connect(cp) as candidate_a, _connect(rp) as reference_a, \
                     _connect(cp) as candidate_b, _connect(rp) as reference_b:
                    sockets = {
                        'a': (candidate_a, RespReader(candidate_a), reference_a, RespReader(reference_a)),
                        'b': (candidate_b, RespReader(candidate_b), reference_b, RespReader(reference_b)),
                    }
                    for index, row in enumerate(trace):
                        client = row['client']
                        args = decode_request(bytes.fromhex(row['request_hex']))
                        pair = sockets[client]
                        record = {'case': index, 'client': client,
                                  'command': args[0].decode('ascii').upper(),
                                  'request_hex': row['request_hex']}
                        report['cases'].append(record)
                        got, got_raw = _exchange(pair[0], pair[1], *args)
                        record['custom_reply_hex'] = got_raw.hex()
                        expected, expected_raw = _exchange(pair[2], pair[3], *args)
                        record['redis_reply_hex'] = expected_raw.hex()
                        record['match'] = comparable(got, args[0]) == comparable(expected, args[0])
                        if not record['match']:
                            raise HarnessFailure(f'PARITY_MISMATCH case={index} command={args[0]!r}')
                        if args[0].upper() == b'QUIT':
                            for endpoint in (pair[0], pair[2]):
                                if endpoint.recv(1) != b'':
                                    raise HarnessFailure('PARITY_MISMATCH: QUIT did not close socket')
                if procs['reference'].poll() is not None or procs['custom'].poll() is not None:
                    raise HarnessFailure('owned server exited unexpectedly')
            finally:
                for label, proc in procs.items():
                    if proc is None:
                        continue
                    state = {'started': True, 'pid': proc.pid}
                    try:
                        _close_owned(proc)
                    except Exception as exc:
                        cleanup_errors.append(f'{label}: {exc}')
                    state['returncode'] = proc.poll()
                    state['exited'] = proc.poll() is not None
                    report['cleanup'][label] = state
                    if not state['exited']:
                        cleanup_errors.append(f'{label}: owned child still running')
    except BaseException as exc:
        failure = exc
    finally:
        if cleanup_errors:
            report['classification'] = 'HARNESS_INFRA_FAILURE'
        elif failure is None:
            report['classification'] = 'PARITY_PASS'
        elif isinstance(failure, HarnessFailure) and str(failure).startswith('PARITY_MISMATCH'):
            report['classification'] = 'PARITY_MISMATCH'
        elif isinstance(failure, RespProtocolError):
            report['classification'] = 'REFERENCE_OR_PROTOCOL_FAILURE'
        else:
            report['classification'] = 'HARNESS_INFRA_FAILURE'
        report['passed'] = sum(case.get('match') is True for case in report['cases'])
        report['failed'] = sum(case.get('match') is False for case in report['cases'])
        report['failure_reason'] = None if failure is None else f'{type(failure).__name__}: {failure}'
        # Preserve the exact raw command bytes in a complete replayable trace on ALL outcomes.
        report_path.parent.mkdir(parents=True, exist_ok=True)
        temp = None
        try:
            with tempfile.NamedTemporaryFile('w', dir=report_path.parent,
                                             prefix='.curated_', encoding='utf-8', delete=False) as stream:
                temp = Path(stream.name)
                json.dump(report, stream, indent=2, sort_keys=True)
                stream.write('\n')
            os.replace(temp, report_path)
        finally:
            if temp is not None and temp.exists():
                temp.unlink()
    if failure is not None:
        raise failure
    if cleanup_errors:
        raise HarnessFailure('owned process cleanup failed: ' + '; '.join(cleanup_errors))
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--custom', required=True, type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--replay', type=Path, help='original JSON result, exact trace bytes; no generator')
    parser.add_argument('--appendfsync', choices=('off', 'always', 'everysec', 'no'), default='off')
    args = parser.parse_args()
    try:
        if args.replay:
            trace = _replay_trace(args.replay)
        else:
            steps = curated_trace()
            _assert_coverage(steps, Path(__file__).with_name('command_matrix.csv'))
            trace = _pack_trace(steps)
        result = run(args.custom, args.reference, args.report, trace, replay=bool(args.replay),
                     appendfsync=args.appendfsync)
        print(f'PHASE6G_1B1_CURATED_NATIVE=PASS cases={result["passed"]} routes=40 replay={bool(args.replay)}')
        print(f'CURATED_EVIDENCE={args.report}')
        return 0
    except (ValueError, TypeError, OSError, TimeoutError, subprocess.SubprocessError,
            HarnessFailure, RespProtocolError, json.JSONDecodeError) as exc:
        print(f'PHASE6G_1B1_CURATED_NATIVE=FAIL {type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
