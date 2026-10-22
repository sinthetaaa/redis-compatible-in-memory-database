#!/usr/bin/env python3
"""Phase 2G.1B deterministic RESP2 corpus, pinned SHA-256 manifest and replay gate.

No third-party dependencies. Generated seeds and sidecar manifest are build outputs.
A verification failure is fatal; generation never replaces unexpected bytes.
"""

import argparse
import hashlib
from pathlib import Path
import subprocess

SEEDS = {
    "01-simple-ok.resp": b"+OK\r\n",
    "02-empty-simple.resp": b"+\r\n",
    "03-error.resp": b"-ERR invalid\r\n",
    "04-signed-integer.resp": b":+12\r\n",
    "05-null-bulk.resp": b"$-1\r\n",
    "06-empty-bulk.resp": b"$0\r\n\r\n",
    "07-null-array.resp": b"*-1\r\n",
    "08-empty-array.resp": b"*0\r\n",
    "09-binary-bulk.resp": b"$4\r\n\x00\r\n\xff\r\n",
    "10-nested-array.resp": b"*2\r\n:7\r\n*1\r\n+OK\r\n",
    "11-incomplete-bulk.resp": b"$3\r\nab",
    "12-incomplete-crlf.resp": b"$3\r\nabc\r",
    "13-malformed-terminator.resp": b"$3\r\nabcX",
    "14-noncanonical-length.resp": b"$01\r\na\r\n",
    "15-valid-with-suffix.resp": b"+OK\r\n:2\r\n",
    "16-empty-input.resp": b"",
    "17-bad-prefix.resp": b"?ERR\r\n",
    "18-oversized-length.resp": b"*999999\r\n",

    # 2G.1B: stable extensions spanning every RESP2 type and bounded limits.
    "19-negative-integer.resp": b":-42\r\n",
    "20-zero-integer.resp": b":0\r\n",
    "21-plain-bulk.resp": b"$5\r\nhello\r\n",
    "22-two-scalars-array.resp": b"*2\r\n+OK\r\n:2\r\n",
    "23-single-nested-array.resp": b"*1\r\n*1\r\n:7\r\n",
    "24-null-bulk-with-suffix.resp": b"$-1\r\n+OK\r\n",
    "25-null-array-with-suffix.resp": b"*-1\r\n:42\r\n",
    "26-two-bulks-array.resp": b"*2\r\n$3\r\nabc\r\n$0\r\n\r\n",
    "27-binary-nul-crlf-high.resp": b"$6\r\n\x00A\r\n\xffZ\r\n",
    "28-binary-incomplete-cr.resp": b"$6\r\n\x00A\r\n\xffZ\r",
    "29-single-type-prefix.resp": b"+",
    "30-inline-trailing-cr.resp": b"+abc\r",
    "31-bare-lf.resp": b"+OK\n",
    "32-bare-cr-in-line.resp": b"+A\rB\n",
    "33-noncanonical-plus-bulk.resp": b"$+3\r\nabc\r\n",
    "34-noncanonical-zero-array.resp": b"*00\r\n",
    "35-negative-zero-bulk.resp": b"$-0\r\n\r\n",
    "36-negative-leading-zero-array.resp": b"*-01\r\n",
    "37-second-terminator-invalid.resp": b"$3\r\nabc\rX",
    "38-invalid-array-child.resp": b"*2\r\n:1\r\n?bad\r\n",
    "39-incomplete-array-child.resp": b"*2\r\n:1\r\n$3\r\nab",
    "40-positive-integer-overflow.resp": b":9223372036854775808\r\n",
    "41-negative-integer-overflow.resp": b":-9223372036854775809\r\n",
    "42-exact-inline-256.resp": b"+" + b"a" * 256 + b"\r\n",
    "43-over-inline-257.resp": b"+" + b"a" * 257 + b"\r\n",
    "44-exact-bulk-2048.resp": b"$2048\r\n" + b"\x00" * 2048 + b"\r\n",
    "45-over-bulk-2049.resp": b"$2049\r\n" + b"\x00" * 2049 + b"\r\n",
    "46-array-32-elements.resp": b"*32\r\n" + b"$-1\r\n" * 32,
    "47-array-33-elements.resp": b"*33\r\n" + b"$-1\r\n" * 33,
    "48-exact-nesting-8.resp": b"*1\r\n" * 8 + b":1\r\n",
    "49-over-nesting-9.resp": b"*1\r\n" * 9 + b":1\r\n",
    # Header 4+bulk frame 2057+another bulk frame 2035 = 4096 bytes.
    "50-exact-frame-4096.resp": b"*2\r\n$2048\r\n" + b"A" * 2048 + b"\r\n$2026\r\n" + b"B" * 2026 + b"\r\n",
}

