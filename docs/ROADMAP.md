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

**Status:** IN PROGRESS

- [x] 1A — RAII socket abstraction
- [x] 1B — TCP listener
- [x] 1C — Event-loop interface
- [x] 1D — macOS kqueue implementation
- [x] 1E — Linux epoll implementation
- [x] 1F — Connection lifecycle
- [ ] 1G — Multi-client smoke tests

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

Phase 1F is complete. Phase 1G has not started.
Phase 1 overall remains IN PROGRESS.

The redisdb-server executable remains a bootstrap CLI.
Continuous serving and external multi-client smoke
tests are not yet implemented.

### Acceptance Gate

Stable non-blocking TCP server handling multiple
simultaneous connections without resource leaks.

# Phase 2 — RESP2 Protocol Engine

**Status:** NOT STARTED

- [ ] 2A — RESP value model
- [ ] 2B — Response serializer
- [ ] 2C — Incremental parser
- [ ] 2D — Fragmented request handling
- [ ] 2E — Pipelining
- [ ] 2F — Malformed-input and resource-limit handling
- [ ] 2G — Parser fuzz-test foundation

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
