# Development Roadmap

## Redis-Compatible In-Memory Database Server

**Language:** C++20
**Build:** CMake / Ninja
**Local platform:** macOS ARM64
**Target platform:** Linux
**Repository:** `sinthetaaa/redis-compatible-in-memory-database`

This document preserves the approved 20-phase implementation
roadmap, including all 169 subphases.

## Execution Policy

1. Complete subphases in order.
2. Implement one controlled milestone at a time.
3. Validate changes before committing.
4. Use Conventional Commits.
5. Push completed milestones to GitHub.
6. Maintain `docs/PROJECT_STATE.md`.
7. Do not skip acceptance gates.
8. Record actual results rather than expected results.
9. Avoid expanding the approved feature scope silently.
10. Maintain Linux portability throughout development.

## Status Legend

- COMPLETE: Implementation, validation, commit and push complete.
- IN PROGRESS: Work has begun but the gate has not passed.
- NOT STARTED: No milestone implementation yet.
- BLOCKED: A documented prerequisite prevents progress.

# Phase 0 — Repository and Environment Foundation

**Status:** COMPLETE

- [x] 0A — Environment audit
- [x] 0B — Repository initialization
- [x] 0C — C++20 build foundation
- [x] 0D — Project documentation foundation
- [x] 0E — Baseline validation

### Acceptance Gate

- Reproducible project skeleton.
- Working Debug and Release builds.
- Working test system.
- GitHub repository.
- Committed project-state documentation.
- Clean, synchronized main branch.

# Phase 1 — Networking Foundation

**Status:** COMPLETE

- [x] 1A — RAII socket abstraction
- [x] 1B — TCP listener
- [x] 1C — Event-loop interface
- [x] 1D — macOS kqueue implementation
- [x] 1E — Linux epoll implementation
- [x] 1F — Connection lifecycle
- [x] 1G — Multi-client smoke tests

### Phase 1F Detailed Progress

- 1F-A: COMPLETE — RAII Connection ownership.
- 1F-B: COMPLETE — ConnectionManager admission and registration.
- 1F-C: COMPLETE — Readiness dispatch and bounded I/O.
- 1F-D: COMPLETE — Idle eviction, backpressure and error isolation.
- 1F-E.1: COMPLETE — Enhanced Linux CI and test-selection correction.
- 1F-E.2: COMPLETE — Documentation synchronized and validated.
- 1F-E.3: COMPLETE — Final acceptance passed and PR #2 merged.

The networking library passed native macOS and Linux
integration tests. Phase 1F was merged through PR #2
on 2026-10-19.

Merge commit:
290bc71035bbaa0e93a6c0d12a228f7eb9ff4427

Post-merge Linux Debug and Release CI:
39271745832 — SUCCESS.

Phase 1F and Phase 1G are both complete.
Phase 1 Networking Foundation is COMPLETE.

### Phase 1G Detailed Progress

- 1G-A: COMPLETE — Clean baseline and feature branch.
- 1G-B: COMPLETE — Explicit native TCP smoke serving loop.
- 1G-C: COMPLETE — Permanent external multi-client CTest
  integration and initial cross-platform CI.
- 1G-D: COMPLETE — Reset isolation, connection churn,
  deterministic fairness tests and Linux CI acceptance.
- 1G-E: COMPLETE — Documentation, PR #3 merge and
  post-merge cross-platform acceptance.

Implementation commits:

- 4b94facef34a6760a17f71efe18e384c68593ef4 —
  smoke server and permanent multi-client integration.
- f7ed0d5564e411f6598015748a9256f6c85d6745 —
  reset hardening, churn tests and fairness-test corrections.

PR #3: MERGED on 2026-10-19.
Merge commit:
adf021060d7b758ce9224d7d4ac8fe6ed8841b75.

macOS Debug and Release: 10/10 tests passed.
Dispatch repetitions: 100/100 in both configurations.
Smoke repetitions: 10/10 in both configurations.
Repeated full Release suites: 5/5 passed.

Linux workflow 39283579738 failed because the new
CI smoke-test step contained incorrect Bash line
continuations. The full Linux test suites passed.

Commit 3db387d corrected the workflow syntax.

Linux workflow 39288146570: SUCCESS.

Verified in both Linux Debug and Release:

- Full CTest suite: 10/10 PASS.
- TCP smoke repetitions: 10/10 PASS.
- Connection manager repetitions: 20/20 PASS.
- Connection dispatch repetitions: 20/20 PASS.
- Native epoll repetitions: 15/15 PASS.
- Executable version check: PASS.

