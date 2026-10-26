"""Phase 6G.1A bounded native smoke against owned Redis 8.10.2 and redisdb.

Starts only child processes it owns, uses loopback ephemeral ports, and never
sends destructive administrative Redis commands. This is NOT a full parity
campaign; it is a strict foundation smoke that fails closed on mismatch.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from resp2_wire import RespProtocolError, RespReader, RespValue, comparable, encode_command

REFERENCE_SHA256 = "8640ea31c7ece0c823fdf24ea53b8d74fda99d2a129dd69b3cd043cbf6c08176"
REFERENCE_VERSION = "8.10.2"
CUSTOM_READY = re.compile(rb"^RESP_READY 127\.0\.0\.1:(\d+)\r?\n$")
TIMEOUT = 7.0


class HarnessFailure(RuntimeError):
    pass


def _digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _readline_deadline(pipe, deadline: float) -> bytes:
    out = bytearray()
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        ready, _, _ = select.select([pipe], [], [], min(0.1, remaining))
        if not ready:
            continue
        byte = os.read(pipe.fileno(), 1)
        if not byte:
            raise HarnessFailure("child closed readiness pipe")
        out.extend(byte)
        if len(out) > 256:
            raise HarnessFailure("child readiness line exceeds bound")
        if byte == b"\n":
            return bytes(out)
    raise HarnessFailure("child readiness timeout")


def _port_available() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _connect(port: int, *, timeout: float = 2.0) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def _exchange(sock: socket.socket, reader: RespReader, *args: bytes) -> tuple[RespValue, bytes]:
    sock.sendall(encode_command(list(args)))
    return reader.read_reply()


def _close_owned(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()  # handle belongs to this harness invocation
        proc.wait(timeout=3)


def _start_custom(binary: Path, log_dir: Path, *, appendfsync: str = 'off') -> tuple[subprocess.Popen, int]:
    if appendfsync not in ('off', 'always', 'everysec', 'no'):
        raise HarnessFailure('invalid custom persistence policy')
    options = [] if appendfsync == 'off' else [
        '--appendonly', 'yes', '--aof-file', str(log_dir / 'custom.aof'),
        '--appendfsync', appendfsync]
    stderr = (log_dir / "custom-stderr.txt").open("wb")
    try:
        proc = subprocess.Popen([str(binary), "--resp2-server", "--port", "0", "--max-clients", "16", *options],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stderr,
                                close_fds=True)
    finally:
        stderr.close()
    try:
        line = _readline_deadline(proc.stdout, time.monotonic() + TIMEOUT)
        match = CUSTOM_READY.fullmatch(line)
        if not match or proc.poll() is not None:
            raise HarnessFailure(f"custom server readiness invalid: {line[:200]!r}")
        port = int(match.group(1))
        if not 1 <= port <= 65535:
            raise HarnessFailure("custom server invalid ephemeral port")
        with _connect(port) as connection:
            reply, _ = _exchange(connection, RespReader(connection), b"PING")
            if reply != RespValue("simple", b"PONG"):
                raise HarnessFailure("custom process readiness PING mismatch")
        return proc, port
    except BaseException:
        _close_owned(proc)
        raise


def _start_reference(binary: Path, work_dir: Path) -> tuple[subprocess.Popen, int]:
    port = _port_available()
    logfile = work_dir / "redis.log"
    args = [str(binary), "--bind", "127.0.0.1", "--port", str(port),
            "--protected-mode", "yes", "--save", "", "--appendonly", "no",
            "--daemonize", "no", "--dir", str(work_dir), "--pidfile", str(work_dir / "redis.pid"),
            "--logfile", str(logfile), "--maxclients", "32"]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True)
    try:
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise HarnessFailure("owned reference Redis exited before readiness")
            try:
                with _connect(port, timeout=0.25) as sock:
                    reader = RespReader(sock)
                    reply, _ = _exchange(sock, reader, b"PING")
                    if reply != RespValue("simple", b"PONG"):
                        raise HarnessFailure("reference PING mismatch")
                    info, _ = _exchange(sock, reader, b"INFO", b"SERVER")
                    if info.kind != "bulk":
                        raise HarnessFailure("reference INFO SERVER must be bulk")
                    fields = dict(line.split(b":", 1) for line in info.value.splitlines() if b":" in line)
                    if fields.get(b"process_id") != str(proc.pid).encode():
                        raise HarnessFailure("reference TCP port not owned by child process")
                    if fields.get(b"redis_version") != REFERENCE_VERSION.encode():
                        raise HarnessFailure("reference INFO redis_version mismatch")
                    return proc, port
            except (OSError, TimeoutError):
                time.sleep(0.05)
        raise HarnessFailure("reference Redis startup timeout")
    except BaseException:
        _close_owned(proc)
        raise


def smoke(custom: Path, reference: Path, out_path: Path) -> None:
    if not custom.is_file() or not os.access(custom, os.X_OK):
        raise HarnessFailure("custom executable missing or not executable")
    if not reference.is_file() or not os.access(reference, os.X_OK):
        raise HarnessFailure("approved Redis executable missing or not executable")
    if _digest(reference) != REFERENCE_SHA256:
        raise HarnessFailure("reference Redis binary SHA256 does not match frozen contract")
    version = subprocess.run([str(reference), "--version"], capture_output=True,
                             text=True, timeout=3, check=True).stdout.strip()
    if f"v={REFERENCE_VERSION} " not in version:
        raise HarnessFailure("reference Redis server version mismatch")

    # Deliberately no global FLUSHDB; both servers are fresh ephemeral processes.
    trace = [
        (b"PING",),
        (b"ECHO", b"a\x00\xff\r\nb"),
        (b"SET", b"phase6g:bin\x00\xff", b"a\x00\xff\r\nb"),
        (b"GET", b"phase6g:bin\x00\xff"),
        (b"MSET", b"phase6g:m1", b"1", b"phase6g:m2", b"2"),
        (b"MGET", b"phase6g:m2", b"phase6g:missing", b"phase6g:m1"),
        (b"EXISTS", b"phase6g:m1", b"phase6g:m1", b"phase6g:missing"),
        (b"RPUSH", b"phase6g:list", b"a", b"b", b"c"),
        (b"LRANGE", b"phase6g:list", b"0", b"-1"),
        (b"GET", b"phase6g:list"),  # eligible WRONGTYPE category/bytes
        (b"HSET", b"phase6g:hash", b"x", b"1", b"y", b"2"),
        (b"HGETALL", b"phase6g:hash"),
        (b"SADD", b"phase6g:set", b"red", b"blue", b"red"),
        (b"SMEMBERS", b"phase6g:set"),
        (b"TYPE", b"phase6g:set"),
        (b"DEL", b"phase6g:m1", b"phase6g:m1"),
    ]
    if any(len(encode_command(item)) > 4096 for item in trace):
        raise HarnessFailure("smoke command budget exceeded")

    outcomes = []
    owned_ref = owned_custom = None
    owned_cleanup = {"custom": {"started": False}, "reference": {"started": False}}
    cleanup_errors = []
    failure = None
    try:
        with tempfile.TemporaryDirectory(prefix="phase6g_1a_owned_") as tmp:
            root = Path(tmp)
            try:
                owned_ref, ref_port = _start_reference(reference, root)
                owned_custom, custom_port = _start_custom(custom, root)
                with _connect(custom_port) as candidate, _connect(ref_port) as oracle:
                    cand_reader, oracle_reader = RespReader(candidate), RespReader(oracle)
                    for index, item in enumerate(trace):
                        record = {"case": index, "command": item[0].decode("ascii"),
                                  "request_hex": encode_command(item).hex()}
                        outcomes.append(record)  # Preserve the attempted request even on timeout.
                        got, got_raw = _exchange(candidate, cand_reader, *item)
                        record["custom_reply_hex"] = got_raw.hex()
                        expected, expected_raw = _exchange(oracle, oracle_reader, *item)
                        record["redis_reply_hex"] = expected_raw.hex()
                        ok = comparable(got, item[0]) == comparable(expected, item[0])
                        record["match"] = ok
                        if not ok:
                            raise HarnessFailure(f"PARITY_MISMATCH at case={index} command={item[0]!r}")
                if owned_ref.poll() is not None or owned_custom.poll() is not None:
                    raise HarnessFailure("owned server terminated unexpectedly")
            finally:
                # Always attempt both cleanups; a failure for one child must not
                # prevent terminating the other owned child.
                for label, proc in (("custom", owned_custom), ("reference", owned_ref)):
                    if proc is None:
                        continue
                    state = {"started": True, "pid": proc.pid}
                    try:
                        _close_owned(proc)
                    except Exception as exc:
                        cleanup_errors.append(f"{label}: {type(exc).__name__}: {exc}")
                    state["returncode"] = proc.poll()
                    state["exited"] = state["returncode"] is not None
                    if not state["exited"]:
                        cleanup_errors.append(f"{label}: owned child remains alive")
                    owned_cleanup[label] = state
                if cleanup_errors:
                    raise HarnessFailure("owned resource cleanup incomplete: " + "; ".join(cleanup_errors))
    except BaseException as exc:
        failure = exc
    finally:
        if cleanup_errors:
            classification = "HARNESS_INFRA_FAILURE"
        elif failure is None:
            classification = "PARITY_PASS"
        elif isinstance(failure, HarnessFailure) and str(failure).startswith("PARITY_MISMATCH"):
            classification = "PARITY_MISMATCH"
        elif isinstance(failure, RespProtocolError):
            classification = "REFERENCE_OR_PROTOCOL_FAILURE"
        else:
            classification = "HARNESS_INFRA_FAILURE"
        report = {"phase": "6G.1A", "reference_version": REFERENCE_VERSION,
                  "reference_binary_sha256": REFERENCE_SHA256, "case_count": len(outcomes),
                  "planned_cases": len(trace),
                  "passed": sum(item.get("match") is True for item in outcomes),
                  "failed": sum(item.get("match") is False for item in outcomes),
                  "classification": classification,
                  "scope": "SMOKE_ONLY_NOT_40_COMMAND_CAMPAIGN", "cases": outcomes,
                  "owned_cleanup": owned_cleanup,
                  "cleanup_errors": cleanup_errors,
                  "failure_reason": None if failure is None else f"{type(failure).__name__}: {failure}"}
        # Atomically preserve diagnostic evidence whether the run passes or fails.
        out_path.parent.mkdir(parents=True, exist_ok=True)
        staging = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=out_path.parent,
                                             prefix=".phase6g_1a_report_", delete=False) as stream:
                staging = Path(stream.name)
                json.dump(report, stream, indent=2, sort_keys=True)
                stream.write("\n")
            os.replace(staging, out_path)
        finally:
            if staging is not None and staging.exists():
                staging.unlink()
    if failure is not None:
        raise failure
    print(f"PHASE6G_1A_OWNED_NATIVE_SMOKE=PASS cases={len(outcomes)}")
    print(f"SMOKE_EVIDENCE={out_path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--custom", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        smoke(arguments.custom, arguments.reference, arguments.report)
        return 0
    except (OSError, ValueError, TimeoutError, HarnessFailure, RespProtocolError) as exc:
        print(f"PHASE6G_1A_OWNED_NATIVE_SMOKE=FAIL {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
