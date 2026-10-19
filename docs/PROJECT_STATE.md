# PROJECT STATE — Redis-Compatible In-Memory Database Server

## 1. Snapshot

- Updated: 2026-10-19
- Current phase: Phase 1 — Networking Foundation
- Current subphase: 1G — Multi-client smoke tests (next; not started)
- Status: PHASE 1 IN PROGRESS
- Last completed milestone: 1F-E.3 — Final acceptance and PR #2 merge
- Exact next task: Plan Phase 1G, integrate continuous TCP serving into redisdb-server, and validate multiple simultaneous real clients.
- Implementation maturity: Cross-platform networking library with connection ownership, dispatch, idle handling, backpressure and failure isolation
- Working TCP database: NOT IMPLEMENTED

## 2. Project Identity

- Title: Redis-Compatible In-Memory Database Server
- Language: C++20
- Repository: redis-compatible-in-memory-database
- Local directory: RedisCompatibleDB
- Default server executable: redisdb-server
- Core library: redisdb-core
- Test executable: redisdb-tests
- Default planned address: 127.0.0.1
- Default planned port: 6380
- Development platform: macOS
- Target platform: Linux

## 3. Repository

- Local path: /Users/sinthetaaa/Desktop/Projects/SDE-Projects/RedisCompatibleDB
- Remote: https://github.com/sinthetaaa/redis-compatible-in-memory-database
- Branch: main
- Phase 0C commit: b2c668f180495dfc76bd47bf906ff34fe4e90b0b
- Phase 0D commit: 409eff47c627eb4d3c54846e06826b6659760446
- Phase 0D remote synchronization: PASS
- Working tree after 0D: CLEAN
- Phase 0E commit: 0cc4594d692f10f9a70ed6c9ae6107b0610a624f
- Phase 1A commit: 1981e8b806944f19783f1de30c581eae1b41871b
- Phase 1B commit: dda6c4f562cb304c2986257890a05e1dba42b8f2
- Phase 1C commit: d7dffafc6f0474ee8a4950caf16d47418f39e86e
- Phase 1D commit: 142fd98b69e01bbd62aac627732233a9d397a9a6
- Phase 1E implementation commit: 912cac4437ebed2531624da00ec3a11d7ae6bfc3
- Phase 1E merge commit: fb242b2f3d72d29b198cbc2356ccfd57319b607e.
- Phase 1E Linux CI run: 39233458374 (Debug and Release passed).
- Phase 1E pull request: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/1
- Branch tracking: origin/main
- Phase 1F-A/B: c8ec996 — Connection ownership and admission.
- Phase 1F-C: 50ead92 — Readiness dispatch.
- Phase 1F-D: 6881b04 — Lifecycle and backpressure.
- Phase 1F-D: f5bdacc — Socket failure isolation.
- Phase 1F-E.1: a01ae98 — Enhanced Linux CI.
- Phase 1F-E.1b: 51a363b — CI test-selection correction.
- Phase 1F PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/2 (MERGED).
- Phase 1F implementation tip: 748bda315b1d97b0c6a7318e927fa630cc59ffbe.
- Phase 1F merge commit: 290bc71035bbaa0e93a6c0d12a228f7eb9ff4427.
- Phase 1F post-merge Linux CI: 39271745832 (Debug and Release passed).
- Verified Linux CI run: 39256465515.
- Current HEAD and working-tree state must always be checked directly through Git.

## 4. Local Environment

- Operating system: macOS 26.5.2
- Architecture: arm64
- Physical memory: 24 GiB
- Shell: zsh
- Compiler: Apple Clang 17.0.0
- CMake: 4.1.2
- Ninja: 1.13.2
- clang-format: 23.1.3
- Git: 2.52.0
- GitHub CLI: 2.97.0
- Python: 3.13.5
- redis-cli: 8.10.2
- redis-benchmark: 8.10.2
- Docker CLI: 29.1.3
- Docker engine during 0A: NOT RUNNING
- Available disk during 0A: approximately 12 GiB
- Available disk during 0E: approximately 13 GiB
- Disk utilization during 0A: 98 percent

