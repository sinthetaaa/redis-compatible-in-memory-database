# Continuation checkpoint — Stage 6A (2026-10-26)

Repository: https://github.com/sinthetaaa/redis-compatible-in-memory-database
Local path: `/Users/sinthetaaa/Desktop/Projects/SDE-Projects/RedisCompatibleDB`.

Authority: [ROADMAP](ROADMAP.md), [PROJECT_STATE](PROJECT_STATE.md),
[ARCHITECTURE](ARCHITECTURE.md), [DECISIONS](DECISIONS.md), [AOF](AOF.md),
[SNAPSHOTS](SNAPSHOTS.md), [BENCHMARKS](BENCHMARKS.md).
Dated records are historical evidence; current checkpoint notes supersede their
future-work statements. Do not delete or rewrite historical engineering records.

## Scope and accepted baseline

The original plan has 20 phases (0–19), 169 subphases. Phases 0–5 and the
collection/accounting/differential work of Phase 6 have accepted historical
records. Fast-track Stages 1–5A are accepted per the current task baseline:
RESP2 commands/TTL, memory limits/exact LRU (Phase 7), transactional AOF
(Phase 8), snapshots/recovery (Phase 9), differential/performance foundations
(Phases 13–14), Docker/Linux CI and private-deployment preparation (Phase 15
and preparation formerly associated with Phase 16). Original checkbox
reconciliation beyond 11–12 is deferred to Stage 6D; these equivalents do
not assert that every original subphase is formally closed.

**Approved scope revision:** actual private remote Linux deployment, original
Phase 16, is EXCLUDED from revised completion scope. No VPS, card verification,
remote deployment or SSH demonstration is required or claimed. Docker packaging,
Linux container validation, non-root operation and private host publication remain
mandatory. Existing deployment runbooks are retained as historical preparation.
All other original phases remain in scope.

Last independently verified accepted main:
`832631ecec6afb55869514d2f3fa6fdd8155dc77`.
Local main, fetched origin/main and `git ls-remote origin refs/heads/main` matched
exactly; clean initial main; origin HTTPS URL matches the repository; `gh api user`
returned `sinthetaaa`. No conflicting local/remote feature branch existed.

Current branch: `fasttrack/network-observability`. The publication-review baseline
HEAD equals accepted main; no prior feature commits or unexpected staged files.
The user has authorized a normal feature commit and push after final review, then
exact-feature-head Linux CI verification. This handoff accompanies that publication
checkpoint; obtain exact commit identity from `git rev-parse HEAD` and
`git rev-parse origin/fasttrack/network-observability` rather than self-referential
metadata. Stage 6A is **not merged and not accepted**. Main must remain unchanged.
No force push, history rewrite, credentials change, remote provisioning, Docker
daemon startup or merge is authorized. Acceptance requires a separately authorized
guarded merge and successful post-merge exact-main CI.

## Capabilities and invariants

C++20, one serialized event loop; macOS kqueue/Linux epoll; nonblocking TCP;
owned RAII sockets and non-reused registration tokens; bounded accept/read/write/
parse/deferred work, 64 KiB application buffers per direction/client, RESP2
binary-safe incremental parsing and bounded replies. Strings, signed integers,
keys, lists, hashes, sets; lazy and indexed bounded active TTL; deterministic
logical memory ledger (not RSS); noeviction and exact allkeys-lru.

Persistent noeviction uses first-preimage undo staging; exact LRU retains its
full-clone/full-order fallback. Successful data publication follows AOF append
and required synchronization, before reply. Failures roll back and persistence
failures stop serving; a complete unacknowledged record may still recover.
Always/everysec/no policies retain their distinct durability windows. Clean
shutdown explicitly finishes the journal. Snapshots serialize with commands,
select only validated CURRENT and coordinated AOF prefix, and publish after
required directory synchronization. AOF/snapshot/watermark V1 unchanged.
Never truncate/delete corrupt recovery files to force startup. No compaction,
replication, modules, clustering, authentication, TLS or telemetry worker threads.
Loopback default and private Docker host publication preserved.

