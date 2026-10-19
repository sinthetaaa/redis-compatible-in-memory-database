# Architecture Decision Records

## Redis-Compatible In-Memory Database Server

**Initial decision date:** 2026-10-19
**Implementation language:** C++20

This document records approved architecture decisions,
their consequences, and outstanding implementation choices.

An accepted architecture decision does not imply that
the corresponding functionality has been implemented.

Decision changes require a documented technical reason,
impact assessment, and updated project documentation.

---

## ADR-001 — C++20 and CMake

**Status:** Accepted
**Implementation:** Build foundation complete

### Decision

Use C++20 for the server implementation.

Use CMake for configuration, building and testing.
Use Ninja as the preferred local build generator.

### Rationale

- Explicit resource ownership and lifetimes.
- RAII for sockets, file descriptors and file handles.
- Modern standard-library types.
- Efficient in-memory data structures.
- Direct operating-system networking interfaces.
- Support for sanitizers and profiling.
- Cross-platform development without a large framework.

### Consequences

Avoid unnecessary dependencies.

Use strict compiler warnings, automated tests
and formatting from the start.

Do not introduce unsafe ownership patterns merely
for micro-optimizations.

---

## ADR-002 — Single-Threaded Command Execution

**Status:** Accepted
**Implementation:** Not started

### Decision

Initially execute commands on a single event-loop thread.

Support multiple simultaneous client connections
through non-blocking, multiplexed I/O.

### Rationale

Sequential command execution simplifies:

- Keyspace mutation atomicity.
- Ordering of client operations.
- TTL interactions.
- Eviction state updates.
- Initial persistence coordination.
- Data-race avoidance.

### Trade-offs

Long-running commands and synchronous filesystem
operations can delay all connected clients.

A future persistence worker is permissible only
with explicit sequencing and durability guarantees.

Do not claim multithreaded command execution.

---

## ADR-003 — Platform-Specific Event Multiplexing

**Status:** Accepted
**Implementation:** Native kqueue/epoll, connection dispatch and Phase 1G smoke-serving integration implemented

### Decision

Use kqueue on macOS and epoll on Linux.

Provide a common poller abstraction.

### Rationale

Both platforms expose efficient readiness-based
network event notification.

A shared interface preserves server-level architecture
while permitting native platform integrations.

### Consequences

Both implementations require independent testing.

Readiness notifications do not guarantee that an
entire request can be read or response can be written.

Handle EAGAIN, EINTR, EOF, errors, socket lifetimes
and registration changes correctly.

Readiness and registration semantics were validated
through Phase 1F. Phase 1G adds a continuous executable
smoke loop and external multi-client integration tests.
Final Phase 1G acceptance is still pending.

---

## ADR-004 — RESP2 Protocol Compatibility

**Status:** Accepted
**Implementation:** Not started

### Decision

Implement a documented subset of Redis RESP2.

Use incremental parsing and binary-safe value handling.

### Rationale

Standard Redis clients should be able to communicate
with the server for implemented commands.

### Consequences

Support fragmented input and pipelined commands.

Preserve response ordering.

Enforce maximum request, array, bulk-string and
nesting limits.

Do not advertise unsupported Redis commands,
RESP3 behavior or full Redis compatibility.

Client handshake compatibility must be tested
against real clients rather than assumed.

---

## ADR-005 — In-Memory Primary Keyspace

**Status:** Accepted
**Implementation:** Not started

### Decision

Use a hash-table-backed authoritative active keyspace.

An initial candidate representation is:

    std::unordered_map<std::string, Entry>

Use a type-safe value model as collection support
is introduced.

### Rationale

The primary target is Redis-like key/value access,
not disk-oriented relational or range-query storage.

### Consequences

Average-case key lookup is efficient.

Actual memory usage includes allocation, metadata
and container overhead.

The storage engine must support expiration,
accounting, eviction and persistence integration.

No B+ trees, LSM trees or disk-page indexing
are included in the project.

---

## ADR-006 — Explicit Persistence Durability

