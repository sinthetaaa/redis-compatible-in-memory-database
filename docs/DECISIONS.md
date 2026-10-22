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
Phase 1G passed Linux Debug and Release acceptance.
PR #3 merged as adf0210; post-merge CI 39290777669
also passed in both configurations.

---

## ADR-004 — RESP2 Protocol Compatibility

**Status:** Accepted
**Implementation:** Partial — Phases 2A-2E protocol library,
framing, and bounded pipeline are merged and validated through
PR #8. Phase 2F parser/framing safety and native peer-isolation
regressions are merged through PR #9, with post-merge macOS/Linux
Debug and Release validation. Production RESP2 command serving
and Redis command execution remain pending.

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
**Implementation:** Phase 1G COMPLETE;
merged and cross-platform validated

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

## ADR-014 — Reuse Connection Buffer for RESP2 Framing

**Status:** Accepted
**Implementation:** Phase 2D COMPLETE

### Decision

Use the existing owned Connection input buffer
to accumulate fragmented RESP2 frames.

Introduce a separate protocol adapter that
invokes the stateless RespParser without owning
another socket receive buffer.

Consume only fully decoded RESP2 frames.

Preserve incomplete frames without consumption.

Preserve invalid input for caller-level failure
handling rather than silently discarding it.

Choose a default 64 KiB framed-input limit,
matching the existing Connection input cap.

Require explicit coordination when using larger
custom receive and parser limits.

Keep the transport-only smoke server unchanged.

### Rationale

Connection already implements bounded nonblocking
reads and owns the persistent input string.

A second buffer would duplicate state, allocation,
and consumption responsibilities.

The small adapter isolates framing semantics
from TCP ownership and native event polling.

### Consequences

Phase 2D tests exercise real loopback TCP with
the native ConnectionManager and event poller.

Malformed clients are isolated in the test handler
through existing manager failure handling.

The adapter does not implement a production
protocol endpoint or error-response policy.

One-frame extraction is intentional.
Pipelining and ordered response dispatch remain
Phase 2E responsibilities.

Implementation commit: 4f2535a.
PR #7: MERGED.
Initial Linux workflow 39376269629: SUCCESS.
Final PR-head CI 39380341041: SUCCESS.
Merge commit: f1bc679ed707404cbc8d0a569c2eadb9af4d6687.
Post-merge CI 39381045190: SUCCESS.

---

## ADR-015 — Bounded FIFO RESP2 Pipelining and Resumable Scheduling

**Status:** Accepted and implemented; Phase 2E COMPLETE.
**Implementation:** Commits 861ebb4, 169d915 and 84d4ebc.
**Validation:** Linux CI #39420606587, #39422679453,
#39625859355 and post-merge #39627679647 SUCCESS.
**Merge:** ba97b8bcbb11edacbbb7fb823cce02fa765118e8.
**Closure:** 3315a3b; Linux CI #39629733366 SUCCESS.

### Decision

Process buffered RESP2 frames synchronously in FIFO order
without a second receive buffer. Peek before consuming a
frame. Before invoking a reply factory, reserve up to the
configured maximum reply size in the existing connection
output buffer. Queue the completed reply before consuming
the request. Bound work per drain (default 16 frames).

Keep protocol logic free of socket and poller dependencies.
Add an optional transport-independent resumable handler to
ConnectionManager instead of replacing the legacy echo path.
Use `AwaitRead`, `Continue` and `AwaitWrite` to distinguish
incomplete input, deferred CPU work and output pressure.
Cap deferred client work per polling turn (default 32).

Require progress before accepting `Continue`. Remove stale
tokens on close; isolate invalid callbacks, malformed input,
and output-cap failures to the affected connection.

### Alternatives and rationale

- Draining all frames per read was rejected because one busy
  client could monopolize the event loop.
- Requiring another socket read for buffered work was rejected
  because complete requests could remain stranded.
- Reexecuting a request while its reply is backpressured was
  rejected due to duplication and side-effect risks.
- Making ConnectionManager aware of RESP2 protocol result
  types was rejected to keep networking transport-agnostic.
- Replacing the smoke-echo handler was rejected because it
  would regress the only executable serving mode.

### Consequences and limits

The reply factory remains test-only; there is no production
Redis command executor or database endpoint. Custom parser,
reply and connection buffer limits must be coordinated.
Exceptions after side effects require a caller close policy;
automatic retry is not guaranteed safe.

