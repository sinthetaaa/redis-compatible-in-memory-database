# Snapshots and crash recovery (Fast-Track Stage 3B)

Stage 3B is merged at `798010ff34ec0c094aa37f7821524277edbb75dd`; V1 is unchanged
through accepted Stage 4B main `d69b3ede7405b4fe843ffb1c3bbfe4d957810615`.
See [private deployment and stopped backups](PRIVATE_DEPLOYMENT.md) for Stage 5A
preparation; actual remote acceptance remains pending. No AOF rotation or compaction.

## Configuration and operator trigger

```sh
mkdir -p data/snapshots
./build/debug/redisdb-server --resp2-server --appendonly yes \
  --aof-file data/appendonly.aof --appendfsync always \
  --snapshot-dir data/snapshots
# After RESP_READY, send SIGUSR1 to this exact server PID:
kill -USR1 "$SERVER_PID"
```

Snapshots are opt-in and require AOF. The snapshot directory must already exist;
use a dedicated directory, with trusted ancestors and no external concurrent
modification. The snapshot directory must be a real directory rather than a
symlink, including when spelled with trailing slashes or dot components. The
selected snapshot, CURRENT and LOCK must be regular files rather than symlinks.
An advisory exclusive directory lock supplements the existing journal lock.
Configuration is startup-only. No new Redis commands or background workers.

The signal handler only sets `volatile sig_atomic_t`. The serialized event loop
handles capture at a maintenance boundary, normally within its 100 ms poll
interval, plus any ongoing command/I/O stalls. Repeated signals can coalesce.
A request during capture can schedule another capture. Signals before readiness
are not supported. Snapshot creation is not automatically triggered on shutdown.
A successful publication emits `SNAPSHOT_PUBLISHED <generation> <aof-sequence>`
after the final directory synchronization; receipt of a signal is not success.

## Generation selection and publication

`CURRENT` is the sole selector. Generation zero means ordinary AOF-only recovery.
First use atomically publishes and directory-syncs this zero selection before any
snapshot is constructed. Existing final `generation-*` files without CURRENT
are inconsistent and reject startup. A missing CURRENT in a directory containing
only initialization temporaries can safely retry initialization.

A positive selection names exactly `generation-<unsigned-decimal>.snap`.
Recovery never chooses the highest filename, scans for a usable fallback, or
ignores a corrupt/missing selected file. All previous immutable generations remain
on disk. Final candidates not named by CURRENT and unique `.tmp.*` files from
interrupted publication are unselected artifacts, never recovery sources. They
are retained across process crashes. A retry skips occupied generation names;
it never overwrites an existing final candidate. Generations may have gaps.

Publication, on the command execution thread:

1. Synchronize the complete written AOF prefix and its atomic commit watermark,
   including pending writes under `everysec`/`no`. Obtain sequence, exact end
   offset, last-record CRC32C and CRC32C of the entire prefix including its header.
2. With other commands and maintenance paused, capture physical entries, absolute
   deadlines, logical charge and current LRU order. Capture itself neither expires
   nor touches keys. A deadline may pass during capture; recovery handles it.
3. Write a uniquely created, owner-only temporary snapshot in the snapshot
   directory. Retry short writes and EINTR; reject zero/invalid progress.
4. `fsync` the snapshot, rename it to an unused final generation name, then `fsync`
   the directory. The data file is durable before any selector can reference it.
5. Write a unique temporary CURRENT, `fsync` it, atomically rename over CURRENT,
   then `fsync` the directory. Only then report publication success.

| Interruption boundary | Selection on process restart |
|---|---|
| During snapshot construction, after data sync, after data rename/directory sync, or while preparing CURRENT | Previous CURRENT; replay the full newer AOF suffix. |
| After CURRENT rename, before directory sync | New CURRENT is visible after process death. A host crash may retain old or new selection; both data generations were synchronized first. No publication success is claimed. |
| After final directory sync | New CURRENT, subject to successful POSIX synchronization being honored by the OS/filesystem/device. |

Any capture/allocation/write/sync/rename failure stops serving with a nonzero
exit. A final rename/sync failure can have an uncertain publication outcome; the
next startup follows the visible, validated CURRENT. No previous generation or
journal bytes are modified. Normal unwinding removes only the exact temporary
file owned by the failed operation; crash leftovers are not automatically removed.
There is no automatic repair of incomplete selected recovery data.

## Portable format, version 1

All numbers are unsigned big-endian unless the inherited entry codec specifies
signed Unix microseconds. No native object layouts, pointers, charges from
allocator internals, hash iterators or LRU iterators are serialized. CRC32C detects
accidental corruption; it does not authenticate data or defend against deliberate
checksum reconstruction. Bit-identical AOF prefixes are equivalent associations.

CURRENT is exactly 32 bytes:

- `RDBSEL01` (8), version `1` (u32), flags `0` (u32), generation (u64).
- CRC32C of preceding 24 bytes (u32), `END!` (4).

The snapshot header is exactly 80 bytes:

| Offset | Field |
|---|---|
| 0 | `RDBSNP01` (8 bytes) |
| 8 | Version `1` (u32) |
| 12 | Flags: `0` or `1` (u32), bit 0 means captured LRU order |
| 16 | Generation (u64), must equal CURRENT |
| 24 | Synchronized AOF sequence (u64) |
| 32 | AOF end offset at that sequence (u64) |
| 40 | Last AOF record CRC32C (u32), zero at sequence zero |
| 44 | Entire AOF prefix CRC32C (u32), including the 20-byte journal header |
| 48 | Physical entry count (u64) |
| 56 | Total logical charge (u64) |
| 64 | LRU key count (u64): entry count when enabled, otherwise zero |
| 72 | Logical-memory accounting model version `1` (u32) |
| 76 | CRC32C of preceding 76 bytes (u32) |