Final PR-head Linux acceptance:
39289273326 — SUCCESS.

Post-merge Linux Debug and Release CI:
39290777669 — SUCCESS.

Both configurations passed the complete 10-test suite,
10 external smoke repetitions, 20 repetitions each of
connection-manager and connection-dispatch integration,
and 15 native epoll repetitions.

The executable has a loopback-only transport echo mode.
It does not implement RESP2 or Redis database commands.

Phase 1 networking acceptance: COMPLETE.
Phases 2A and 2B COMPLETE; Phase 2C Parser IN PROGRESS.

### Acceptance Gate

Stable non-blocking TCP server handling multiple
simultaneous connections without resource leaks.

# Phase 2 — RESP2 Protocol Engine

**Status:** IN PROGRESS

- [x] 2A — RESP value model
- [x] 2B — Response serializer
- [ ] 2C — Incremental parser
- [ ] 2D — Fragmented request handling
- [ ] 2E — Pipelining
- [ ] 2F — Malformed-input and resource-limit handling
- [ ] 2G — Parser fuzz-test foundation

### Phase 2A Detailed Progress

- 2A.0: COMPLETE — Clean Phase 1 baseline and
  dedicated feature branch established.
- 2A.1: COMPLETE — Owned RESP2 value representation
  implemented and validated on macOS.
- 2A.1b: COMPLETE — Nested binary deep-copy
  regression added, tested, committed and pushed.
- 2A.2: COMPLETE — Draft PR #4 created and
  Linux Debug/Release validation passed.
- 2A.3: COMPLETE — Documentation synchronized,
  final PR-head Linux validation passed.
- 2A.4: COMPLETE — PR #4 merged into main and
  macOS/Linux post-merge acceptance passed.
- 2A.5: COMPLETE — Formal milestone closure recorded.

Implementation commit:
7981c8c428f64eba76158029d56e7aca81c6a38b.

PR #4:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/4

Initial Linux workflow 39297170492: SUCCESS.
Final PR-head workflow 39298460768: SUCCESS.

PR #4: MERGED.
Merge commit:
861f0491fdb8af490ed6da820a8024541f26f9fa.

Post-merge Linux workflow 39304412144: SUCCESS.

Both Linux Debug and Release discovered and passed
all 11 CTest tests, including unit.resp_value.
Existing TCP smoke, lifecycle and epoll regression
steps also passed.

RESP2 value categories:

- Simple strings and error replies.
- Signed 64-bit integers.
- Binary-safe bulk strings.
- Null bulk strings.
- Recursive arrays.
- Null arrays.

Phase 2A: COMPLETE.

The owned value model is validated on macOS and
Linux in Debug and Release configurations.

At Phase 2A closure, neither serializer nor
parser was implemented. The running server still
remains transport-smoke-only.

At Phase 2A closure, Phase 2B was next.
The following Phase 2B record captures its
subsequent implementation and completion.

### Phase 2B Detailed Progress

- 2B.0: COMPLETE — Clean Phase 2A closure baseline
  verified and dedicated serializer branch created.
- 2B.1: COMPLETE — Two-pass bounded serializer
  implemented and locally tested.
- 2B.1b: COMPLETE — Boundary regressions added,
  implementation committed and pushed.
- 2B.2: COMPLETE — Draft PR #5 created; Linux
  Debug and Release validation passed.
- 2B.3: COMPLETE — Documentation synchronized
  and final PR-head Linux acceptance passed.
- 2B.4: COMPLETE — PR #5 merged into main;
  macOS and Linux post-merge tests passed.
- 2B.5: COMPLETE — Formal milestone closure recorded.

Implementation commit:
7835524ca00acd02e9ec16f8bd297b59b750e2c9.

PR #5:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/5

Initial Linux workflow 39311216460: SUCCESS.
Final PR-head workflow 39314875934: SUCCESS.

PR #5: MERGED.
Merge commit:
0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.

Post-merge Linux workflow 39320475360: SUCCESS.

Both Linux Debug and Release passed 12/12 tests.
The serializer unit test and existing networking
regressions passed.

Serializer implementation:

- RESP2 framing for all seven value categories.
- Binary-safe owned response bytes.
- Decimal encoding with std::to_chars.
- Recursive array encoding.
- Two-pass measurement and writing.
- Checked cumulative response-size accounting.
- Output, array-element and nesting-depth limits.

