# Redis-Compatible In-Memory Database Server

A C++20 systems engineering project to build a
Redis-compatible in-memory database server from scratch.

The planned server combines non-blocking TCP networking,
an incremental RESP2 protocol engine, in-memory storage,
key expiration, eviction, append-only persistence,
snapshotting and crash recovery.

**Project status: Phases 0–4 COMPLETE. Phase 4A–4H core string database accepted on 2026-10-23 after two-parent merge `31914ae` and successful exact-head Linux Debug/Release and Clang 18 sanitizer/fuzz CI. Phase 5A injectable clock abstraction is merged and validated; Phase 5B expiration metadata is under validation on a feature branch. Expiration enforcement and TTL commands are NOT IMPLEMENTED. Supported RESP2 commands: PING/ECHO/QUIT, SET/GET, DEL/EXISTS/TYPE, MSET/MGET, APPEND/STRLEN, and INCR/DECR/INCRBY/DECRBY.**

The executable supports separate loopback-only TCP echo and RESP2 command-server modes.
The latter executes PING, ECHO, QUIT and bounded unknown-command errors.
Phase 4A adds binary-safe storage. Phase 4B wires process-shared in-memory SET/GET over RESP2.

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
- Continuous native event-loop serving in explicit smoke mode.
- Eight-client external TCP integration and binary-safe echo.
- Connection churn, admission recovery and half-close tests.
- Abrupt reset isolation during accepted-socket setup.
- SIGINT/SIGTERM smoke-server shutdown tests.
- Repeated macOS Debug and Release smoke validation.
- Owned RESP2 value model, bounded serializer and parser (library only).
- Fragmented-input adapter reusing Connection's receive buffer.
- Bounded FIFO RESP2 pipeline processor with output admission checks.
- Transport-independent resumable handler with AwaitRead, Continue and AwaitWrite.
- Deferred scheduling, output-pressure recovery and EOF handling.
- Scheduler hardening for progress, input saturation and stale tokens.
- Native macOS kqueue and Linux epoll RESP2 pipeline integration tests.
- Early rejection of malformed RESP2 bulk terminators and noncanonical length spellings.
- Parser frame-budget compatibility checks against Connection input capacity.
- Native TCP malformed-peer isolation and binary-fragmentation regressions.
- Linux Debug/Release pipeline repetition gates in GitHub Actions.
- Formatting and compiler-warning configuration.

Not yet implemented:

- A production Redis-compatible TCP command-serving mode.
- Production RESP2 TCP command-serving path (protocol exists only in library/test integrations).
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
- Python 3 for the external-process CTest integration test.

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

Run the transport-only TCP echo server:

```bash
./build/debug/redisdb-server --smoke-echo --port 0 --max-clients 8
```

The server prints `SMOKE_READY 127.0.0.1:<port>`.
Port `0` requests an OS-assigned ephemeral port.
The listener always binds to `127.0.0.1`.
Stop the foreground process with Ctrl+C.

`--smoke-echo` reflects received bytes unchanged.
It is not a Redis protocol endpoint, and `redis-cli`
cannot be used to validate Redis commands against it.
The future production database server is not yet
implemented.

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

The CTest suite includes a real external
server-process smoke test. Linux Debug and Release
acceptance passed in GitHub Actions run 39288146570,
including repeated smoke, lifecycle and epoll tests.

Future work includes RESP2 parsing, command execution,
persistence, recovery and the production serving path.

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

Phase 1G — Multi-Client Smoke Tests is complete.

Phase 1 Networking Foundation is COMPLETE.

PR #3 merged on 2026-10-19.
Merge commit: adf021060d7b758ce9224d7d4ac8fe6ed8841b75.

Post-merge Linux Debug and Release CI:
39290777669 — SUCCESS.

Phase 2A — RESP2 Value Model is COMPLETE.