Each frame is a 4-byte tag, u32 payload length, payload, u32 CRC32C of tag/length/
payload, and `END!`. Frames appear in this exact order:

- One `KEY1` per entry, in strictly ascending binary-key order. Its payload reuses
  the [AOF V1 effect codec](AOF.md): exactly one replacement, no tombstones and no
  LRU data. Strings include integer strings unchanged; collections retain list
  order and canonical hash/set encodings. Absolute microsecond deadlines retain
  the AOF codec's platform-range validation and sub-microsecond floor semantics.
- One `LRU1` per ordered key, oldest to newest, with raw binary key as payload.
  The keys must form a complete, duplicate-free permutation of physical entries.
- Exactly one `DONE`, containing u32 CRC32C of all preceding snapshot bytes.
  Its own frame checksum/trailer must also validate. EOF must follow immediately.

The snapshot has no aggregate 64 MiB or 1,048,576-entry limit. Frame parsing is
streaming and file-size bounded. Each `KEY1` payload retains the existing
64 MiB codec bound; individual keys/strings are bounded at 8 MiB and each
collection at 1,048,576 elements, matching the state admitted by this AOF version.
LRU keys are separate frames bounded at 8 MiB, not one aggregate journal record.
Counts/lengths are validated before payload allocation. Logical-memory charges
use existing platform-independent V1 policy coefficients, not `sizeof` or RSS.

## Startup algorithm

1. Lock the snapshot directory and validate CURRENT. With selection zero, use
   the unchanged AOF-only replay path. With a positive selection, require the
   named regular, nonsymlink snapshot. Validate header/version/generation, every
   frame, counts, unique ordered keys, LRU permutation, whole-file footer and EOF.
2. Restore entries without executing commands or eviction. Recompute charges and
   rebuild the expiry index through existing storage insertion primitives. Check
   the total against the header before removing expired entries. Rebuild LRU
   iterators; an LRU snapshot restores read touches as of capture. If a snapshot
   has no LRU metadata and LRU is now configured, start in sorted-key cold order.
3. Require the existing AOF and commit watermark; never create a replacement
   journal for a selected snapshot. Lock and validate the journal header/marker.
   The snapshot checkpoint must be at or before the synchronized watermark.
4. Stream the entire AOF from byte 20, validating every frame, sequence, checksum,
   trailer and effect-codec structure, including history before the checkpoint.
   Recompute prefix CRC. At the snapshot sequence require exact offset,
   last-record CRC and full-prefix CRC equality. Validate the durable watermark's
   exact record boundary too. Any truncated/corrupt tail rejects startup.
5. Apply only records with sequence strictly greater than the snapshot checkpoint.
   These are committed postimages/tombstones; no command re-execution, new TTLs
   or new eviction choices. Later AOF LRU metadata supersedes captured recency.
6. Use the existing AOF startup transaction to remove keys due at the current
   wall time, journal their tombstones, enforce the configured live-memory budget
   without eviction, and synchronize the recovered baseline. Only then serve.

Failure discards the startup database and reports nonzero, without changing
journal/selector/snapshot bytes during validation. The existing expiry cleanup
can write only after validation succeeds. An undersized live-memory budget fails.
Explicitly omitting `--snapshot-dir` chooses AOF-only mode; it does not validate
snapshot files. Keep the same configuration when validating snapshot recovery.

## Validation and limits

`unit.snapshot` covers binary/type/TTL/charge roundtrips, empty checkpoints,
policy changes and read-touch LRU, checkpoint/prefix mismatch, invalid metadata,
missing/truncated/symlink/FIFO files, locking, ten injected publication syscall
failures, failed AOF checkpoint, short writes/EINTR, eight deterministic SIGKILL
publication boundaries, retained previous generations and a 72 MiB snapshot.
`integration.resp_snapshot` drives actual TCP server processes under all three
sync policies: multiple generations, suffix writes, expiry while stopped,
eviction, AOF-only migration, invalid recovery combinations, real EFBIG failure,
SIGKILL during construction and after acknowledged publication, repeated restarts.
These are process-crash tests, not power-cut or filesystem fault-model proofs.

Local validation on macOS ARM64 (2026-10-26): Debug **73/73**, Release **73/73**,
and full-program Apple Clang 17 UBSan **74/74** (including the pinned 50-seed
parser corpus) passed. Existing nonpersistent Redis 8.10.2 differential suites
passed: curated **54 cases / 40 routes**, negative **35**, TTL **44**.
Formatting, diff whitespace and sanitizer compilation coverage checks passed.
The repository's documented Apple ASan runtime startup limitation still applies;
Linux ASan/UBSan/libFuzzer is wired to discover/instrument the new tests but was
not run or published during this local-only milestone.

Snapshot capture blocks the event loop for encoding, writes and synchronization.
It sorts key views (O(K log K)), copies LRU keys, and temporarily copies/encodes
one entry at a time; temporary memory is outside the logical-memory budget.
Restore consumes the live database plus bounded frames and O(K) LRU/validation
metadata. Full historical AOF validation still reads/decodes O(journal bytes),
so no bounded startup time or snapshot speedup is claimed. CRC tracking adds
work to journal append. Stage 4B uses affected-key undo under noeviction; exact
LRU retains the full-database staging fallback.
All generations, abandoned candidates and the complete AOF continue growing;
there is no retention worker, automatic cleanup, rotation, compaction or salvage.
Synchronization relies on the same POSIX filesystem/device assumptions as AOF;
SIGKILL does not prove power-loss durability. Snapshots strengthen only the
explicit checkpoint: subsequent weaker-policy acknowledgments retain their
existing loss windows and uncertain client outcomes.
