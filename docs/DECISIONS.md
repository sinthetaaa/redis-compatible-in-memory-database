# Architecture decisions

These decisions explain the implemented server and its tradeoffs. The portfolio
release is feature-frozen on V1 persistence. Deferred designs are not promises of
implemented functionality. The [architecture guide](ARCHITECTURE.md) describes
current command, resource and operational contracts.

## ADR-001 — C++20 and CMake

**Decision:** implement the server in C++20, with CMake and Ninja presets for
Debug and Release on macOS and Linux.

RAII makes descriptor and resource ownership explicit; standard containers and
variants provide owned, type-safe state. Platform-specific polling stays behind
a common interface. Compiler warnings and sanitizer builds complement tests;
they do not replace reasoning about lifetime, overflow or failure ordering.

## ADR-002 — Serialized command execution

**Decision:** one event-loop thread executes commands, maintenance and persistence
against a shared database. Multiple client sockets remain active concurrently.

This makes command ordering, TTL, eviction and persistence boundaries tractable
without a multithreaded keyspace. A command cannot expose its intermediate state
to another command. This does not provide multi-command transactions or
exactly-once execution. Synchronous disk I/O, large commands and snapshots can
stall every client. There is no background persistence worker.

## ADR-003 — Native readiness polling

**Decision:** use kqueue on macOS and epoll on Linux behind `EventPoller`.

Native readiness supports many nonblocking clients while keeping server-level
logic portable. Both backends require independent validation. Readiness does
not guarantee a complete request or write: handle EAGAIN, EINTR, EOF, resets and
partial progress. Registration tokens distinguish lifetimes when descriptors
are reused; unregister before closing owned descriptors.

## ADR-004 — A bounded RESP2 subset

**Decision:** incremental, binary-safe RESP2 parsing and ordered replies, with an
explicit supported command/option subset rather than full Redis parity.

The parser distinguishes Complete, NeedMore and Invalid; it validates declared
lengths, nesting and total frame size. Requests become owned Command objects
only from nonempty arrays of bulk strings. Only ASCII command-name letters are
normalized; arguments retain their exact bytes. A modular registry routes once
to typed handlers, and reusable arity checks remain separate from option/type
validation. Unknown commands and WRONGTYPE errors are explicit.

