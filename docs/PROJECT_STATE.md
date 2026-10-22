# PROJECT STATE — Redis-Compatible In-Memory Database Server

## 1. Snapshot

- Updated: 2026-10-22
- Current phase: Phase 2 — RESP2 Protocol Engine (IN PROGRESS)
- Current subphase: 2G.3A — feature-branch documentation synchronization (IN PROGRESS)
- Status: PHASE 1 COMPLETE; PHASE 2A-2F COMPLETE; PHASE 2G LINUX FUZZ CI PASSED; PR MERGE / POST-MERGE CLOSURE PENDING
- Last completed milestone: 2G.2D — Linux Clang 18 ASan+UBSan + libFuzzer CI #39754741830 SUCCESS on b7edd754
- Exact next task: Review and commit Phase 2G.3A documentation, verify feature branch PR-head CI, open and review Phase 2G PR; merge only after passing gates.
- Implementation maturity: Networking and transport-only smoke server; RESP2 model/parser/serializer, framing and bounded ordered pipeline plus resumable event-loop scheduling validated in tests; no production RESP2 command endpoint
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
- Branch: phase2g/resp2-parser-fuzz-foundation (Phase 2G.2D CI accepted; merge pending)
- Phase 2D starting baseline: 2515f7f1d10067113865f1d4e0ced9ce5d92c180
- Phase 2C starting baseline: 1e7c726969cc437fdecfc0532785de84c24bd4be
- Phase 2B starting baseline: b16dd382e213ae541a91d4f298f288fa80b68aa7
- Phase 2A starting baseline: 3e06210adbb1dae0f1bc4549e06af5ec551f8dc2
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
- Phase 1G branch: phase1g/multi-client-smoke.
- Phase 1G implementation: 4b94facef34a6760a17f71efe18e384c68593ef4.
- Phase 1G hardening: f7ed0d5564e411f6598015748a9256f6c85d6745.
- Phase 1G PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/3 (MERGED).
- Phase 1G CI failure: 39283579738 (workflow shell continuation error).
- Phase 1G documentation checkpoint: 10c9dc9.
- Phase 1G CI correction: 3db387d0c70460c4ccf395b3da6f5f2f76ffb007.
- Phase 1G Linux acceptance: 39288146570 (Debug and Release SUCCESS).
- Phase 1G acceptance documentation: 1775aca0f5c211332b1613ba59a7342315cfd547.
- Phase 1G final PR-head CI: 39289273326 (SUCCESS).
- Phase 1G merge: adf021060d7b758ce9224d7d4ac8fe6ed8841b75.
- Phase 1G post-merge Linux CI: 39290777669 (SUCCESS).
- Current development branch: phase2g/resp2-parser-fuzz-foundation (Linux CI accepted; PR pending).
- Phase 2D implementation: 4f2535a97e6bf3350222896818337cfb294eae11.
- Phase 2D documentation: cdac9f9ebb306f51ccde99380bcf80808aba9341.
- Phase 2D PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/7 (MERGED).
- Phase 2D initial Linux CI: 39376269629 (SUCCESS).
- Phase 2D final PR-head Linux CI: 39380341041 (SUCCESS).
- Phase 2D merge commit: f1bc679ed707404cbc8d0a569c2eadb9af4d6687.
- Phase 2D post-merge Linux CI: 39381045190 (SUCCESS).
- Phase 2D formal closure baseline on main: e36d3a113b2b038208ff1c8fbd9fe50221690461.
- Phase 2E branch: phase2e/resp2-pipelining.
- Phase 2E protocol pipeline commit: 861ebb464a79ffb1dd389aa37c34d9da85f4d3bb.
- Phase 2E native event-loop commit: 169d915ee5507e3ef211321205ce14e2219a298b.
- Phase 2E scheduler-hardening commit: 84d4ebcd1cede43c5e389aa8b24e8e15efa0f4e5.
- Phase 2E CI repetition commit: 3e4bc6f1364d51de5d042b1175831d1537e00654.
- Phase 2E documentation: 7d1533ab00e9a0a95d4f62c9ea9ccfe4fcd03dd1.
- Phase 2E PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/8 (MERGED).
- Phase 2E merge commit: ba97b8bcbb11edacbbb7fb823cce02fa765118e8.
- Phase 2E final PR-head Linux CI: 39625859355 (SUCCESS).
- Phase 2E post-merge Linux CI: 39627679647 (SUCCESS).
- Phase 2E formal closure checkpoint: 3315a3b34977c7c113f814b0e4d73ea08649867f.
- Phase 2E post-closure Linux CI: 39629733366 (SUCCESS).
- Phase 2E initial Linux CI: 39420606587 (SUCCESS).
- Phase 2E repeated Linux CI: 39422679453 (SUCCESS).
- Phase 2F baseline: 55055ea8175898088452249a4ed0c7e9cefc999d.
- Phase 2F branch: phase2f/resp2-malformed-limits (retained).
- Phase 2F parser commits: 9781a7a062a92a9baea66288a5eca57769aef495 and 4dcab5f6c08ee27dbac693d16d787211f4863e0d.
- Phase 2F frame/connection budget commit: d555b4741aef2417072995f505ef58ae7c36f8c6.
- Phase 2F native TCP peer-isolation tests: b86ee47dab943db48f6e8e48d40908386a9d4373.
- Phase 2F PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/9 (MERGED).
- Phase 2F merge commit: ce65649db98faea9ee4f9d9d8c24c4cbf5e6d6e4.
- Phase 2F PR-head Linux Debug/Release CI: 39735651885 (SUCCESS).
- Phase 2F post-merge main Linux Debug/Release CI: 39737223938 (SUCCESS).
- Phase 2F docs closure commit: b54ab7d56997c71fee20eb0281175a9725e9dae7.
- Phase 2F post-closure Linux Debug/Release CI: 39739519389 (SUCCESS).
- Phase 2C implementation: 82b7dc98dbb99c83146d95c90fb3feb1408e4a8a.
- Phase 2C documentation: 9e873ddcb1130a5ba20155f9f61e805e1aa519b2.
- Phase 2C PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/6 (MERGED).
- Phase 2C initial Linux CI: 39327039720 (SUCCESS).
- Phase 2C final PR-head Linux CI: 39332004304 (SUCCESS).
- Phase 2C merge commit: 68dd83137ca27a35c78907907b731ef8c1fedf4a.
- Phase 2C post-merge Linux CI: 39370627026 (SUCCESS).
- Phase 2B implementation: 7835524ca00acd02e9ec16f8bd297b59b750e2c9.
- Phase 2B documentation: 7c8aa375a94fa9e3a5f084f276bdd3cd164d6b11.
- Phase 2B PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/5 (MERGED).
- Phase 2B initial Linux CI: 39311216460 (SUCCESS).
- Phase 2B final PR-head Linux CI: 39314875934 (SUCCESS).
- Phase 2B merge commit: 0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.
- Phase 2B post-merge Linux CI: 39320475360 (SUCCESS).
- Phase 2A implementation: 7981c8c428f64eba76158029d56e7aca81c6a38b.
- Phase 2A documentation: 23f266fddd85d8e76c1ce49f3a6f619a7c816867.
- Phase 2A PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/4 (MERGED).
- Phase 2A initial Linux CI: 39297170492 (SUCCESS).
- Phase 2A final PR-head Linux CI: 39298460768 (SUCCESS).
- Phase 2A merge commit: 861f0491fdb8af490ed6da820a8024541f26f9fa.
- Phase 2A post-merge Linux CI: 39304412144 (SUCCESS).
- Phase 1G merged main: adf021060d7b758ce9224d7d4ac8fe6ed8841b75.
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
| 1 | Networking Foundation | COMPLETE |
| 2 | RESP2 Protocol Engine | IN PROGRESS |
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
- 1G: COMPLETE — PR #3 merged; macOS and Linux post-merge acceptance passed.