The C++20 owned value model supports simple strings,
errors, signed 64-bit integers, binary-safe bulk strings,
null bulk strings, nested arrays and null arrays.

The model was implemented in commit 7981c8c.
PR #4 MERGED on 2026-10-20.
Merge commit: 861f0491fdb8af490ed6da820a8024541f26f9fa.

Local macOS Debug and Release: 11/11 tests passed.
Value-model unit test: 30/30 repetitions per configuration.

Initial Linux Debug and Release CI:
39297170492 — SUCCESS.

Final PR-head Linux CI:
39298460768 — SUCCESS.

Post-merge Linux Debug and Release CI:
39304412144 — SUCCESS.

Phase 2A implementation, merge and post-merge
acceptance are complete.

Phase 2B — Response Serializer is COMPLETE.

A standalone bounded RESP2 wire serializer is
implemented in commit 7835524.

It supports all seven RESP2 value categories,
binary-safe bulk strings, recursive arrays,
signed integers and distinct null encodings.

Two-pass size accounting validates limits before
allocating and writing the encoded response.

Default serializer limits:

- Output bytes: 1 MiB.
- Elements per array: 65,536.
- Array nesting depth: 32.

Local macOS Debug and Release: 12/12 tests passed.
Serializer repetitions: 30/30 per configuration.

Initial Linux Debug and Release CI:
39311216460 — SUCCESS.

Final PR-head Linux Debug and Release CI:
39314875934 — SUCCESS.

Post-merge Linux Debug and Release CI:
39320475360 — SUCCESS.

PR #5: MERGED on 2026-10-20.
Merge commit:
0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.

Phase 2B implementation, final review, merge,
and post-merge acceptance are complete.

Phase 2C — Incremental RESP2 Parser is COMPLETE.

A bounded, stateless RESP2 single-frame parser
is implemented in commit 82b7dc9.

It decodes all seven supported RESP2 value
categories into the existing owned RespValue model.

The parser reports three explicit outcomes:
Complete, NeedMore and Invalid.

Successful decoding returns exact consumed-byte
accounting; incomplete and invalid frames do not
expose partial values.

Initial parser limits:

- Maximum frame bytes: 1 MiB.
- Maximum bulk-string bytes: 1 MiB.
- Maximum array elements: 65,536.
- Maximum array nesting depth: 32.
- Maximum inline metadata bytes: 64 KiB.

Local macOS Debug and Release: 13/13 PASS.
Parser unit-test repetitions: 30/30 per configuration.

Initial Linux Debug and Release workflow:
39327039720 — SUCCESS.

Final PR-head Linux Debug and Release workflow:
39332004304 — SUCCESS.

Post-merge Linux Debug and Release workflow:
39370627026 — SUCCESS.

PR #6: MERGED on 2026-10-20.

Merge commit:
68dd83137ca27a35c78907907b731ef8c1fedf4a.

Phase 2C implementation, review, merge and
post-merge acceptance are complete.

The parser is validated on macOS and Linux
in Debug and Release.

Phase 2D — Fragmented Request Handling is COMPLETE.

Implementation commit:
4f2535a97e6bf3350222896818337cfb294eae11.

PR #7: MERGED on 2026-10-20.

Merge commit:
f1bc679ed707404cbc8d0a569c2eadb9af4d6687.

The RespFramedInput adapter connects the existing
Connection input buffer to RespParser.

It extracts one RESP2 frame per call:

- Complete: return an owned value and consume
  exactly the completed wire frame.
- NeedMore: retain incomplete input unchanged.
- Invalid: retain malformed input for caller policy.

The default adapter frame limit is 64 KiB,
matching the default Connection input limit.
Custom configurations must coordinate these limits.

Dedicated integration tests use real POSIX stream
sockets and native kqueue/epoll TCP dispatch.

The tests cover byte-by-byte fragmentation,
binary strings, nested arrays, independent clients,
malformed input, incomplete EOF, declared lengths,
exact buffer boundaries and coalesced frames.