RESP3, client handshake extensions and commands outside the
[supported subset](ARCHITECTURE.md#supported-command-subset) are not implied by
compatibility with `redis-cli` examples.

## ADR-005 — Owned typed in-memory values

**Decision:** use a hash-table primary keyspace with owned keys and a variant of
strings, lists, hashes and sets. Persist state through explicit codecs, not
container layouts.

Strings own bytes; lists use `std::list<std::string>`; hashes and sets use
unordered containers. Lists preserve order and duplicates; hashes/sets have no
iteration-order promise. Integer commands operate on checked decimal strings,
not an additional stored type. All nested data is binary-safe and copies are
independent.

Node-based lists support end operations and nonthrowing moves at the cost of
allocation/cache locality. Collection updates may stage a copy of the affected
value to preserve exception safety. Nonempty mutations preserve TTL; deleting
the last member removes the key and expiry metadata. SET/MSET replace types and
clear old TTL unless SET provides a new EX/PX deadline. MGET returns nil for
non-string keys. No relational, B+ tree, LSM or disk-page engine is included.

## ADR-006 — Explicit persistence durability

**Decision:** journal canonical changed-key postimages and tombstones in V1,
with `always`, `everysec` and `no` synchronization policies.

Tentative execution uses affected-key undo under noeviction or a private database
copy under exact LRU. One journal transaction includes actual effects and eviction
victims; recovery does not reexecute commands or choose fresh eviction victims.
Required append/sync succeeds before allocation-free memory publication and a
successful reply. A checksummed atomic commit watermark detects missing
synchronized history. Failed commands roll back; persistence errors stop serving.

Mutation validation, record encoding, write completion, synchronization, memory
publication and client reply are distinct events. A complete write can recover
even if the client never received success. Weaker policies can lose recently
acknowledged data on host crash; even always depends on honest OS/filesystem/device
synchronization. Partial tails fail closed without automatic truncation or repair.
See [AOF.md](AOF.md) for exact bytes, guarantees and costs.

## ADR-007 — Evidence before performance claims

**Decision:** define semantics and failure behavior, validate them, then measure
and profile specific costs before optimizing.

Unit, actual-process TCP, rollback and corruption tests cover different failure
surfaces. Sanitizer timing and RSS are not comparable to native performance.
The parser fuzz harness exercises real parser/value code with bounded inputs,
a pinned 50-seed SHA-256 corpus and a separate mutable corpus copy. Linux runs
Clang ASan/UBSan and libFuzzer; deterministic development-host replay used UBSan
where that host's Apple ASan/libFuzzer runtime was unavailable.

Recorded environment, executable identity and workload determine what a benchmark
supports. Closed-loop Python measurements have coordinated omission and client
CPU limits. See [methodology and dated measurements](BENCHMARKS.md); no invented
throughput, latency, memory or Redis-wide performance claim is accepted.

## ADR-008 — Redis as a reference

**Decision:** implement this engine from scratch; use Redis as protocol/semantic
reference and differential oracle, not as the embedded database engine.

The harness pins Redis 8.10.2 identity, compares typed binary-safe responses and
keeps comparisons within the implemented subset. SMEMBERS/HGETALL use unordered
comparison with duplicate checks; TTL tests account for timing ambiguity.
Reference identity failures must not silently select a different binary or
floating image. Details: [differential testing](../tests/differential/README.md)
and [Linux reference validation](DOCKER.md#repeatable-container-checks-and-linux-reference).

## ADR-009 — Absolute expiry with monotonic scheduling

**Decision:** combine lazy expiration with a deadline-ordered bounded active
sweep, using an injectable clock.

Lazy checks hide expired keys even before maintenance runs. Active work checks
at most 32 due records on a nominal 100 ms monotonic cadence. Persist absolute
wall-clock deadlines, never steady-clock timestamps or renewed relative TTLs.
Downtime consumes TTL; wall-clock changes affect absolute expiration. Overwrites,
collection mutations, deletes, eviction and replay must keep metadata consistent.
Persisted microsecond deadlines round down on finer-clock hosts, never extending
lifetime during recovery. See [AOF](AOF.md) and [snapshots](SNAPSHOTS.md).

## ADR-010 — Logical accounting and exact LRU

**Decision:** enforce deterministic V1 logical byte charges with startup-only
maxmemory and noeviction/allkeys-lru policies.

Cache each entry's charge and the database total; use checked arithmetic for
replacement/removal. Expired physical entries remain charged until removed.
Policy units include key/value/collection/expiry coefficients, not measured
allocator capacity, temporary clones, network buffers or RSS.

Noeviction rejects writes that cannot be admitted. Allkeys-lru uses exact recency
rather than sampled approximation. Exact LRU persistence retains a full-database
staging fallback and full-order record, so its memory/time costs remain explicit.
Admission, mutation, expiration and eviction must not publish partial rejected
writes. Startup rejects a budget below recovered live charge rather than silently
evicting restored state. The logical budget does not bound disk use or process RSS.

## ADR-011 — Portable coordinated snapshots

**Decision:** use framed, checksummed V1 immutable snapshots associated with an
exact synchronized AOF prefix, selected only through CURRENT.

Serialize explicit big-endian fields, binary values, absolute deadlines, V1
logical charges and LRU order. Do not dump C++ object layouts or iterators.
Synchronize the journal checkpoint; write/sync/rename the snapshot and sync its
directory before publishing a synced CURRENT through rename and directory sync.
A successful rename alone is not a durability guarantee.

Retain previous generations and the complete journal history. Recovery validates
the selected file and full history, then replays only newer effects. Missing or
corrupt selected data fails closed without choosing another snapshot. SIGUSR1
requests capture; only SNAPSHOT_PUBLISHED confirms successful publication.
Snapshots block the event loop, do not compact the AOF and do not establish a
bounded recovery time. See [SNAPSHOTS.md](SNAPSHOTS.md).

## ADR-012 — Private container operation

**Decision:** package a non-root Linux runtime with persistent storage and host
loopback-only port publication. Real remote deployment is not claimed.

Native bind defaults to 127.0.0.1. The container deliberately binds 0.0.0.0 inside
its namespace, while Compose restricts the published host port. The distinction
is essential because the server has no AUTH or TLS. Docker Engine >=28 avoids
the documented older localhost-publication isolation caveat.

UID/GID 10001, dropped capabilities, read-only rootfs and resource limits reduce
container privileges without providing authentication or public-service safety.
Keep recovery files together in stopped backups, verify restored copies and
budget disk/RSS separately from logical maxmemory. An optional private Compose
overlay/tool exists, but its disposable CI acceptance is not real-host or SSH
acceptance. See [DOCKER.md](DOCKER.md).

## ADR-013 — Separate transport smoke mode

**Decision:** retain explicit loopback-only `--smoke-echo` for actual networking
and lifecycle tests, separate from the production `--resp2-server` path.

Smoke mode echoes bytes without Redis interpretation and supports ephemeral
ports. It exercises real descriptors, native polling, admission, disconnects and
shutdown without claiming RESP2 compatibility. It is a test facility, not the
server mode used by `redis-cli` examples.

## ADR-014 — Reuse connection input for framing

**Decision:** `RespFramedInput` calls the stateless parser over the existing
owned Connection input buffer rather than introducing a second receive buffer.

Consume only complete frames; preserve incomplete or invalid bytes for the caller's
policy. The default framed budget is 64 KiB and must agree with the connection
cap. This keeps parsing separate from TCP ownership and avoids duplicate buffer
state. The adapter itself does not close sockets or implement command semantics.

## ADR-015 — Bounded FIFO pipeline scheduling

**Decision:** process at most 16 frames per drain, checking output capacity before
execution, queueing the reply before consuming the request, and resuming deferred
work through transport-independent handler actions.

Draining an unbounded pipeline can starve other clients. Requiring a new socket
write can strand complete buffered requests. Reexecuting after backpressure can
duplicate side effects. FIFO deferred scheduling with membership checks, a
32-client per-poll bound and a progress requirement addresses these concerns
without making ConnectionManager understand RESP2 types.

The reply callback is also used by the production command server; failures after
side effects require caller closure, not automatic retry. Custom parser, reply
and buffer limits must remain coordinated. The smoke handler remains separate.

## ADR-016 — Minimal operational observability

**Decision:** serve restricted read-only INFO/CONFIG GET from cached event-loop
state before entering the database clock/transaction path.

Counters reset on restart, saturate at UINT64_MAX and do not require key scans.
Do not expose private paths, mutate LRU or confuse written journal sequence with
weaker-policy durability. Settings remain startup-only. Static JSON lifecycle/error
categories go to stderr, while stdout retains readiness/publication/shutdown
markers. Diagnostic write/flush failure exits 74 rather than reporting success.

Idle reaping exempts queued output and scheduled work. Shutdown stops admission,
drains observed pending network work for up to one monotonic second, then checks
journal finish. Filesystem stalls are outside that drain bound, and unread future
requests have no completion guarantee. See [operational visibility](ARCHITECTURE.md#cli-and-operational-visibility).

## Frozen scope

No production V2 compaction, automatic history pruning, generation replacement or
V2 recovery pipeline is implemented. Isolated V2 prerequisites are not evidence
of a usable V2 mode. Replication, clustering, consensus, SQL, Lua/modules, RESP3
completeness, Pub/Sub and Streams are outside the implemented subset. No resumed
engineering roadmap or remote deployment is implied by these decisions.