### Phase 2 Detail

- 2A: COMPLETE — Owned RESP2 value model merged
  through PR #4 and validated on macOS and Linux.
- 2B: COMPLETE — Bounded RESP2 serializer
  merged through PR #5 and validated on macOS
  and Linux in Debug and Release.
- 2C: COMPLETE — Stateless bounded RESP2 parser
  merged through PR #6 and validated on macOS
  and Linux in Debug and Release.
- 2D: COMPLETE — Framed-input adapter and native
  TCP fragmentation tests merged through PR #7
  and validated on macOS and Linux.
- 2E: COMPLETE — PR #8 merged; local, post-merge and post-closure Linux acceptance passed.
- 2F: NOT STARTED — Malformed input and resource limits.
- 2G: NOT STARTED — Parser fuzz-test foundation.

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
- Explicit --smoke-echo executable mode.
- Continuous native event-loop serving on loopback.
- Configurable smoke TCP port and maximum clients.
- Binary-safe echo callback, without RESP2 parsing.
- External-process Python integration registered with CTest.
- Multi-client, churn, admission and half-close tests.
- Accept-time reset handling with client isolation.
- SIGINT/SIGTERM shutdown in smoke mode.
- Owned RESP2 values, bounded serialization and single-frame parsing.
- Fragmented-input adapter with reuse of Connection receive buffer.
- Bounded synchronous FIFO RespPipeline with conservative reply admission.
- Resumable ConnectionManager handler with three scheduling outcomes.
- Deferred work queue, input progress guard, backpressure and EOF processing.
- Native kqueue/epoll pipelining and scheduler regression tests.
- Linux Debug/Release 18-test suite and pipeline repetition CI gates.