## Stage 6A implementation and audit

Objective: original Phase 11 network robustness and Phase 12 operational
observability, preserving accepted database/persistence behavior.

| Item | Baseline evidence reused | Branch addition / acceptance evidence |
|---|---|---|
| 11A input caps | Connection receive cap; framed-input compatibility; full-input close | Existing unit.connection, connection_dispatch, scheduler tests |
| 11B output caps | queue_output cap; reply reservation; 64 KiB bound | Existing connection/pipeline tests |
| 11C slow readers | Watermarks and AwaitWrite resumption; persistent hardening | New idle-protected slow pipeline and deadline shutdown tests |
| 11D connections | Admission suspension; fresh-token listener recovery | New real TCP capacity/recovery check; existing manager tests |
| 11E idle cleanup | Manager timeout infrastructure; public server disabled | --idle-timeout-ms, default zero; protect output/deferred work; deterministic + TCP tests |
| 11F shutdown | SIGINT/SIGTERM, journal finish, markers | Stop admission and drain pending work up to one monotonic second; warning on timeout |
| 11G partial I/O/disconnect | Budgeted recv/send; EOF/reset/stale-token isolation | Existing unit.connection, connection_dispatch, pipeline suites revalidated |
| 11H soak | Persistent four-client churn/slow-reader/RSS checks | 32 clients × 8 turns × 16 commands = 4096; hard cap on old soak rounds |
| 12A parser | Strict startup CLI; all existing flags/defaults | Bounded startup-only idle option; duplicate and invalid cases |
| 12B validation | Numeric/bind/policy/AOF/snapshot combination rejection | Idle range 0–31536000000 ms; malformed/duplicate tests |
| 12C logs | Plain diagnostics and stdout markers | Low-frequency JSON lines to stderr; literal categories; checked diagnostic sink |
| 12D counters | Per-turn manager stats | Saturating process-lifetime operational counters, exact TCP checks |
| 12E INFO | Absent | Five small sections; no keyspace scan, expiry, LRU or transaction |
| 12F CONFIG GET | Absent | Nine safe startup settings; bulk pairs; narrow patterns; no paths or SET |
| 12G errors | Client isolation and fatal persistence latch | Structured categories; existing disk/corruption/rollback tests retained |

Every row has successful local coverage as recorded below and requires subsequent
Linux CI, publication and merge acceptance before formal closure. No item is claimed accepted merely
because its implementation is present. See validation record below.

Operational semantics: INFO accepts zero or one section, ASCII case-insensitive;
default/all include server, clients, stats, memory, persistence. Unknown section
returns empty bulk; multiple sections return arity error. CRLF payload headings;
custom fields have redisdb_ prefix; no claim of complete Redis INFO parity.
CONFIG GET takes one or more patterns, exact names or one trailing `*`, with
ASCII-insensitive matching, fixed canonical lowercase reply names, startup-order
pairs and duplicates removed. Unknown/unimplemented patterns match nothing.
Full Redis glob syntax, multi-section INFO and CONFIG SET are not supported.
Redis preserves requested uppercase exact names; this subset canonicalizes them.
`timeout` is whole seconds truncated from milliseconds; `idle-timeout-ms` is the
exact custom setting. Port reports the effective listener port (including port 0).

Counters reset on process restart and saturate at UINT64_MAX. Commands count
requests whose reply factory returns, excluding the current INFO itself; error
replies count rejected, all other replies count succeeded. Fatal exceptions and
malformed RESP frames are excluded from commands; parser_errors counts invalid
pipeline frames, transport_errors counts manager io_failures. Connection/byte
counters aggregate completed poll turns, so an INFO issued within a turn excludes
that turn's transport deltas. Accepted means successfully registered clients,
closed means manager removals (including idle); final destructor teardown is not
included. Current clients is sampled directly. AOF sequence is written transaction
sequence, not an assertion that weaker-policy writes are already durable.
Logical memory includes physically resident charged expired entries until cleanup.