## 5. Roadmap Status

| Phase | Description | Status |
|---|---|---|
| 0 | Repository and Environment Foundation | COMPLETE |
| 1 | Networking Foundation | IN PROGRESS |
| 2 | RESP2 Protocol Engine | NOT STARTED |
| 3 | Command Framework | NOT STARTED |
| 4 | Core String Database | NOT STARTED |
| 5 | TTL and Expiration | NOT STARTED |
| 6 | Collection Data Types | NOT STARTED |
| 7 | Memory Management and Eviction | NOT STARTED |
| 8 | AOF Persistence | NOT STARTED |
| 9 | Snapshots and Crash Recovery | NOT STARTED |
| 10 | Log Compaction and Persistence Lifecycle | NOT STARTED |
| 11 | Network Robustness and Backpressure | NOT STARTED |
| 12 | Configuration and Observability | NOT STARTED |
| 13 | Comprehensive Correctness Validation | NOT STARTED |
| 14 | Performance Engineering | NOT STARTED |
| 15 | Linux, Docker and CI | NOT STARTED |
| 16 | Private Remote Linux Deployment | NOT STARTED |
| 17 | Documentation and Demo | NOT STARTED |
| 18 | Final Engineering Audit | NOT STARTED |
| 19 | Portfolio and Resume Finalization | NOT STARTED |

### Phase 0 Detail

- 0A: COMPLETE — Environment inspected.
- 0B: COMPLETE — Local and remote Git repositories initialized.
- 0C: COMPLETE — C++20 foundation built, tested, committed and pushed.
- 0D: COMPLETE — Documentation foundation and validation.
- 0E: COMPLETE — Fresh-build acceptance audit passed.

### Phase 1 Detail

- 1A: COMPLETE — Move-only POSIX descriptor ownership validated.
- 1B: COMPLETE — Non-blocking loopback TCP listener validated.
- 1C: COMPLETE — Portable event-poller contract and test double validated.
- 1D: COMPLETE — Native kqueue implementation tested on macOS.
- 1E: COMPLETE — Native Linux epoll validated on Ubuntu 24.04 x86-64.
- 1F: COMPLETE — A-E validated and merged through PR #2.
- 1G: NOT STARTED — Multi-client smoke tests.

Complete subphase roadmap: `docs/ROADMAP.md`.

## 6. Implemented Architecture

### Implemented

- CMake-based C++20 project.
- Debug and Release Ninja presets.
- Static redisdb-core library.
- redisdb-server executable.
- redisdb-tests executable.
- CLI version and help responses.
- Basic automated test registration.
- Compiler warning configuration.
- clang-format configuration.
- Move-only POSIX descriptor wrapper: redisdb::net::UniqueFd.
- Automatic descriptor cleanup via RAII.
- Explicit release/reset and move ownership transfer.
- Real Unix socket-pair ownership tests.
- Dedicated redisdb-fd-tests executable.
- TcpListener with loopback-only IPv4 binding.
- Non-blocking and close-on-exec descriptor configuration.
- Ephemeral port support and configurable listen backlog.
- accept_one() returning owned client descriptors.
- Dedicated redisdb-listener-tests executable.
- Real TCP loopback connection and payload tests.
- Header-only EventPoller interface.
- Type-safe Read/Write/Error/Hangup event masks.
- ReadyEvent registration tokens.
- Registration and timeout contract.
- Test-double registration and event-delivery tests.
- Simulated stale-event filtering tests.
- Dedicated redisdb-poller-tests executable.
- Native macOS KqueuePoller implementation.
- RAII-owned kqueue kernel descriptor.
- Level-triggered read/write event registration.
- Native event-token delivery and filter modification.
- Zero, finite and infinite wait support.
- Socket hangup and real TCP readiness testing.
- Dedicated redisdb-kqueue-tests executable.
- Linux-only EpollPoller implementation.
- RAII-owned epoll descriptor using EPOLL_CLOEXEC.
- Level-triggered read/write socket registration.
- 64-bit registration token delivery.
- Native epoll error and hangup mapping.
- Zero, finite and infinite wait handling.
- Linux socket and TCP integration tests.
- Ubuntu Debug/Release GitHub Actions validation.
- RAII-owned nonblocking Connection objects.
- ConnectionManager admission and ownership.
- Bounded accept, receive and send operations.
- Readiness dispatch and transport callbacks.
- Event coalescing and stale-token handling.
- Input/output safety limits and backpressure.
- Configurable idle eviction and polling deadlines.
- Half-close output draining and connection cleanup.
- Callback and unexpected socket failure isolation.
- Native macOS and Linux connection integration tests.