### Not Yet Implemented

- Production TCP serving with RESP2 command execution.
- Production RESP2 parser/serializer invocation in the executable serving path.
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
- --smoke-echo
- --port PORT (smoke mode)
- --max-clients COUNT (smoke mode)

The smoke listener binds to 127.0.0.1.
Port 0 selects an ephemeral listener port.
Smoke mode echoes raw bytes and is not RESP2.
No database commands are supported.

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
- Phase 1G: COMPLETE.

The networking library is implemented and tested.
The executable continuously serves multiple clients
only in explicit transport smoke mode.
Redis commands remain unimplemented.

### Phase 1G — 2026-10-19

Feature branch: `phase1g/multi-client-smoke`.
PR #3: MERGED on 2026-10-19.
Merge commit: adf021060d7b758ce9224d7d4ac8fe6ed8841b75.

Committed checkpoints:

- 1G-C: 4b94fac — Native smoke serving and external TCP tests.
- 1G-D: f7ed0d5 — Accept-time reset isolation, additional
  process tests and deterministic fairness-test setup.

Verified macOS results:

- Debug full CTest suite: 10/10 PASS.
- Release full CTest suite: 10/10 PASS.
- Dispatch integration: 100/100 repetitions per configuration.
- External smoke: 10/10 repetitions per configuration.
- Full Release suite: five consecutive successful runs.
- C++ formatting: PASS.
- Local/remote feature-branch HEAD: synchronized.
- Working tree after implementation push: CLEAN.

The abrupt-reset test exposed a macOS accepted-socket
initialization failure. A peer-specific exception is now
handled during admission without terminating the server.
The existing fairness test was updated to wait for actual
kernel listener and healthy-client readiness.

Linux workflow 39283579738: FAILED due to
malformed shell line continuations in the smoke
repetition step. Full suites had passed.

The workflow was corrected in commit 3db387d.

Linux workflow 39288146570: SUCCESS.

Verified for BOTH Linux Debug and Release:

- Full automated suite: 10/10 PASS.
- External TCP smoke: 10/10 repetitions.
- Connection manager: 20/20 repetitions.
- Connection dispatch: 20/20 repetitions.
- Native epoll: 15/15 repetitions.
- Executable version verification: PASS.

