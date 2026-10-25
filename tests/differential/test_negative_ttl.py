"""Pure offline acceptance tests for Phase 6G.2A.1 wrongtype and TTL policies."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

from negative_ttl import (
    EXCLUSIONS, ParityMismatch, TimingAmbiguous, compare_step,
    negative_steps, ttl_steps, serialize_plan, read_replay_plan, run_suite,
)
from curated_parity import decode_request
from resp2_wire import RespValue, encode_command


def val(kind, value):
    return RespValue(kind, value)


class NegativeTTLTests(unittest.TestCase):
    def test_wrongtype_trace_has_state_probes_and_binary_keys(self):
        steps = negative_steps()
        self.assertGreaterEqual(sum(x['policy'] == 'wrongtype' for x in steps), 12)
        self.assertTrue(any(b'\x00\xff' in arg for x in steps for arg in x['args']))
        commands = {x['args'][0] for x in steps}
        for name in (b'GET', b'APPEND', b'INCR', b'HGET', b'RPUSH', b'SADD', b'LRANGE', b'MGET'):
            self.assertIn(name, commands)

    def test_ttl_suite_positive_stable_expired_and_persist(self):
        steps = ttl_steps()
        commands = [x['args'][0] for x in steps if 'args' in x]
        for name in (b'TTL', b'PTTL', b'EXPIRE', b'PEXPIRE', b'PERSIST', b'SET', b'GET', b'TYPE'):
            self.assertIn(name, commands)
        self.assertIn({'wait_ms': 1000}, steps)
        self.assertGreaterEqual(sum(x.get('policy', '').startswith('positive_') for x in steps), 6)

    def test_plan_stores_raw_binary_requests_and_ignores_waits(self):
        plan = serialize_plan(negative_steps())
        self.assertEqual(len(plan), len(negative_steps()))
        for step in plan:
            raw = bytes.fromhex(step['request_hex'])
            self.assertEqual(encode_command(decode_request(raw)), raw)
        self.assertEqual(sum('wait_ms' in x for x in serialize_plan(ttl_steps())), 1)

    def test_saved_trace_replay_is_exact_and_generator_free(self):
        for suite, steps in (('negative', negative_steps()), ('ttl', ttl_steps())):
            original = serialize_plan(steps)
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'report.json'
                path.write_text(json.dumps({'format': 'phase6g-negative-ttl-v1',
                                            'suite': suite, 'trace': original}))
                with patch('negative_ttl.negative_steps', side_effect=AssertionError('generator called')), \
                     patch('negative_ttl.ttl_steps', side_effect=AssertionError('generator called')):
                    self.assertEqual(read_replay_plan(path, suite), original)

    def test_replay_rejects_tampered_unsupported_set(self):
        original = serialize_plan(ttl_steps())
        bad = list(original)
        for i, row in enumerate(bad):
            if 'request_hex' in row:
                bad[i] = dict(row, request_hex=encode_command((b'SET', b'k', b'v', b'NX')).hex())
                break
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'report.json'
            path.write_text(json.dumps({'format': 'phase6g-negative-ttl-v1',
                                        'suite': 'ttl', 'trace': bad}))
            with self.assertRaises(ValueError):
                read_replay_plan(path, 'ttl')

    def test_reject_unsafe_admin_and_unapproved_forms(self):
        with self.assertRaises(ValueError):
            serialize_plan([{'client': 'a', 'args': (b'FLUSHALL',), 'policy': 'strict'}])
        with self.assertRaises(ValueError):
            serialize_plan([{'client': 'a', 'args': (b'QUIT',), 'policy': 'strict'}])
        with self.assertRaises(ValueError):
            serialize_plan([{'client': 'a', 'args': (b'GET', b'k'), 'policy': 'skip'}])
        with self.assertRaises(ValueError):
            serialize_plan([{'wait_ms': 10000}])

    def test_wrongtype_typed_and_exact_error(self):
        w = val('error', b'WRONGTYPE Operation against a key holding the wrong kind of value')
        self.assertTrue(compare_step(w, w, b'GET', 'wrongtype')['match'])
        with self.assertRaises(ParityMismatch):
            compare_step(val('simple', b'OK'), val('simple', b'OK'), b'GET', 'wrongtype')
        with self.assertRaises(ParityMismatch):
            compare_step(val('error', b'ERR wrong type'), w, b'GET', 'wrongtype')

    def test_invalid_set_ttl_category_and_no_false_success(self):
        e = val('error', b"ERR invalid expire time in 'set' command")
        self.assertTrue(compare_step(e, e, b'SET', 'invalid_set_ttl')['match'])
        with self.assertRaises(ParityMismatch):
            compare_step(val('error', b'ERR unknown'), e, b'SET', 'invalid_set_ttl')

    def test_positive_pttl_nearby_but_not_identical(self):
        x = compare_step(val('integer', 19892), val('integer', 19904), b'PTTL',
                         'positive_pttl', duration_ms=20000, prior={}, client_key=b'k')
        self.assertTrue(x['match'])
        self.assertEqual(x['allowed_delta_ms'], 2000)

    def test_positive_ttl_rounding_tolerated(self):
        self.assertTrue(compare_step(val('integer', 19), val('integer', 20), b'TTL',
                                     'positive_ttl', duration_ms=20000)['match'])
        with self.assertRaises(ParityMismatch):
            compare_step(val('integer', 20), val('integer', 30), b'TTL',
                         'positive_ttl', duration_ms=20000)

    def test_positive_ttl_straddled_expiry_blocks_acceptance(self):
        with self.assertRaises(TimingAmbiguous):
            compare_step(val('integer', -2), val('integer', 100), b'PTTL',
                         'positive_pttl', duration_ms=10000)

    def test_positive_ttl_requires_math_bound_when_clock_origin_known(self):
        with self.assertRaises(ParityMismatch):
            compare_step(val('integer', 1000), val('integer', 1001), b'PTTL',
                         'positive_pttl', duration_ms=20000, elapsed_since_lease_ms=100.0)
        self.assertTrue(compare_step(val('integer', 19800), val('integer', 19799), b'PTTL',
                                     'positive_pttl', duration_ms=20000,
                                     elapsed_since_lease_ms=110.0)['match'])

    def test_positive_ttl_nonincrease_for_each_server(self):
        history = {}
        compare_step(val('integer', 2000), val('integer', 1990), b'PTTL',
                     'positive_pttl', duration_ms=5000, prior=history, client_key=b'k')
        compare_step(val('integer', 1999), val('integer', 1980), b'PTTL',
                     'positive_pttl', duration_ms=5000, prior=history, client_key=b'k')
        with self.assertRaises(ParityMismatch):
            compare_step(val('integer', 2001), val('integer', 1970), b'PTTL',
                         'positive_pttl', duration_ms=5000, prior=history, client_key=b'k')

    def test_exclusion_catalog_is_separate_and_non_destructive(self):
        forms = [name for name, reason in EXCLUSIONS]
        self.assertEqual(len(forms), len(set(forms)))
        self.assertIn('FLUSHDB', forms)
        self.assertTrue(all(reason for _, reason in EXCLUSIONS))
        requests = [decode_request(bytes.fromhex(x['request_hex']))[0]
                    for x in serialize_plan(negative_steps())]
        self.assertNotIn(b'FLUSHDB', requests)

    def test_startup_failure_reports_full_trace_and_unowned_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = root / 'failure.json'
            with self.assertRaises(Exception):
                run_suite(root / 'no-server', root / 'no-redis', report, 'negative')
            result = json.loads(report.read_text())
            self.assertEqual(result['classification'], 'HARNESS_INFRA_FAILURE')
            self.assertEqual(result['passed'], 0)
            self.assertEqual(result['cleanup']['custom'], {'started': False})
            self.assertEqual(result['cleanup']['reference'], {'started': False})
            self.assertGreater(result['planned_commands'], 30)
            self.assertEqual(len(result['trace']), result['planned_commands'])

    def test_mock_mismatch_preserves_request_and_owned_exit(self):
        class Proc:
            pid = 99841
            alive = True
            def poll(self):
                return None if self.alive else -15

        class Sock:
            def __enter__(self):
                return self
            def __exit__(self, *a):
                return False

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            server, redis, path = root / 'srv', root / 'redis', root / 'report.json'
            for p in (server, redis):
                p.write_bytes(b'x')
                p.chmod(0o755)
            cp, rp = Proc(), Proc()
            with patch('negative_ttl._digest', return_value='8640ea31c7ece0c823fdf24ea53b8d74fda99d2a129dd69b3cd043cbf6c08176'), \
                 patch('negative_ttl.subprocess.run', return_value=SimpleNamespace(stdout='Redis server v=8.10.2 sha=00000000:1')), \
                 patch('negative_ttl._start_reference', return_value=(rp, 33331)), \
                 patch('negative_ttl._start_custom', return_value=(cp, 33332)), \
                 patch('negative_ttl._connect', side_effect=lambda _: Sock()), \
                 patch('negative_ttl._exchange', side_effect=[(val('integer', 4), b':4\r\n'),
                                                              (val('integer', 9), b':9\r\n')]), \
                 patch('negative_ttl._close_owned', side_effect=lambda p: setattr(p, 'alive', False)):
                with self.assertRaises(ParityMismatch):
                    run_suite(server, redis, path, 'negative')
            result = json.loads(path.read_text())
            self.assertEqual(result['classification'], 'PARITY_MISMATCH')
            self.assertEqual(result['failed'], 1)
            self.assertTrue(result['cleanup']['reference']['exited'])
            self.assertTrue(result['cleanup']['custom']['exited'])
            self.assertTrue(result['cases'][0]['request_hex'])
            self.assertEqual(result['cases'][0]['custom_reply_hex'], b':4\r\n'.hex())


if __name__ == '__main__':
    unittest.main()