macOS Debug: 15/15 tests PASS.
macOS Release: 15/15 tests PASS.
Both Phase 2D tests: 20/20 repetitions each,
in each local build configuration.

Initial Linux workflow 39376269629: SUCCESS.

Final PR-head Linux workflow 39380341041:
SUCCESS.

Post-merge Linux workflow 39381045190:
SUCCESS.

Linux Debug and Release: 15/15 tests PASS each.
Native epoll and repeated networking tests: PASS.

The adapter is not yet wired into a production
RESP2 command-serving endpoint.

The --smoke-echo server remains unchanged.

Phase 2D implementation, documentation, review,
merge and post-merge acceptance are complete.

The fragmented-input adapter is validated on
macOS and Linux in Debug and Release.

Phase 2E — RESP2 Pipelining is COMPLETE and formally closed.

The bounded RespPipeline processor handles multiple buffered
requests in FIFO order, reserves reply capacity before dispatch,
and preserves incomplete or backpressured input. A test-specific
reply factory is not a Redis command executor.

ConnectionManager now supports optional resumable input callbacks:
AwaitRead, Continue and AwaitWrite. A bounded deferred work queue
resumes buffered frames without requiring another client send.
The transport-only smoke-echo pathway remains unchanged.

Implementation commits: 861ebb4, 169d915 and 84d4ebc.
Linux CI repetition commit: 3e4bc6f.
PR #8 MERGED on 2026-10-21.
Merge commit: ba97b8bcbb11edacbbb7fb823cce02fa765118e8.
Documentation commit: 7d1533ab00e9a0a95d4f62c9ea9ccfe4fcd03dd1.

macOS Debug and Release: 18/18 CTest tests PASS each.
Linux Debug and Release: 18/18 CTest tests PASS each.
Initial Linux PR CI #39420606587: SUCCESS.
Repeated Linux PR CI #39422679453: SUCCESS.
Each Linux build ran all three new pipeline tests 20 additional times.

Phase 2E.3 documentation synchronization is COMPLETE.
Final PR-head CI #39625859355: SUCCESS in Debug and Release.
Phase 2E.4 controlled merge and post-merge acceptance: COMPLETE.
Post-merge Linux CI #39627679647: SUCCESS in Debug and Release.
Phase 2E.5 closure checkpoint 3315a3b was committed on main.
Post-closure Linux Debug/Release CI #39629733366: SUCCESS.
Phase 2E is formally complete.

Phase 2F — RESP2 malformed-input/resource-limit hardening:
implementation and post-merge validation COMPLETE (2026-10-22).

The parser rejects a visible incorrect bulk terminator without
waiting for another byte. Bulk/array lengths require canonical
decimal spelling; signed RESP integer values retain their own
numeric grammar. Framing rejects a parser frame budget larger
than a connection's input capacity before consuming data.

Native TCP integration tests exercise early malformed terminators,
noncanonical lengths, valid-then-invalid batches, and isolation
of healthy peers with fragmented binary payloads.

Feature branch: `phase2f/resp2-malformed-limits`.
Implementation/test commits: `9781a7a`, `4dcab5f`, `d555b47`,
`b86ee47`. PR #9 MERGED on 2026-10-22 using a true merge commit:
`ce65649db98faea9ee4f9d9d8c24c4cbf5e6d6e4`.

macOS Debug/Release after merge: 18/18 tests each, 140/140
protocol repetitions and 10/10 kqueue repetitions per build.
PR-head Linux CI #39735651885: SUCCESS (Debug/Release).
Post-merge main Linux CI #39737223938: SUCCESS (Debug/Release).

Phase 2F.5C closure documentation commit
`b54ab7d56997c71fee20eb0281175a9725e9dae7` was pushed to main.
Post-closure Linux Debug/Release CI #39739519389: SUCCESS
on that exact commit, including full and repeated network test gates.
Phase 2F and Phase 2G are formally COMPLETE. Phase 2G
closure documentation CI #39772960967 passed on main.
Final Phase 2G.5C status-commit CI requires verification.