Linux implementation acceptance: PASS.

Final PR-head Linux CI: 39289273326 SUCCESS.
PR #3: MERGED.
Merge commit: adf021060d7b758ce9224d7d4ac8fe6ed8841b75.
Post-merge Linux CI: 39290777669 SUCCESS.

Both Debug and Release on the merged macOS main
passed all 10 CTest tests. Main was synchronized
with origin/main and the worktree was clean.

Phase 1G: COMPLETE.
Phase 1 Networking Foundation: COMPLETE.
At the Phase 1G closure checkpoint,
Phase 2A had not started.

Since then, the Phase 2A typed RESP2 value
representation has been implemented. Wire parsing,
database commands, storage and persistence remain
unimplemented.

### Phase 2A — 2026-10-19

Branch: `phase2a/resp2-value-model`.
Base: 3e06210adbb1dae0f1bc4549e06af5ec551f8dc2.
Implementation: 7981c8c428f64eba76158029d56e7aca81c6a38b.

PR #4: MERGED on 2026-10-20.
Merge commit: 861f0491fdb8af490ed6da820a8024541f26f9fa.

Implemented:

- Owned std::variant-based RESP2 value representation.
- Seven distinct value alternatives.
- Binary-safe std::string payload ownership.
- Recursive std::vector<RespValue> arrays.
- Distinct null bulk and null array values.
- Signed 64-bit integer representation.
- Inline CR/LF validation.
- Structural equality and copy/move support.
- Nested binary deep-copy regression test.

Local macOS validation:

- Debug full suite: 11/11 PASS.
- Release full suite: 11/11 PASS.
- Value-model repetitions: 30/30 per configuration.
- clang-format: PASS.
- Feature branch synchronized and clean after push.

GitHub Actions workflow 39297170492: SUCCESS.

Linux Debug and Release:

- Full test suite: 11/11 PASS.
- unit.resp_value: PASS.
- Repeated TCP smoke: PASS.
- Connection lifecycle regressions: PASS.
- Native epoll regressions: PASS.

Final PR-head Linux CI: 39298460768 SUCCESS.
PR #4: MERGED.
Merge commit: 861f0491fdb8af490ed6da820a8024541f26f9fa.
Post-merge Linux CI: 39304412144 SUCCESS.

Both merged macOS Debug and Release configurations
passed all 11 CTest tests. The merged main branch
was synchronized with origin/main and clean.

Phase 2A: COMPLETE.
Phase 2B was not started at this historical
Phase 2A closure checkpoint.

The value model alone does not parse or serialize
wire messages and is not connected to the TCP server.

### Phase 2B — 2026-10-20

Branch: `phase2b/resp2-response-serializer`.
Base: b16dd382e213ae541a91d4f298f288fa80b68aa7.
Implementation: 7835524ca00acd02e9ec16f8bd297b59b750e2c9.

PR #5: MERGED on 2026-10-20.
Merge commit: 0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.

Implemented:

- Owned RESP2 wire response serialization.
- Exact framing for all seven value categories.
- Binary-safe payload and nested-array encoding.
- Signed 64-bit decimal formatting.
- Two-pass exact output-size measurement.
- Overflow-safe cumulative accounting.
- Configurable byte, array and nesting limits.
- Dedicated unit tests and boundary regressions.

Default output limit: 1 MiB.
Default per-array element limit: 65,536.
Default array nesting-depth limit: 32.

Local macOS validation:

- Debug: 12/12 CTest tests PASS.
- Release: 12/12 CTest tests PASS.
- Serializer repetitions: 30/30 in each configuration.
- C++ formatting: PASS.
- Feature-branch commit pushed and synchronized.

Linux workflow 39311216460: SUCCESS.

Linux Debug and Release:

- Full suite: 12/12 PASS each.
- unit.resp_serializer: PASS.
- unit.resp_value: PASS.
- Repeated TCP smoke and lifecycle tests: PASS.
- Native epoll regressions: PASS.