Idle timeout excludes pending output, AwaitWrite and scheduled CPU work. Incomplete
input awaiting more bytes can expire. Slow readers can occupy their bounded client
slot indefinitely; the timeout is not an output-stall policy. Shutdown stops new
admission and drains observed pending work; it does not promise to read all future
kernel input. The one-second network drain excludes blocking filesystem operations;
persistence finalization may take longer. Remaining sockets close by RAII.
JSON logging never includes command payloads, keys, values, paths or exception text.
A failed diagnostic write/flush exits 74; logs have no per-request filesystem work.

## Files, environment and validation

Important source: `src/main.cpp`, `src/server/RespCommandServer.cpp`,
`src/server/Operations.cpp`, `include/redisdb/server/Operations.hpp`,
`src/net/ConnectionManager.cpp`, `src/net/Connection.cpp`,
`src/protocol/RespPipeline.cpp`, `src/commands/CommandDispatcher.cpp`,
`src/storage/Database.cpp`, `src/persistence/{AofLog,Snapshot}.cpp`.
Tests: `tests/unit/operations_test.cpp`,
`tests/integration/resp_operations_test.py`, existing connection/scheduler/hardening,
AOF/snapshot suites, `tests/differential/operations_parity.py` and curated parity.
Build/test registration: `CMakeLists.txt`; debug/release presets in CMakePresets.json.

Environment: macOS ARM64, Apple Clang 17, C++20, CMake 4.1.2/Ninja;
clang-format from /opt/homebrew/bin. CTest Python interpreter is the configured
Python3 installation; native Redis 8.10.2 reference is
`/opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server`, checked against the frozen
native differential harness SHA-256. Local UBSan is available; prior Apple ASan
startup limitations are historical and Linux ASan remains the acceptance gate.
Sandbox blocks loopback binds; authorized CTest execution outside it is necessary.
Mac Docker was previously unavailable; no local Linux Docker validation claimed.

Final local validation (2026-10-26):

| Check | Result |
|---|---|
| CMake Debug/Release configure and build | PASS, Apple Clang 17 |
| Full Debug CTest, parallel 2 | 79/79 PASS |
| Full Release CTest, parallel 2 | 79/79 PASS |
| Full UBSan + fuzz-target CTest, parallel 2 | 80/80 PASS, includes frozen 50-seed parser smoke |
| Final diagnostic SIGPIPE/CLI adjustment | Focused unit.operations, resp_operations, resp_bind, resp_command_server, cli.version: 5/5 each Debug, Release, UBSan |
| New operational pinned Redis comparison | 10/10 PASS against native Redis 8.10.2 fingerprint |
| Existing persistent curated parity | 54 cases / 40 routes PASS, candidate appendfsync always |
| Differential harness unit discovery | 41/41 PASS |
| clang-format 23.1.3 dry-run/Werror, ten changed/new C++ files | PASS |
| git diff --check | PASS |
| Linux Debug/Release/epoll, ASan/UBSan/libFuzzer, Docker/Compose | NOT RUN locally; pending authorized feature publication/CI |

The complete suites include command dispatch, TTL, exact LRU/memory limits,
malformed-client limits, bounded client churn, partial I/O, backpressure, snapshots,
AOF disk failure/corruption, transactional rollback and recovery. The final
stderr-pipe adjustment was followed by the five focused tests in each build;
no persistence/storage implementation changed. UBSan flags:
`-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`,
`REDISDB_ENABLE_FUZZ_TARGETS=ON`, directory `build/stage4b-ubsan` reused by name.
CMake uses Python 3.13 at `/opt/anaconda3/bin/python3.13`; Ninja 1.13.2.
The native launcher retains the frozen macOS reference fingerprint. Its ten
operational checks are now shared with `tests/differential/linux_container.py`,
which reuses the existing immutable Redis 8.10.2 OCI reference, checks manifest,
config, executable and version identity, and retains all 54 command cases/40 routes.
Linux uses candidate AOF always versus reference off, so appendonly/aof_enabled
are checked against their respective startup profiles; custom counters, journal
sequence and logical memory are excluded from Redis-internal metric comparisons.
No additional Redis installation or replacement of the original parity suite.

