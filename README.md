# Redis-Compatible In-Memory Database Server

A C++20 systems engineering project to build a
Redis-compatible in-memory database server from scratch.

The planned server combines non-blocking TCP networking,
an incremental RESP2 protocol engine, in-memory storage,
key expiration, eviction, append-only persistence,
snapshotting and crash recovery.

**Project status: Phase 1 in progress — Phase 1F Connection Lifecycle complete and merged; Phase 1G Multi-Client Smoke Tests next.**

The current executable is a bootstrap CLI.
It does not yet accept TCP connections or execute
Redis commands.

## Project Goals

The project is designed to demonstrate:

- Modern C++20 systems programming.
- Resource ownership and RAII.
- Non-blocking TCP socket programming.
- kqueue and epoll event multiplexing.
- Incremental, binary-safe protocol parsing.
- Hash-table-based database storage.
- Atomic command execution.
- Expiration and memory-bounded eviction.
- AOF durability and crash recovery.
- Reproducible performance engineering.
- Linux and Docker deployment.

These are project goals, not claims that all
features have been implemented.

## Current Functionality

Implemented and validated:

- CMake project configuration.
- C++20 core static library.
- Debug and Release build presets.
- Executable version and help commands.
- Automated baseline tests.
- Move-only POSIX file-descriptor ownership with RAII.
- Unix socket-pair ownership and cleanup tests.
- Non-blocking, loopback-only IPv4 TCP listener.
- Real TCP connection and listener integration tests.
- Portable EventPoller interface and event masks.
- Registration token and timeout contracts.
- Test-double validation of event-poller behavior.
- Native macOS kqueue implementation and integration tests.
- Native Linux epoll implementation and integration tests.
- Ubuntu Debug and Release validation through GitHub Actions.
- RAII-managed connections and bounded nonblocking I/O.
- ConnectionManager admission and registration management.
- Readiness dispatch and stale registration-token handling.
- Coalescing of separate Read/Write readiness notifications.
- Output backpressure with high and low watermarks.
- Configurable idle eviction and deadline-aware polling.
- Half-close handling and queued-output draining.
- Per-connection callback and socket failure isolation.
- Native macOS and Linux connection integration tests.
- Kernel-backed read/write readiness and hangup detection.
- Formatting and compiler-warning configuration.

Not yet implemented:

- A continuously running TCP serving loop in redisdb-server.
- Redis protocol processing.
- Database command execution.
- In-memory key/value storage.
- Expiration or eviction.
- Persistence and recovery.
- Docker deployment.

See [Project State](docs/PROJECT_STATE.md)
for the authoritative implementation status.

## Technology

| Component | Technology |
|---|---|
| Language | C++20 |
| Build system | CMake |
| Build generator | Ninja |
| Local compiler | Apple Clang |
| Development platform | macOS ARM64 |
| Target platform | Linux |
| Planned protocol | RESP2 subset |
| Planned networking | kqueue / epoll |
| Planned persistence | AOF and snapshots |
| Testing | CTest and additional planned suites |

## Prerequisites

For local macOS development:

- A C++20-capable compiler.
- CMake 3.25 or newer.
- Ninja.
- clang-format.
- Git.

Docker and Redis command-line utilities become
necessary for later integration and deployment stages.

## Build

Clone the repository:

```bash
git clone \
  https://github.com/sinthetaaa/redis-compatible-in-memory-database.git

cd redis-compatible-in-memory-database
```

Configure and build Debug:

```bash
cmake --preset debug
cmake --build --preset debug
```

Configure and build Release:

```bash
cmake --preset release
cmake --build --preset release
```

## Run the Current Executable

Print the version:

```bash
./build/debug/redisdb-server --version
```

Expected current output:

```text
redisdb-server 0.1.0-dev
```

Show the bootstrap usage:

```bash
./build/debug/redisdb-server --help
```

The TCP database service has not yet been implemented.

