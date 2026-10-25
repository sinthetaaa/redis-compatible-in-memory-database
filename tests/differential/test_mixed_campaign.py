"""Offline checks for bounded generation, exact replay and failure propagation."""
import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from curated_parity import _pack_trace, _replay_trace
from mixed_campaign import COMMANDS_PER_SEED, ROUTES, SEEDS, main, mixed_trace, run_seed
from native_smoke import HarnessFailure, REFERENCE_SHA256
from resp2_wire import RespValue, encode_command


class MixedCampaignTests(unittest.TestCase):
    def test_fixed_budget_routes_clients_and_reproducibility(self):
        fingerprints = set()
        for seed in SEEDS:
            trace = mixed_trace(seed)
            self.assertEqual(trace, mixed_trace(seed))
            self.assertEqual(len(trace), COMMANDS_PER_SEED)
            self.assertEqual({args[0].decode().upper() for _, args in trace}, set(ROUTES))
            self.assertEqual({client for client, _ in trace}, {'a', 'b'})
            fingerprints.add(hashlib.sha256(json.dumps(_pack_trace(trace)).encode()).hexdigest())
            self.assertTrue(all(len(encode_command(args)) < 4096 for _, args in trace))
        self.assertEqual(len(SEEDS), 10)
        self.assertEqual(len(fingerprints), 10)
        # Changing the byte streams requires an explicit generator-version review.
        combined = json.dumps([_pack_trace(mixed_trace(seed)) for seed in SEEDS], sort_keys=True)
        self.assertEqual(hashlib.sha256(combined.encode()).hexdigest(),
                         '9dd2bbbaac8db96f385a34b8dd5c433ad5a1643d016226ef00c1097efe6b6c74')

    def test_stable_forms_and_final_state_probes(self):
        for seed in SEEDS:
            trace = mixed_trace(seed)
            for _, args in trace:
                command = args[0].upper()
                if command == b'SET':
                    self.assertEqual(len(args), 3)
                if command in (b'EXPIRE', b'PEXPIRE'):
                    self.assertIn(args[2], (b'0', b'-1'))
                if command in (b'INCR', b'DECR', b'INCRBY', b'DECRBY'):
                    self.assertEqual(args[1], b'mix:number')
            self.assertEqual([args[0] for _, args in trace[-32:]],
                             [b'GET', b'LRANGE', b'HGETALL', b'SMEMBERS'] * 8)
            self.assertTrue(any(b'\x00\xff' in arg for _, args in trace for arg in args))

    def test_startup_failure_keeps_full_seed_trace_and_replays_without_generator(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original, replay = root / 'original.json', root / 'replay.json'
            for output, source in ((original, None), (replay, original)):
                with patch('mixed_campaign.mixed_trace', wraps=mixed_trace) as generator:
                    with self.assertRaises(HarnessFailure):
                        run_seed(root / 'missing', root / 'missing-redis', output, 42,
                                 replay_path=source)
                    self.assertEqual(generator.call_count, 0 if source else 1)
                saved = json.loads(output.read_text())
                self.assertEqual(saved['seed'], 42)
                self.assertEqual(saved['classification'], 'HARNESS_INFRA_FAILURE')
                self.assertEqual(len(saved['trace']), 100)
                self.assertFalse(saved['cleanup']['reference']['started'])
                self.assertEqual(_replay_trace(output), _pack_trace(mixed_trace(42)))
            self.assertEqual(json.loads(original.read_text())['trace_sha256'],
                             json.loads(replay.read_text())['trace_sha256'])
            before = original.read_bytes()
            with self.assertRaises(ValueError):
                run_seed(root / 'missing', root / 'missing', original, 0, replay_path=original)
            self.assertEqual(original.read_bytes(), before)

    def test_cli_injected_mismatch_and_timeout_fail_and_clean_owned_children(self):
        class Proc:
            pid = 92345
            alive = True

            def poll(self):
                return None if self.alive else -15

        class Socket:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

        for timeout in (False, True):
            with self.subTest(timeout=timeout), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                custom, reference = root / 'custom', root / 'redis'
                for binary in (custom, reference):
                    binary.write_bytes(b'x')
                    binary.chmod(0o700)
                cp, rp = Proc(), Proc()
                with contextlib.ExitStack() as stack:
                    stack.enter_context(patch('mixed_campaign.platform.platform', return_value='test-platform'))
                    stack.enter_context(patch('sys.argv', ['mixed_campaign.py', '--custom', str(custom),
                        '--reference', str(reference), '--report-dir', str(root / 'out')]))
                    stack.enter_context(patch('curated_parity._digest', return_value=REFERENCE_SHA256))
                    stack.enter_context(patch('curated_parity.subprocess.run', return_value=
                        SimpleNamespace(stdout='Redis server v=8.10.2 sha=00000000:1')))
                    stack.enter_context(patch('curated_parity._start_reference', return_value=(rp, 31001)))
                    stack.enter_context(patch('curated_parity._start_custom', return_value=(cp, 31002)))
                    stack.enter_context(patch('curated_parity._connect', side_effect=lambda port: Socket()))
                    stack.enter_context(patch('curated_parity._exchange',
                        side_effect=TimeoutError('injected socket timeout') if timeout else [
                            (RespValue('simple', b'OK'), b'+OK\r\n'),
                            (RespValue('integer', 99), b':99\r\n')]))
                    stack.enter_context(patch('curated_parity._close_owned',
                        side_effect=lambda proc: setattr(proc, 'alive', False)))
                    stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                    self.assertEqual(main(), 1)
                saved = json.loads((root / 'out' / 'seed-0.json').read_text())
                self.assertEqual(saved['classification'],
                                 'HARNESS_INFRA_FAILURE' if timeout else 'PARITY_MISMATCH')
                self.assertEqual(saved['seed'], 0)
                self.assertEqual(len(saved['trace']), 100)
                self.assertTrue(all(state['exited'] for state in saved['cleanup'].values()))
                if not timeout:
                    self.assertEqual(saved['failed'], 1)
                    self.assertEqual(saved['cases'][0]['redis_reply_hex'], b':99\r\n'.hex())


if __name__ == '__main__':
    unittest.main()