Executed commands include `cmake --preset debug/release`, matching builds,
`ctest --preset debug/release --parallel 2 --output-on-failure`,
`ctest --test-dir build/stage4b-ubsan --parallel 2 --output-on-failure`,
focused `ctest -R` gates, `python3 -B -m unittest discover -s tests/differential
-p 'test_*.py'`, `tests/differential/operations_parity.py --custom
build/release/redisdb-server --reference
/opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server`, and curated_parity.py
with `--appendfsync always --report /tmp/stage6a-curated-parity.json`.
That temporary JSON is a local harness result, not a repository evidence archive.
No performance benchmark or Linux Docker result is claimed.

Failure history and corrections (not suppressed): initial focused tests passed
1/3 because sandbox prohibited TCP bind; authorized rerun passed 2/3 and found
missing snapshot fixture directory; fixed, focused retry 2/2 passed. Initial full
Debug/Release each passed 78/79: static logging categories omitted an established
maxmemory diagnostic. Further focused runs passed 4/5 and 3/4 while refining AOF
corruption and combined OOM/maxmemory categories; first UBSan full run passed
78/80 for the same diagnostic assertions. Static safe categories now preserve all
expected diagnostics and final suites pass. Operational differential iterations
exposed Redis's uppercase exact-name spelling and broader wildcard configuration
surface; comparisons now cover canonical shared names and filter custom subset
keys explicitly, without equating Redis-only settings or internal counters.

Network tests: new soak is 32 workers, 8 bounded turns each, 16 commands/turn,
5-second socket limits and 60-second CTest ceiling. Server maxclients 40;
RSS before/after sampled growth envelope 32 MiB native, 512 MiB when actual ASan
instrumentation is detected by CMake; these samples are not peak RSS. Slow pipeline
checks use 1024 replies with a 64 KiB receive buffer; successful shutdown uses a
separate normal-window client and 128 replies; shutdown exhaustion uses 4096 requests/potential 16 MiB
stream with 64 KiB server application output, verifies a warning and bounded exit.
Existing persistent soak now also has a 64-round hard cap per policy in addition
to its time bound, connection bound, 512-reconnect cap and RSS/FD checks.
None of these short workloads proves indefinite leak freedom.

Publication review (2026-10-26): reviewed the complete diff and adjacent command,
transport, poller and persistence boundaries. No production-code correction was
necessary. Corrected one deterministic test to measure idle expiry from the new
last-activity timestamp after flush, avoiding a sub-millisecond scheduling assumption.
Added the missing Linux execution of ten shared operational comparison cases to the
existing Docker adapter and workflow, with explicit 54/40 and 10 execution guards.
Both operations CTests are registered for Linux Debug/Release and Clang 18 ASan/
UBSan; full-suite discovery has no fixed total that rejects new tests. Existing
pipeline/lifecycle selection counts remain correct. Docker builds the revised
Release server, retains non-root/private publication, ten owned private Compose
checks and the 24-profile/48-run benchmark with no throughput threshold.

Review validation: focused Debug/Release operations, connection dispatch and
scheduler checks 4/4 each; native operational comparison 10/10 with persistence
off and 10/10 with always; existing differential harness unit suite 41/41;
changed C++ format checks and diff integrity passed. Python AST checks passed;
PyYAML is unavailable locally, so workflow YAML parsing used Ruby Psych and passed. Prior full suites above remain
applicable: this review changes test/CI code and documentation only.