The executable still serves only raw TCP transport echo data;
it does not parse RESP2 on its serving path or execute Redis commands.

Current milestone information is maintained in
[PROJECT_STATE.md](docs/PROJECT_STATE.md).

## Phase 2G — RESP2 Parser Fuzz Testing (Complete)

The actual RESP2 parser is exercised by a shared, bounded
invariant harness. An opt-in CMake build provides a deterministic
50-seed corpus runner with SHA-256 manifest checks; Linux Clang 18
also builds a real coverage-guided libFuzzer entry point.

The local Apple Clang 17 AddressSanitizer runtime hangs at startup
on this development Mac, so the local corpus runner uses UBSan.
Ubuntu 24.04 GitHub Actions run
[#39754741830](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39754741830)
passed on feature commit `b7edd754` using ASan+UBSan:
19/19 Linux CTests and 10,000 bounded libFuzzer executions.
All 50 immutable seeds were verified and replayed successfully.
This is bounded fuzzing evidence, not proof that all bugs are absent.

To reproduce the local macOS corpus smoke, configure the
`REDISDB_ENABLE_FUZZ_TARGETS=ON` CMake option in an isolated
build directory with Apple Clang and run:
`ctest --test-dir build/fuzz-sanitize -R fuzz.resp_parser_corpus_smoke`.
See [fuzz architecture](docs/PHASE2G_FUZZ_DESIGN.md) for the
exact limit profile and design decisions.

PR [#10](https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/10)
was merged as `36ec43d8ce247d50c81bf90d60641d4db9b802b8`.
At the final PR head, Linux Clang 18 ASan+UBSan fuzz CI
[#39767524405](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39767524405)
passed 19/19 CTests, 50 verified seeds and 10,000 fuzz executions.
Post-merge main Linux Debug/Release CI
[#39770284668](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39770284668)
passed 18/18 CTests per configuration and all repeat gates.
Post-merge local macOS Debug and Release passed 18/18 each;
50-seed UBSan replay also passed. Documentation closure commit
`09f1fc77f14dfd3b4f12dfb17148b60bbfe0a94b` passed
[Linux Debug/Release CI #39772960967](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39772960967)
on exact main HEAD (18/18 CTests each plus repeat gates).
Phase 2G and Phase 2 were formally accepted: final-status
commit `e7dfbbf5d64795e225c69e5813f75101d0c90067` passed
Linux Debug/Release CI #39774474442 (18/18 each).
The production executable is still transport-only TCP echo.

## Phase 3A — Owned Command Representation (COMPLETE)

C++20 `redisdb::commands::Command` owns ASCII-normalized command names
and binary-safe argument strings decoded from RESP2 array requests.
Structural decoding rejects empty/malformed commands without partial
results. Dispatcher lookup, command arity validation, responses,
keyspace operations and production command-serving integration
are deferred to later milestones.

PR [#11](https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/11)
merged as true two-parent commit
`4b493ac7984b4c3bdf47a24d41b517930cd689b3` (feature head
`9d64403925b62b108ee1172c06ad9b866592d509`).
The PR-head [Linux Debug/Release CI #39784944527](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39784944527)
passed 19/19 tests per configuration. PR-head
[Clang 18 ASan+UBSan fuzz CI #39784944465](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39784944465)
passed 20/20 tests, 50 immutable corpus seeds, and 10,000
bounded libFuzzer executions. The CI-only follow-up fixed the
previous obsolete exact-19 discovery assertion without altering
RESP2 parser or server semantics.

Post-merge [Linux CI #39786009239](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39786009239)
passed 19/19 Debug and 19/19 Release tests plus repeated
network/protocol gates. Local synchronized macOS `main` also passed
19/19 in both configurations. Formal documentation commit
`c369fdf3c223361bfd7a7966dc3228d16b8ec2b7` passed
[Linux Debug/Release CI #39789969583](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39789969583)
(19/19 each and repeat gates). Phase 3A final-status commit
`bb9610e4422dac7fa383a3473abf2e67dbe1766b` passed
[Linux Debug/Release #39792311541](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39792311541)
(19/19 each). Phase 3A COMPLETE.

## Phase 3B — Isolated Command Dispatcher (COMPLETE)

Phase 3B adds a small constructor-built, test-injected C++20 command
registry with deterministic name lookup, strict registration checks,
explicit typed unknown-command outcomes and owned handler replies.
`ConnectionDisposition` is response metadata only; it does not close
sockets. Handler exceptions propagate. The production TCP server,
RESP2 pipeline, parser and persistence implementations were not changed.

[PR #12](https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/12)
merged as true two-parent commit
`84d95500545eb4c0a417cd95cc98c62e30384424` from retained
feature head `e92aaf54cd00e090620be64ede21f32cde7e0549`.
PR-head [Linux Debug/Release #39805019741](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39805019741)
passed 20/20 each, and [Clang 18 sanitizer/fuzz #39805019351](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39805019351)
passed 21/21, 50 immutable seeds and 10,000 libFuzzer executions.
[Post-merge Linux #39806894796](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39806894796)
passed 20/20 Debug and Release plus repeated networking checks.
Local synchronized macOS main also passed 20/20 Debug and Release,
repeated dispatcher/command/protocol/TCP tests and UBSan seed replay.
Five-document acceptance commit `cc88f88948acd96574479750eefc7ede6445c70a`
passed [Linux Debug/Release CI #39810737185](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/39810737185)
with 20/20 tests in both configurations and network/protocol repeats.

**Phase 3B COMPLETE:** Final Linux CI #39811900890 passed.
PING/ECHO/QUIT, redis-cli command execution, wire-level errors, keyspace and persistence remain unimplemented.

## Phase 3C — Argument Validation (COMPLETE)

The isolated ArityValidator applies caller-supplied minimum,
optional maximum and argument-grouping rules. It returns typed
count errors and throws on invalid rule configuration.

The validator does not modify binary-safe arguments, the
Command decoder, Dispatcher, networking or server execution.

Feature `38c5354` was directly merged into main as
`d733daa13cb9c1d8741c4c8d1e1080fe593d32b7`.

Post-merge validation:
- macOS Debug and Release: 21/21 each.
- macOS fuzz-enabled UBSan suite: 22/22.
- Linux CI #39828953490: Debug/Release 21/21 each.
- Clang 18 CI #39829737702: ASan+UBSan 22/22.
- Immutable corpus: 50 verified seeds.
- libFuzzer: 10,000 completed executions.

Historical 3C checkpoint: the following milestone was Phase 3D.
Feature branches now use push-triggered CI and direct merges without PRs.

## Phase 3D–3F — Command Server and Error Behavior (COMPLETE)

- 3D: binary-safe PING/ECHO/QUIT handlers, merge `35a925a`.
- 3E: loopback RESP2 TCP command server and redis-cli smoke, merge `a9da868`.
- 3F: bounded, control-character-safe unknown-command replies,
  merge `e7e8af1adea8a12213b2aa9c1637709133907071`.
- Final Phase 3 main CI: Linux Debug/Release 24/24,
  Clang sanitizer/fuzz 25/25, 50 immutable seeds and 10,000 fuzz executions.

## Phase 4A — In-Memory Keyspace and Entry Model

The storage foundation uses `std::unordered_map<std::string, Entry>`
with owned, binary-safe values, an initial string alternative,
non-allocating heterogeneous lookup and deterministic model tests.
It is not yet wired into command serving. TTL, eviction, AOF and
snapshot behavior remain future milestones.

## Phase 4B — SET/GET RESP2 Integration (COMPLETE)

SET key value returns +OK and overwrites the process-local keyspace;
GET key returns a binary-safe bulk string or RESP2 nil bulk. Commands
are case-insensitive, keys remain case-sensitive, and all TCP clients
share the same database for the life of the server process.

The current default RESP2 pipeline limits a serialized response to
8192 bytes. Phase 4B therefore imposes an **8176-byte value limit on
SET** (16 bytes reserved for response framing) so every accepted value
can be safely returned by GET. Larger SET values return an ERR without
mutating data. This is an intentional Redis compatibility limitation
pending a later, separately validated reply-budget redesign. SET options
such as NX/XX/EX/PX are not supported by Phase 4B. No persistence,
TTL, eviction or collection types are included.

## Phase 4C — DEL / EXISTS / TYPE (COMPLETE)

- `DEL key [key ...]` returns an integer count of removed keys;
  repeated key names count at most once because subsequent removals
  find no entry.
- `EXISTS key [key ...]` returns an integer count of present key
  arguments; repeated existing names count repeatedly, matching Redis.
- `TYPE key` returns `+string` or `+none` in this string-only phase.
  Collection types will require a typed lookup in Phase 6.

All three accept binary-safe and empty keys and operate on the same
process-local keyspace as SET/GET. Command verbs are case-insensitive,
keys are case-sensitive, and invalid argument counts produce RESP2 ERR
without mutating storage. This phase adds no TTL, eviction, AOF,
snapshots or new Redis SET options.

## Phase 4D — Atomic MSET and Ordered MGET (COMPLETE)

MSET validates all key/value pairs and per-value limits before writing. It
stages the next authoritative keyspace and commits using a single map swap:
allocation failures during staging leave previous data intact. Repeated keys
use the last supplied value. This correctness-first mechanism copies the full
map on each MSET (O(total keys + batch) memory/time); incremental transactional
staging is a separately measured future optimization.

MGET preserves order and duplicate arguments, returning RESP2 nil bulk strings
for absent keys. The 8192-byte single-reply pipeline cap is checked including
array headers and element framing before returning a result. Oversized MGET
responses produce a bounded ERR without closing the TCP connection. The
existing 8176-byte maximum individual stored string value remains unchanged.

## Phase 4E — Binary-Safe APPEND and STRLEN (COMPLETE)

`APPEND key suffix` atomically extends an existing string, or creates a
missing key (including for an empty suffix), and returns the new byte length.
`STRLEN key` returns the number of stored bytes, or zero if absent; neither
operation treats UTF-8 sequences as characters. Keys and suffixes are binary-safe.

APPEND validates the complete new size against the existing 8176-byte
individual-value ceiling, using subtraction to avoid arithmetic overflow.
Rejected writes do not modify existing keys or create absent keys; allocation
failure during staging also leaves the old value intact. Existing values
created internally above that ceiling reject APPEND (even an empty suffix).
The limit is a temporary reply-budget compatibility constraint, not Redis's
native string capacity. GET/MGET limits, keyspace ownership and nonpersistent
process-local semantics remain unchanged. No TTL, AOF, or eviction is added.

## Phase 4F — Signed Integer String Commands (COMPLETE)

`INCR`, `DECR`, `INCRBY`, and `DECRBY` treat missing keys as numeric zero,
then store a canonical signed 64-bit decimal string and return the resulting
RESP2 integer. Command names are case-insensitive, keys are binary-safe and
case-sensitive. Invalid arity, noncanonical integer text, invalid deltas and
out-of-range values receive bounded RESP2 errors without modifying storage.

Arithmetic is validated before execution so no C++ signed overflow occurs,
including when subtracting INT64_MIN. Storage changes are staged before commit.
Four-command integration includes 8 concurrent TCP clients incrementing one
shared counter and verifying all 240 results are unique. This is a correctness
checkpoint, not a throughput benchmark; Phase 4G added numeric
edge-case hardening. There is no TTL, persistence or new SET option support.

## Phase 4G — Numeric Edge-Case and Overflow Hardening

This phase adds independent numeric regression coverage without changing the
already-validated storage algorithm or RESP2 command handlers. C++20 unit tests
use unsigned two's-complement operations and sign-transition checks to avoid
repeating the production arithmetic's overflow guard. They cover canonical
and malformed integers, a full signed-64-bit boundary grid, deterministic wide
values, binary keys, and preservation of stored values after rejected changes.

External-process TCP integration independently computes results using Python
arbitrary-precision integers, verifies state after every operation across two
clients, checks malformed bytes and missing-key rollback, and validates exact
pipelined replies after overflow. This is a correctness milestone, not a
performance benchmark. Phase 4H subsequently expanded multi-client correctness coverage.

## Phase 4H — Multi-Client Correctness (COMPLETE)

This acceptance milestone exercises the full existing Phase 4 string/key command
subset through one process-local keyspace shared by multiple live TCP peers. A
2400-step composite C++ command-dispatch reference model checks command-level
behavior, including binary keys, duplicate MSET/MGET operands, APPEND, and
integer operations. The TCP suite checks 640 counter increments across eight
writers and three readers, 280 MSET writes with four concurrent MGET snapshot
readers, and 360 concurrent APPENDs with unique token preservation. All expected
results are order-independent where scheduling is nondeterministic.

A single MGET command is used to test an atomic multi-key snapshot; separate
GET requests cannot promise a consistent snapshot if another client writes
between them. Further checks cover FIFO pipelining, error isolation, QUIT,
connection churn, and clean restart. This is a correctness and integration
checkpoint, not a latency or throughput benchmark. The 8176-byte individual
string and 8192-byte reply limits remain unchanged. Phase 4 closure was accepted after exact-head post-merge Linux CI;
TTL/persistence are not yet implemented.

## Phase 5A — Injectable Clock Abstraction (feature implementation)

Phase 5A introduces `redisdb::storage::Clock` and `SystemClock` for two
separate time domains. `monotonic_now()` is based on `steady_clock` and is
intended for in-process elapsed time and scheduling; `wall_now()` uses
`system_clock` for future persistence-compatible absolute deadlines.
Consumers borrow a `const Clock&`, allowing deterministic tests with an
injectable fake. The unit test controls wall and monotonic time independently,
including backward wall jumps, without sleeping.

This is the time-source foundation only. No expiration deadlines are stored,
no keyspace operations are changed, no TTL commands are added, and no value,
RESP2 reply, or MSET staging limits are modified. Per ADR-009, future persisted
expiration deadlines must never serialize a raw steady-clock time point.
Phase 5B will add metadata; expiration semantics belong to 5C–5H.

## Phase 5B — Expiration Deadline Metadata (feature implementation)

Phase 5B extends each owned in-memory entry with an optional absolute
`WallTimePoint` deadline. The deadline is data only: reads, EXISTS and TYPE
do not treat it as expired until Phase 5C implements lazy expiration. These
metadata-only storage APIs do not create keys or consult the system clock:
`set_expiration(key, deadline)`, `expiration(key)` and
`clear_expiration(key)`. Callers can inject the 5A clock and supply a
controlled `wall_now()` value. No raw `steady_clock` time points are stored.

SET and MSET clear existing deadlines on overwritten keys; APPEND and integer
arithmetic preserve deadlines of existing keys; DEL/clear erase metadata and
new keys start without deadlines. Rejected commands preserve both value and
metadata. MSET remains clone-and-swap. The 8,176-byte value and 8,192-byte
serialized reply limits remain unchanged; no EXPIRE/TTL commands, lazy or
active expiration, eviction or persistence are implemented in 5B.

## License

No license has been selected yet.

Until a license is added, the repository should
not be assumed to grant reuse permissions.