Phase 2B: COMPLETE.

The standalone bounded serializer is validated
on macOS and Linux in Debug and Release.

Phase 2C — Incremental Parser: IN PROGRESS.
Phase 2D through 2G: NOT STARTED.

The standalone parser has been implemented, but
is not integrated with the running TCP server.

### Phase 2C Detailed Progress

- 2C.0: COMPLETE — Phase 2B closure baseline
  verified and parser feature branch created.
- 2C.1: COMPLETE — Bounded single-frame parser
  implemented and validated locally.
- 2C.1b: COMPLETE — Round-trip and boundary
  regressions added; implementation committed
  and pushed after correcting a test namespace.
- 2C.2: COMPLETE — Draft PR #6 created and
  initial Linux Debug/Release acceptance passed.
- 2C.3: IN PROGRESS — Documentation synchronization,
  final PR-head CI and source review.
- 2C.4: NOT STARTED — PR merge and post-merge
  macOS/Linux acceptance.
- 2C.5: NOT STARTED — Formal milestone closure.

Implementation commit:
82b7dc98dbb99c83146d95c90fb3feb1408e4a8a.

PR #6:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/6

Initial Linux workflow 39327039720: SUCCESS.

Linux Debug and Release each passed all 13 tests,
including unit.resp_parser and unit.resp_serializer.

Existing TCP smoke, connection lifecycle and
native epoll regression steps passed.

Parser implementation:

- Stateless, one-frame RESP2 decoding.
- Complete, NeedMore and Invalid result states.
- Owned RespValue on successful decoding.
- Exact consumed-byte reporting.
- Binary-safe bulk strings and recursive arrays.
- Signed 64-bit integers via std::from_chars.
- Checked lengths and configured resource bounds.
- Parser/serializer round-trip regressions.
- Incomplete prefix and invalid framing tests.

Phase 2C remains IN PROGRESS until final PR
acceptance, merge and post-merge verification.

Phase 2D through 2G remain NOT STARTED.

The server remains transport-smoke-only.
No RESP2 network execution or database storage
has been implemented.

### Acceptance Gate

Binary-safe incremental RESP2 parser and serializer
with resource limits and comprehensive edge-case tests.

# Phase 3 — Command Framework

**Status:** NOT STARTED

- [ ] 3A — Command representation
- [ ] 3B — Dispatcher
- [ ] 3C — Argument validation
- [ ] 3D — PING, ECHO, QUIT
- [ ] 3E — Redis CLI compatibility smoke test
- [ ] 3F — Unknown-command behavior

### Acceptance Gate

Standard redis-cli can connect and successfully execute
the supported connectivity commands.

# Phase 4 — Core String Database

**Status:** NOT STARTED

- [ ] 4A — Keyspace and entry model
- [ ] 4B — SET, GET
- [ ] 4C — DEL, EXISTS, TYPE
- [ ] 4D — MSET, MGET
- [ ] 4E — APPEND, STRLEN
- [ ] 4F — INCR, DECR, INCRBY, DECRBY
- [ ] 4G — Numeric edge cases and overflow
- [ ] 4H — Multi-client correctness tests

### Acceptance Gate

Supported string and key commands match the documented
Redis compatibility subset.

# Phase 5 — TTL and Expiration

**Status:** NOT STARTED

- [ ] 5A — Clock abstraction
- [ ] 5B — Expiration metadata
- [ ] 5C — Lazy expiration
- [ ] 5D — EXPIRE, PEXPIRE
- [ ] 5E — TTL, PTTL, PERSIST
- [ ] 5F — SET EX/PX
- [ ] 5G — Active expiration
- [ ] 5H — Deterministic expiration tests

### Acceptance Gate

Correct TTL semantics, invisible expired keys,
bounded active expiration and deterministic tests.

# Phase 6 — Collection Data Types

**Status:** NOT STARTED

- [ ] 6A — Type-safe value representation
- [ ] 6B — List storage and commands
- [ ] 6C — Hash storage and commands
- [ ] 6D — Set storage and commands
- [ ] 6E — Wrong-type errors
- [ ] 6F — Collection memory accounting hooks
- [ ] 6G — Differential tests

### Acceptance Gate

Correct list, hash and set operations with type safety
and verified Redis-subset compatibility.

# Phase 7 — Memory Management and Eviction

**Status:** NOT STARTED