First publication CI on `208df581a8eaacf02faa9f74608092e4697e6289`:
networking [40370462196](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40370462196)
failed with 78/79 in each build; sanitizer
[40370462216](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40370462216)
failed with 79/80. Only integration.resp_operations failed: it demanded complete
shutdown delivery through an artificially tiny receive window, exceeding the
specified one-second network drain. Later networking repeats and coverage-guided
fuzz executions were skipped, not passed. No sanitizer defect was reported.
Docker [40370462235](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40370462235)
passed, including 54/40 command parity, ten operational cases, private Compose and
24-profile/48-run baselines; this does not substitute for corrected-head CI.

Focused correction: retain slow-reader idle/backpressure coverage with a bounded
1024-reply stream and 64 KiB receive buffer, use a separate normal-window client
for successful shutdown draining, and retain the separate non-reader test that
requires shutdown_drain_timeout. Do not require slow-reader completion beyond the
server's documented deadline. Production behavior and durability are unchanged.
Corrected local operations checks passed 2/2 each in Debug, Release and UBSan;
Python syntax and git diff integrity passed. Follow-up exact-head CI is PENDING
at this correction checkpoint; inspect all
three new runs and their execution logs after the normal follow-up push.

CI at feature publication: PENDING; no feature run is claimed passed in this commit.
The Docker workflow now requires real execution of all ten operational cases,
not mere discovery. After publication, verify the exact feature SHA for all three
workflows and inspect test execution logs, fuzz completion and Docker reports.
Historical accepted Stage 4B
runs [40316862854](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40316862854),
[40316862866](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40316862866),
[40316862873](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40316862873)
are recorded in the prior authoritative documents for `d69b3ede...`, and were not
independently rechecked here. They are not evidence for this branch. Existing
`.github/workflows/phase1e-linux.yml`, `phase2g-fuzz-linux.yml`, `docker-linux.yml`
retain their complete suites; only the Docker workflow adds the operational
execution guard. Full CTest discovery includes both new targets automatically.
Mac Docker was not run and no daemon/system configuration was changed.

Final changed-file manifest: six added — this handoff,
`include/redisdb/server/Operations.hpp`, `src/server/Operations.cpp`,
`tests/unit/operations_test.cpp`, `tests/integration/resp_operations_test.py`,
`tests/differential/operations_parity.py`. Sixteen modified — `.github/workflows/docker-linux.yml`,
`tests/differential/linux_container.py`, `CMakeLists.txt`,
`README.md`, `docs/{ROADMAP,PROJECT_STATE,ARCHITECTURE,DECISIONS}.md`,
`include/redisdb/net/ConnectionManager.hpp`,
`include/redisdb/server/RespCommandServer.hpp`, `src/main.cpp`,
`src/net/ConnectionManager.cpp`, `src/server/RespCommandServer.cpp`,
`tests/integration/{connection_dispatch_test.cpp,resp_hardening_test.py,
resp_pipeline_scheduler_hardening_test.cpp}`. Only this explicit 22-file manifest is intended for feature publication.

## Remaining milestones and exact next step

- 6B: safe AOF compaction, original Phase 10; generation/offset coordination,
  interrupted rewrite recovery, unchanged acknowledgment guarantees.
- 6C: comprehensive correctness/performance, original Phases 13–14.
- 6D: documentation, original roadmap reconciliation and demo, Phases 6–9, 15, 17.
- 6E: final engineering audit and portfolio closure, Phases 18–19.
- Phase 16 actual remote deployment: explicitly excluded, never marked complete.

Exact next action after authorized feature publication: monitor all three workflows
at exact feature HEAD, fix relevant failures via ordinary follow-up commits and
normal pushes, and revalidate corrected exact HEAD. After successful feature CI,
STOP and obtain separate guarded merge authorization. Recheck accepted main and
feature ancestry, review the tested tree, merge only if authorized, then require
successful exact post-merge main Linux CI before recording Stage 6A accepted.
Do not begin Stage 6B in this task.
