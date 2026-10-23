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
**Implementation:** RESP2 parsing, framed pipelining and the
loopback-only serialized command server are live. Phases 3–5 implement
connectivity/string/key/TTL behavior; 6B provides six list commands;
6C provides seven hash commands after exact-head local and Linux CI.
Other Redis commands and RESP3 remain unsupported.

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
**Implementation:** Phase 4A keyspace and Phase 6A variant, Phase 6B typed lists, Phase 6C typed hashes and Phase 6D binary-safe sets are implemented. Phase 6D is formally closed at `281d044`. Phase 6E cross-type WRONGTYPE acceptance tests merged on main as `37aaad4` with exact-head Linux networking/sanitizer CI passing; the Phase 6E docs closure remains gated. Memory accounting, differential tests and persistence remain future milestones.

### Decision

Use a hash-table-backed authoritative active keyspace.

An initial candidate representation is:

    std::unordered_map<std::string, Entry>

Use a type-safe value model as collection support
is introduced.

Phase 6A concrete selection (2026-10-23):

- `StringValue`: owned `std::string` bytes (existing contract).
- `ListValue`: `std::list<std::string>`, preserving order and duplicates,
  constant-time insertion/removal at both ends and non-throwing moves.
  Per-element node allocation and cache locality are later benchmark concerns.
- `HashValue`: `std::unordered_map<std::string, std::string>`, unique fields
  and last-write-wins field replacement; no iteration-order promise.
- `SetValue`: `std::unordered_set<std::string>`, unique byte-exact members;
  no iteration-order promise.

All nested bytes are owned, including empty, NUL and non-UTF8 values. The
copyable variant supports existing `Database::set_many` clone-and-swap.
The stored `Entry` deadline remains absolute wall time. Phase 6A does not
open collection-valued database insertion or RESP2 collection commands;
wrong-type safety on reachable multi-type entries must be implemented as
those APIs are introduced in subsequent Phase 6 subphases.

Phase 6B accepted continuation (2026-10-23): lists now have process-owned
binary-safe operations, with whole-list staging on pushes to retain strong
exception safety. Mutations preserve existing expiration deadlines; last-pop
deletes the key and its index record. Existing string routes use checked
variant access and issue `WRONGTYPE` on lists; MGET returns nil for non-string
keys, while SET/MSET replace typed keys with strings. The six basic list
commands are registered on the single-threaded RESP2 execution path.
LRANGE uses inclusive negative index normalization, overflow-safe bounds,
and a premeasured 8,192-byte response limit. Per-element wire input is
bounded to 8,176 bytes. The list node allocation and repeated-copy costs
remain deliberate performance tradeoffs for Phase 14 to measure.

Acceptance: feature commits `18e27a0`, `00dddb0`, direct implementation
merge `7df2e08`, exact-head post-merge Linux Debug/Release `39973111123`,
Clang 18 sanitizer/fuzz `39973111117` — all PASS. Hash/set routes,
comprehensive Phase 6E type errors, eviction and persistence are not yet
implemented. The separate Phase 6B documentation closure remains pending.

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
**Implementation:** Phase 5A–5H COMPLETE AND ACCEPTED (audited baseline `479fdf4`)

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


### Phase 5 closure note (2026-10-23)

Phase 5 implements injectable wall/monotonic time, lazy expiration and
bounded indexed active expiration (maximum 32 due records per event-loop
maintenance turn, nominal 100 ms cadence). Deadline indexing and metadata
are maintained through overwrites, mutations, MSET and deletes. Fixed-seed
model testing, exact deadline tests and post-merge Linux sanitizer/fuzz CI
passed. Serializing/restoring TTL across restarts remains deferred to
AOF/snapshot/recovery phases; no disk durability is implied by this ADR.

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

**Status: Phase 2G implemented, merged, post-merge validated and formally recorded COMPLETE.**

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
Closure-documentation commit
`09f1fc77f14dfd3b4f12dfb17148b60bbfe0a94b` passed
main Linux Debug/Release CI #39772960967 (18/18 each,
existing repetition gates). Phase 2G / Phase 2 COMPLETE status
has now been recorded; final status commit `e7dfbbf5`
passed Linux Debug/Release #39774474442 before Phase 3A.

# Phase 3A Command Representation Implementation Record (2026-10-22)

**Status:** Phase 3A command representation COMPLETE;
final status-commit `bb9610e4422dac7fa383a3473abf2e67dbe1766b`
passed Linux Debug/Release CI #39792311541 (19/19 each).

- Preserve RESP2 `RespValue` as protocol syntax; introduce a
  distinct owned Command type at the request-to-command boundary.
- Require a nonempty array of bulk strings; normalize only ASCII
  command-name bytes. Keep arguments byte-for-byte binary-safe.
- Report structural decoding failure as a typed outcome without
  producing partial commands. Defer registry lookup, arity,
  execution and error responses to Phases 3B-3F.
- Keep the CLI executable transport-only and do not change the
  parser, pipeline, persistence or storage layers in Phase 3A.
