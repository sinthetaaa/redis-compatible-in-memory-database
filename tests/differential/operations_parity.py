"""Limited Stage 6A operational parity against pinned Redis 8.10.2.

Only selected startup settings, RESP2 shapes and stable INFO fields are compared.
Custom counters, version, process details and full INFO compatibility are excluded.
"""
import argparse
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
from native_smoke import (REFERENCE_SHA256, REFERENCE_VERSION, _digest, _start_custom,
                          _start_reference, _close_owned, _connect, _exchange)
from resp2_wire import RespReader


def pairs(value):
    assert value.kind == 'array' and len(value.value) % 2 == 0, value
    assert all(item.kind == 'bulk' for item in value.value), value
    result = dict((value.value[i].value, value.value[i+1].value)
                  for i in range(0, len(value.value), 2))
    assert len(result) * 2 == len(value.value), 'duplicate CONFIG reply names'
    return result


def check_operations(exchange, custom_port, reference_port, *, custom_aof=False):
    """Ten shared checks on already-owned, identity-verified endpoints.

    Linux reuses its persistent candidate and frozen OCI reference. Check the
    deliberate appendonly difference against each startup profile, never compare
    custom counters, journal sequence or logical memory with Redis internals.
    """
    # Shared values are equal except for explicitly checked persistence profiles.
    for pattern in (b'maxmemory', b'maxmemory-policy', b'appendonly', b'timeout',
                    b'unknown-option', b'maxmemory*'):
        cv, rv = exchange(b'CONFIG', b'GET', pattern)
        if pattern.endswith(b'*'):
            assert cv.kind == rv.kind == 'array'
            selected = pairs(cv)
            reference_settings = pairs(rv)
            assert selected == {key: reference_settings[key] for key in selected}
            assert set(selected) == {b'maxmemory', b'maxmemory-policy'}
        else:
            if pattern == b'appendonly':
                assert pairs(cv) == {b'appendonly': b'yes' if custom_aof else b'no'}
                assert pairs(rv) == {b'appendonly': b'no'}
            else:
                assert cv == rv, (pattern, cv, rv)
    for args in ((b'CONFIG', b'GET'), (b'INFO', b'unknown-section')):
        cv, rv = exchange(*args)
        assert cv == rv, (args, cv, rv)
    cv, rv = exchange(b'INFO', b'persistence')
    def fields(value):
        assert value.kind == 'bulk'
        assert value.value.startswith(b'# Persistence\r\n')
        return dict(line.split(b':', 1) for line in value.value.split(b'\r\n')
                    if line and not line.startswith(b'#'))
    assert fields(cv)[b'aof_enabled'] == (b'1' if custom_aof else b'0')
    assert fields(rv)[b'aof_enabled'] == b'0'
    # Port values differ but both use alternating bulk name/value arrays.
    cv, rv = exchange(b'CONFIG', b'GET', b'port')
    assert cv.kind == rv.kind == 'array'
    assert cv.value[0] == rv.value[0]
    assert cv.value[1].value == str(custom_port).encode()
    assert rv.value[1].value == str(reference_port).encode()


def run(custom, reference, appendfsync='off'):
    assert _digest(reference) == REFERENCE_SHA256, 'reference fingerprint mismatch'
    count = 0
    with tempfile.TemporaryDirectory(prefix='redisdb-operational-parity-') as temporary:
        root = Path(temporary)
        candidate = oracle = None
        try:
            candidate, cp = _start_custom(custom.resolve(), root, appendfsync=appendfsync)
            oracle, rp = _start_reference(reference.resolve(), root)
            with _connect(cp) as cs, _connect(rp) as rs:
                cr, rr = RespReader(cs), RespReader(rs)
                def exchange(*args):
                    nonlocal count
                    cv = _exchange(cs, cr, *args)[0]
                    rv = _exchange(rs, rr, *args)[0]
                    count += 1
                    return cv, rv
                check_operations(exchange, cp, rp, custom_aof=appendfsync != 'off')
        finally:
            _close_owned(candidate)
            _close_owned(oracle)
    print(f'PASS {count} limited operational cases against pinned Redis {REFERENCE_VERSION}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--custom', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--appendfsync', choices=('off', 'always'), default='off')
    args = parser.parse_args()
    run(args.custom, args.reference, args.appendfsync)
