"""6G.1B.1 offline (no subprocesses/network) coverage and replay guard tests."""
import json
import tempfile
import unittest
from pathlib import Path

from curated_parity import (
    ALLOWED, _assert_coverage, _pack_trace, _replay_trace,
    curated_trace, decode_request,
)
from resp2_wire import encode_command


class CuratedSuiteTests(unittest.TestCase):
    def test_all_40_routes_and_frozen_matrix(self):
        trace = curated_trace()
        _assert_coverage(trace, Path(__file__).with_name('command_matrix.csv'))
        self.assertEqual({parts[0].decode('ascii').upper() for _, parts in trace}, ALLOWED)
        self.assertEqual(len(ALLOWED), 40)

    def test_two_clients_and_quit_terminal(self):
        trace = curated_trace()
        self.assertEqual({who for who, _ in trace}, {'a', 'b'})
        quits = [(i, who) for i, (who, parts) in enumerate(trace) if parts[0] == b'QUIT']
        self.assertEqual(quits, [(len(trace) - 2, 'a'), (len(trace) - 1, 'b')])

    def test_encoded_trace_binary_roundtrip(self):
        trace = _pack_trace(curated_trace())
        self.assertTrue(any(b'\x00\xff' in decode_request(bytes.fromhex(c['request_hex']))[-1] for c in trace))
        for row in trace:
            raw = bytes.fromhex(row['request_hex'])
            self.assertEqual(encode_command(decode_request(raw)), raw)

    def test_replay_exact_bytes_without_case_generator(self):
        steps = _pack_trace(curated_trace())
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'case.json'
            source.write_text(json.dumps({'format': 'phase6g-curated-v1', 'trace': steps}), encoding='utf-8')
            self.assertEqual(_replay_trace(source), steps)

    def test_replay_refuses_unsupported_command(self):
        with self.assertRaises(ValueError):
            _pack_trace([('a', (b'FLUSHALL',))])

    def test_replay_refuses_reused_quit_socket(self):
        with self.assertRaises(ValueError):
            _pack_trace([('a', (b'QUIT',)), ('a', (b'PING',))])

    def test_replay_rejects_overbudget_and_trailing_bytes(self):
        with self.assertRaises(ValueError):
            decode_request(encode_command((b'ECHO', b'x')) + b'\r\n')
        with self.assertRaises(ValueError):
            _pack_trace([('a', (b'ECHO', b'x' * 4096))])

    def test_replay_rejects_tampered_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'case.json'
            source.write_text(json.dumps({'format': 'phase6g-curated-v1',
                                          'trace': [{'client': 'a', 'request_hex': 'ff'}]}),
                              encoding='utf-8')
            with self.assertRaises(ValueError):
                _replay_trace(source)

    def test_failure_writes_full_replay_trace(self):
        from curated_parity import run
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / 'failure.json'
            planned = _pack_trace([('a', (b'PING',))])
            with self.assertRaises(Exception):
                run(Path(tmp) / 'missing-custom', Path(tmp) / 'missing-reference',
                    report, planned, replay=False)
            saved = json.loads(report.read_text(encoding='utf-8'))
            self.assertEqual(saved['classification'], 'HARNESS_INFRA_FAILURE')
            self.assertEqual(saved['trace'], planned)
            self.assertEqual(saved['passed'], 0)
            self.assertEqual(saved['cleanup']['custom']['started'], False)
            self.assertEqual(saved['cleanup']['reference']['started'], False)

    def test_replay_does_not_depend_on_generator_function(self):
        from unittest.mock import patch
        trace = _pack_trace([('a', (b'ECHO', b'\x00\xff'))])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'source.json'
            path.write_text(json.dumps({'format': 'phase6g-curated-v1', 'trace': trace}), encoding='utf-8')
            with patch('curated_parity.curated_trace', side_effect=AssertionError('generator called')):
                self.assertEqual(_replay_trace(path), trace)

    def test_owned_lifecycle_report_with_mock_transports(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        from curated_parity import run
        from resp2_wire import RespValue

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
            def recv(self, size):
                return b''  # simulate terminal QUIT EOF

        trace = _pack_trace([('a', (b'PING',)), ('a', (b'QUIT',)), ('b', (b'QUIT',))])
        with tempfile.TemporaryDirectory() as root:
            folder = Path(root)
            custom, reference, artifact = (folder / 'custom', folder / 'reference', folder / 'report.json')
            custom.write_bytes(b'x')
            reference.write_bytes(b'x')
            custom.chmod(0o700)
            reference.chmod(0o700)
            custom_proc, ref_proc = Proc(), Proc()
            with patch('curated_parity._digest', return_value='8640ea31c7ece0c823fdf24ea53b8d74fda99d2a129dd69b3cd043cbf6c08176'), \
                 patch('curated_parity.subprocess.run', return_value=SimpleNamespace(stdout='Redis server v=8.10.2 sha=00000000:1')), \
                 patch('curated_parity._start_reference', return_value=(ref_proc, 31001)), \
                 patch('curated_parity._start_custom', return_value=(custom_proc, 31002)), \
                 patch('curated_parity._connect', side_effect=lambda port: Socket()), \
                 patch('curated_parity._exchange', return_value=(RespValue('simple', b'OK'), b'+OK\r\n')), \
                 patch('curated_parity._close_owned', side_effect=lambda proc: setattr(proc, 'alive', False)):
                result = run(custom, reference, artifact, trace, replay=False)
            self.assertEqual(result['classification'], 'PARITY_PASS')
            self.assertEqual(result['passed'], 3)
            self.assertEqual(result['trace'], trace)
            self.assertTrue(result['cleanup']['custom']['exited'])
            self.assertTrue(result['cleanup']['reference']['exited'])
            self.assertTrue(artifact.exists())


if __name__ == '__main__':
    unittest.main()