- Extend the fuzz workflow discovery guard to accommodate new
  tests while requiring the command unit and fuzz smoke tests.

Evidence: implementation commit `6c402b8a929b695f9abd51949c5d3967f1ce95c9`,
CI repair `9d64403925b62b108ee1172c06ad9b866592d509`,
merged PR #11 as `4b493ac7984b4c3bdf47a24d41b517930cd689b3`.
PR-head Linux Debug/Release #39784944527: 19/19 each;
Clang 18 ASan+UBSan #39784944465: 20/20, 50 seeds,
10,000 runs. Post-merge Linux main #39786009239:
Debug/Release 19/19 each plus repeat gates.
Local macOS Debug/Release 19/19 each on synchronized main.
Five-document closure checkpoint
`c369fdf3c223361bfd7a7966dc3228d16b8ec2b7` passed
Linux Debug/Release CI #39789969583 (19/19 each);
Phase 3A COMPLETE with final status-commit CI accepted.

This record documents implementation of existing architectural
boundaries; it does not silently amend ADR-002, ADR-004 or ADR-015.

# Phase 3B Dispatcher Implementation Record (2026-10-22)

**Status:** Isolated dispatcher COMPLETE status recorded: implementation,
merge, cross-platform validation and five-document acceptance accepted;
final 3B.2C status-commit Linux CI verification pending.

- Preserve ADR-002's planned single-threaded command-execution model:
  Phase 3B does not introduce a thread-safe execution guarantee.
- Retain explicit registry construction (not a giant switch or global
  registry), normalized printable-ASCII registration names and strict
  rejection of empty, duplicated or uncallable handler registrations.
- Route existing owned `Command` objects without mutating arguments,
  parsing RESP2 again, or accessing socket/connection/keyspace state.
- Return an owned typed `HandlerResult` with RESP2 value and future
  `ConnectionDisposition`, or a typed `UnknownCommand` outcome;
  do not invent Redis wire error semantics in this phase.
- Invoke a matching handler exactly once; propagate handler exceptions
  without converting to unknown-command or retrying.
- Defer real handler arity (3C), connectivity commands (3D), CLI
  compatibility (3E), wire error formatting (3F) and production
  connection integration. The executable remains transport-only.

Evidence: retained feature SHA
`e92aaf54cd00e090620be64ede21f32cde7e0549`, merged PR #12
`84d95500545eb4c0a417cd95cc98c62e30384424`; feature-head
Linux CI #39805019741 (20/20 Debug and Release), Clang 18 fuzz
#39805019351 (21/21, 50 seeds, 10,000 runs), post-merge main Linux
#39806894796 (20/20 Debug and Release plus repeat gates). Clean
synchronized local macOS Debug and Release 20/20 plus repeat gates
and immutable 50-seed UBSan corpus replay.

This is an implementation record within accepted architectural
boundaries, not an amendment to ADR-002, ADR-004 or ADR-015.
3B.2B five-document acceptance commit
`cc88f88948acd96574479750eefc7ede6445c70a` passed exact-head
Linux Debug/Release CI #39810737185 (20/20 each and repeat gates).
Phase 3B closure CI #39811900890 passed. Phase 3C was subsequently implemented and accepted.

# Phase 3C Implementation Record (2026-10-22)

No new ADR is required.

The isolated count-only validator implements the
previously approved Phase 3C design:

- Caller-selected policies, no new command registry.
- Typed count errors and configuration exceptions.
- Precedence: invalid policy, too few, too many,
  invalid argument group.
- No argument-byte inspection or mutation.
- No dispatcher, RESP2 or server integration changes.

Feature `38c5354` merged directly as `d733daa`.
Post-merge Linux #39828953490 and fuzz
#39829737702 both passed. macOS Debug/Release
and UBSan-enabled regressions also passed.

# Phase 6C Hash Storage and RESP2 Implementation Record (2026-10-23)

No new architecture decision record is necessary: Phase 6C implements
ADR-002 (serialized execution), ADR-004 (bounded RESP2 subset) and
ADR-005 (typed, authoritative hash-table keyspace).

- **Representation:** `HashValue::fields` owns a binary-safe
  `std::unordered_map<std::string, std::string>`; order is unspecified.
  Field overwrites use last-write-wins, and HSET counts newly inserted
  distinct fields rather than total arguments.
- **Exception and type safety:** mutations stage owned replacements
  before updating the authoritative entry. Wrong types produce `WRONGTYPE`
  without silently rewriting strings/lists; SET/MSET may replace a hash,
  and MGET returns nil for non-string keys as already specified.
- **Expiration:** modifying fields preserves the existing key deadline;
  removing the final field removes the key and its expiry-index record.
  Lazy expiration continues to treat expired hashes as missing.
- **Protocol boundary:** only HSET, HGET, HEXISTS, HLEN, HDEL, HMGET and
  HGETALL are supported; requests and outputs remain bounded. HMGET has
  positional nil results; HGETALL iteration order is intentionally
  unspecified, and the entire RESP2 HGETALL response is budget checked.