### Not Yet Implemented

- Continuous TCP serving in redisdb-server; library-level connection management is implemented.
- Continuous executable event loop; native poller and ConnectionManager integration is tested.
- External multi-client serving smoke tests.
- RESP2 parser and serializer.
- Command execution.
- In-memory key-value database.
- Expiration.
- Eviction.
- Append-only persistence.
- Snapshotting and recovery.
- Observability.
- Deployment.

## 7. Supported Commands

No Redis database commands have been implemented.

Currently available executable options:

- --version
- --help

These are bootstrap CLI options, not RESP commands.

## 8. Persistence Guarantees

NOT IMPLEMENTED.

No durability or crash-recovery guarantees exist yet.

## 9. Files Created

### Phase 0C

- .clang-format
- .gitignore
- CMakeLists.txt
- CMakePresets.json
- include/redisdb/server/Version.hpp
- src/server/Version.cpp
- src/main.cpp
- tests/unit/version_test.cpp

### Phase 0D

- README.md
- docs/ROADMAP.md
- docs/PROJECT_STATE.md
- docs/DECISIONS.md
- docs/ARCHITECTURE.md

### Phase 1A

- CMakeLists.txt (modified)
- include/redisdb/net/UniqueFd.hpp (created)
- src/net/UniqueFd.cpp (created)
- tests/unit/unique_fd_test.cpp (created)
- README.md (updated)
- docs/ROADMAP.md (updated)
- docs/PROJECT_STATE.md (updated)
- docs/ARCHITECTURE.md (updated)

### Phase 1B

- CMakeLists.txt (modified)
- include/redisdb/net/TcpListener.hpp (created)
- src/net/TcpListener.cpp (created)
- tests/integration/tcp_listener_test.cpp (created)
- README.md (updated)
- docs/ROADMAP.md (updated)
- docs/PROJECT_STATE.md (updated)
- docs/ARCHITECTURE.md (updated)

### Phase 1C

- CMakeLists.txt (modified)
- include/redisdb/net/EventPoller.hpp (created)
- tests/unit/event_poller_test.cpp (created)
- README.md (updated)
- docs/ROADMAP.md (updated)
- docs/PROJECT_STATE.md (updated)
- docs/ARCHITECTURE.md (updated)

### Phase 1D

- CMakeLists.txt (modified)
- include/redisdb/net/KqueuePoller.hpp (created)
- src/net/KqueuePoller.cpp (created)
- tests/integration/kqueue_poller_test.cpp (created)
- README.md (updated)
- docs/ROADMAP.md (updated)
- docs/PROJECT_STATE.md (updated)
- docs/ARCHITECTURE.md (updated)

### Phase 1E

- CMakeLists.txt (modified)
- include/redisdb/net/EpollPoller.hpp (created)
- src/net/EpollPoller.cpp (created)
- tests/integration/epoll_poller_test.cpp (created)
- .github/workflows/phase1e-linux.yml (created)
- README.md (updated)
- docs/ROADMAP.md (updated)
- docs/PROJECT_STATE.md (updated)
- docs/ARCHITECTURE.md (updated)

