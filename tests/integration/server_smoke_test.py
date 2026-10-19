#!/usr/bin/env python3
"""Cross-platform external-process tests for transport-only smoke mode."""

import contextlib
import re
import select
import signal
import socket
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor


if len(sys.argv) != 2:
    raise SystemExit("Usage: server_smoke_test.py SERVER_EXECUTABLE")

EXECUTABLE = sys.argv[1]
HOST = "127.0.0.1"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def receive_exact(client, count):
    result = bytearray()

    while len(result) < count:
        data = client.recv(count - len(result))

        if not data:
            raise AssertionError(
                "Peer closed before all expected bytes arrived"
            )

        result.extend(data)

    return bytes(result)


def connect(port):
    client = socket.create_connection(
        (HOST, port),
        timeout=5.0,
    )
    client.settimeout(5.0)
    return client


def exchange(client, payload):
    client.sendall(payload)
    actual = receive_exact(client, len(payload))

    check(
        actual == payload,
        "Binary-safe echo mismatch",
    )


@contextlib.contextmanager
def running_server(max_clients=16):
    process = subprocess.Popen(
        [
            EXECUTABLE,
            "--smoke-echo",
            "--port",
            "0",
            "--max-clients",
            str(max_clients),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    try:
        readable, _, _ = select.select(
            [process.stdout],
            [],
            [],
            5.0,
        )

        check(bool(readable), "Server startup timed out")

        line = process.stdout.readline()

        match = re.fullmatch(
            r"SMOKE_READY 127[.]0[.]0[.]1:([0-9]+)\n",
            line,
        )

        check(
            match is not None,
            f"Unexpected startup output: {line!r}",
        )

        port = int(match.group(1))

        check(
            1 <= port <= 65535,
            "Invalid listening port",
        )

        check(
            process.poll() is None,
            "Server exited during startup",
        )

        yield process, port

    finally:
        if process.poll() is None:
            process.terminate()

        try:
            process.communicate(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5.0)


def test_invalid_cli():
    invalid = [
        [],
        ["--smoke-echo", "--port"],
        ["--smoke-echo", "--port", "abc"],
        ["--smoke-echo", "--port", "-1"],
        ["--smoke-echo", "--port", "65536"],
        ["--smoke-echo", "--max-clients", "0"],
        ["--smoke-echo", "--max-clients", "-1"],
        ["--smoke-echo", "--unknown", "1"],
        ["--smoke-echo", "--port", "0", "--port", "0"],
        [
            "--smoke-echo",
            "--max-clients",
            "2",
            "--max-clients",
            "3",
        ],
    ]

    for arguments in invalid:
        result = subprocess.run(
            [EXECUTABLE, *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=4.0,
        )

        check(
            result.returncode == 2,
            f"CLI did not reject {arguments!r}",
        )

    print("PASS: Invalid CLI arguments rejected")


def test_concurrent_clients():
    with running_server(max_clients=16) as (process, port):
        with contextlib.ExitStack() as stack:
            clients = [
                stack.enter_context(connect(port))
                for _ in range(8)
            ]

            payloads = [
                b"\x00\xff\r\n"
                + bytes([index]) * (4096 + index * 779)
                + bytes([255 - index])
                for index in range(len(clients))
            ]

            def worker(item):
                client, payload = item

                exchange(client, payload)

                second = b"SECOND\x00" + payload[:137]

                exchange(client, second)

            with ThreadPoolExecutor(
                max_workers=len(clients)
            ) as executor:
                results = [
                    executor.submit(worker, item)
                    for item in zip(clients, payloads)
                ]

                for result in results:
                    result.result(timeout=10.0)

            check(
                process.poll() is None,
                "Concurrent clients terminated the server",
            )

    print("PASS: Eight concurrent binary-safe clients")
    print("PASS: Multiple exchanges per connection")


def test_half_close_and_isolation():
    with running_server(max_clients=4) as (process, port):
        with connect(port) as first, connect(port) as second:
            payload = b"half-close\x00\xff" * 512

            first.sendall(payload)
            first.shutdown(socket.SHUT_WR)

            check(
                receive_exact(first, len(payload)) == payload,
                "Half-closed client lost pending response",
            )

            # Peer should eventually close after its
            # queued output has been sent.
            check(
                first.recv(1) == b"",
                "Half-closed connection was not cleaned up",
            )

            exchange(
                second,
                b"healthy client remains available\x00",
            )

            check(
                process.poll() is None,
                "Half-close terminated the server",
            )

    print("PASS: Half-close output drained")
    print("PASS: Other clients remain healthy")


def test_connection_limit_and_recovery():
    with running_server(max_clients=2) as (process, port):
        with contextlib.ExitStack() as stack:
            first = stack.enter_context(connect(port))
            second = stack.enter_context(connect(port))

            exchange(first, b"first admitted")
            exchange(second, b"second admitted")

            # TCP connect may succeed while the
            # application has stopped accepting:
            # the connection sits in the kernel backlog.
            third = stack.enter_context(connect(port))
            third.sendall(b"waiting client")

            third.settimeout(0.3)

            try:
                early = third.recv(1)
            except socket.timeout:
                early = None

            check(
                early is None,
                "Third client was serviced above admission limit",
            )

            first.close()

            third.settimeout(5.0)

            check(
                receive_exact(
                    third,
                    len(b"waiting client"),
                ) == b"waiting client",
                "Waiting client not admitted after capacity freed",
            )

            exchange(second, b"second still works")
            exchange(third, b"third now active")

            check(
                process.poll() is None,
                "Admission recovery terminated the server",
            )

    print("PASS: Admission limit enforced")
    print("PASS: Waiting client admitted after disconnect")
    print("PASS: Existing clients remain isolated")


def test_duplicate_port():
    with running_server() as (process, port):
        second = subprocess.run(
            [
                EXECUTABLE,
                "--smoke-echo",
                "--port",
                str(port),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=4.0,
        )

        check(
            second.returncode == 1,
            "Duplicate listening port was not rejected",
        )

        with connect(port) as client:
            exchange(client, b"original listener healthy")

        check(
            process.poll() is None,
            "Binding collision affected original server",
        )

    print("PASS: Duplicate port rejected")
    print("PASS: Original listener remains healthy")


def test_signal_shutdown(requested_signal):
    with running_server() as (process, port):
        with connect(port) as client:
            exchange(client, b"shutdown-test\x00")

        process.send_signal(requested_signal)

        stdout, stderr = process.communicate(
            timeout=5.0
        )

        check(
            process.returncode == 0,
            f"Signal shutdown returned {process.returncode}",
        )

        check(
            stdout == "SMOKE_STOPPED\n",
            f"Unexpected shutdown output: {stdout!r}",
        )

        check(
            stderr == "",
            f"Unexpected shutdown error: {stderr!r}",
        )

    print(
        f"PASS: Clean shutdown using signal "
        f"{requested_signal}"
    )


def main():
    test_invalid_cli()
    test_concurrent_clients()
    test_half_close_and_isolation()
    test_connection_limit_and_recovery()
    test_duplicate_port()
    test_signal_shutdown(signal.SIGTERM)
    test_signal_shutdown(signal.SIGINT)

    print("PASS: All external-process smoke tests")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(
            f"FAIL: {type(error).__name__}: {error}",
            file=sys.stderr,
        )
        raise