- [ ] 7A — Memory accounting
- [ ] 7B — Configurable data-memory limit
- [ ] 7C — noeviction
- [ ] 7D — LRU-style metadata
- [ ] 7E — LRU eviction policy
- [ ] 7F — Expiration/eviction interaction
- [ ] 7G — Memory pressure tests

### Acceptance Gate

Controlled accounted-memory growth and validated
memory-limit and eviction semantics.

# Phase 8 — AOF Persistence

**Status:** NOT STARTED

- [ ] 8A — Persistence format and invariants
- [ ] 8B — AOF writer
- [ ] 8C — AOF reader
- [ ] 8D — Mutation recording
- [ ] 8E — always synchronization policy
- [ ] 8F — everysec synchronization policy
- [ ] 8G — no synchronization policy
- [ ] 8H — Persistence failure handling
- [ ] 8I — AOF replay
- [ ] 8J — Restart integration tests

### Acceptance Gate

AOF recovery works. Documented durability guarantees
match tested synchronization and acknowledgment behavior.

# Phase 9 — Snapshots and Crash Recovery

**Status:** NOT STARTED

- [ ] 9A — Snapshot format
- [ ] 9B — Snapshot writer
- [ ] 9C — Snapshot reader
- [ ] 9D — Safe snapshot publication
- [ ] 9E — Snapshot plus AOF recovery
- [ ] 9F — Recovery of TTL metadata
- [ ] 9G — Corruption handling
- [ ] 9H — Fault injection
- [ ] 9I — Abrupt termination tests
- [ ] 9J — Recovery validation

### Acceptance Gate

Correct recovery from valid state with explicit,
safe handling of incomplete or corrupted files.

# Phase 10 — Log Compaction and Persistence Lifecycle

**Status:** NOT STARTED

- [ ] 10A — AOF growth analysis
- [ ] 10B — Rewrite/compaction design
- [ ] 10C — Generation and offset coordination
- [ ] 10D — Safe file replacement
- [ ] 10E — Interrupted rewrite recovery
- [ ] 10F — Compaction correctness tests
- [ ] 10G — Recovery-time measurements

### Acceptance Gate

Safe log compaction without losing acknowledged state,
including interruption and recovery tests.

# Phase 11 — Network Robustness and Backpressure

**Status:** NOT STARTED

- [ ] 11A — Input-buffer limits
- [ ] 11B — Output-buffer limits
- [ ] 11C — Slow-reader handling
- [ ] 11D — Connection limits
- [ ] 11E — Idle connection cleanup
- [ ] 11F — Graceful shutdown
- [ ] 11G — Partial-write and disconnect tests
- [ ] 11H — High-concurrency soak tests

### Acceptance Gate

Slow, disconnected or malformed clients cannot
trivially exhaust server resources.

# Phase 12 — Configuration and Observability

**Status:** NOT STARTED

- [ ] 12A — Configuration parser
- [ ] 12B — Startup validation
- [ ] 12C — Structured logging
- [ ] 12D — Runtime counters
- [ ] 12E — INFO subset
- [ ] 12F — Selected CONFIG GET
- [ ] 12G — Operational error reporting

### Acceptance Gate

Configurable server with operational metrics,
diagnostic logs and useful errors.

# Phase 13 — Comprehensive Correctness Validation

**Status:** NOT STARTED

- [ ] 13A — Expand unit tests
- [ ] 13B — Expand TCP integration tests
- [ ] 13C — Redis differential harness
- [ ] 13D — Parser fuzzing
- [ ] 13E — AddressSanitizer
- [ ] 13F — UndefinedBehaviorSanitizer
- [ ] 13G — Persistence fault injection
- [ ] 13H — Extended soak tests
- [ ] 13I — Compatibility matrix

### Acceptance Gate

Defensible correctness coverage for all supported
commands and critical failure paths.

# Phase 14 — Performance Engineering

**Status:** NOT STARTED

- [ ] 14A — Benchmark methodology
- [ ] 14B — Baseline GET/SET benchmarks
- [ ] 14C — Mixed workload benchmarks
- [ ] 14D — Concurrent-client benchmarks
- [ ] 14E — Pipeline-depth benchmarks
- [ ] 14F — Persistence-overhead benchmarks
- [ ] 14G — Memory profiling
- [ ] 14H — CPU profiling
- [ ] 14I — Targeted optimizations
- [ ] 14J — Regression validation
- [ ] 14K — Redis comparison

### Acceptance Gate

Reproducible throughput, latency and memory
measurements with methodology and limitations.

# Phase 15 — Linux, Docker and CI