Final PR-head Linux CI: 39314875934 SUCCESS.
PR #5: MERGED.
Merge commit: 0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.
Post-merge Linux CI: 39320475360 SUCCESS.

Both merged macOS Debug and Release builds
passed all 12 CTest tests.

Linux Debug and Release also passed all 12 tests,
plus the existing repeated networking regressions.

Phase 2B: COMPLETE.
Phase 2C: NEXT, NOT STARTED.

Serializer is not connected to the TCP server.
No RESP2 parser or Redis command execution exists.

### Phase 2C — 2026-10-20

Branch: `phase2c/resp2-incremental-parser`.
Base: 1e7c726969cc437fdecfc0532785de84c24bd4be.
Implementation: 82b7dc98dbb99c83146d95c90fb3feb1408e4a8a.

PR #6: MERGED on 2026-10-20.
Merge commit: 68dd83137ca27a35c78907907b731ef8c1fedf4a.

Implemented:

- Stateless RESP2 single-frame decoder.
- Three-state ParseResult contract.
- Owned decoded values for Complete.
- Exact consumed-byte accounting.
- Binary-safe bulk-string decoding.
- Signed 64-bit integer decoding.
- Recursive arrays and distinct null values.
- Strict CRLF framing.
- Input, metadata, bulk, array and depth limits.
- Parser/serializer round-trip regression tests.
- Incomplete-prefix and malformed-frame tests.

Default parser limits:

- Maximum frame bytes: 1 MiB.
- Maximum bulk-string bytes: 1 MiB.
- Maximum array elements: 65,536.
- Maximum nested array depth: 32.
- Maximum inline metadata bytes: 64 KiB.

Local macOS validation:

- Debug: 13/13 CTest tests PASS.
- Release: 13/13 CTest tests PASS.
- Parser unit-test repetitions: 30/30 per mode.
- C++ formatting: PASS.
- Feature branch pushed and synchronized.

Initial Linux workflow 39327039720: SUCCESS.

Linux Debug and Release:

- Full test suite: 13/13 PASS each.
- unit.resp_parser: PASS.
- unit.resp_serializer: PASS.
- Repeated TCP smoke and lifecycle: PASS.
- Native epoll regressions: PASS.

Final PR-head Linux CI: 39332004304 SUCCESS.
PR #6: MERGED.
Merge commit: 68dd83137ca27a35c78907907b731ef8c1fedf4a.
Post-merge Linux CI: 39370627026 SUCCESS.

Both merged macOS Debug and Release builds
passed all 13 CTest tests.

Linux Debug and Release each passed all 13 tests,
including unit.resp_parser and the existing
networking regression suites.

Phase 2C: COMPLETE.
Phase 2D: NEXT, NOT STARTED.
Phase 2E through 2G: NOT STARTED.

The parser is not connected to the TCP server.
Persistent input buffering, pipelining, command
execution and database storage remain unimplemented.

### Phase 2D — 2026-10-20

Branch: `phase2d/resp2-fragmented-requests`.

Baseline:
2515f7f1d10067113865f1d4e0ced9ce5d92c180.

Implementation:
4f2535a97e6bf3350222896818337cfb294eae11.

PR #7: MERGED on 2026-10-20.
Merge commit: f1bc679ed707404cbc8d0a569c2eadb9af4d6687.

Milestones:

- 2D.0 COMPLETE: Baseline and interface review.
- 2D.1 COMPLETE: Framed-input adapter and tests.
- 2D.1b COMPLETE: Native loopback TCP integration;
  code committed and pushed.
- 2D.2 COMPLETE: Draft PR and initial Linux CI.
- 2D.3 COMPLETE: Documentation and final review.
- 2D.4 COMPLETE: PR merge and post-merge CI.
- 2D.5 COMPLETE: Formal closure documentation.

Implementation:

- RespFramedInput reuses Connection's receive
  buffer without creating another input buffer.
- NeedMore retains incomplete data unchanged.
- Complete returns an owned RespValue and consumes
  precisely the completed frame's bytes.
