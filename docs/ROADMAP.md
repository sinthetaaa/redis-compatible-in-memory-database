# Development Roadmap

## Current engineering status (2026-10-28) — accepted 10D.1B slice

**10D.1A read-only root/LOCK ownership remains accepted; 10D.1B unpublished owned
staging and checked file-I/O engineering is COMPLETE AND ACCEPTED ON MAIN.**
Accepted merge `bc66ebb293c26a54f696a972416c00f4c0a4ff23` has exact-main
[Networking 40652303976](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303976)
Debug/Release **84/84**,
[Clang 18 sanitizer/fuzz 40652303905](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303905)
**85/85**, **50 seeds/10,000 executions**, and
[Docker 40652303970](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303970)
all required gates and **24 profiles/48 timed runs** passed. The new staging
warning is absent in GCC 13 Release and GCC 12 Docker; inherited warnings remain.
See the [10D.1B handoff](CONTINUATION_HANDOFF.md#stage-6b--10d1b--engineering-accepted-on-main-formal-documentation-closure-preparation).

**The broader 10D.1 workstream, Stage 6B and original Phase 10 remain unfinished.**
Complete P1–P7 generation publication and R4/R5 remain incomplete; low-level
staging does not complete those gates. 10D.1C is the next proposed engineering
slice, unimplemented and subject to separately authorized continuation after
formal documentation acceptance. This note grants no 10D.1C authority. Older
status/action statements below are historical and superseded by this dated note.
All **20 original phases, 169 subphase IDs**, original descriptions, checkboxes,
acceptance gates and historical entries remain byte-preserved. Original Phase 16
remains **EXCLUDED BY APPROVED SCOPE REVISION**, not complete; no VPS deployment
is claimed. No checklist reconciliation is performed.

At preparation time on 2026-10-28, this four-document closure was local, unstaged,
uncommitted and unpublished. Accepted engineering and documentation publication
are separate: independent documentation source review, separately authorized
guarded documentation-feature commit/push, exact-documentation-feature Linux CI,
separately authorized guarded two-parent documentation main merge, and
exact-documentation-main Linux CI are distinct gates. Consult actual Git refs
and workflow evidence for subsequent completion; no future documentation gate or
new test execution is claimed by this preparation checkpoint.


## Redis-Compatible In-Memory Database Server

**Language:** C++20
**Build:** CMake / Ninja
**Local platform:** macOS ARM64
**Target platform:** Linux
**Repository:** `sinthetaaa/redis-compatible-in-memory-database`

This document preserves the approved 20-phase implementation
roadmap, including all 169 subphases.

## Latest fast-track status (2026-10-28) — accepted 10D.1A submilestone

**10D.1A engineering is COMPLETE AND ACCEPTED ON MAIN**, merge
`d8f301ac384bae34fcf47b430766096cee5ec217`, tree
`4347de301ebc14fee0d9b5f0e70b524625feb945`; retained feature
`fasttrack/v2-root-identity-validation` at
`91b30f3c8529ed80954414a601b4ce102770888e`. The
[authoritative handoff](CONTINUATION_HANDOFF.md#stage-6b--10d1a--formally-accepted-engineering-checkpoint)
records ordered parents, independent source/correction acceptance and exact-main
CI: Networking 40561936792 (83/83 Debug and Release), Clang 18 sanitizer/fuzz
40561936629 (84/84, 50 seeds, 10,000 executions), Docker 40561936642 (including
54 command cases/40 routes, 10 operational cases, 24 profiles/48 timed runs).
All completed/success on the merge SHA; V2Root source/test warning cleanup passed.

This is the root/LOCK identity and bounded read-only foundation submilestone.
**Original Phase 10 and complete Stage 6B compaction are not finished.** No
production V2 writer/recovery, P1–P7 publication, R4/R5 barriers or later 10D.1B–F
work is accepted by this checkpoint. Frozen architecture/format authority is unchanged.
At preparation time on 2026-10-28, this four-document closure was an unstaged,
uncommitted draft; engineering acceptance was already independently established.
From that checkpoint, the required sequence is independent documentation review,
separately authorized guarded docs feature publication/exact-feature CI, then
separately authorized guarded main merge/exact-main CI. Consult current refs and
workflow results to determine which gates have subsequently completed. After those
gates are verified, the next frozen implementation slice (10D.1B) may be separately
authorized. This checkpoint itself grants no implementation authorization.

The older “no Stage 6B work has begun,” “Stage 6B is not authorized,” and Phase 10
“NOT STARTED — pending Stage 6B” statements below are **historical and superseded
by this dated checkpoint**, including their old continuation/publication instructions.
Original checklist status reconciliation remains a separate milestone; this note
changes no original inventory item, checkbox, acceptance criterion or exclusion.
All **20 original phases and 169 subphase IDs** remain intact. Original Phase 16
remains **EXCLUDED BY APPROVED SCOPE REVISION**, not completed; no remote deployment
is claimed. Broader Stage 6B and original Phase 10 acceptance remain pending.

## Historical fast-track checkpoint (2026-10-27; superseded)

The earlier checkpoint below is preserved verbatim as historical evidence.

**Current fast-track checkpoint (2026-10-27):** Stages 1–5A and Stage 6A
engineering are accepted on main `bf1f19f2961544b6524a3e31b4250ce4e9ff0952`.
Original **Phase 11 (11A–11H) and Phase 12 (12A–12G): COMPLETE AND ACCEPTED**;
11H is limited to its documented bounded workload/resource acceptance.
Feature `6f11ff21eeb5725e6b23cb5e45ecb57aaebe5059`, exact merge parents/tree and post-merge
CI are recorded in the [durable handoff](CONTINUATION_HANDOFF.md).
Networking 40375036254 passed Debug/Release 79/79 each; sanitizer/fuzz 40375036340
passed 80/80 plus 10,000 fuzz executions; Docker 40375036503 passed 15 container
and 10 private Compose cases, 54 command cases/40 routes plus 10 operational cases,
and 24 benchmark profiles/48 runs. Both operations tests actually executed.

The six-document closure on `fasttrack/stage6a-closure` was uncommitted/unpublished
at preparation. Its normal commit/push and exact-head CI are now authorized after
review; merge requires separate authorization. This publication is separate from
accepted engineering. Main remains unchanged; Stage 6B is not authorized.

**Original Phase 16: EXCLUDED BY APPROVED SCOPE REVISION**, not completed.
No VPS or remote deployment occurred; no card, SSH host or public server is
required. Docker/Linux packaging and CI remain mandatory. All other original
phases remain in scope. The full 20-phase/169-subphase plan is preserved below.
Historical statuses outside Phases 11/12 await comprehensive Stage 6D
reconciliation; they do not override the accepted fast-track capabilities above.

Remaining engineering: **6B** safe AOF compaction (Phase 10); **6C** correctness
and performance gates (Phases 13–14); **6D** roadmap reconciliation, final docs and
demo (Phases 6–9, 15, 17); **6E** final engineering audit and portfolio closure
(Phases 18–19). Stage 6B starts with detailed architecture and crash-safety design
review before any persistence-code modification; no Stage 6B work has begun.

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
Phases 2A-2F implemented and post-merge validated; Phase 2G NEXT, NOT STARTED.

### Acceptance Gate

Stable non-blocking TCP server handling multiple
simultaneous connections without resource leaks.

# Phase 2 — RESP2 Protocol Engine

**Status:** COMPLETE

- [x] 2A — RESP value model
- [x] 2B — Response serializer
- [x] 2C — Incremental parser
- [x] 2D — Fragmented request handling
- [x] 2E — Pipelining
- [x] 2F — Malformed-input and resource-limit handling
- [x] 2G — Parser fuzz-test foundation

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

Phase 2C — Incremental Parser: COMPLETE.
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
- 2C.3: COMPLETE — Documentation synchronized,
  final PR-head Linux acceptance passed.
- 2C.4: COMPLETE — PR #6 merged into main;
  macOS and Linux post-merge tests passed.
- 2C.5: COMPLETE — Formal milestone closure recorded.

Implementation commit:
82b7dc98dbb99c83146d95c90fb3feb1408e4a8a.

PR #6:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/6

Initial Linux workflow 39327039720: SUCCESS.
Final PR-head workflow 39332004304: SUCCESS.

PR #6: MERGED.

Merge commit:
68dd83137ca27a35c78907907b731ef8c1fedf4a.

Post-merge Linux workflow 39370627026: SUCCESS.

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

Phase 2C: COMPLETE.

The bounded single-frame parser is validated
on macOS and Linux in Debug and Release.

Phase 2D — Fragmented Request Handling:
COMPLETE.

Phase 2E — Pipelining: NEXT, NOT STARTED.

Phase 2E through 2G: NOT STARTED.

The server remains transport-smoke-only.
No RESP2 network execution or database storage
has been implemented.

### Phase 2D Detailed Progress

- 2D.0: COMPLETE — Verified Phase 2C closure,
  established clean feature branch and reviewed
  existing connection/parser interfaces.
- 2D.1: COMPLETE — Framed-input adapter and POSIX
  socket fragmentation tests validated locally.
- 2D.1b: COMPLETE — Native loopback TCP integration
  added, tested, committed and pushed.
- 2D.2: COMPLETE — Draft PR #7 created; initial
  Linux Debug and Release acceptance passed.
- 2D.3: COMPLETE — Documentation synchronized,
  final PR-head Linux CI and source review passed.
- 2D.4: COMPLETE — PR #7 merged into main;
  macOS and Linux post-merge tests passed.
- 2D.5: COMPLETE — Formal Phase 2D closure recorded.

Starting baseline:
2515f7f1d10067113865f1d4e0ced9ce5d92c180.

Implementation:
4f2535a97e6bf3350222896818337cfb294eae11.

PR #7:
https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/7

PR #7 status: MERGED.

Merge commit:
f1bc679ed707404cbc8d0a569c2eadb9af4d6687.

Initial Linux workflow 39376269629: SUCCESS.
Final PR-head workflow 39380341041: SUCCESS.
Post-merge workflow 39381045190: SUCCESS.

Linux Debug and Release both passed 15/15 CTest
tests, including two Phase 2D integration targets.

Implementation details:

- Reuses Connection::input and consume_input.
- Adds no duplicate per-client receive buffer.
- Uses RespParser for one-frame extraction.
- Retains incomplete frames across TCP reads.
- Returns owned binary-safe RESP2 values.
- Consumes exactly one complete frame.
- Retains invalid input for a caller policy.
- Uses a 64 KiB default framed-input limit.
- Tests native kqueue/epoll ConnectionManager
  dispatch over real loopback TCP connections.
- Keeps the smoke-echo executable unchanged.

A test-specific input handler closes malformed
clients through the existing manager failure path.
This is not a production Redis server endpoint.

Multiple frames can remain in Connection::input.
Automatic pipelining and ordered responses belong
to Phase 2E.

Phase 2D: COMPLETE.

The framed-input adapter and native TCP tests
passed macOS and Linux Debug/Release acceptance.

At Phase 2D closure, Phase 2E pipelining was next.
Phase 2E progress is recorded below.
Phase 2F and 2G remain NOT STARTED.

### Phase 2E Detailed Progress — COMPLETE

- 2E.0: COMPLETE — Verified Phase 2D formal closure at
  e36d3a113b2b038208ff1c8fbd9fe50221690461;
  dedicated feature branch created and macOS baseline tested.
- 2E.1: COMPLETE — Bounded, ordered RespPipeline processor,
  peek-before-consume framing and output admission checks;
  implementation 861ebb4.
- 2E.1b: COMPLETE — Native event-loop resumable callbacks,
  deferred work, write-pressure recovery and EOF tests;
  implementation 169d915.
- 2E.1c: COMPLETE — Progress safeguards, deferred token cleanup,
  full-buffer stall handling and deterministic socket tests;
  implementation 84d4ebc.
- 2E.2A: COMPLETE — Draft PR #8 created; initial Linux
  Debug/Release 18/18 CI #39420606587 SUCCESS.
- 2E.2B: COMPLETE — Three-test Linux repetition gate added
  in 3e4bc6f; both Linux jobs SUCCESS in #39422679453,
  including 20 additional repetitions per pipeline test.
- 2E.3: COMPLETE — Five project documents synchronized
  in 7d1533a; final PR-head Linux CI #39625859355 SUCCESS.
- 2E.4: COMPLETE — PR #8 merged into main as
  ba97b8bcbb11edacbbb7fb823cce02fa765118e8;
  macOS Debug/Release 18/18 and Linux post-merge
  CI #39627679647 SUCCESS, including repeat gates.
- 2E.5: COMPLETE — Closure checkpoint 3315a3b on main;
  post-closure Linux Debug/Release CI #39629733366
  SUCCESS, including pipeline and networking repetitions.

Branch: `phase2e/resp2-pipelining`.
PR: https://github.com/sinthetaaa/redis-compatible-in-memory-database/pull/8
PR state: MERGED on 2026-10-21.
Merge commit: ba97b8bcbb11edacbbb7fb823cce02fa765118e8.
Final PR-head Linux CI #39625859355: SUCCESS.
Post-merge Linux CI #39627679647: SUCCESS.

Phase 2E is formally COMPLETE with 2E.5 accepted.
Phase 2F is formally COMPLETE, including documentation closure;
the detailed Phase 2F record follows. Phase 2G is NEXT, NOT STARTED.
The production executable still runs only transport echo;
no Redis command-serving endpoint or data keyspace exists.

### Phase 2F Detailed Progress — IMPLEMENTATION AND POST-MERGE COMPLETE

Baseline/main: `55055ea8175898088452249a4ed0c7e9cefc999d`.
Feature branch: `phase2f/resp2-malformed-limits`.

- 2F.1: COMPLETE — Early-invalid bulk terminator rejection and
  canonical bulk/array length spellings (commits `9781a7a`,
  `4dcab5f`); existing RESP integer grammar preserved.
- 2F.2: COMPLETE — Parser frame-budget/input-buffer compatibility
  checks, guarded before input consumption (`d555b47`).
- 2F.3: COMPLETE — Native TCP malformed-peer/client-isolation
  integration tests (`b86ee47`), including fragmented binary
  healthy traffic and valid-then-invalid batches.
- 2F.4: COMPLETE — Local audit and PR #9 validation;
  PR-head Linux CI #39735651885 SUCCESS in Debug and Release.
- 2F.5A: COMPLETE — PR #9 merged on 2026-10-22 with a true
  two-parent merge commit `ce65649db98faea9ee4f9d9d8c24c4cbf5e6d6e4`.
- 2F.5B: COMPLETE — Post-merge Linux CI #39737223938 SUCCESS;
  clean local main synchronized, macOS Debug/Release validated.
- 2F.5C: COMPLETE — Formal five-document closure commit
  `b54ab7d56997c71fee20eb0281175a9725e9dae7` pushed to main;
  post-closure Linux Debug/Release CI #39739519389 SUCCESS.
  Phase 2G is the next controlled milestone.

Total PR scope: seven files, 280 additions and one deletion.
macOS Debug and Release post-merge: 18/18 CTests each;
seven protocol CTest targets x20 each (140 executions)
and native kqueue x10 per configuration.
Linux CI passed full Debug/Release, repeated pipeline,
TCP smoke, connection lifecycle and native epoll gates.

Only the RESP2 library/test pipeline is hardened. The executable
remains transport-smoke-only; Redis commands, storage,
persistence and a production RESP2 endpoint are not implemented.
Phase 2G — parser fuzz-test foundation: COMPLETE.
PR #10 merged and post-merge validated. Closure-documentation
commit `09f1fc77f14dfd3b4f12dfb17148b60bbfe0a94b` passed
Linux Debug and Release CI #39772960967 on exact main HEAD.

### Phase 2G Detailed Progress — COMPLETE

- 2G.0A-0C: COMPLETE — toolchain audit, dedicated feature branch,
  and parser fuzzing architecture/design.
- 2G.1A-1C: COMPLETE — shared parser invariants, isolated macOS
  UBSan seed runner, deterministic 50-seed SHA-256 corpus and
  local foundation commit `ee87032d1ea590f32f8feb3567553ebb9170cd16`.
- 2G.2A-2C: COMPLETE — real Linux `LLVMFuzzerTestOneInput` target,
  isolated Ubuntu 24.04 workflow and pushed feature commit
  `b7edd7540ec3d46008ee7878fb55063c26334e9f`.
- 2G.2D: COMPLETE — GitHub Actions run #39754741830 SUCCESS
  on the exact pushed commit; Linux Clang 18 ASan+UBSan
  instrumentation verified; all 19 CTests passed;
  deterministic 50-seed replay passed; 10,000 coverage-guided
  libFuzzer executions passed, maximum input 4096 bytes.
- 2G.3A-3E: COMPLETE — five-document feature-branch records,
  commit `c089890d1dda45bc7eef0591bfb21cab21ca0cf4`,
  PR #10 review and acceptance. PR-head Linux Debug/Release
  CI #39767524677 SUCCESS; PR-head Clang 18 fuzz CI
  #39767524405 SUCCESS (19/19 tests, 10,000 fuzz runs).
- 2G.4A: COMPLETE — PR #10 merged by a true two-parent commit
  `36ec43d8ce247d50c81bf90d60641d4db9b802b8`.
- 2G.4B: COMPLETE — post-merge Linux main CI #39770284668
  SUCCESS (Debug/Release 18/18 each, all repeat gates).
  Local main fast-forwarded; macOS Debug/Release each 18/18,
  50-seed UBSan corpus smoke PASS; clean, synchronized main.
- 2G.5A: COMPLETE — five-document closure draft, verified on
  merged main; macOS Debug/Release 18/18 and 50-seed UBSan PASS.
- 2G.5B: COMPLETE — closure-documentation commit
  `09f1fc77f14dfd3b4f12dfb17148b60bbfe0a94b` on main;
  GitHub Actions #39772960967 SUCCESS (Debug/Release 18/18
  each and existing protocol/network repetition gates).
- 2G.5C: COMPLETE — final Phase 2 status commit
  `e7dfbbf5d64795e225c69e5813f75101d0c90067`; exact-head
  Linux Debug/Release CI #39774474442 SUCCESS (18/18 each).

All 2G targets are opt-in; default Debug and Release builds
remain unchanged at 18 CTests each. On this Mac, ASan hangs
during runtime initialization; the seed runner uses UBSan.
Linux CI executes both sanitizers and the real fuzzer.
The network executable is still transport-echo-only.

### Acceptance Gate

Binary-safe incremental RESP2 parser and serializer
with resource limits and comprehensive edge-case tests.

# Phase 3 — Command Framework

**Status:** COMPLETE (3A–3F; merged final Phase 3 CI SUCCESS)

- [x] 3A — Command representation (COMPLETE; final status CI #39792311541 SUCCESS)
- [x] 3B — Dispatcher (COMPLETE; CI #39811900890)
- [x] 3C — Argument validation (COMPLETE)
- [x] 3D — PING, ECHO, QUIT (merge `35a925a`)
- [x] 3E — Redis CLI compatibility smoke test (merge `a9da868`)
- [x] 3F — Unknown-command behavior (merge `e7e8af1`)

### Phase 3A Detailed Progress — COMPLETE (final status-commit CI passed)

- 3A.0A: COMPLETE — final Phase 2 baseline `e7dfbbf5`,
  feature branch created after accepted Linux CI #39774474442.
- 3A.0B: COMPLETE — owned binary-safe Command representation
  design in `docs/PHASE3A_COMMAND_DESIGN.md`.
- 3A.1A-1B: COMPLETE — `Command.hpp`, `Command.cpp`, unit
  tests and CMake registration; clang-format; macOS Debug/Release
  19/19 each; 20 command and 10 selected protocol test repeats
  per configuration; 50-seed UBSan smoke PASS.
- 3A.1C-1D: COMPLETE — feature commit
  `6c402b8a929b695f9abd51949c5d3967f1ce95c9`,
  pushed; draft PR #11 created.
- 3A.1E-R2: COMPLETE — repaired obsolete Linux fuzz workflow
  discovery guard with CI-only commit
  `9d64403925b62b108ee1172c06ad9b866592d509`.
  PR-head Linux Debug/Release #39784944527 SUCCESS (19/19 each);
  ASan+UBSan Clang 18 fuzz #39784944465 SUCCESS (20/20,
  50 verified seeds and 10,000 libFuzzer executions).
- 3A.1F: COMPLETE — PR #11 merged as true two-parent commit
  `4b493ac7984b4c3bdf47a24d41b517930cd689b3`;
  post-merge Linux Debug/Release CI #39786009239 SUCCESS
  (19/19 each plus repeat suites).
- 3A.1G: COMPLETE — local `main` safely fast-forwarded to
  merge commit; macOS Debug/Release 19/19 each; clean synchronized tree.
- 3A.2A: COMPLETE — five-document status draft verified on
  merged main; macOS Debug/Release 19/19 and UBSan seed replay PASS.
- 3A.2B: COMPLETE — documentation-only commit
  `c369fdf3c223361bfd7a7966dc3228d16b8ec2b7`;
  Linux main Debug/Release CI #39789969583 SUCCESS (19/19 each
  and repeated protocol/network validation).
- 3A.2C: COMPLETE — final status commit
  `bb9610e4422dac7fa383a3473abf2e67dbe1766b` passed exact-head
  Linux CI #39792311541 (19/19 Debug and Release). Phase 3A closed.

Phase 3A introduced no dispatcher, arity validation, built-in
commands, keyspace or production RESP2 socket handler.

### Phase 3B Detailed Progress — COMPLETE

- 3B.0A: COMPLETE — baseline `bb9610e`, final 3A CI verified;
  feature branch `phase3b/command-dispatcher` created.
- 3B.0B: COMPLETE — documented dispatcher registry, owned responses,
  typed unknown outcome and negative-case matrix.
- 3B.1A-1B: COMPLETE — isolated dispatcher and CMake tests;
  formatting/repeat regression and 50-seed UBSan smoke PASS.
- 3B.1C: COMPLETE — one five-file feature commit
  `e92aaf54cd00e090620be64ede21f32cde7e0549`, non-force pushed.
- 3B.1D-1E: COMPLETE — PR #12 draft and ready; exact feature-head
  Linux #39805019741 20/20 Debug/Release; Clang 18 ASan/UBSan
  #39805019351 21/21, 50 seeds, 10,000 fuzz runs SUCCESS.
- 3B.1F: COMPLETE — true two-parent merge
  `84d95500545eb4c0a417cd95cc98c62e30384424` with identical
  feature tree and preserved branch. Main Linux CI #39806894796
  SUCCESS (20/20 each plus protocol/network repeats).
- 3B.1G: COMPLETE — clean synchronized local/remote `main`;
  macOS 20/20 Debug/Release, dispatcher/command/protocol/TCP repeats,
  UBSan dispatcher and 50-seed parser corpus replay PASS.
- 3B.2A: COMPLETE — five acceptance documents drafted and locally
  validated (20/20 macOS Debug and Release, UBSan and 50 seeds).
- 3B.2B: COMPLETE — five-document-only commit
  `cc88f88948acd96574479750eefc7ede6445c70a`; exact-head Linux
  Debug/Release #39810737185 SUCCESS (20/20 each plus repeat gates).
- 3B.2C: COMPLETE STATUS RECORDED — five-document final-status
  checkpoint; final exact-status-commit Linux CI verification PENDING.
  Phase 3C starts only after this last CI is independently accepted.

Phase 3B closure CI #39811900890 passed. Phase 3C is COMPLETE.
Phases 3D–3F subsequently completed, including redis-cli
compatibility, bounded error responses and final Phase 3 CI
#39841314816 / #39841314983 SUCCESS.

### Phase 3C Acceptance — COMPLETE

- Isolated argument validator and 20,000 policy checks.
- Feature SHA `38c5354`; direct merge `d733daa`.
- macOS Debug/Release 21/21; UBSan-enabled 22/22.
- Linux #39828953490: Debug/Release 21/21 each.
- Clang 18 #39829737702: 22/22, 50 seeds, 10,000 fuzz runs.
- Production server remains transport-only TCP echo.

### Acceptance Gate

Standard redis-cli can connect and successfully execute
the supported connectivity commands.

# Phase 4 — Core String Database

**Status:** COMPLETE (4A–4H merged, validated and formally accepted on 2026-10-23)

- [x] 4A — Keyspace and entry model (merged `ed3056c`, main CI accepted)
- [x] 4B — SET, GET (merged `e1dd5e8`, post-merge Linux CI passed)
- [x] 4C — DEL, EXISTS, TYPE (merged `d739707`, main CI accepted)
- [x] 4D — MSET, MGET (merged `b457987`, main CI accepted)
- [x] 4E — APPEND, STRLEN (merged `18965b3`, post-merge Linux CI passed)
- [x] 4F — INCR, DECR, INCRBY, DECRBY (merged `f752bed`, main CI accepted)
- [x] 4G — Numeric edge cases and overflow (merged `d7877af`, post-merge Linux CI passed)
- [x] 4H — Multi-client correctness tests (merged `31914ae`, main CI accepted)

### Acceptance Gate — PASS

Supported string and key commands match the documented Redis compatibility subset.
Phase 4H exact-head main Linux networking CI #39915994311 passed Debug 39/39
and Release 39/39; Clang 18 ASan/UBSan + fuzz CI #39915994295 passed 40/40,
verified 50 immutable seeds and completed 10,000 libFuzzer executions.
macOS Debug 39/39, Release 39/39 and fuzz-enabled 40/40 also passed.
Known intentional limits: 8,176-byte stored values, 8,192-byte single replies,
full-keyspace clone-and-swap for MSET. TTL, eviction, AOF, snapshots and
unsupported Redis command options remain future phases.

# Phase 5 — TTL and Expiration

**Status:** COMPLETE AND ACCEPTED (2026-10-23; audited implementation baseline `479fdf4`)

- [x] 5A — Clock abstraction (merged `05d0ca4`, post-merge Linux CI passed)
- [x] 5B — Expiration metadata (merged `f51d8ad`, post-merge Linux CI passed)
- [x] 5C — Lazy expiration (merged `11b1326`, post-merge Linux CI passed)
- [x] 5D — EXPIRE, PEXPIRE (merged `7169576`, post-merge Linux CI passed)
- [x] 5E — TTL, PTTL, PERSIST (merged `7782dfc`, post-merge Linux CI passed)
- [x] 5F — SET EX/PX (merged `2756f01`, post-merge Linux CI passed)
- [x] 5G — Active expiration (merged `5c1cf93`, bounded index, 32 checks per 100 ms, post-merge Linux CI passed)
- [x] 5H — Deterministic expiration acceptance (merged `479fdf4`, post-merge Linux CI passed)

### Acceptance Gate — PASS

Expired keys are invisible on access; injectable wall-time deadlines and
monotonic event-loop scheduling coexist; active cleanup is deadline-ordered
and bounded. Correct TTL/PTTL rounding, value/deadline atomicity, mutation
index consistency, cross-client behavior and deterministic model testing
were verified in macOS Debug 52/52, Release 52/52, fuzz-enabled 53/53;
Linux Debug 52/52, Release 52/52, Clang 18 ASan+UBSan 53/53 at exact
baseline `479fdf4`. The immutable 50-seed RESP2 corpus and 10,000 Linux
libFuzzer executions also passed. See `docs/PROJECT_STATE.md` for the
formal closure record and deliberate compatibility limitations.

# Phase 6 — Collection Data Types

**Status:** IN PROGRESS — Phases 6A–6F formally COMPLETE.
Phase 6F implementation was merged at `dabe5941e385180d49da2abfbed9b818e4e06487`;
reviewed five-document closure `fbd3e9de90d77eeecc665abfad71f68c2f0ff86f` has tree `8698677050cecbe36cb1588ac02610876cc2c5a4`.
Docs feature Linux CI `40070113583` / `40070113584` and final exact-main
networking `40070533556` (Debug/Release) and Clang 18 ASan/UBSan/
libFuzzer `40070533439` passed all required jobs/steps.
Phase 6G differential testing is NEXT and NOT STARTED.

- [x] 6A — Type-safe value representation (merge `5043cc7`; exact-head Linux CI `39962981311` and `39962981227` PASS)
- [x] 6B — List storage and commands (implementation `7df2e08`; formal closure `8179872`; post-closure Linux `39975274770` / `39975274694` PASS)
- [x] 6C — Hash storage and commands (formal docs closure `201c3f1`; final networking `39987655559` and fuzz `39987655669` PASS)
- [x] 6D — Set storage and commands (formal docs closure `281d044`; final Linux networking `39999397205` / fuzz `39999397202` PASS)
- [x] 6E — Wrong-type errors (formal closure `d2dc17d`; final Linux `40007963691` / `40007963507` PASS)
- [x] 6F — Collection memory accounting hooks (formal closure `fbd3e9de90d77eeecc665abfad71f68c2f0ff86f`; final Linux `40070533556` / `40070533439` PASS)
- [ ] 6G — Differential tests

### Phase 6F formal closure — VERIFIED PASS (2026-10-24)

The reviewed docs feature `698b800b4e794d437f4bc6f21d7d1264283b5705` contains exactly the five
authoritative documentation changes and is the second parent of guarded
two-parent non-fast-forward main merge `fbd3e9de90d77eeecc665abfad71f68c2f0ff86f`; the first
parent is implementation merge `dabe5941e385180d49da2abfbed9b818e4e06487`.
The common documentation tree is `8698677050cecbe36cb1588ac02610876cc2c5a4`. The documentation
merge introduced no production source, test, CMake or workflow changes.
Feature networking Linux `40070113583` and Clang 18 sanitizer/fuzz
`40070113584` passed; exact-main post-closure networking `40070533556`
and Clang 18 ASan/UBSan/libFuzzer `40070533439` completed successfully
with every required job/step. Phases 6A–6F are formally complete.
Phase 6G is not started. The implementation acceptance paragraph below
retains its historical, pre-closure gate wording.

### Phase 6F implementation acceptance — VERIFIED PASS; formal docs closure pending (2026-10-24)

The V1 deterministic logical-byte estimator, checked transitions, and live
O(1) cached database ledger are implemented. Policy constants: key 64,
list element 48, hash field 80, set member 56, expiry 48 plus binary lengths.
Expired entries still physically resident remain charged. This is not RSS,
`maxmemory`, noeviction enforcement or LRU and adds no RESP2 command.

Five-file source feature `9ff7b8eab1bdbf216346c337e911effcbfe136e8`
was merged via exact two-parent main commit
`dabe5941e385180d49da2abfbed9b818e4e06487`, common tree
`405a8f6a322e6c2ddd17e259abb2ba0e263c8c14`. macOS Debug and
Release each passed 67/67 CTests; fuzz-enabled passed 68/68. Independent
fault-path harness exercised 27 scenarios with 142 injected allocation
failures, not 142 different internal allocation sites. Feature Linux
networking/fuzz `40067133805`/`40067133793` and postmerge-main Linux
networking/fuzz `40067532663`/`40067532713` passed all required jobs.

**Remaining formal 6F gates:** independent five-document draft review;
docs-only feature commit and exact-head Linux CI; direct guarded non-FF
five-document merge; final exact-head main CI and status acceptance. Keep
6F unchecked here until these gates pass. 6G differential tests have not
started. Phase 7 memory-limit/eviction and Phase 8+ persistence remain future.

### Phase 6E formal closure — VERIFIED PASS (2026-10-24)

The exact reviewed implementation merge `37aaad4068ca0cf4452635737bd219a11d120d4c`
and documentation feature `46a20ad3b2da9237741194bd8ddfd2e68a30ab0a`
were merged in a guarded two-parent docs-only closure commit
`d2dc17de7b046ed8cf007189a698d7b61e4344f3`, tree
`aff0976dbffd7e479a63f11b50019f1eabdbb1a7`. Only the five
approved documentation paths changed. Final `main` Linux Debug/Release
`40007963691` and Clang 18 ASan/UBSan/fuzz `40007963507` passed all
jobs and steps. Phase 6F.0A independently reverified the complete ten-
workflow lineage and macOS Debug/Release 64/64, fuzz-enabled 65/65.
The Phase 6E implementation record below is retained as historical context;
its original unclosed gate was satisfied by this final acceptance.
Phase 6F and 6G engineering remain unstarted.

### Phase 6E implementation acceptance — PASS (2026-10-24)

Retained feature `phase6e/wrongtype-acceptance` was committed as
`a66128b077cfbfc22fbe6daca1e053e5fdaa111e` from final Phase 6D
closure `281d044f4f09ac4b33f4323067ef4bc20c46bbbe`. It added the
permanent CTests `unit.wrongtype_matrix` and
`integration.resp_wrongtype_matrix`, each with 26 typed commands x three
incompatible stored values = 78 precise WRONGTYPE rejection cases.
The acceptance also verifies error atomicity, full value/type/TTL
preservation, missing keys, binary-safe values, permitted SET/MSET
replacement, MGET nil for incompatible stored kinds and pipeline
recovery. The two-parent no-ff implementation merge to main is
`37aaad4068ca0cf4452635737bd219a11d120d4c`, with second parent
being the feature SHA and first parent being the Phase 6D closure.
Feature and merge tree `2ff6ff4be49c6b81cd787a8c5a4ec4b992b10712`;
exactly four modified build/docs files plus two added tests. Production
command/server/storage sources and CI workflows are unchanged.

Local macOS Debug/Release 64/64 and fuzz-enabled 65/65 passed. Both
feature-head Linux workflows `40004381493` and `40004381506` passed;
post-implementation main Linux networking `40005159337` and Clang 18
sanitizer/fuzz `40005159401` passed with every job and step green.

**Formal closure gate remains open:** separately reviewed exactly-five-
document feature, its exact-head Linux Debug/Release and ASan/UBSan/fuzz,
guarded two-parent docs-only merge, and final post-closure main CI.
Only then update the 6E checkbox and begin Phase 6F memory accounting.
Phase 6G differential testing, eviction, persistence and deployment
remain future work.

### Phase 6D formal closure acceptance — PASS (2026-10-23)

Final two-parent documentation merge `281d044f4f09ac4b33f4323067ef4bc20c46bbbe`
uses Phase 6D implementation merge `d21421ad910a179ea877fc7f1e555688ca8a81a8`
and reviewed five-document feature `c030abe0d6339a78bad62542f715581b1f1b5f3e`.
Both final `main` push workflows `39999397205` and `39999397202` passed all
Linux Debug/Release and Clang 18 sanitizer/fuzz jobs and steps. The 6E.0A
read-only audit again passed macOS Debug/Release 62/62 and fuzz-enabled 63/63.
Phase 6E cross-type WRONGTYPE acceptance is a separate, unfinished milestone.

### Phase 6A acceptance — PASS (2026-10-23)

Four owned, binary-safe variants are type-distinct; list ordering, hash
field overwrite, set member uniqueness, deep copy/move semantics, aggregate
`Entry` initialization, and optional wall-clock TTL metadata are tested.
The existing string-command and expiration engine is unchanged; there is
no public collection insertion route yet. Local Debug and Release each
passed 53/53; fuzz-enabled 54/54. The feature and two-parent merge have
separately green exact-head Linux networking and Clang 18 ASan+UBSan/fuzz
workflows (run IDs recorded in `docs/PROJECT_STATE.md`). No collection
commands, general WRONGTYPE handling, eviction, or persistence are claimed.

### Phase 6B implementation acceptance — PASS (2026-10-23)

Feature branch `phase6b/list-storage-commands` contains two reviewed commits:
`18e27a0995071acfed579aa52d23167f15e16a09` and
`00dddb0db60913c0a9d8e990c4dd49b71c6b8eb6`.
Two-parent merge `7df2e08209468ae3e8a619e25451abe9196bda11` has
parents `e895e7cd06474a0ae2e1079591409eace86f517c` and the latter
feature commit. Its tree equals the feature tree
`1acc6498c99f007fa4f747e578ebc43ca85ee814`.

`LPUSH`, `RPUSH`, `LLEN`, `LPOP`, `RPOP`, and `LRANGE` are reachable through
live TCP RESP2 and shared storage. Tests validate binary-safe payloads,
list ordering, duplicates, missing keys, negative and extreme range indexes,
WRONGTYPE handling, SET/MSET replacement, MGET nil for non-string entries,
TTL preservation and last-element expiration-index deletion, capped
8,192-byte LRANGE replies, parser fragmentation, pipelining, cross-client
visibility and optional redis-cli interoperability. Local Debug/Release
passed 56/56 each; fuzz-enabled passed 57/57. Exact-head feature CI
`39972085606` (Linux Debug/Release) and `39972085386` (Clang 18 fuzz/
sanitation) passed. Post-merge main CI `39973111123` and `39973111117`
passed with all jobs and steps green. The audited combined implementation
scope is eleven files. No PR, force push or branch deletion.

This Phase 6B implementation acceptance is historical; its separately
audited five-document closure later merged at `8179872` and passed exact
post-closure Linux runs `39975274770` and `39975274694`. Hash functionality
was implemented in Phase 6C. Set commands, blocking list operations,
general Phase 6E wrong-type coverage, eviction, persistence and deployment
remain outside Phase 6B.

### Phase 6C implementation acceptance — PASS (2026-10-23)

The accepted baseline is Phase 6B final docs closure commit
`81798723a794f261682844a4fe01dc5adf9eeb35`, with independently
accepted Linux networking `39975274770` and fuzz `39975274694`.

The retained feature branch `phase6c/hash-storage-commands` holds storage
foundation commit `0a6a560dc6bc9746617bd8da7d020cd921993916` and RESP2
hash-route commit `88817b205b511f7bf4799ea2a675a74845427f16`.
Direct two-parent non-fast-forward implementation merge
`7b0abaa20b65b6095dd432784aced4a16fc70d32` has the prior 6B closure
and second feature commit as parents. Merge and feature tree are both
`1fdac28176454971c28281da8ded2ed0a2108d2e`; nine implementation
files changed, with no documentation edits or workflow modifications.

Six binary-safe hash storage APIs and seven TCP RESP2 commands
(HSET/HGET/HEXISTS/HLEN/HDEL/HMGET/HGETALL) work with the shared
single-threaded database, lazy/active expiry and existing string/list
routes. They support field overwrite, missing/nil semantics, wrong-type
rejection, TTL retention on mutations, last-field deletion with expiry-index
cleanup, bounded replies, fragmented/pipelined requests, cross-client
visibility and optional redis-cli. Hash iteration order is unspecified;
full Redis hash compatibility and future Phase 6E type matrices are not
claimed. Existing 8,192-byte reply and reply-safe input limits apply.

macOS Debug/Release 59/59 and fuzz-enabled 60/60 passed. Feature-head Linux
networking `39983170325` and fuzz `39983170439` passed. Exact-head main
post-implementation Linux `39984084722` (Debug/Release) and `39984084843`
(Clang 18 sanitizer/fuzz) passed with every job and step green. Both
feature commits, remote pushes and implementation merge were guarded;
no PR, history rewrite, forced push or branch deletion occurred.

**Historical Phase 6C closure:** separately reviewed documentation
merged as `201c3f1f3036ad70e69d8a15b84fc5202782fc97` and final
main Linux networking `39987655559` and fuzz `39987655669` passed.
Phase 6D implementation was started only after that closure. The Phase 6
acceptance gate remains open until 6D formal closure and 6E–6G.

### Phase 6D implementation acceptance — PASS (2026-10-23)

The accepted starting point is formal Phase 6C closure
`201c3f1f3036ad70e69d8a15b84fc5202782fc97`.
Binary-safe set storage and RESP2 routes were implemented on retained
branch `phase6d/set-storage-commands`. First commit
`743261636c2926f40b56f61856bcb9725e8e9b72` introduces `set_add`,
`set_remove`, `set_contains`, `set_cardinality`, `set_members` plus tests.
Second commit `40657326e93631d8851c780424a730389b1f2cbb` adds SADD,
SREM, SISMEMBER, SCARD, SMEMBERS and SMISMEMBER, command tests and
real TCP integration coverage. The direct two-parent no-ff implementation
merge `d21421ad910a179ea877fc7f1e555688ca8a81a8` has parents
`201c3f1f3036ad70e69d8a15b84fc5202782fc97` and the second feature
commit. Merge/feature tree `e1cadc1b2c86b24d37ce6819bd3926c87eda507d`;
exactly nine source/test/build files and no documentation/CI changes.

Exact byte-wise uniqueness, duplicate argument counts, missing key reads,
WRONGTYPE handling, TTL preservation, deleting the last member and the
expiry-index record, reply-safe writes, 8,192-byte bounded unordered
SMEMBERS and positional integer SMISMEMBER are verified. Coverage includes
request fragmentation, pipelining, concurrent TCP clients and existing
string/list/hash interoperability. No set algebra, random sampling,
blocking commands or full Redis set compatibility is claimed.

Local macOS Debug/Release 62/62 and fuzz-enabled 63/63 passed, including
three new permanent CTests. Feature-head Linux networking `39995306323`
and fuzz `39995306807` passed. Post-implementation main networking
`39996097334` and Clang 18 sanitizer/fuzz `39996097046` both passed
at the exact implementation merge, with every job and step green.

**Formal closure gate:** independent five-document-only `phase6d/closure-audit`
branch, exact-head docs Linux CI, a two-parent no-ff docs-only merge and
post-closure main Linux CI remain required before Phase 6E begins.
The Phase 6 acceptance gate is still open for 6E–6G; eviction,
persistence, remote deployment, memory accounting and differential
Redis-subset testing have not been implemented in Phase 6D.

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

**Status:** NOT STARTED — pending Stage 6B

Begin with detailed architecture and crash-safety design review (10A/10B) before
modifying persistence code. All seven original tasks remain pending.

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

**Status:** COMPLETE AND ACCEPTED through Stage 6A exact-main CI.

11A–11D, core 11F–11G and bounded 11H foundations pre-existed. Stage 6A supplied
public 11E timeout with active-work protection, 11F drain deadline and additional
11H bounded 32-client correctness stress. Every original item is implemented and
validated; 11H acceptance is explicitly bounded, not an indefinite soak guarantee.
The [accepted audit](CONTINUATION_HANDOFF.md#stage-6a-implementation-and-audit)
retains reused/new scope and failed/corrected historical evidence.

- [x] 11A — Input-buffer limits
- [x] 11B — Output-buffer limits
- [x] 11C — Slow-reader handling
- [x] 11D — Connection limits
- [x] 11E — Idle connection cleanup
- [x] 11F — Graceful shutdown
- [x] 11G — Partial-write and disconnect tests
- [x] 11H — High-concurrency soak tests

### Historical Stage 6A pre-acceptance checkpoint (superseded)

The following publication-time wording is retained as history; accepted status
above supersedes it:

> **Status:** Stage 6A local audit/implementation; acceptance PENDING
>
> Previously implemented: 11A–11D, core 11F–11G and bounded 11H foundations.
> New branch: public 11E timeout with active-work protection, 11F drain deadline,
> 11H bounded 32-client correctness stress. Source/test evidence, local results and
> Linux CI state for every row: [handoff audit](CONTINUATION_HANDOFF.md#stage-6a-implementation-and-audit).
> Checkboxes remain open until publication/CI/merge acceptance.

### Acceptance Gate

Slow, disconnected or malformed clients cannot
trivially exhaust server resources.

# Phase 12 — Configuration and Observability

**Status:** COMPLETE AND ACCEPTED through Stage 6A exact-main CI.

12A–12B startup configuration/validation and core 12G failure isolation pre-existed.
Stage 6A extended idle validation and implemented 12C–12F plus structured 12G
diagnostics. INFO/CONFIG GET retain their documented subsets and read-only
behavior. Exact-main execution and the accepted audit are in the
[handoff](CONTINUATION_HANDOFF.md); historical publication failures remain recorded.

- [x] 12A — Configuration parser
- [x] 12B — Startup validation
- [x] 12C — Structured logging
- [x] 12D — Runtime counters
- [x] 12E — INFO subset
- [x] 12F — Selected CONFIG GET
- [x] 12G — Operational error reporting

### Historical Stage 6A pre-acceptance checkpoint (superseded)

The following publication-time wording is retained as history; accepted status
above supersedes it:

> **Status:** Stage 6A local implementation; acceptance PENDING
>
> 12A–12B startup flags/validation and core 12G client/persistence failure behavior
> pre-existed. This branch extends idle validation and adds 12C–12F and structured
> 12G diagnostics. [Handoff](CONTINUATION_HANDOFF.md) distinguishes reused/new work,
> local validation and pending Linux CI; no formal completion claimed.

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

**Status:** Linux/Docker core accepted through Fast-track 4A/4B; Stage 5A health/Compose additions pending validation
**Deployment Milestone A: MANDATORY**

- [x] 15A — Linux build validation
- [x] 15B — Multi-stage Dockerfile
- [x] 15C — Minimal runtime image
- [x] 15D — Non-root container execution
- [x] 15E — Persistent data volume
- [ ] 15F — Container health checks
- [x] 15G — Graceful container shutdown
- [x] 15H — Container restart recovery
- [x] 15I — GitHub Actions CI
- [x] 15J — Fresh-clone reproducibility (clean CI checkout/build)
- [x] 15K — Docker-based demo script (Stage 4A owned container harness)

### Acceptance Gate

Reproducible Linux container build, startup, testing,
shutdown, persistence recovery and demonstration.

# Phase 16 — Private Remote Linux Deployment

**Status:** EXCLUDED BY APPROVED SCOPE REVISION (2026-10-26); not complete.
**Not completed; no actual remote deployment claimed.**

The following original plan is preserved for historical context. Stage 5A
preparation/runbooks remain available; provisioning, remote storage and SSH
acceptance are no longer mandatory. Docker/Linux validation in Phase 15 remains
mandatory. These unchecked historical items do not block revised completion.

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
- Original private remote Linux deployment outcome excluded by revised scope;
  no remote deployment claimed.
- Final engineering audit.
- Verified resume-ready technical achievements.

# Source of Truth

- Approved planning handoff: October 19, 2026.
- Live progress: `docs/PROJECT_STATE.md`.
- Engineering decisions: `docs/DECISIONS.md`.
- Architecture: `docs/ARCHITECTURE.md`.

This roadmap describes planned work. It is not
evidence that future phases have been implemented.