**Status:** NOT STARTED
**Deployment Milestone A: MANDATORY**

- [ ] 15A — Linux build validation
- [ ] 15B — Multi-stage Dockerfile
- [ ] 15C — Minimal runtime image
- [ ] 15D — Non-root container execution
- [ ] 15E — Persistent data volume
- [ ] 15F — Container health checks
- [ ] 15G — Graceful container shutdown
- [ ] 15H — Container restart recovery
- [ ] 15I — GitHub Actions CI
- [ ] 15J — Fresh-clone reproducibility
- [ ] 15K — Docker-based demo script

### Acceptance Gate

Reproducible Linux container build, startup, testing,
shutdown, persistence recovery and demonstration.

# Phase 16 — Private Remote Linux Deployment

**Status:** NOT STARTED
**Deployment Milestone B: PLANNED FINAL MILESTONE**

- [ ] 16A — Provider evaluation
- [ ] 16B — Server provisioning
- [ ] 16C — Secure host configuration
- [ ] 16D — Container deployment
- [ ] 16E — Persistent storage
- [ ] 16F — Process lifecycle
- [ ] 16G — Private connectivity
- [ ] 16H — SSH tunnel demonstration
- [ ] 16I — Remote command validation
- [ ] 16J — Remote restart and recovery
- [ ] 16K — Deployment documentation
- [ ] 16L — Optional shutdown

### Acceptance Gate

Verified remote Linux deployment with restricted
network access, persistent storage and recovery.

Expenses require approval before provisioning.
Never publicly expose an unauthenticated database port.

# Phase 17 — Documentation and Demo

**Status:** NOT STARTED

- [ ] 17A — Final README
- [ ] 17B — Architecture diagrams
- [ ] 17C — Protocol compatibility documentation
- [ ] 17D — Persistence guarantees
- [ ] 17E — Recovery walkthrough
- [ ] 17F — Benchmark report
- [ ] 17G — Docker quick start
- [ ] 17H — Terminal demo
- [ ] 17I — Deployment guide
- [ ] 17J — Known limitations
- [ ] 17K — Interview discussion guide

### Acceptance Gate

An engineer can understand, run and evaluate the
project from a fresh clone.

# Phase 18 — Final Engineering Audit

**Status:** NOT STARTED

- [ ] 18A — Fresh clone
- [ ] 18B — Debug build
- [ ] 18C — Release build
- [ ] 18D — Full test suite
- [ ] 18E — Sanitizers
- [ ] 18F — Differential tests
- [ ] 18G — Persistence recovery
- [ ] 18H — Docker verification
- [ ] 18I — CI status
- [ ] 18J — Documentation consistency
- [ ] 18K — Git cleanliness
- [ ] 18L — Remote HEAD verification

### Acceptance Gate

All mandatory validations pass and the repository
is reproducible, synchronized and documented.

# Phase 19 — Portfolio and Resume Finalization

**Status:** NOT STARTED

- [ ] 19A — Select strongest engineering achievements
- [ ] 19B — Verify benchmark claims
- [ ] 19C — Draft resume bullets
- [ ] 19D — Add GitHub project link
- [ ] 19E — Prepare interview explanations
- [ ] 19F — Final project lock

### Acceptance Gate

A validated, interview-ready project suitable for
a backend/SDE-1 resume.

# Completion Rules

A subphase is complete only after:

1. Intended implementation is finished.
2. Relevant tests pass.
3. Relevant failure paths are tested.
4. No known regression remains.
5. Documentation is updated.
6. Git diff is reviewed.
7. A logical commit is created.
8. The commit is pushed.
9. Remote synchronization is verified.

A phase is complete only when every required subphase
and its acceptance gate have passed.

# Mandatory Project Outcomes

- Redis-compatible documented RESP2 subset.
- Event-driven non-blocking TCP server.
- String, key and selected collection commands.
- Expiration and memory-bounded eviction.
- AOF, snapshots and crash recovery.
- Automated correctness, differential and fuzz tests.
- Reproducible performance measurements.
- Linux and Docker packaging.
- GitHub Actions CI.
- Private remote Linux deployment.
- Final engineering audit.
- Verified resume-ready technical achievements.

# Source of Truth

- Approved planning handoff: October 19, 2026.
- Live progress: `docs/PROJECT_STATE.md`.
- Engineering decisions: `docs/DECISIONS.md`.
- Architecture: `docs/ARCHITECTURE.md`.

This roadmap describes planned work. It is not
evidence that future phases have been implemented.
