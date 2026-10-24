"""Bounded, binary-preserving RESP2 wire codec for Phase 6G differential tests.

No redis-py dependency; never decode payloads as UTF-8. No network side effects
on import. Protocol failures are distinguished from parity differences.
"""
from __future__ import annotations

import socket
from dataclasses import dataclass
from typing import Any, Tuple


class RespProtocolError(Exception):
    """Malformed RESP2 or a locally imposed resource bound was encountered."""


@dataclass(frozen=True)
class RespValue:
    kind: str
    value: Any


MAX_WIRE_REPLY = 16384
MAX_LINE = 128
MAX_ARRAY_ITEMS = 2048
MAX_BULK = 8192
MAX_DEPTH = 12


def encode_command(parts: tuple[bytes, ...] | list[bytes]) -> bytes:
    if not 1 <= len(parts) <= 512:
        raise ValueError("command must have 1..512 bulk-string elements")
    result = [b"*" + str(len(parts)).encode("ascii") + b"\r\n"]
    total = len(result[0])
    for part in parts:
        if not isinstance(part, bytes):
            raise TypeError("RESP2 command arguments must be bytes")
        if len(part) > 8176:
            raise ValueError("bulk command argument too large for bounded harness")
        fragment = b"$" + str(len(part)).encode("ascii") + b"\r\n" + part + b"\r\n"
        total += len(fragment)
        if total > MAX_WIRE_REPLY:
            raise ValueError("encoded request exceeds harness limit")
        result.append(fragment)
    return b"".join(result)


class RespReader:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.raw = bytearray()
        self._buffer = bytearray()

    def read_reply(self) -> tuple[RespValue, bytes]:
        # Use one reader per connection to retain any overread bytes, if added later.
        self.raw.clear()
        value = self._parse(0)
        return value, bytes(self.raw)

    def _recv(self, count: int) -> bytes:
        if count < 0 or count > MAX_WIRE_REPLY - len(self.raw):
            raise RespProtocolError("RESP reply exceeds maximum wire size")
        while len(self._buffer) < count:
            chunk = self.sock.recv(min(4096, MAX_WIRE_REPLY - len(self.raw) - len(self._buffer)))
            if not chunk:
                raise RespProtocolError("unexpected EOF in RESP reply")
            self._buffer.extend(chunk)
            if len(self.raw) + len(self._buffer) > MAX_WIRE_REPLY:
                raise RespProtocolError("RESP reply exceeds maximum wire size")
        part = bytes(self._buffer[:count])
        del self._buffer[:count]
        self.raw.extend(part)
        return part

    def _line(self) -> bytes:
        line = bytearray()
        while True:
            if len(line) > MAX_LINE:
                raise RespProtocolError("RESP line too long")
            byte = self._recv(1)
            if byte == b"\r":
                if self._recv(1) != b"\n":
                    raise RespProtocolError("invalid RESP CRLF")
                return bytes(line)
            line.extend(byte)

    @staticmethod
    def _length(line: bytes) -> int:
        if not line or len(line) > 20:
            raise RespProtocolError("malformed RESP numeric field")
        try:
            value = int(line)
        except ValueError as exc:
            raise RespProtocolError("malformed RESP numeric field") from exc
        if str(value).encode("ascii") != line:
            raise RespProtocolError("noncanonical RESP numeric field")
        if value < -(1 << 63) or value > (1 << 63) - 1:
            raise RespProtocolError("RESP integer overflow")
        return value

    def _parse(self, depth: int) -> RespValue:
        if depth > MAX_DEPTH:
            raise RespProtocolError("maximum RESP nesting exceeded")
        kind = self._recv(1)
        if kind in (b"+", b"-"):
            line = self._line()
            return RespValue("simple" if kind == b"+" else "error", line)
        if kind == b":":
            return RespValue("integer", self._length(self._line()))
        if kind == b"$":
            length = self._length(self._line())
            if length == -1:
                return RespValue("null_bulk", None)
            if length < 0 or length > MAX_BULK:
                raise RespProtocolError("invalid or excessive RESP bulk length")
            value = self._recv(length)
            if self._recv(2) != b"\r\n":
                raise RespProtocolError("invalid RESP bulk terminator")
            return RespValue("bulk", value)
        if kind == b"*":
            count = self._length(self._line())
            if count == -1:
                return RespValue("null_array", None)
            if count < 0 or count > MAX_ARRAY_ITEMS:
                raise RespProtocolError("invalid or excessive RESP array length")
            return RespValue("array", tuple(self._parse(depth + 1) for _ in range(count)))
        raise RespProtocolError("unsupported RESP prefix")


def comparable(value: RespValue, command: bytes) -> Any:
    """Strict typed reply, except Redis's unordered members and hash pairs."""
    if command.upper() == b"SMEMBERS":
        if value.kind != "array":
            return (value.kind, value.value)
        members = []
        for item in value.value:
            if item.kind != "bulk":
                raise RespProtocolError("SMEMBERS non-bulk member")
            members.append(item.value)
        if len(members) != len(set(members)):
            raise RespProtocolError("SMEMBERS duplicate member")
        return ("set_members", tuple(sorted(members)))
    if command.upper() == b"HGETALL":
        if value.kind != "array":
            return (value.kind, value.value)
        if len(value.value) % 2:
            raise RespProtocolError("HGETALL odd field/value count")
        mapping = {}
        for index in range(0, len(value.value), 2):
            key, item = value.value[index:index + 2]
            if key.kind != "bulk" or item.kind != "bulk":
                raise RespProtocolError("HGETALL non-bulk field/value")
            if key.value in mapping:
                raise RespProtocolError("HGETALL duplicate field")
            mapping[key.value] = item.value
        return ("hash_map", tuple(sorted(mapping.items())))
    if value.kind == "array":
        return ("array", tuple(comparable(item, b"") for item in value.value))
    return (value.kind, value.value)