**Status:** Accepted
**Implementation:** Not started

### Decision

Implement append-only persistence with configurable
synchronization policies:

- always
- everysec
- no

Implement snapshotting and crash recovery.

### Required Invariants

Distinguish these events:

1. Mutation validated.
2. Mutation represented in the log.
3. Log write completed.
4. Required synchronization completed.
5. Authoritative memory updated.
6. Successful response acknowledged.

These events are not interchangeable.

For the strict always policy, do not acknowledge
successful durable mutation before the required
log synchronization succeeds.

For weaker policies, document potential loss of
recently acknowledged writes.

### Critical Design Concern

The exact mutation lifecycle must be finalized
in Phase 8 before implementation.

A naive "apply in memory, then append" sequence
can acknowledge or expose state inconsistently
when persistence fails.

A naive "append, sync, then apply" sequence also
needs careful handling when preparing or applying
a mutation can fail.

Commands whose results depend on existing state,
expiration or eviction require deterministic logging.

Do not claim transaction-level crash atomicity
before testing the actual design.

---

## ADR-007 — Correctness Before Optimization

**Status:** Accepted
**Implementation:** Ongoing process

### Decision

Require correctness and failure-path tests before
performance optimization.

### Required Workflow

1. Define expected semantics.
2. Implement the minimal correct behavior.
3. Add unit and integration tests.
4. Exercise failure paths.
5. Establish a performance baseline.
6. Profile actual bottlenecks.
7. Optimize a specific problem.
8. Re-run correctness and benchmarks.

### Consequences

Performance claims must use recorded measurements.

Comparisons with Redis require equivalent workloads
and comparable configurations.

No invented throughput, latency or memory figures.

---

## ADR-008 — Redis as Reference, Not Implementation

**Status:** Accepted
**Implementation:** Differential testing pending

### Decision

Implement the database server from scratch.

Redis may be used as:

- Protocol reference.
- Semantic compatibility reference.
- Differential-test oracle.
- Benchmark comparison target.

Do not embed or repackage Redis as the database engine.

### Consequences

Document every implemented command's syntax,
responses, errors, atomicity, TTL and persistence
behavior.

Compatibility differences must be explicit.

---

## ADR-009 — Expiration Time Model

**Status:** Accepted
**Implementation:** Not started

### Decision

Combine lazy expiration and bounded active expiration.

Use an injectable clock abstraction for tests.

Use appropriate monotonic time for in-process
scheduling and wall-clock deadlines for
persisted expiration state.

### Rationale

Expired keys must be invisible even if an
active cleanup cycle has not removed them.

Persistent deadlines must retain their meaning
across application restarts.

### Consequences

Do not serialize steady_clock timestamps.

Account for clock changes when restoring TTLs.

Ensure overwrites, deletes, collection operations
and recovery interact consistently with expiration.

---

## ADR-010 — Memory Accounting and Eviction

**Status:** Accepted
**Implementation:** Not started

### Decision

Start with configurable data-memory accounting
and the noeviction policy.

Then implement one documented LRU-style policy.

### Consequences

Reject eligible memory-increasing writes when
noeviction limits are reached.

Failed writes must not partially modify the
authoritative keyspace.

Account for key, value and metadata costs,
while recognizing that estimates differ from RSS.

Track network buffers separately.

Decide exact versus approximate LRU based on
measured implementation complexity and overhead.

Never describe approximate LRU as exact LRU.

---

## ADR-011 — Portable, Versioned Snapshots

**Status:** Accepted
**Implementation:** Not started

### Decision

Use a versioned snapshot format with integrity checks.

Do not dump raw in-memory C++ object layouts.

### Required Properties

- Format identification.
- Supported version metadata.
- Explicit record framing.
- Binary-safe keys and values.
- Persistent expiration deadlines.
- Snapshot/AOF coordination metadata.
- Corruption detection.
- Controlled publication and recovery.

### Publication

Evaluate temporary-file writing, file synchronization,
atomic rename, and directory synchronization.