## Test

Run Debug tests:

```bash
ctest --preset debug --output-on-failure
```

Run Release tests:

```bash
ctest --preset release --output-on-failure
```

Check source formatting:

```bash
find include src tests -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0 |
  xargs -0 clang-format --dry-run --Werror
```

Current tests cover version reporting, descriptor
ownership, TCP listener behavior, native kqueue/epoll
readiness, connection admission, event dispatch,
backpressure, idle eviction and client failures.

Linux CI validates Debug and Release configurations,
including repeated connection-manager, dispatch and
native epoll integration tests.

Future work includes the continuously running server
entry point, external multi-client smoke tests, RESP2
parsing, command execution, persistence and recovery.

## Planned Architecture

```text
Redis Clients
     |
  TCP / RESP2
     |
Non-Blocking Network I/O
     |
Event Loop (kqueue / epoll)
     |
Connection Management
     |
Incremental RESP2 Parser
     |
Command Dispatcher
     |
In-Memory Database
     |
Persistence Coordination
     |
AOF / Snapshots / Recovery
```

The initial command executor will be single-threaded
while supporting multiple client connections.

## Planned Command Support

Connectivity:

`PING`, `ECHO`, `QUIT`

Strings:

`SET`, `GET`, `MSET`, `MGET`, `APPEND`,
`STRLEN`, `INCR`, `DECR`, `INCRBY`, `DECRBY`

Keys:

`DEL`, `EXISTS`, `TYPE`

Expiration:

`EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`

Collections:

Selected list, hash and set operations.

Introspection:

Documented subsets of `INFO` and `CONFIG GET`.

Full Redis compatibility is explicitly out of scope.

No Redis command support is implemented yet.

## Persistence Design

Planned persistence features:

- Append-only mutation recording.
- Configurable synchronization policies.
- Versioned snapshots.
- Safe snapshot publication.
- Startup recovery.
- Corruption and incomplete-write handling.
- AOF compaction.
- Controlled crash and recovery testing.

Durability guarantees will be documented after
implementation and verification.

## Performance Validation

Planned benchmark measurements include:

- Operations per second.
- P50, P95 and P99 latency.
- CPU utilization.
- Memory consumption.
- Concurrent client scaling.
- Pipeline performance.
- Persistence overhead.
- Recovery time.

Comparisons with Redis will use documented,
reproducible workloads.

No performance benchmark results are available yet.

## Deployment Plan

Two deployment milestones are planned.

**Milestone A — Mandatory**

Linux build, Docker image, persistent storage,
restart recovery and reproducible local demo.

**Milestone B — Planned Final Deployment**

Private remote Linux deployment with restricted
connectivity, persistent storage, SSH tunneling
and recovery verification.

An unauthenticated database port will not be
intentionally exposed to the public Internet.

## Documentation

- [Roadmap](docs/ROADMAP.md)
- [Project State](docs/PROJECT_STATE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Architecture Decisions](docs/DECISIONS.md)

Additional protocol, persistence, benchmarking
and deployment documents will be added as
their implementation phases begin.

## Engineering Principles

Correctness before optimization.

Reliability before performance claims.

Measured performance before portfolio claims.

All major milestones require validation,
documentation, commits and GitHub synchronization.

## Development Status

Phase 0 — Repository and Environment Foundation is complete.

Phase 1A — RAII Socket Abstraction is complete.

Phase 1B — TCP Listener is complete.

Phase 1C — Event-Loop Interface is complete.

Phase 1D — macOS kqueue is complete.

Phase 1E — Linux epoll is complete.

Phase 1F — Connection Lifecycle is complete.

Phase 1G — Multi-Client Smoke Tests is next
and has not yet started.

Current milestone information is maintained in
[PROJECT_STATE.md](docs/PROJECT_STATE.md).

## License

No license has been selected yet.

Until a license is added, the repository should
not be assumed to grant reuse permissions.
