# AOF persistence and restart recovery (Stage 3A)

Persistence is opt-in and loopback serving remains unchanged:

```sh
mkdir -p data
./build/debug/redisdb-server --resp2-server --appendonly yes \
  --aof-file data/appendonly.aof --appendfsync always
```

Default: persistence disabled. When enabled, `always` is the default policy;
the default file is `./appendonly.aof`. The parent directory must exist.
Configuration is startup-only. No new Redis commands, snapshots, compaction,
rewriting, replication or background persistence worker are implemented.

## Commit boundary

1. Prepare a private database copy and execute the command tentatively, including
   validation, memory admission, TTL handling, collection results and eviction.
   Rejected commands roll back and produce no journal record.
2. Compare the tentative state with the original. Encode actual changed-key
   postimages and tombstones as one transaction, including every eviction victim.
   MSET is one transaction; duplicate destinations have their final values.
3. Write the complete framed record, retrying short writes and EINTR.
4. Perform synchronization required by the policy below.
5. Publish memory without allocation, then allow a successful response to be queued.

No other command runs during this sequence. A persistent command uses one wall-clock
sample so reads cannot cross a deadline outside their expiry transaction. Read-only
traffic avoids the copy unless expiry cleanup may mutate the keyspace. Maintenance
expiry uses the same journal boundary. Startup expiry cleanup is journaled and
synchronized before readiness, preventing later LRU records from referring to an
inconsistent keyset.

Writes may be committed even when the client receives no reply: a crash, socket
failure, or response allocation failure can occur after logging. Complete records
beyond the last watermark can be replayed. This is an uncertain client outcome,
not exactly-once execution. Allocation failures before logging restore memory;
any append, watermark publication or required sync error latches persistence failure
and terminates serving with a nonzero exit. The failed command gets no success reply,
and subsequent callbacks cannot acknowledge successful operations. Earlier weaker-policy
acknowledgments cannot be retroactively withdrawn after a periodic sync failure.

## Synchronization guarantees

| Policy | Before a successful mutation acknowledgment |
|---|---|
| `always` | Complete journal write; journal `fsync`; checksummed watermark written to a unique temporary file and `fsync`ed; atomic rename; parent directory `fsync`; memory publication. |
| `everysec` | Complete journal write and memory publication. Pending bytes and the watermark are synchronized on a monotonic one-second schedule, checked on event-loop turns. |
| `no` | Complete journal write and memory publication. No per-write or periodic explicit synchronization. |

Creation synchronizes the journal header and directory publication. Startup establishes
a synchronized recovered baseline. Clean shutdown checks synchronization of pending
records under every policy. Destructors only close descriptors and cannot substitute
for checked shutdown.

`everysec` can lose recently acknowledged writes on a host crash. Its loss window is
approximately the scheduling interval **plus event-loop and filesystem stalls**, not
a hard one-second bound. `no` relies on OS writeback and can lose a larger, unbounded
recent suffix. A process SIGKILL test does not simulate power loss. `always` relies on
the OS/filesystem/device honoring successful POSIX `fsync` and directory synchronization;
it is not a guarantee against lying hardware, media loss or removal of both recovery files.

## Binary format, version 1

All integers use big-endian encoding, with no serialized C++ object layouts.
CRC32C detects accidental corruption; it is not authentication.

- File header (20 bytes): `RDBAOF01`, u32 version `1`, u32 flags `0`, CRC32C of
  the preceding 16 bytes.
- Each transaction: `TXN1`, consecutive u64 sequence starting at 1, u32 payload
  length, payload, CRC32C of the transaction header plus payload, `END!` trailer.
- Payload: u32 changed-key count; strictly sorted unique effects; one-byte LRU
  flag and a u32-counted oldest-to-newest key order. Every byte string is
  u32-length-prefixed and binary-safe. An effect contains a key, a one-byte tag
  (`0` tombstone, `1` string, `2` list, `3` hash, `4` set), and, for a value,
  a one-byte expiry flag plus optional signed i64 Unix microseconds. Strings
  contain one byte string; collections contain u32-counted elements or field/value
  pairs. Empty collections, duplicate hash fields/set members and unknown tags
  are rejected. Hash/set encoding is sorted; list order is preserved.
- Deadlines are absolute, never reconstructed from relative TTL commands.
  Nanosecond hosts round deadlines down by less than one microsecond; recovery
  never extends their lifetime. Out-of-range native clock deadlines fail closed.
- Bounds: 64 MiB payload, 8 MiB individual byte string, 1,048,576 effects,
  collection items or LRU keys per transaction. Parsing is streaming by record,
  checks lengths before allocation, and does not load the entire journal file.
- `<aof-file>.commit` (32 bytes): `RDBACM01`, u64 synchronized sequence,
  u64 end offset, u32 last synchronized record CRC, CRC32C of those 28 bytes.
  This atomic watermark also detects loss of an entire synchronized suffix,
  including truncation exactly at a record boundary.

The journal and `.commit` file form one recovery unit; retain/copy both together
while the server is stopped. Orphan temporary watermark files after a crash are
not recovery sources and are not automatically removed. An exclusive advisory
file lock prevents simultaneous server writers. Files must be regular files;
final-component symlinks are refused. The data directory must be trusted against
external modification.

## Replay and limits

Startup validates version, flags, framing, sequence, checksums, trailer, watermark
and value structure before readiness. **Any incomplete tail or corruption fails
startup without truncating or repairing the files.** No automatic salvage is
claimed; even an unacknowledged partial tail requires operator inspection.
Successful startup can append checked tombstones for keys expired during downtime.

Replay installs committed effects without executing commands, choosing new eviction
victims or renewing TTLs. It reconstructs the existing logical-memory ledger and
expiry index. LRU restores the order at the latest data commit; subsequent read-only
touches are not durable. Changing from a journal written without LRU to LRU uses a
documented sorted-key cold order. Startup removes due keys and rejects a configured
memory limit too small for the recovered live state, rather than silently evicting it.

Persistent writes currently stage a complete database copy (O(database size) extra
time and temporary memory), then encode only changed values and LRU metadata.
Collection postimages and the complete LRU order can make records large. Disk writes,
`fsync`, replay and watermark publication block the event loop. The logical data budget
does not bound temporary copies, journal size, metadata or RSS. These are explicit
Stage 3A tradeoffs; benchmarks, incremental transaction staging and compaction are later work.
