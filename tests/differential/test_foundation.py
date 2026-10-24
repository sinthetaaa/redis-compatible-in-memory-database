"""Offline deterministic RESP2 foundation tests (no Git or network side effects)."""
import json
import socket
import tempfile
from unittest import mock
from types import SimpleNamespace
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from resp2_wire import RespProtocolError, RespReader, RespValue, comparable, encode_command
import native_smoke


class CodecFoundation(unittest.TestCase):
    def parse(self, payload):
        a, b = socket.socketpair()
        try:
            a.settimeout(2)
            b.settimeout(2)
            b.sendall(payload)
            b.shutdown(socket.SHUT_WR)
            return RespReader(a).read_reply()
        finally:
            a.close()
            b.close()

    def test_binary_encoding(self):
        request = encode_command([b"SET", b"k\x00\xff", b"a\r\nb"])
        self.assertEqual(request, b"*3\r\n$3\r\nSET\r\n$3\r\nk\x00\xff\r\n$4\r\na\r\nb\r\n")

    def test_binary_roundtrip(self):
        value, raw = self.parse(b"$4\r\na\x00\xffb\r\n")
        self.assertEqual(value, RespValue("bulk", b"a\x00\xffb"))
        self.assertEqual(raw, b"$4\r\na\x00\xffb\r\n")

    def test_null_distinct_from_empty(self):
        self.assertNotEqual(self.parse(b"$-1\r\n")[0], self.parse(b"$0\r\n\r\n")[0])

    def test_array_and_error(self):
        self.assertEqual(self.parse(b"*2\r\n:2\r\n-WRONGTYPE sample\r\n")[0],
                         RespValue("array", (RespValue("integer", 2), RespValue("error", b"WRONGTYPE sample"))))

    def test_unordered_only_for_two_commands(self):
        a = self.parse(b"*2\r\n$1\r\nb\r\n$1\r\na\r\n")[0]
        b = self.parse(b"*2\r\n$1\r\na\r\n$1\r\nb\r\n")[0]
        self.assertEqual(comparable(a, b"SMEMBERS"), comparable(b, b"SMEMBERS"))
        self.assertNotEqual(comparable(a, b"MGET"), comparable(b, b"MGET"))
        h1 = self.parse(b"*4\r\n$1\r\nb\r\n$1\r\n2\r\n$1\r\na\r\n$1\r\n1\r\n")[0]
        h2 = self.parse(b"*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n")[0]
        self.assertEqual(comparable(h1, b"HGETALL"), comparable(h2, b"HGETALL"))

    def test_duplicates_do_not_normalize_away(self):
        a = self.parse(b"*2\r\n$1\r\na\r\n$1\r\na\r\n")[0]
        with self.assertRaises(RespProtocolError):
            comparable(a, b"SMEMBERS")
        h = self.parse(b"*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\na\r\n$1\r\n2\r\n")[0]
        with self.assertRaises(RespProtocolError):
            comparable(h, b"HGETALL")

    def test_malformed_rejected(self):
        for payload in (b"$4\r\nx\r\n", b"$-2\r\n", b":01\r\n", b"?x\r\n", b"$1\r\na\n\n", b"*99999\r\n"):
            with self.subTest(payload=payload), self.assertRaises(RespProtocolError):
                self.parse(payload)

    def test_wrong_types_rejected(self):
        with self.assertRaises(TypeError):
            encode_command([b"PING", "not bytes"])
        with self.assertRaises(ValueError):
            encode_command([])
        with self.assertRaises(ValueError):
            encode_command([b"SET", b"x", b"a" * 8177])


class FailureEvidence(unittest.TestCase):
    """Injected local failures: never contact Redis or terminate real processes."""

    class DummyProcess:
        def __init__(self, pid):
            self.pid = pid
            self.alive = True

        def poll(self):
            return None if self.alive else -15

        def terminate(self):
            self.alive = False

        def wait(self, timeout):
            return -15

        def kill(self):
            self.alive = False

    class DummyConnection:
        def __init__(self, label):
            self.label = label

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

    def test_mismatch_writes_replay_and_verifies_cleanup(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            reference, custom = root / "redis-server", root / "redisdb-server"
            for path in (reference, custom):
                path.write_bytes(b"synthetic executable for mocked test")
                path.chmod(0o755)
            report = root / "negative_case.json"
            ref_proc = self.DummyProcess(12301)
            custom_proc = self.DummyProcess(12302)

            def exchange(sock, reader, *parts):
                if sock.label == "custom":
                    return RespValue("integer", 1), b":1\r\n"
                return RespValue("simple", b"PONG"), b"+PONG\r\n"

            def connect(port):
                return self.DummyConnection("custom" if port == 41001 else "reference")

            with (mock.patch.object(native_smoke, "_digest", return_value=native_smoke.REFERENCE_SHA256),
                  mock.patch.object(native_smoke.subprocess, "run",
                                    return_value=SimpleNamespace(stdout="Redis server v=8.10.2 sha=00000000:1")),
                  mock.patch.object(native_smoke, "_start_reference", return_value=(ref_proc, 41002)),
                  mock.patch.object(native_smoke, "_start_custom", return_value=(custom_proc, 41001)),
                  mock.patch.object(native_smoke, "_connect", side_effect=connect),
                  mock.patch.object(native_smoke, "_exchange", side_effect=exchange)):
                with self.assertRaisesRegex(native_smoke.HarnessFailure, "PARITY_MISMATCH"):
                    native_smoke.smoke(custom, reference, report)
            data = json.loads(report.read_text())
            self.assertEqual(data["classification"], "PARITY_MISMATCH")
            self.assertEqual(data["case_count"], 1)
            self.assertEqual(data["passed"], 0)
            self.assertEqual(data["failed"], 1)
            self.assertEqual(data["cases"][0]["request_hex"], encode_command((b"PING",)).hex())
            self.assertEqual(data["cases"][0]["custom_reply_hex"], b":1\r\n".hex())
            self.assertEqual(data["cases"][0]["redis_reply_hex"], b"+PONG\r\n".hex())
            for label in ("reference", "custom"):
                self.assertTrue(data["owned_cleanup"][label]["exited"])
                self.assertEqual(data["owned_cleanup"][label]["returncode"], -15)
            self.assertFalse(ref_proc.alive or custom_proc.alive)

    def test_startup_failure_writes_report_without_starting_other_child(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            reference, custom = root / "redis-server", root / "redisdb-server"
            for path in (reference, custom):
                path.write_bytes(b"synthetic executable for mocked test")
                path.chmod(0o755)
            report = root / "failed_start.json"
            with (mock.patch.object(native_smoke, "_digest", return_value=native_smoke.REFERENCE_SHA256),
                  mock.patch.object(native_smoke.subprocess, "run",
                                    return_value=SimpleNamespace(stdout="Redis server v=8.10.2 sha=00000000:1")),
                  mock.patch.object(native_smoke, "_start_reference",
                                    side_effect=native_smoke.HarnessFailure("simulated Redis startup failure"))):
                with self.assertRaisesRegex(native_smoke.HarnessFailure, "simulated Redis"):
                    native_smoke.smoke(custom, reference, report)
            data = json.loads(report.read_text())
            self.assertEqual(data["classification"], "HARNESS_INFRA_FAILURE")
            self.assertEqual(data["case_count"], 0)
            self.assertIn("startup failure", data["failure_reason"])
            self.assertFalse(data["owned_cleanup"]["reference"]["started"])
            self.assertFalse(data["owned_cleanup"]["custom"]["started"])


if __name__ == "__main__":
    unittest.main()