Preserve a previously valid recovery generation
until the replacement is safely published.

A successful rename alone does not establish
power-loss durability.

---

## ADR-012 — Conservative Private Deployment

**Status:** Accepted
**Implementation:** Not started

### Decision

Complete the mandatory Linux/Docker milestone.

Then perform a private remote Linux deployment.

The remote service must not expose an unauthenticated
Redis-compatible database port to the public Internet.

### Requirements

- Persistent mounted storage.
- Restricted connectivity.
- Non-root container execution.
- Verified startup and restart recovery.
- SSH-tunnel demonstration.
- Deployment and cleanup instructions.

### Consequences

Container networking must distinguish the service
bind address from the host's published port.

Local-loopback-only defaults must not be silently
changed into public remote exposure.

Any paid infrastructure requires approval.

---

## ADR-013 — Transport-Only Smoke Serving Mode

**Status:** Accepted
**Implementation:** Phase 1G implementation complete;
final acceptance pending

### Decision

Expose an explicit `--smoke-echo` executable mode to
exercise the networking stack before RESP2 is built.

Bind only to loopback. Support ephemeral test ports,
bounded admission and a single native polling thread.

Echo arbitrary input bytes without interpreting them
as Redis commands or protocol frames.

### Rationale

External-process tests must exercise the real serving
loop, socket ownership, failure isolation and shutdown
behavior without inventing premature Redis compatibility.

### Consequences

- Keep smoke mode explicitly labeled as a test facility.
- Do not claim `redis-cli` compatibility.
- Preserve platform-specific kqueue/epoll validation.
- Test abrupt disconnects and accepted-socket setup races.
- Keep the production protocol pathway for later phases.
- Do not weaken connection lifecycle failure handling.

This decision does not replace the Phase 2 RESP2 design
or the Phase 3 command-dispatch architecture.

---

# Outstanding Implementation Decisions

These questions are intentionally deferred to their
relevant phases rather than decided prematurely.

## Phase 1

Implemented and validated through Phase 1F:

- Portable readiness-event representation.
- Level-triggered kqueue and epoll backends.
- RAII descriptor ownership.
- Registration-token-safe connection lifecycle.
- Bounded I/O, idle handling and backpressure.

Phase 1G implemented the external transport smoke
tests and serving loop. Remaining work is CI acceptance,
documentation verification and PR #3 merge.

## Phase 2

- Parser state representation.
- Buffer compaction strategy.
- Default protocol resource limits.
- Malformed-frame connection policy.

## Phases 4–6

- Final value ownership and container types.
- Mutation result representation.
- Collection memory-accounting strategy.
- Exact supported command option matrix.

## Phase 7

- Exact versus approximate LRU.
- Memory estimation formulas.
- Write reservation and eviction coordination.

## Phases 8–10

- Canonical mutation log representation.
- Failed-write behavior and rollback boundaries.
- AOF record framing and integrity.
- Synchronization scheduling.
- Snapshot generation and log offsets.
- AOF rewrite/compaction strategy.
- Recovery rules for incomplete final records.

## Phases 11–12

- Configurable buffer thresholds.
- Connection limit defaults.
- Configuration file format.
- Metrics representation and INFO schema.

## Phases 14–16

- Benchmark harness details.
- Linux image base and runtime dependencies.
- Container network binding.
- Remote deployment provider and cost.

All such decisions require tests and documentation
before the corresponding acceptance gate passes.

# Explicit Non-Goals

- Full Redis compatibility.
- Redis Cluster and replication.
- Raft or distributed consensus.
- SQL or relational queries.
- B+ trees and LSM storage.
- Lua scripting and Redis Modules.
- RESP3 completeness.
- Pub/Sub and Streams.
- Web frontend or dashboard.
- Kubernetes.
- Multi-region deployment.

# Decision Maintenance

Record changes with:

- Decision ID.
- Date.
- Previous decision.
- New decision.
- Technical justification.
- Consequences.
- Affected phases.
- Relevant tests and commits.

Do not silently alter previously accepted decisions.