- **Validation:** live TCP, multiple clients, pipelines, fragmented
  requests, binary field/value bytes, wrong-type/TTL cases, optional
  redis-cli, reply limits and existing regressions. macOS Debug/Release
  59/59 each; fuzz-enabled 60/60. Feature CI `39983170325` and
  `39983170439`; implementation-main CI `39984084722` and `39984084843`
  passed at the exact accepted SHA.
- **Delivery:** storage commit `0a6a560`, command commit `88817b2`,
  two-parent merge `7b0abaa20b65b6095dd432784aced4a16fc70d32`;
  nine-file implementation diff and retained feature branch; no PR,
  history rewrite or force push. Formal five-document closure CI follows.

These are accepted implementation choices, not claims of a complete
Redis hash API, persistence, eviction, general Phase 6E wrong-type
matrix, set command support or performance benchmarks.

# Phase 6E WRONGTYPE Cross-Type Acceptance Record (2026-10-24)

This phase verifies the compatibility behavior already selected under
ADR-002 (serialized command execution), ADR-004 (bounded RESP2 subset)
and ADR-005 (authoritative typed keyspace). No new architectural
decision was introduced and no production C++ command implementation,
protocol handling or CI workflow was changed.

- **Rejection contract:** The 26 typed command signatures reject all
  three incompatible stored types with the exact WRONGTYPE response.
  Each rejected operation preserves the full prior value/type/TTL.
- **Compatibility exceptions:** SET and MSET deliberately replace the
  previous type; MGET returns nil for non-string entries. Missing keys
  retain each command's documented empty or zero-like semantics.
- **Evidence:** The two new permanent CTests are `unit.wrongtype_matrix`
  and `integration.resp_wrongtype_matrix`, each covering 78 typed
  rejection cases plus compatibility, mutation-integrity and pipeline
  checks. macOS Debug 64/64, Release 64/64, fuzz-enabled 65/65 passed.
- **Git history:** feature `a66128b077cfbfc22fbe6daca1e053e5fdaa111e`
  on `phase6e/wrongtype-acceptance`, direct two-parent implementation
  merge `37aaad4068ca0cf4452635737bd219a11d120d4c`, matching tree
  `2ff6ff4be49c6b81cd787a8c5a4ec4b992b10712`, four modified and
  two added CMake/docs/tests paths; runtime source unchanged.
- **Linux CI:** feature `40004381493` (networking) / `40004381506`
  (Clang 18 libFuzzer ASan/UBSan), and post-implementation main
  `40005159337` / `40005159401` passed every job and step.
- **Not yet accepted:** separate five-document closure branch, docs
  branch CI, two-parent documentation merge and final main CI.
  Phase 6F memory-accounting hooks and Phase 6G differential tests
  remain future scope, as do eviction and persistence.

# Phase 6D Set Storage and RESP2 Implementation Record (2026-10-23)

The accepted implementation continues ADR-002 (serialized commands),
ADR-004 (bounded RESP2 subset) and ADR-005 (authoritative typed keyspace),
without adding a new incompatible architecture decision.

- **Representation:** `SetValue::members` owns an unordered collection
  of exact binary byte strings. Set cardinality counts unique members;
  iteration order is deliberately unspecified.
- **Exception safety:** SADD and SREM use staged owned replacements;
  wrong-type access rejects the operation without rewriting prior values.
  Missing-key reads are empty/zero; no empty set is kept after SREM.
- **Expiration:** surviving set mutations keep the existing deadline and
  expiry-index entry. Removing the last member removes both key and index
  entry. The existing lazy/active expiry mechanisms remain unchanged.
- **Wire contract:** six supported routes SADD/SREM/SISMEMBER/SCARD/
  SMEMBERS/SMISMEMBER, with an 8,192-byte RESP2 reply ceiling and
  8,176-byte reply-safe member limit for SADD. SMEMBERS is unordered;
  SMISMEMBER returns 0/1 in requested argument order.
- **Evidence:** first commit `743261636c2926f40b56f61856bcb9725e8e9b72`,
  second commit `40657326e93631d8851c780424a730389b1f2cbb`,
  retained branch `phase6d/set-storage-commands`, direct two-parent merge
  `d21421ad910a179ea877fc7f1e555688ca8a81a8` with nine source/test/
  build files; feature/merge tree `e1cadc1b2c86b24d37ce6819bd3926c87eda507d`.
  macOS 62/62 Debug, 62/62 Release, 63/63 fuzz-enabled; exact-head feature
  Linux `39995306323` / `39995306807` and main Linux `39996097334` /
  `39996097046` passed, including all jobs and steps.
- **Exclusions:** set algebra, pop/random sampling, scans, full Redis set
  options, Phase 6E wrong-type matrix, memory accounting, eviction and
  disk persistence remain separate work. The formal docs-only closure,
  its Linux CI and final main acceptance are still gated.

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