- Invalid retains malformed bytes for caller policy.
- Default adapter frame budget is 64 KiB.
- Adapter remains separate from executable serving.
- Native TCP tests use ConnectionManager's existing
  handler and native kqueue/epoll event pollers.
- Smoke-echo behavior is preserved.
- Automatic processing of multiple frames is
  deferred to Phase 2E.

Source files:

- include/redisdb/protocol/RespFramedInput.hpp
- src/protocol/RespFramedInput.cpp
- tests/integration/resp_framed_input_test.cpp
- tests/integration/resp_fragmented_dispatch_test.cpp
- CMakeLists.txt

macOS Debug: 15/15 CTest tests PASS.
macOS Release: 15/15 CTest tests PASS.
Both new integration targets: 20 repetitions
each in Debug and Release.

Initial Linux workflow 39376269629: SUCCESS.
Final PR-head Linux workflow 39380341041: SUCCESS.
Merge commit: f1bc679ed707404cbc8d0a569c2eadb9af4d6687.
Post-merge Linux workflow 39381045190: SUCCESS.

macOS Debug and Release: 15/15 tests PASS each.
Linux Debug and Release: 15/15 tests PASS each.
Existing networking repetition jobs: PASS.
Native epoll repetitions: PASS.

The production executable remains transport-only.
Redis commands, persistent keyspace, pipelined
dispatch and response ordering are unimplemented.

At the Phase 2D closure checkpoint, Phase 2E was next.
The following Phase 2E record is the current progression.
Phase 2F and 2G remain NOT STARTED.

### Phase 2E — 2026-10-20 to 2026-10-21 (COMPLETE)

Baseline/main: e36d3a113b2b038208ff1c8fbd9fe50221690461.
Feature branch: `phase2e/resp2-pipelining`.

- 2E.0 COMPLETE — Verified clean Phase 2D closure baseline.
- 2E.1 COMPLETE — `RespPipeline`, framing peek and FIFO admission
  (861ebb464a79ffb1dd389aa37c34d9da85f4d3bb).
- 2E.1b COMPLETE — Resumable native event-loop dispatch and tests
  (169d915ee5507e3ef211321205ce14e2219a298b).
- 2E.1c COMPLETE — Scheduler safety and fairness hardening
  (84d4ebcd1cede43c5e389aa8b24e8e15efa0f4e5).
- 2E.2A COMPLETE — Draft PR #8 opened; initial Linux CI passed.
- 2E.2B COMPLETE — Linux pipeline repetition gates added
  (3e4bc6f1364d51de5d042b1175831d1537e00654).
- 2E.3 COMPLETE — Five-file documentation commit 7d1533a,
  final PR-head Linux CI #39625859355 SUCCESS.
- 2E.4 COMPLETE — PR #8 merged as ba97b8b,
  macOS 18/18 each and Linux post-merge CI #39627679647 SUCCESS.
- 2E.5 COMPLETE — Closure checkpoint 3315a3b on main;
  post-closure Linux CI #39629733366 SUCCESS.

PR #8 MERGED on 2026-10-21:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/8
Merge: ba97b8bcbb11edacbbb7fb823cce02fa765118e8

Local macOS Debug and Release: 18/18 CTest targets PASS each.
Scheduler-hardening local repetition: 50/50 per configuration.
Native TCP pipeline local repetition: 20/20 per configuration.
Local formatting and patch checks: PASS.

Initial Linux CI run #39420606587: SUCCESS, Debug/Release
18/18 CTest targets each, native epoll and network regressions.

Linux repeat gate #39422679453: SUCCESS, Debug/Release
18/18 CTest targets each, 20 additional executions of each
of three Phase 2E pipeline tests, and previously existing
smoke, connection lifecycle and epoll repetition gates.

PR #8 merged 17 files across five feature commits:
11 implementation/build/test files, one CI workflow,
and five project documentation files. No post-merge
implementation modifications were required.