The three Phase 2E pipeline test targets are validated on
macOS and Linux in Debug and Release. PR #8 was merged and
formally closed. Phase 2F hardens malformed input and parser
frame-budget compatibility (PR #9 merged, post-merge CI PASS).
Phase 2G parser fuzzing remains a separate future milestone.

---

## Phase 2F Protocol-Hardening Implementation Record

Phase 2F implements existing RESP2 compatibility and bounded
input principles; it does not change an accepted ADR or invent
new command semantics. Bulk and array lengths must use canonical
decimal spelling; signed RESP integer grammar remains distinct.
Malformed bulk terminators are rejected at the earliest known
invalid byte; valid incomplete input remains buffered.

A configured parser max_frame_bytes exceeding a connection's
max_input_bytes is rejected before any input consumption.
Bad-client failures isolate that peer within native event-loop
dispatch. Reply delivery before a fail-fast close is not promised.

Four commits `9781a7a`, `4dcab5f`, `d555b47`, `b86ee47`
were merged via PR #9 as `ce65649` on 2026-10-22.
Linux PR-head CI #39735651885 and main post-merge CI
#39737223938 succeeded in both Debug and Release.
Local post-merge macOS suites: 18/18 each; repeated protocol
and native kqueue regression gates passed.

No Redis command executor, production RESP2 endpoint, database,
persistence or fuzz-test harness is implemented by this phase.
Phase 2G parser fuzz-test foundation remains NOT STARTED.

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
tests and serving loop. Linux acceptance passed in
workflow 39288146570; PR #3 was merged and post-merge
workflow 39290777669 succeeded.

Phase 1 Networking Foundation is COMPLETE.
Phase 2A RESP2 Value Model is COMPLETE.
PR #4 was merged as 861f049 and post-merge
Linux workflow 39304412144 succeeded.

Phase 2B Response Serializer is COMPLETE.
PR #5 merged as 0fa0269.
Post-merge Linux workflow 39320475360 succeeded.

Phase 2C single-frame parsing is COMPLETE.
PR #6 merged as 68dd831.
Post-merge Linux workflow 39370627026 succeeded.

Phase 2D fragmented-input handling is COMPLETE.
PR #7 merged as f1bc679.
Post-merge Linux workflow 39381045190 succeeded.

Phase 2E pipelining is COMPLETE through PR #8; macOS/Linux
post-closure acceptance passed in CI #39629733366.
Phase 2F hardening is IMPLEMENTED and MERGED through PR #9;
post-merge Linux CI #39737223938 and macOS Debug/Release passed.
Docs closure commit `b54ab7d` passed post-closure Linux Debug/Release
CI #39739519389. Phase 2F formally COMPLETE; Phase 2G NOT STARTED.

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

# Phase 2G Fuzzing Implementation Decisions (2026-10-22)

**Status: implementation merged and post-merge accepted;
formal documentation closure and exact-head CI pending.**

- Fuzz the actual bounded production RESP2 parser and value model,
  not a mock, through a shared deterministic invariant checker.
- Use isolated opt-in CMake targets instead of changing normal
  production library or server instrumentation.
- Keep a reproducible, 50-input binary-safe seed corpus with a
  strict SHA-256 manifest; reject corrupt, missing or extra seeds.
- Limit individual fuzz inputs to 4096 bytes and parser resources
  to a smaller fixed validation profile, without changing
  production default limits or accepted RESP2 syntax.
- Use local macOS UBSan for deterministic replay because Apple
  Clang ASan hangs before `main()` on this host; do not claim
  local ASan acceptance.
- Run real Clang 18 libFuzzer with ASan+UBSan on Ubuntu 24.04,
  with 10,000 iterations, a time bound, a memory budget,
  and failure-artifact capture.
- Allow fuzzer-generated mutations only in a temporary corpus copy.
  Preserve the original manifest-verified seed set unchanged.
- Keep merge, post-merge regression and final project-state
  closure as distinct acceptance gates.

Evidence: GitHub Actions run #39754741830 at feature commit
`b7edd7540ec3d46008ee7878fb55063c26334e9f` passed
19/19 Linux CTests and 10,000 ASan+UBSan libFuzzer runs.
No production parser, TCP serving mode or database behavior
was changed by this milestone.

PR #10 was merged as true two-parent commit
`36ec43d8ce247d50c81bf90d60641d4db9b802b8`.
Final PR-head Linux Debug/Release CI #39767524677 and
ASan+UBSan fuzz CI #39767524405 both succeeded.
Post-merge Linux main CI #39770284668 passed Debug/Release
18/18 tests per build and the repeat suites; the merged tree
matches the previously fuzz-tested final PR head.
Post-merge macOS Debug/Release and 50-seed UBSan PASS.
No production semantic, Redis command, storage or networking
changes are authorized by this test-only decision.
Formal Phase 2G closure-status CI is still pending.

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