## 10. Validation Results

### Phase 0C — 2026-10-19

- CMake Debug configure: PASS
- Debug build: PASS
- Debug tests: 2/2 PASS
- CMake Release configure: PASS
- Release build: PASS
- Release tests: 2/2 PASS
- clang-format check: PASS
- CLI version output: PASS
- CLI help output: PASS
- Test discovery: PASS
- Git push: PASS
- Local/remote HEAD equality: PASS

### Phase 0E — 2026-10-19

- Fresh Debug CMake configuration: PASS
- Fresh Debug build: PASS
- Fresh Debug tests: 2/2 PASS
- Fresh Release CMake configuration: PASS
- Fresh Release build: PASS
- Fresh Release tests: 2/2 PASS
- CTest test discovery: 2 tests
- clang-format: PASS
- CLI version in both builds: PASS
- CLI help output: PASS
- Invalid argument exit status 2: PASS
- Project documentation structure: PASS
- Local/remote HEAD equality before checkpoint: PASS
- Clean working tree before checkpoint: PASS
- Phase 0 acceptance gate: PASS

These validations establish the build and repository
foundation only. They do not test a TCP server,
Redis commands, storage, persistence or recovery.

These are foundation tests only.

No event-loop, RESP2, database, persistence or
benchmark tests have been executed.

### Phase 1A — 2026-10-19

- Apple Clang C++20 Debug build: PASS
- Apple Clang C++20 Release build: PASS
- Debug tests: 3/3 PASS
- Release tests: 3/3 PASS
- Dedicated UniqueFd test: PASS
- Repeated UniqueFd test: 25/25 PASS
- Compile-time move-only ownership checks: PASS
- Real Unix socket-pair send/receive test: PASS
- Scope-based descriptor cleanup test: PASS
- Move construction and assignment tests: PASS
- release/reset ownership tests: PASS
- clang-format: PASS
- CMake integration and staged file-set checks: PASS

These tests validate file-descriptor ownership on macOS.
They are not evidence of a working TCP listener,
Linux portability, or complete networking correctness.

### Phase 1B — 2026-10-19

- Apple Clang C++20 Debug build: PASS
- Debug automated tests: 4/4 PASS
- Apple Clang C++20 Release build: PASS
- Release automated tests: 4/4 PASS
- Integration test discovered by CTest: PASS
- Repeated TCP integration test: 20/20 PASS
- Loopback IPv4 bind and ephemeral port: PASS
- Listener O_NONBLOCK and FD_CLOEXEC: PASS
- Empty non-blocking accept: PASS
- Accepted client socket flags: PASS
- Real TCP send/receive: PASS
- Four queued client connections: PASS
- Move construction, assignment and RAII cleanup: PASS
- Invalid backlog handling: PASS
- Duplicate listener bind handling: PASS
- Invalid moved-from listener handling: PASS
- CMake integration and staged file review: PASS

Scope: macOS loopback component tests only.
The server executable still has no TCP serving loop.
Linux validation, event multiplexing and RESP2 are pending.

### Phase 1C — 2026-10-19

- Apple Clang C++20 Debug build: PASS
- Debug automated tests: 5/5 PASS
- Apple Clang C++20 Release build: PASS
- Release automated tests: 5/5 PASS
- Direct EventPoller interface test: PASS
- Repeated interface test: 25/25 PASS
- Abstract interface and virtual destructor checks: PASS
- Type-safe event mask checks: PASS
- Read/Write interest validation: PASS
- Registration add/modify/remove tests: PASS
- Duplicate live registration rejection: PASS
- Stable live registration tokens: PASS
- Simulated stale token filtering: PASS
- Timeout argument validation: PASS
- FD ownership separation using socketpair: PASS
- clang-format: PASS
- CMake integration and staged file review: PASS