The `redisdb-server` executable remains a transport-only echo
server. Neither Redis commands nor an in-memory database are
implemented. Phase 2F and Phase 2G are NOT STARTED.

### Phase 2F — 2026-10-22 (IMPLEMENTATION AND POST-MERGE COMPLETE)

Baseline `55055ea8175898088452249a4ed0c7e9cefc999d`.
Branch `phase2f/resp2-malformed-limits`.

Four reviewed commits:

- `9781a7a` — reject malformed bulk terminators early.
- `4dcab5f` — reject noncanonical bulk/array length spellings.
- `d555b47` — reject incompatible parser/connection input caps.
- `b86ee47` — prove native TCP malformed-peer isolation.

Parser maintains Complete/NeedMore/Invalid semantics and
binary-safe payloads, while length spellings reject signed zero
and leading zeros. The framing adapter fails fast when
max_frame_bytes exceeds Connection's max_input_bytes, before
consuming data. Healthy clients survive bad-peer disconnects.

PR #9: MERGED through a true two-parent merge on 2026-10-22.
Merge SHA `ce65649db98faea9ee4f9d9d8c24c4cbf5e6d6e4`.
Scope: seven files, +280/-1.
PR-head Linux CI #39735651885: SUCCESS (Debug/Release).
Post-merge main Linux CI #39737223938: SUCCESS (Debug/Release),
including the complete suite and repeated pipeline, TCP smoke,
connection lifecycle and native epoll integration gates.

Local post-merge macOS Debug: 18/18 CTests PASS.
Local post-merge macOS Release: 18/18 CTests PASS.
Seven protocol targets x20: 140/140 PASS in each build.
Native kqueue x10: 10/10 PASS in each build.
C++ formatting, Git history, merge parents and clean worktree: PASS.

No production RESP2 command endpoint or keyspace exists;
`redisdb-server --smoke-echo` remains a raw TCP echo mode.

Phase 2F.5C formal documentation closure commit `b54ab7d`
was pushed, and exact-head Linux Debug/Release CI
#39739519389 completed successfully. Phase 2F is formally
COMPLETE. Phase 2G fuzz-test foundation is NEXT, NOT STARTED.

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

Phases 0-1 and 2A-2F: COMPLETE and formally closed.
Phase 2G is IN PROGRESS on feature branch
`phase2g/resp2-parser-fuzz-foundation`, not yet merged.

Foundation commit:
`ee87032d1ea590f32f8feb3567553ebb9170cd16`.
Linux target and CI commit:
`b7edd7540ec3d46008ee7878fb55063c26334e9f`.

Real Linux Ubuntu 24.04 Clang 18 validation:
GitHub Actions run #39754741830 SUCCESS at commit `b7edd754`.
- Opt-in build instruments real parser/model and fuzz harness.
- Linux ASan + UBSan test discovery: 19 CTests.
- Linux full CTest suite: 19/19 PASS.
- Manifest-first deterministic corpus replay: 50 seeds, PASS.
- Coverage-guided libFuzzer: 10,000 completed invocations,
  maximum input 4096 bytes; no sanitizer failures reported.
- macOS UBSan corpus replay: PASS; Debug/Release 18/18 each.
- Original pinned 50-seed corpus remained immutable.
- Original macOS ASan startup issue is documented; the
  verified Linux ASan+UBSan job is the authoritative sanitizer gate.

Next controlled checkpoints:

1. Review this five-document Phase 2G.3A update,
   perform documentation/test checks, commit, and push feature branch.
2. Verify PR-head Linux regression jobs and fuzz CI at the
   final documentation commit; do not assume previous SHA gates
   apply to a new commit.
3. Open and review a Phase 2G pull request against main.
4. Merge only after PR-head acceptance and appropriate review.
5. Verify macOS and Linux post-merge acceptance on exact main SHA.
6. Record formal Phase 2G / Phase 2 closure status; then start
   Phase 3A command representation on a separate branch.

The production executable remains transport-only TCP echo.
No Redis command endpoint, key/value storage or persistence exists.

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