# Changing the seed set requires intentional re-pinning and review.
EXPECTED_SEED_COUNT = 50
EXPECTED_MANIFEST_SHA256 = "e5ff669b8cd8ac01f3d81dcbddcadceba63426a503a7d55a5fb1ddc7faa62327"
MANIFEST_SUFFIX = ".sha256"


def expected_manifest() -> bytes:
    result = b"".join(
        hashlib.sha256(data).hexdigest().encode("ascii") + b"  " + name.encode("ascii") + b"\n"
        for name, data in sorted(SEEDS.items())
    )
    assert len(SEEDS) == EXPECTED_SEED_COUNT, "Seed count changed without review"
    assert hashlib.sha256(result).hexdigest() == EXPECTED_MANIFEST_SHA256, (
        "Seed table differs from the reviewed pinned manifest"
    )
    return result


def sidecar(directory: Path) -> Path:
    return directory.with_name(directory.name + MANIFEST_SUFFIX)


def preflight(directory: Path, manifest_file: Path, expected: bytes, generating: bool) -> None:
    if directory.exists():
        if not directory.is_dir() or directory.is_symlink():
            raise SystemExit("Refusing corpus path that is not a real directory")
        extras = sorted(p.name for p in directory.iterdir() if p.name not in SEEDS)
        if extras:
            raise SystemExit(f"Unexpected corpus entries: {extras}")
        for name, data in SEEDS.items():
            path = directory / name
            if not path.exists():
                if not generating:
                    raise SystemExit(f"Missing corpus seed: {name}")
                continue
            if path.is_symlink() or not path.is_file() or path.read_bytes() != data:
                raise SystemExit(f"Corrupt or non-file corpus seed: {name}")
    elif not generating:
        raise SystemExit("Missing generated corpus directory")

    if manifest_file.exists() or manifest_file.is_symlink():
        if (manifest_file.is_symlink() or not manifest_file.is_file()
                or manifest_file.read_bytes() != expected):
            raise SystemExit("Corrupt or non-file SHA-256 corpus manifest")
    elif not generating:
        raise SystemExit("Missing SHA-256 corpus manifest")


def fingerprint(directory: Path) -> str:
    digest = hashlib.sha256()
    for name, data in sorted(SEEDS.items()):
        encoded_name = name.encode("utf-8")
        digest.update(len(encoded_name).to_bytes(4, "big"))
        digest.update(encoded_name)
        digest.update(len(data).to_bytes(4, "big"))
        digest.update(data)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--runner", type=Path)
    args = parser.parse_args()
    if args.runner is not None and not args.verify:
        parser.error("--runner requires --verify")

    destination = args.output
    manifest_file = sidecar(destination)
    pinned = expected_manifest()
    preflight(destination, manifest_file, pinned, generating=not args.verify)

    if args.verify:
        print(f"PASS: SHA-256 manifest verified for {len(SEEDS)} immutable RESP2 seeds; "
              f"manifest-sha256={EXPECTED_MANIFEST_SHA256}", flush=True)
    else:
        destination.mkdir(parents=True, exist_ok=True)
        for name, data in sorted(SEEDS.items()):
            path = destination / name
            if not path.exists():
                path.write_bytes(data)
        if not manifest_file.exists():
            manifest_file.write_bytes(pinned)
        preflight(destination, manifest_file, pinned, generating=False)
        print(f"PASS: {len(SEEDS)} deterministic RESP2 seeds, "
              f"manifest-sha256={EXPECTED_MANIFEST_SHA256}, "
              f"fingerprint={fingerprint(destination)}", flush=True)

    if args.runner:
        completed = subprocess.run([str(args.runner), str(destination)], check=False)
        if completed.returncode:
            raise SystemExit(completed.returncode)


if __name__ == "__main__":
    main()