Scope: Header-only interface and test-double validation.
The test double injects events; it does not monitor
kernel readiness or actually wait for timeouts.
Token uniqueness across successive registration
lifetimes remains a caller responsibility.
Native kqueue/epoll, running event loop, and
connection dispatch are not yet implemented.

### Phase 1D — 2026-10-19

- Native macOS kqueue source compilation: PASS
- Apple Clang C++20 Debug build: PASS
- Debug automated tests: 6/6 PASS
- Apple Clang C++20 Release build: PASS
- Release automated tests: 6/6 PASS
- Direct native kqueue test: PASS
- Repeated native kqueue integration test: 15/15 PASS
- Kernel queue creation and descriptor cleanup: PASS
- FD_CLOEXEC verification: PASS
- Real socket read readiness: PASS
- Real socket write readiness: PASS
- Read/write filter modification: PASS
- Multiple registration token delivery: PASS
- Duplicate registration and invalid argument checks: PASS
- Registration removal and re-registration: PASS
- Finite timeout behavior: PASS
- Infinite wait awakened by socket data: PASS
- Peer hangup detection: PASS
- Native TCP listener readiness: PASS
- Poller does not own registered sockets: PASS
- Source formatting: PASS
- CMake integration and exact staged file set: PASS

Scope: Native macOS kqueue component validation.
The tests exercise real kernel readiness notifications.
They do not establish Linux epoll behavior, complete
error-injection coverage, or a running database server.

### Phase 1E — 2026-10-19

- Linux platform: Ubuntu 24.04 x86-64.
- Linux compiler: GCC 13.3.0.
- Linux Debug build: PASS.
- Linux Debug automated tests: 6/6 PASS.
- Linux Release build: PASS.
- Linux Release automated tests: 6/6 PASS.
- Debug epoll repeated integration test: 15/15 PASS.
- Release epoll repeated integration test: 15/15 PASS.
- Native Linux socket readiness: PASS.
- Read/write interest modification: PASS.
- Registration tokens and removal tests: PASS.
- Epoll descriptor ownership and CLOEXEC: PASS.
- Peer half-close and timeout tests: PASS.
- Real TCP listener readiness: PASS.
- Linux CLI version verification: PASS.
- GitHub Actions run: 39233458374 SUCCESS.
- macOS Debug regression tests: 6/6 PASS.
- macOS Release regression tests: 6/6 PASS.
- C++ formatting and file-set validation: PASS.

Native epoll was executed on an actual Linux kernel
using GitHub-hosted Ubuntu runners. This does not
establish Linux ARM64 compatibility or a running
database server. The connection lifecycle and
continuous serving loop remain unimplemented.

### Phase 1F — 2026-10-19

Implementation checkpoints:

- 1F-A/B: c8ec996.
- 1F-C: 50ead92.
- 1F-D: 6881b04 and f5bdacc.
- 1F-E.1: a01ae98.
- 1F-E.1b: 51a363b.

Verified results:

- macOS Debug: 9/9 tests passed.
- macOS Release: 9/9 tests passed.
- macOS dispatch integration: 20/20 repetitions passed.
- Linux Debug full test suite: PASS.
- Linux Release full test suite: PASS.
- Linux connection-manager test: 20/20 repetitions per configuration.
- Linux connection-dispatch test: 20/20 repetitions per configuration.
- Linux native epoll repetitions: PASS.
- Linux test discovery: exactly two lifecycle tests selected.
- GitHub Actions: 39256465515 SUCCESS.
- PR #2: MERGED on 2026-10-19.
- Merge commit: 290bc71035bbaa0e93a6c0d12a228f7eb9ff4427.
- Post-merge Linux CI: 39271745832 SUCCESS.
- Phase 1F acceptance: COMPLETE.
- Phase 1G: NOT STARTED.

The networking library is implemented and tested.
The executable does not yet continuously serve TCP
clients or implement Redis commands.

## 11. Benchmarks

NOT STARTED.

No operations-per-second, latency or memory
performance claims are currently supported.

## 12. Deployment State

- Linux build: VALIDATED on Ubuntu 24.04 x86-64 in Debug and Release; deployment not started
- Docker packaging: NOT IMPLEMENTED
- Local container test: NOT STARTED
- Remote Linux deployment: NOT STARTED
- Persistent remote storage: NOT CONFIGURED
- Remote restart recovery: NOT TESTED

Mandatory Milestone A: Phase 15.
Planned Milestone B: Phase 16.

## 13. Architecture Decisions

Approved architecture:

- C++20 and CMake.
- Non-blocking TCP sockets.
- kqueue on macOS.
- epoll on Linux.
- Single-threaded initial command execution.
- Incremental RESP2 parser.
- Hash-table-backed keyspace.
- Lazy and active expiration.
- Data-memory limits and eviction.
- Append-only persistence.
- Versioned snapshots.
- Crash recovery.
- Documented Redis-compatible command subset.

See `docs/DECISIONS.md` for the accepted architecture decisions.

Approved decisions are not proof of implementation.

## 14. Known Issues and Risks

### Low Disk Space

The initial audit reported about 12 GiB free.

Docker data was approximately 52 GiB.

Do not delete Docker data or existing project
resources without inspecting them first.

Recover additional space before heavy Linux,
Docker, sanitizer or persistence workloads.

### Docker Engine

Docker CLI is installed.

The engine was not running during Phase 0A.

Container validation is deferred.

### Platform Portability

Local platform is macOS arm64.

Linux epoll and connection lifecycle tests have passed on Ubuntu 24.04 x86-64. Linux ARM64 and deployment remain untested.

### Compatibility

Full Redis compatibility is not a project goal.

Only implemented and validated commands may be
advertised as supported.

### Durability

Persistence is not yet implemented.

Do not claim crash safety until fault-injection and
recovery testing has been completed.

## 15. Development Rules

- Use macOS Terminal commands.
- Inspect files and Git status before modification.
- Avoid manual editor-based implementation.
- Prefer complete-file heredocs or guarded patches.
- Implement one controlled milestone at a time.
- Validate before committing.
- Use Conventional Commits.
- Push validated milestones.
- Verify local and remote HEAD.
- Never overwrite uncommitted work.
- Never fabricate tests, benchmarks or deployments.
- Update this file after meaningful milestones.

## 16. Exact Next Steps

1. Commit and push the Phase 1F documentation closure.
2. Verify main is clean and synchronized with origin/main.
3. Confirm the post-closure Linux CI result.
4. Create a new Phase 1G feature branch from main.
5. Inspect ConnectionManager, EventPoller and server entry point.
6. Design continuous TCP serving and graceful lifecycle behavior.
7. Implement the minimum serving loop needed for multi-client tests.
8. Add external real-TCP multi-client smoke tests.
9. Validate Debug and Release builds on macOS and Linux.
10. Update Phase 1G status, documentation and PR evidence.
11. Merge Phase 1G only after its acceptance gate passes.
12. Preserve the approved RESP2 and database roadmap.

Do not claim the working TCP database or RESP2 command
support before those capabilities have been implemented.

## 17. Continuation Instructions

On a new chat:

1. Read docs/PROJECT_STATE.md.
2. Read docs/ROADMAP.md.
3. Read docs/DECISIONS.md.
4. Read docs/ARCHITECTURE.md.
5. Inspect git status and recent commits.
6. Verify the actual repository state.
7. Check current test results.
8. Continue from the exact next incomplete subphase.

Do not assume an unverified milestone is complete.

Project priority:

Correctness -> Reliability -> Measured Performance
-> Deployment -> Portfolio Presentation
