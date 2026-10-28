# Stage 6B / 10D.1C — Engineering accepted on main; formal documentation closure preparation

**2026-10-28: Stage 6B / 10D.1C engineering is COMPLETE AND ACCEPTED ON MAIN.**
Independent verification accepted the corrected two-commit feature, genuine
engineering merge and exact-main Linux evidence. 10D.1A and 10D.1B remain
accepted. This checkpoint supersedes older current-status/next-action statements,
including statements that 10D.1C is unimplemented; all previous handoff bytes are
retained as an exact suffix below. Historical statements remain records of their
original preparation time, not retroactively edited claims.

## Preparation status and separate documentation gates

10D.1C engineering is accepted on main. Formal documentation closure has been
prepared locally; independent review, feature publication/exact-feature CI,
guarded main merge and exact-main CI remain separate gates. At preparation time
on 2026-10-28, local `fasttrack/10d1c-formal-closure` points directly to the accepted
engineering merge with exactly four unstaged, uncommitted documentation changes.
It has not been pushed; no docs-feature CI or documentation acceptance is claimed.
Future readers may consult actual Git refs/workflow evidence to determine which
subsequent gates completed. No new C++ build/CTest or fuzz run was executed for
this documentation-only preparation; all execution evidence below is retained
engineering evidence.

## Exact accepted Git identities and retained branches

- Engineering merge: `a7692101da60213c0838d1a372787e8429337797`.
- Merge and corrected-feature tree: `3293a6bc61908b4d9eac56718d8627f1fa24f5a8`.
- Exactly two ordered merge parents: first
  `1be4505f30f0c87b6d71e79eafb0a63477855b18`, then
  `762c53285810fbf61aa4c85be296727e0da8a077`.
- Original implementation: `819543dfdcd39d68ff7496c7c421608d2d283774`, sole parent
  `1be4505f30f0c87b6d71e79eafb0a63477855b18`; original tree
  `cddbefa76800665646c58e2121032958ce4b05bb`.
- Warning correction: `762c53285810fbf61aa4c85be296727e0da8a077`, sole parent
  `819543dfdcd39d68ff7496c7c421608d2d283774`. Feature was exactly two commits
  ahead of old main, zero behind; both commits remain reachable through the
  engineering merge's second parent. `--no-ff --no-commit` staging and the final
  merge produced exactly the independently accepted corrected-feature tree.

| Retained branch | Accepted SHA |
|---|---|
| `fasttrack/v2-generation-candidate-search` | `762c53285810fbf61aa4c85be296727e0da8a077` |
| `fasttrack/10d1b-formal-closure` | `8a8758692c27f1cbf5be94a20b72092ed4e2564e` |
| `fasttrack/v2-owned-data-staging` | `5694cda2b886f298124705b12ce6bca3aa286287` |
| `fasttrack/v2-root-identity-validation` | `91b30f3c8529ed80954414a601b4ce102770888e` |
| `fasttrack/10d1a-formal-closure` | `ab7179ee5abc4c28e0ebf6552eb514d91bfe2a79` |

Retained branches are historical evidence, not branches to rewrite or delete.

## Exact seven-file engineering manifest

Cumulative old-main-to-corrected-feature/main diff: **seven paths, 1754 additions
and one deletion**, four additions and three modifications, with no mode change.

| Status | Accepted path |
|---|---|
| NEW | [include/redisdb/persistence/V2Generation.hpp](../include/redisdb/persistence/V2Generation.hpp) |
| NEW | [src/persistence/V2Generation.cpp](../src/persistence/V2Generation.cpp) |
| NEW | [src/persistence/V2GenerationAccess.hpp](../src/persistence/V2GenerationAccess.hpp) |
| NEW | [tests/unit/v2_generation_test.cpp](../tests/unit/v2_generation_test.cpp) |
| MODIFIED | [include/redisdb/persistence/V2Root.hpp](../include/redisdb/persistence/V2Root.hpp) |
| MODIFIED | [src/persistence/V2Root.cpp](../src/persistence/V2Root.cpp) |
| MODIFIED | [CMakeLists.txt](../CMakeLists.txt) |

Corrected generation-test Git blob: `9efee38090352238401ba4b697a91346aae8e7fa`.
The other six files retain original accepted implementation bytes. All 214
original non-scope tracked paths were preserved by the engineering change. This
formal preparation adds only README, CONTINUATION_HANDOFF, PROJECT_STATE and
ROADMAP checkpoints; all source/tests/build/CI/Docker/benchmark files and frozen
contracts remain byte-identical to accepted engineering main.

## Source/API, root ownership and observation semantics

`GenerationCandidate` is move-only with noexcept lifecycle. Its public surface
is `find_after(RootLease&, GenerationId, LogicalSequence, max_candidates)`,
`valid()`, `error()`, borrowed `generation()` and `recheck_unused()`. `valid()`
means a healthy attached observation; the borrowed generation confers no
ownership and is invalidated by move, failure or destruction. **GenerationCandidate
is an observation, NOT a reservation, publication or proof of durable selection.**

The private `detail::GenerationAccess` bridge shares the SAME existing
`detail::RootState`, held root/LOCK ownership and first terminal-error latch as
RootLease, pins and staging siblings. There is no artifact descriptor, root
reopen, second LOCK, independent failure domain, claim registry or durable
ownership token. Dependents retain root/LOCK lifetime; the final reference closes
existing descriptors once, preserving operational close failure without retrying
consumed descriptor numbers. Move assignment preserves target-disposal failure
without poisoning an unrelated incoming healthy root. Callers must serialize
operations/destruction. The lease excludes cooperating writers, not hostile
directory mutation. Exact duplicate observations on the SAME lease are allowed.

Search is successor-only from a caller-provided selected GenerationId; there is
no first-generation factory or `FirstGenerationContext`. The caller supplies the
authoritative selected generation and synchronized captured LogicalSequence.
The search checks exhaustion but does not read CURRENT, authenticate that tuple,
advance a sequence or synchronize a writer. Caller authenticity/synchronization
remains a future coordinator responsibility.

## Exact occupancy, bounds and failure semantics

- Explicit budget is **1–4096** candidate triples. Invalid budget is rejected
  before filesystem entry as local `ResourceLimit` on a healthy lease; an existing
  root error takes precedence. It does not poison an otherwise healthy root.
- After root health/identity checks, captured sequence `UINT64_MAX` yields shared
  terminal `SequenceExhausted`; selected generation `UINT64_MAX` yields shared
  terminal `GenerationExhausted`. Sequence exhaustion is checked first. Arithmetic
  never wraps to zero. A free successor at `UINT64_MAX` may still be observed;
  no greater successor exists after it.
- Names are canonical checked decimal `base-<g>.snap`, `tail-<g>.aof` and
  `tail-<g>.commit`, derived internally with round-trip checks into fixed
  33-byte buffers (at most 32 basename bytes plus NUL). No arbitrary-name API.
- Held-root-relative Base/Tail/Watermark inspection uses existing
  `RootIo::named_stat` with `AT_SYMLINK_NOFOLLOW`, with root identity checked
  before/after each triple. Any existing final entry, including a directory,
  symlink/dangling symlink or unrelated file type, means occupied; its target is
  never followed and its artifact is not opened, overwritten, repaired or removed.
- All three names are inspected even after an earlier occupied name, unless a
  terminal failure interrupts inspection. Only actual `-1/ENOENT` means absence;
  actual `-1/EINTR` retries after accepted observers. Observer errors, including
  observer EINTR, are terminal. Unexpected errno and invalid syscall results
  fail closed as `IoFailure`; impossible results/invalid errno map to EIO.
- An occupied triple during initial search skips to the next greater generation.
  If the occupied generation is `UINT64_MAX`, `GenerationExhausted` takes
  precedence over budget exhaustion. Exhausting the entered budget while greater
  candidates could exist yields shared terminal `ResourceLimit`.
- An initially absent triple gets a complete second absence confirmation.
  Occupancy on that confirmation is terminal `CandidateOccupied`, not another
  search/skip. `recheck_unused()` inspects the SAME observed triple; later
  occupancy is likewise terminal `CandidateOccupied`, not a new candidate.
- Entered I/O/identity/exhaustion/occupancy failures preserve the shared first
  terminal RootError (operation, role, stage and errno); siblings cannot continue
  as healthy after that failure. Candidate probing creates, reads, writes,
  renames, removes or syncs no generation artifact and performs no root fsync.

## Local validation and corrected-warning history

Accepted **local macOS** Debug **85/85**, Release **85/85** and preserved
UBSan/fuzz **86/86** executions each included **328 executed generation scenarios
and 190 reached boundary injections**. These detailed scenario/injection counts
are attributed to local execution, not an independent Linux scenario recount.
Complete generation/root/staging stdout after the one-line correction matched
accepted pre-correction stdout byte-for-byte in all three local configurations.
The accepted root coverage (23 positive / 57 rejection / 67 syscall-boundary /
20 reached allocation failures) and staging coverage (266 scenarios / 195 reached
injections) were preserved. Pinned local 50-seed smoke and strict C++20 -O0/-O3
warning-as-error compilation passed without weakening flags, tests or sanitizers.

The original feature SHA `819543dfdcd39d68ff7496c7c421608d2d283774` had successful
workflow wrappers but **FAILED engineering Linux acceptance**: both actual GCC 13
Debug/Release logs reported `-Wrange-loop-construct` in the new generation test
at line 813. Original runs were [Networking 40707899542](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40707899542),
[Clang 18 40707899440](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40707899440) and
[Docker 40707899368](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40707899368). They are failed-acceptance history,
not corrected acceptance evidence.

The sole corrective commit changed exactly +1/-1 line, from `const auto answer`
to `const auto& answer` in the four-invalid-result loop. Cases, body and `[=]`
capture stayed unchanged; no suppression or other source correction occurred.
Corrected feature and main actual GCC 13 logs contain no generation-test
`-Wrange-loop-construct` or staging `-Wnonnull`. Accepted unrelated ListCommands,
V2TailStream and existing allocation-hook warnings remain; do not claim the
whole repository is warning-free. Docker GCC 12 compiles the runtime, not the
generation-test executable, so it is not GCC 12 test-source warning evidence.

## Accepted exact-feature and exact-main Linux evidence

All six accepted runs below are completed/success, event `push`, at the exact
branch/SHA specified. Full actual job logs and required artifacts were inspected;
old feature results were not substituted for fresh main results.

| Evidence | Exact corrected feature: `fasttrack/v2-generation-candidate-search`, `762c53285810fbf61aa4c85be296727e0da8a077` | Exact engineering main: `main`, `a7692101da60213c0838d1a372787e8429337797` |
|---|---|---|
| Networking | [40710745100](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40710745100): Debug 85/85, Release 85/85 | [40715241101](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40715241101): Debug 85/85, Release 85/85 |
| Clang 18 ASan/UBSan/fuzz | [40710745091](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40710745091): 86/86, 50 seeds, actual 10,000 executions | [40715240672](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40715240672): 86/86, 50 seeds, actual 10,000 executions |
| Docker Linux | [40710745180](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40710745180): all required gates, 24 profiles/48 runs | [40715240649](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40715240649): all required gates, 24 profiles/48 runs |

Actual main jobs: Networking Debug `122911020090`, Release `122911019819`;
Clang `122911016672`; Docker `122911017403`. Corrected feature jobs:
Networking Debug `122895024487`, Release `122895024866`; Clang `122895024276`;
Docker `122895024143`. Named `unit.v2_generation`, `unit.v2_root` and
`unit.v2_staging` actually passed in applicable Debug/Release/sanitizer suites,
with existing V1/V2 persistence/networking/server/TCP/pipeline/epoll regressions.
Instrumented compilation verification and pinned 50-seed manifest checks passed;
real fuzzer output included `#10000 DONE` and `Done 10000 runs`, with no sanitizer
diagnostic, crash artifact or skipped mandatory job. The successful unchanged
workflow uploads no standalone compile_commands.json or fuzz artifact; its full
actual logs preserve the execution/instrumentation proof.

Both accepted Docker runs passed GCC 12 Release image/Compose validation,
**22** tooling checks, **15** container cases, **10** private Compose checks,
immutable pinned Redis **8.10.2** comparison (**54 command cases / 40 routes**
plus **10** operational cases), and the exact **24-profile / 48-timed-run**
benchmark matrix. Persistent-volume recreation, restart/snapshot/AOF recovery,
SIGKILL acknowledgment, isolated restore and corruption/missing-file rejection
checks passed. No new throughput threshold was imposed. Private Compose does
not establish actual VPS/SSH deployment or physical power-loss durability.

## Frozen-contract limits and next authorization boundary

Frozen [AOF compaction design](AOF_COMPACTION_DESIGN.md) SHA-256 remains
`fb57f333eb1efc65101c8f788442066e30a751472bda46ab040ce27e5e53e5d6`;
frozen [V2 persistence format](V2_PERSISTENCE_FORMAT.md) remains
`f078163e99768a84e10fb516ca03674e259a32ce1d09380ecce118ba50091462`.
The candidate observation is a narrow successor-search component, not completion
of the frozen publication/recovery contract.

10D.1C supplies **no** first-generation initialization/import authority,
authentication of selected generation/sequence against CURRENT, durable candidate
reservation, prepared generation coordinator, complete P1/P2/P3 construction,
final artifact installation, P4–P7 publication, CURRENT writing/replacement/rename,
directory durability publication, R4/R5 recovery, writer activation, P8 live
handoff, live compaction, generation garbage collection/reclamation or actual
VPS deployment. 10D.1A/B remain accepted; **broader 10D.1, Stage 6B and original
Phase 10 remain unfinished**. Original Phase 16 remains **EXCLUDED BY APPROVED
SCOPE REVISION**, not completed.

**Next proposed boundary: 10D.1D planning only after separate authorization.**
First independently review this exact four-document local draft. Documentation
feature commit/push and fresh exact-feature CI, guarded documentation main merge
and fresh exact-main CI are subsequent separately authorized gates. Any proposed
10D.1D planning must separately define its frozen-contract scope, files,
invariants, failure model and acceptance gates before implementation authority
can be granted. This checkpoint implements no 10D.1D, does not prescribe an
unreviewed coordinator design, and automatically authorizes no subsequent stage.
Do not follow superseded older next-action instructions as current authorization.

# Stage 6B / 10D.1B — Engineering accepted on main; formal documentation closure preparation

**2026-10-28: Stage 6B / 10D.1B engineering is COMPLETE AND ACCEPTED ON MAIN.**
Independent final verification accepted the genuine main merge and exact-main
Linux evidence below. 10D.1A remains accepted. This checkpoint controls current
status/continuation and supersedes older preparation-only or next-action
instructions below; every older handoff byte, including the original local
10D.1B preparation checkpoint, is retained unchanged beneath this entry.

At preparation time on 2026-10-28, this four-document formal closure was a local,
unstaged, uncommitted and unpublished draft on `fasttrack/10d1b-formal-closure`.
Engineering acceptance was independently established; documentation source review
and publication/acceptance were separate gates. Consult actual Git refs and
workflow evidence to determine which subsequent documentation gates have completed.
No future documentation commit, push, merge or CI result is asserted here.

**Accepted Git identities and retained history**

- Accepted engineering main merge: `bc66ebb293c26a54f696a972416c00f4c0a4ff23`;
  tree `bfd594d2dfb9263fff593db39205d5c18b062c1b`.
- Exactly two ordered merge parents: first
  `120a52c1ea60d61931d34dbe195a455c2313b441`, then
  `5694cda2b886f298124705b12ce6bca3aa286287`. A genuine non-fast-forward merge
  was staged with `--no-ff --no-commit`; its verified index and committed tree
  exactly equal the independently accepted corrected-feature tree.
- Original implementation: `875bd3a5d57363dfaf0d64ad2032ee92fdebd668`, sole
  parent `120a52c1ea60d61931d34dbe195a455c2313b441`. Corrective feature:
  `5694cda2b886f298124705b12ce6bca3aa286287`, sole parent the original
  implementation. Both remain reachable through the merge's second parent.
- Retained `fasttrack/v2-owned-data-staging` remains at corrected feature
  `5694cda2b886f298124705b12ce6bca3aa286287`, tree
  `bfd594d2dfb9263fff593db39205d5c18b062c1b`.
  Historical `fasttrack/v2-root-identity-validation` remains at
  `91b30f3c8529ed80954414a601b4ce102770888e`;
  `fasttrack/10d1a-formal-closure` remains at
  `ab7179ee5abc4c28e0ebf6552eb514d91bfe2a79`. These are retained records, not
  branches to rename, delete or rewrite.
- Accepted engineering manifest: NEW `include/redisdb/persistence/V2Staging.hpp`,
  `src/persistence/V2Staging.cpp`, `src/persistence/V2RootAccess.hpp`,
  `tests/unit/v2_staging_test.cpp`; MODIFIED
  `include/redisdb/persistence/V2Root.hpp`, `src/persistence/V2Root.cpp`,
  `CMakeLists.txt`, `docs/CONTINUATION_HANDOFF.md`. The later warning correction
  changed only V2Root.cpp. This formal documentation preparation modifies only
  README, CONTINUATION_HANDOFF, PROJECT_STATE and ROADMAP through new checkpoints;
  source, tests, CMake, workflows, Docker, benchmarks and frozen designs are untouched.

**Accepted unpublished staging capability**

- Move-only, noexcept-lifecycle `StagedFile` owns one staging descriptor and its
  typed role/generation, fixed temporary basename, inode identity, accepted extent
  and Building/SealedFile/closed lifecycle. Public operations are `create_data`,
  `valid`, `error`, `identity`, `byte_length`, `recheck`, append-only `append`,
  caller-buffer `read_exact`, `seal_file`, `discard_checked` and `close_checked`.
  Historical byte length includes accepted partial progress; it is not validity
  or durability evidence after failure.
- The narrow private `detail::StagingAccess` bridge reuses the SAME existing
  `detail::RootState`, held root descriptor, held LOCK descriptor and shared first
  terminal-error latch as RootLease and sibling PinnedFile owners. RootState stays
  private in V2Root.cpp. No raw descriptor/arbitrary-name public API, independent
  root reopen or second lock is introduced. Dependents retain root/LOCK lifetime;
  original 10D.1A identity validation, pinned reads and DependentsAlive behavior
  are preserved, with existing enum numeric values unchanged.
- Only Base/Tail/Watermark and valid GenerationId are supported. Temporary names
  are derived internally as `<final>.tmp.<32 lowercase hexadecimal digits>` from
  all 128 bits supplied by OS `getentropy`, without fallback or UUID alteration;
  a zero token is legal. Maximum basename is 69 ASCII bytes in fixed NUL-terminated
  storage. Held-root-relative create is O_RDWR/O_CREAT/O_EXCL/O_CLOEXEC/O_NOFOLLOW/
  O_NONBLOCK, mode 0600, never O_TRUNC. Actual EEXIST or create EINTR obtains a
  fresh token, bounded to 64 total create attempts. Collisions and ambiguous names
  are never reopened, adopted or removed.
- Descriptor ownership is adopted before After observers. Newly created temps
  must be regular, single-link, initially empty and held/named identity-consistent,
  with root/LOCK rechecks. Final-artifact minimum lengths are not imposed on
  growing temporaries. Append writes only at the tracked end, with subtraction-first
  accepted-size/native-off_t bounds and at most min(64 KiB, SSIZE_MAX) per syscall.
  Readback uses caller-owned output; failed output may be partially filled and
  must be discarded. No sparse/truncate/arbitrary-offset-write capability is added.
- Separate internal `RootState::staging_write()` and `RootState::staging_read()`
  invoke `RootIo::write_at()` / WriteAt and `RootIo::read()` / Read respectively,
  without nullable dual-pointer dispatch. Before/syscall/After ordering, immediate
  errno capture, actual EINTR retries, valid short-I/O accumulation and zero/invalid
  result rejection are preserved. Successful write progress/extent is recorded
  BEFORE After observers, preventing replay of an already successful write.
  Identity and extent checks surround high-level operations, including empty reads.
- `seal_file()` performs checked fsync only on the owned staging file and verifies
  identities/extent afterward. Repeated successful seal revalidates root/LOCK/file
  identity and extent rather than returning unchecked cached success. Appends after
  sealing are rejected. Success does not prove encoded content, EOF/fidelity,
  complete recovery unit, durable directory entry, final-name installation, valid
  CURRENT selection, P4/P7/R4 completion, readiness or publication authority.
- Healthy discard/destructor cleanup may unlink only the exact owned temporary
  after intended-name/role and root/LOCK/held/named type/link/size/identity checks.
  Successful unlink is recorded before After. Close relinquishes descriptor
  ownership before one close attempt; observers cannot suppress it, and ambiguous
  close/unlink is never retried. Explicit close retains the temporary name and
  root-state lifetime. Failure latches the first shared error, blocks later
  write/sync/cleanup through the failed lease, closes owned descriptors once and
  conservatively preserves remaining linked artifacts. No abandoned-generation or
  unowned-file cleanup, repair, overwrite or automatic writer resumption is supplied.

**Verification attributed to the actual checkpoint**

- Original macOS local 10D.1B draft, prepared from main
  `120a52c1ea60d61931d34dbe195a455c2313b441` before feature publication:
  Debug **84/84**, Release **84/84**, preserved UBSan/fuzz **85/85**, focused
  persistence **7/7** each, and 50-seed parser corpus smoke passed. Staging:
  **266 executed scenarios / 195 reached boundary injections**. Original root:
  **23 positive / 57 rejection / 67 syscall-boundary / 20 reached allocation
  failures**. Strict C++20, formatting and whitespace checks passed.
- Original published feature `875bd3a5d57363dfaf0d64ad2032ee92fdebd668` had
  successful workflow wrappers (Networking **40645479884**, Clang/fuzz
  **40645479977**, Docker **40645479872**), but GCC 13 Release and GCC 12 Docker
  emitted the new staging `-Wnonnull` warning. Those runs did NOT satisfy warning
  acceptance and are not substituted for corrected-feature or exact-main evidence.
- Local warning correction, prepared unstaged on original feature 875bd3a before
  commit 5694cda: Debug **84/84**, Release **84/84**, UBSan/fuzz **85/85**, focused
  **7/7** each and 50-seed smoke passed. Complete staging stdout matched the
  original accepted reference in all three configurations, including all **266**
  scenarios, **195** reached injections, partial-progress/observer/first-error
  traces; root **23/57/67/20** and all test bytes were preserved. Strict C++20 and
  formatting passed. Local Linux GCC reproduction was unavailable; later CI, not
  these macOS checks, established Linux warning resolution.
- Corrected feature `5694cda2b886f298124705b12ce6bca3aa286287`, push event on
  `fasttrack/v2-owned-data-staging`, all completed/success:
  [Networking 40650031860](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40650031860)
  Debug **84/84**, Release **84/84**;
  [Clang 18 ASan/UBSan/fuzz 40650032014](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40650032014)
  **85/85**, pinned **50 seeds**, real **10,000** libFuzzer executions, no
  sanitizer failure; [Docker 40650031814](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40650031814)
  all required container/Compose/Redis gates and **24 profiles/48 timed runs** passed.
  Root/staging passed in each applicable suite; actual GCC 13 Release and GCC 12
  Docker compiler logs confirmed the original warning absent.
- Accepted engineering main merge `bc66ebb293c26a54f696a972416c00f4c0a4ff23`,
  push event on main, all completed/success:
  [Networking 40652303976](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303976)
  discovered/passed Debug **84/84**, Release **84/84**, including root/staging,
  V1/V2 persistence, networking, pipeline, TCP server, connection lifecycle and
  epoll repeats; [Clang 18 ASan/UBSan/fuzz 40652303905](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303905)
  discovered/passed **85/85**, pinned **50-seed** corpus verified before/after
  real **10,000** libFuzzer executions, no sanitizer diagnostic or crash artifact;
  [Docker 40652303970](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40652303970)
  image/Compose, **22** tooling tests, **15** container cases, **10** private Compose
  checks, pinned Redis **8.10.2**, **54 command cases/40 routes**, **10** operational
  cases, exact **24-profile/48-run** bounded matrix passed. The engineering audit
  independently checked full logs and concrete artifacts, not wrapper success alone.
- The original staging warning remains absent in actual exact-main GCC 13
  optimized Release and GCC 12 Docker Release output. Inherited unrelated
  ListCommands redundant-move, V2TailStream Marker-shadow and unchanged test
  allocation instrumentation warnings remain separately recorded; this is not a
  claim that the repository is warning-free. Linux CTest logs prove root/staging
  executable success, not an independent recount of their detailed local scenario
  totals. No fresh C++ build/test or new documentation-branch CI is claimed here.

**Explicit scope limits and frozen authority**

10D.1B does NOT implement root/LOCK initialization, P1 generation search/reservation,
full P2/P3 content construction and fidelity/EOF validation, final-name installation,
CURRENT publication, directory durability barriers, P4–P7, standalone R4, R5 recovery
mutation/readiness, prepared writer handoff, live P0/P8 compaction integration,
generation garbage collection, actual VPS deployment, full Stage 6B or Phase 10
closure. The broader 10D.1 milestone remains unfinished. A synchronized staging
file remains unpublished and unselected; it is not a complete recovery unit.

Frozen [V2 format](V2_PERSISTENCE_FORMAT.md) and
[compaction design](AOF_COMPACTION_DESIGN.md) remain authoritative and byte-identical.
Accepted V1 behavior, V2 byte layouts, P0–P8/R0–R5 ordering and original roadmap
inventory are unchanged. Callers serialize operations/destruction; directory
ownership and ancestors are trusted; advisory locks coordinate cooperating
participants and do not defend against hostile uncooperative directory races.
Identity/checksum evidence is not authentication or physical durability proof.
Ambiguous OS close outcomes retain the accepted platform limitation. CI/container
checks do not establish hardware power-loss behavior or actual VPS/SSH deployment.
Original Phase 16 stays EXCLUDED, not complete.

**Formal documentation gates and continuation boundary**

From this dated preparation checkpoint, documentation acceptance requires:

1. Independent review of this exact four-document additive draft.
2. Separately authorized guarded documentation-feature commit/push.
3. Exact-documentation-feature Linux Networking, Clang sanitizer/fuzz and Docker CI.
4. Separately authorized guarded two-parent documentation main merge.
5. Exact-documentation-main Linux CI.

Future readers must consult actual Git refs and workflow evidence to determine
which subsequent gates have completed. The accepted local, corrected-feature and
engineering-main results above belong to those checkpoints; they do not establish
documentation publication or documentation CI. Every previous handoff entry is
preserved byte-for-byte below. Proposed next engineering slice **10D.1C remains
unimplemented and separately unauthorized**; it may only be considered after formal
documentation acceptance and explicit engineering continuation authorization. This
document grants no 10D.1C or later implementation authority. No later stage,
publication, merge or code change is authorized by this preparation checkpoint.

# 10D.1B — owned unpublished data staging (2026-10-28 preparation)

At preparation time on 2026-10-28, this eight-file draft was **local, unstaged,
uncommitted and unpublished**. Independent complete-source review was the next
gate; passing local tests alone did not establish final 10D.1B engineering
acceptance. Local-source acceptance and final engineering acceptance are separate.
This entry supersedes older continuation instructions only;
every prior handoff byte follows unchanged.

- Fresh preflight verified repository/origin, Git author/committer and authenticated
  GitHub account `sinthetaaa`; clean main/index/worktree, no untracked files or
  unfinished operation; normal fetch; exact local HEAD/main/origin/main/actual
  remote main `120a52c1ea60d61931d34dbe195a455c2313b441`, tree
  `0e08e010e09e8b69dea7f8c2fd00198130c0d4ce`, ordered parents
  `d8f301ac384bae34fcf47b430766096cee5ec217` then
  `ab7179ee5abc4c28e0ebf6552eb514d91bfe2a79`. Both retained local/remote
  features match: engineering `fasttrack/v2-root-identity-validation` at
  `91b30f3c8529ed80954414a601b4ce102770888e`; documentation
  `fasttrack/10d1a-formal-closure` at
  `ab7179ee5abc4c28e0ebf6552eb514d91bfe2a79`. New feature absence was checked
  locally/remotely before creating LOCAL `fasttrack/v2-owned-data-staging`
  directly from accepted main. At preparation time on 2026-10-28, HEAD remained
  that baseline; no commit had been made.
- Exact-main baseline workflow metadata verified completed/success at that SHA:
  Networking `40575656379`, Clang 18 sanitizer/fuzz `40575656524`, Docker
  `40575656395`, including required jobs. These are accepted baseline results,
  not implementation CI. Initial sandbox auth/fetch restrictions were resolved
  through approved outside-sandbox checks/fetch; no Git state was repaired.
- Exact implementation manifest: NEW `include/redisdb/persistence/V2Staging.hpp`,
  `src/persistence/V2Staging.cpp`, `src/persistence/V2RootAccess.hpp`,
  `tests/unit/v2_staging_test.cpp`; MODIFIED
  `include/redisdb/persistence/V2Root.hpp`, `src/persistence/V2Root.cpp`,
  `CMakeLists.txt`, and this additive handoff. Frozen specifications, original
  root/format/base/tail tests and codecs, V1 persistence, UniqueFd, Database,
  server/CLI, workflows, Docker, benchmarks and all other documents are unchanged.
- Move-only, noexcept-lifecycle `StagedFile` provides `create_data`, `valid`,
  `error`, `identity`, historical accepted `byte_length`, `recheck`, append-only
  `append`, caller-buffer `read_exact`, `seal_file`, `discard_checked` and
  `close_checked`. Private typed `detail::StagingAccess` borrows the SAME
  existing `detail::RootState`, root descriptor, LOCK descriptor and shared first
  terminal-error latch; the state definition stays in V2Root.cpp. No raw descriptor
  or arbitrary-name API, independent reopen or second lock is added. Existing
  enum numeric values are preserved; staging operations/roles/errors are appended.
- Only Base/Tail/Watermark and valid GenerationId are accepted. Names are derived
  as `<final>.tmp.<32 lowercase hex digits>` in a NUL-terminated 70-byte array
  (maximum basename 69 ASCII bytes). All 128 bits come from OS `getentropy`
  through `<sys/random.h>`; zero is allowed, with no fallback, UUID bits or global
  entropy changes. Exclusive held-root-relative create uses exactly
  O_RDWR/O_CREAT/O_EXCL/O_CLOEXEC/O_NOFOLLOW/O_NONBLOCK, mode 0600. Actual EEXIST
  or create EINTR obtains a fresh token, bounded to 64 total create attempts.
  Ambiguous names and collisions are never adopted, reopened or removed.
- Owned descriptors are adopted before After hooks, then regular/single-link/zero
  extent and held/named identity are checked with root/LOCK checks. Growing temps
  bypass only final-artifact minimum lengths. Append uses tracked-end pwrite and
  subtraction-first file/native-offset bounds, at most min(64 KiB, SSIZE_MAX)
  per call. Valid partial progress is recorded before After observers. Readback
  shares bounded caller-buffer principles; failed output must be discarded.
  Actual EINTR is retried for read/write/file-sync, never ambiguous unlink/close.
- Seal succeeds only after checked file fsync and identity/extent rechecks.
  Repeated seal success revalidates root/LOCK/staged identity and extent, without
  another sync; append after seal fails terminally. Seal proves neither bytes,
  complete EOF, directory durability, recovery-unit validity nor selection.
  Healthy discard/destructor cleanup can unlink only the exact owned temporary
  after all checks. Errors latch shared state, preserve remaining linked files
  and prohibit further write/sync/cleanup; descriptor release is still attempted
  once. Successful unlink is recorded before After; close relinquishes ownership
  before its one syscall. Explicit close retains the name and root-state lifetime.
  Failed cross-root move-assignment disposal preserves the old root's first error
  locally in the incoming owner; it cannot regain health or mutate its incoming
  temporary, and independently healthy peers on the other root remain isolated.
- Actual local macOS configure/build PASS: Debug, Release, preserved
  `build/stage4b-ubsan` (Debug, fuzz ON, original compiler/sanitizer flags and
  byte-identical CMakeCache). Discovered CTest registrations: 84/84/85, exactly
  one new `unit.v2_staging` each. Focused seven persistence tests PASS 7/7 in
  each; full suites PASS Debug 84/84, Release 84/84, UBSan/fuzz 85/85. Separate
  preserved parser corpus smoke PASS. Full suites used the authorized external
  sandbox loopback route; no tests skipped, suppressed or weakened.
- New staging executable PASS: 266 executed scenarios, including 195 individually
  reached boundary injections and seven healthy boundary-discovery scenarios.
  Tests include exact collision inventories, selected/LOCK/V1 byte-and-membership
  preservation, first-error identities, syscall/count/offset traces and descriptor
  ledgers. Staging lifecycle reaches zero allocation sites under zero allocation
  budget; one genuinely reached shared-state allocation failure is also tested.
  Original `unit.v2_root` remains byte-identical and reports 23 positive / 57
  rejection / 67 syscall-boundary / 20 reached allocation failures in every
  applicable configuration. Full source/catalogue/trace evidence is exported.
- Strict C++20 standalone-header and translation-unit checks, Release/NDEBUG
  warning checks, clang-format dry-run/Werror and whitespace checks PASS. Local
  incremental builds introduce no warnings. Historical unrelated ListCommands
  redundant-move, V2TailStream Marker-shadow and allocation-hook warning evidence
  remains recorded below; no unrelated warning fix is attempted. Development
  compile errors (missing entropy declaration/bridge friendship) and a default
  test-adapter role correction are retained separately in the review evidence.
- Limits: callers serialize operations/destruction; directory ownership and
  ancestors are trusted. Held-root identity does not police hostile ancestor/path
  replacement; advisory locks coordinate cooperating users. Ambiguous kernel close
  can still leak on some OS outcomes, as documented by the accepted root layer.
  macOS execution plus SDK/Linux-manual portability inspection is not Linux
  acceptance. No production persistence directories were accessed.
- Explicitly deferred: root/LOCK init, generation search/reservation, final artifact
  installation/replacement, CURRENT temporary/selector, rename, directory fsync,
  R4, writer activation/handoff, serving/recovery/DB/CLI integration, V1 modification,
  abandoned-file cleanup, and all next-stage work. This implements only unpublished
  low-level staging mechanics, not complete P1/P2/P3 or the broader 10D.1 milestone.
- External review directory: `/private/tmp/redisdb-10d1b-review`. It contains the
  complete new sources, complete tracked diff, eight-file hashes/manifest,
  baseline refs/CI identities, reached scenario/fault evidence, actual local
  build/test/discovery/strict/format logs, preservation proofs and final Git state.
  The previous handoff payload is saved there and checked as an exact suffix;
  final hashes are external to avoid self-reference.

**Preparation-time stop (2026-10-28): retain the eight-file draft locally and
uncommitted for independent complete-source review**, including the private bridge,
move/failure policy, full new untracked tests and local evidence. Publication and
final 10D.1B engineering acceptance require these separately verified gates, in order:

1. Independent complete-source review.
2. Separately authorized guarded feature commit/push.
3. Exact-feature Linux Networking, Clang 18 sanitizer/fuzz and Docker CI.
4. Separately authorized guarded two-parent main merge.
5. Exact-main Linux CI.

At preparation of this wording correction on 2026-10-28, independent complete-source
review had identified no blocking source-level defect. Independent incremental
wording review was the next gate before a separately authorized guarded feature
publication and exact-feature Linux CI task. Local-source acceptance does not
establish final 10D.1B engineering acceptance.

Future readers must consult actual Git refs and workflow evidence to determine
which subsequent gates have completed. This checkpoint does not assert that
feature publication or main acceptance has occurred. It grants no commit, push,
PR, merge, publication, production V2 integration, 10D.1C or later implementation
authorization.

# Stage 6B / 10D.1A — formally accepted engineering checkpoint

**2026-10-28: 10D.1A engineering is COMPLETE AND ACCEPTED ON MAIN.**
At preparation time on 2026-10-28, this four-document closure was an unstaged,
uncommitted draft. Engineering
acceptance is established by the published merge and its exact-main CI below;
documentation publication and acceptance require their own guarded Git and
exact-commit CI evidence. Consult current repository refs and workflow results
to determine whether those gates have subsequently completed.
This checkpoint controls continuation and supersedes older current-action/status
instructions below. Every previous handoff byte is preserved as historical evidence.

- Accepted main: `d8f301ac384bae34fcf47b430766096cee5ec217`.
  Tree: `4347de301ebc14fee0d9b5f0e70b524625feb945`, exactly the accepted feature tree.
  Genuine non-fast-forward merge with exactly two ordered parents:
  `667a9c047d913375263ec8145142bb01854ea5d5`, then
  `91b30f3c8529ed80954414a601b4ce102770888e`.
  Retained `fasttrack/v2-root-identity-validation` remains at
  `91b30f3c8529ed80954414a601b4ce102770888e`; its sole parent is original
  implementation commit `6b9b1c6061aaaaa75baa3aac07a1191c7c7ced25`.
- Original engineering manifest: ADDED
  `include/redisdb/persistence/V2Root.hpp`, `src/persistence/V2Root.cpp`,
  `tests/unit/v2_root_test.cpp`; MODIFIED `CMakeLists.txt` and
  `docs/CONTINUATION_HANDOFF.md`. The current closure draft changes only handoff,
  PROJECT_STATE, README and ROADMAP; it adds no implementation.
- Independent complete-source acceptance covered root/LOCK ownership, stable
  identity validation and bounded pinned reads. Independent correction reviews
  accepted the test fixture template change from `/private/tmp` to `/tmp` for
  macOS/Linux portability, and the single contender pipe-write result check.
  Original implementation and guarded warning-fix publication passed exact-feature
  CI; those feature runs are historical, separate from the exact-main gates below.
- Capability: move-only root leases retain an existing real root and an existing
  regular single-link LOCK under nonblocking exclusive advisory flock. Dependent
  pins retain the lock lifetime. Typed existing artifacts are opened read-only;
  final-component symlinks and unsafe type/link/identity/size states are rejected.
  Held/named identity rechecks surround bounded, caller-buffer pread operations;
  failure latches shared state. Captured identity/length is preliminary evidence,
  not decoded selector/content validity, actual EOF or durable selection proof.
- Assumptions: callers serialize operations/destruction; root ancestors and directory
  ownership are trusted. Advisory locks coordinate cooperating participants and
  do not defend against uncooperative mutation. No authentication guarantee is
  supplied by identities or CRC/checksum validation. Frozen
  [V2 format](V2_PERSISTENCE_FORMAT.md) and [compaction architecture](AOF_COMPACTION_DESIGN.md)
  retain their existing authority; this status checkpoint changes neither.
- Completed final macOS correction validation: Debug **83/83**, Release **83/83**,
  preserved UBSan/fuzz **84/84**, focused persistence **6/6** in each. Root scenarios:
  **23 positive / 57 rejection / 67 syscall-boundary / 20 reached allocation
  failures**. Strict C++20, standalone header, formatting and warning checks passed.
  No fresh C++ execution is claimed for this documentation-only draft.

Exact-main Linux evidence below is for `d8f301ac384bae34fcf47b430766096cee5ec217`,
event `push`, branch `main`; all three runs and required jobs completed/success:

- [Networking 40561936792](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40561936792):
  Linux Debug **83/83**, Release **83/83**; `unit.v2_root` executed and passed in
  both. Existing pipeline, server smoke, connection lifecycle and epoll repeat
  gates passed.
- [Clang 18 ASan/UBSan and fuzz 40561936629](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40561936629):
  **84/84** CTests, including `unit.v2_root` and pinned 50-seed parser corpus
  smoke; real **10,000** libFuzzer executions completed, no sanitizer failure.
- [Docker Linux 40561936642](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40561936642):
  image/Compose validation, 22 tooling tests, 15 container networking/persistence
  cases and 10 owned-resource private Compose checks passed; pinned Redis 8.10.2
  parity passed **54 command cases/40 routes and 10 operational cases**;
  **24 benchmark profiles/48 timed runs** passed under unchanged bounds.

Actual GCC Debug/Release and Clang build logs show **zero new warnings in
`V2Root.cpp` and `v2_root_test.cpp`**; the discarded-write-result warning is absent.
Historical unrelated ListCommands redundant-move, V2TailStream Marker-shadow and
allocation-hook warnings remain separate. Docker smoke does not establish remote
host/SSH acceptance; original Phase 16 remains EXCLUDED, not complete.

**Boundary:** this read-only foundation performs no production V2 persistence
writes or live V2 recovery. No V2 writer activation, compactor, candidate/CURRENT
publication, P1–P7 ordering or R4/R5 restart barrier is implemented. Later
10D.1B–F, 10C.2 and other implementation slices remain deferred. Neither complete
Stage 6B compaction nor original Phase 10 is finished; original roadmap inventory,
acceptance criteria and scope exclusions remain unchanged.

**Required gates from this preparation checkpoint:** independent documentation
review, separately authorized guarded docs feature publication and exact-feature
Linux CI, then separately authorized guarded main merge and exact-main CI.
After those gates are verified, the next frozen implementation slice (10D.1B)
may be separately authorized. Consult current refs and workflow results for gate
completion. This checkpoint itself grants no publication or implementation
authorization.

# Stage 6B / 10D.1A — Linux test-warning correction (2026-10-27)

**LOCAL AND UNCOMMITTED: independent incremental review required before a
separately guarded follow-up feature commit and exact-feature Linux CI.**
A main merge remains separately unauthorized. All previous handoff text follows
verbatim; this checkpoint supersedes its current-action instructions.

- Original independently accepted/published feature commit remains
  6b9b1c6061aaaaa75baa3aac07a1191c7c7ced25, tree
  74d6da37666df0660902c14f4ea06812398c5f36, sole parent/unchanged main
  667a9c047d913375263ec8145142bb01854ea5d5. Successful exact-feature Linux runs
  are historical evidence for that published commit: Networking 40551695101
  (Debug 83/83, Release 83/83), sanitizer/fuzz 40551695304 (84/84, pinned
  50-seed smoke, 10,000 libFuzzer executions), Docker 40551695226 (success).
- Independent inspection observed a new Linux Release -Wunused-result warning
  at v2_root_test.cpp:410: contender() discarded write()'s result. At that single
  location, replace the discarded call with an explicit result != 1 check and
  child _exit(1) on failure. The one-byte blocking pipe write, independent flock
  contention, child/pipe ownership, parent poll/read/wait assertions, fixture
  safety and all scenarios remain intact. No contention rewrite or test weakening.
- V2Root.hpp, V2Root.cpp and CMakeLists.txt are byte-for-byte unchanged from the
  published commit. No other source, test, workflow, specification or roadmap edit.
- Actual local macOS rebuilds all PASS without compiler warnings. Focused six-test
  persistence suites: Debug 6/6 (7.69 s), Release 6/6 (4.76 s), preserved UBSan
  6/6 (11.66 s). Complete suites: Debug 83/83 (28.30 s), Release 83/83 (20.02 s),
  UBSan/fuzz 84/84 (34.71 s), including corpus smoke. All root-test runs preserve
  23 positive, 57 rejection, 67 syscall/boundary and 20 reached allocation failures.
  UBSan cache/compiler/instrumentation remain byte-identical. Full suites used
  the authorized outside-sandbox loopback route; no tests skipped or weakened.
- Apple Clang 17 strict C++20 standalone-header and source/test checks PASS with
  Wall/Wextra/Wpedantic/Wconversion/Wshadow/Wunused-result/Werror, including an
  O3/NDEBUG test warning check. clang-format dry-run/Werror, registration counts
  83/83/84 and whitespace checks PASS. These macOS checks do not establish that
  Linux GCC's warning is resolved; subsequent exact-feature Linux CI must verify it.
- Updated v2_root_test.cpp SHA-256:
  7d2827c9377fcb08e31b56706b00d7ce292f89b49ffb43a286de7bfff711d480
  Previous handoff bytes preserved, SHA-256:
  13ced5e852b98fb775a843389ddc84b64ca0558a97e64877af9d13867e10f1e3
  Final complete handoff and corrected-test hashes are recorded externally in
  /private/tmp/redisdb-10d1a-warning-review-kwvd1vjk/SHA256SUMS.txt
  and review.txt beside it, avoiding a self-referential handoff hash.
- Final scope: exactly test and additive handoff modifications, nothing staged,
  no new commit/push/PR/merge. Published feature and main refs unchanged. No
  production persistence access or later-stage implementation. Next gates:
  independent incremental acceptance, guarded follow-up feature commit,
  exact-feature Linux CI, then separately authorized main merge. STOP LOCAL
  AND UNCOMMITTED; do not publish this correction without incremental acceptance.

# Stage 6B / 10D.1A — independent-review portability correction (2026-10-27)

**LOCAL, UNCOMMITTED AND UNPUBLISHED; corrected-source independent acceptance
is required before any feature publication.** This checkpoint supersedes current
action instructions below; all historical handoff text follows verbatim.

- Independent full-source review found one blocking Linux portability issue:
  Fixture::Fixture() used the macOS-specific /private/tmp pathname, generally
  absent on Ubuntu runners. The sole test-source correction changes the template
  to /tmp/redisdb-v2-root-XXXXXX, supported by macOS and Linux. No fallback exists
  into the repository or a user's persistence directory. A trusted /tmp ancestor
  (a symlink on this Mac) is distinct from a symlink at the final V2 root component.
  Production final-component O_NOFOLLOW rules are unchanged.
- Exclusive mkdtemp creation/private ownership, saved directory device/inode,
  non-following cleanup, assertions and independent-process locking scenarios
  remain byte-for-byte unchanged. The test differs from the previous review by
  exactly that one pathname replacement; no scenarios were removed or weakened.
- Starting guards matched all five supplied hashes, exact five-path manifest,
  clean index and branch fasttrack/v2-root-identity-validation. HEAD, local main,
  origin/main and actual remote main remain
  667a9c047d913375263ec8145142bb01854ea5d5; no remote feature branch exists.
- Unchanged SHA-256 values:
  V2Root.hpp: 70a1107c1b0d155c73175068b5a9a4a78e05f30ff58984fca6214bff6e8fb080
  V2Root.cpp: e16ab6383166a7d3132ef964949e138d85a9eb044b00507035894eb73fe5a37c
  CMakeLists.txt: f3e60c0b2e8b8fd1f799318ebbb2b2899d55b3972dafbaed9e69c47955760f6b
- Corrected v2_root_test.cpp SHA-256:
  65a1329b460f738a958750eff580af6c3841fbb48c205076036315ac0b7cd40b
  Historical handoff payload SHA-256 (all bytes preserved after this checkpoint):
  40fd84ce98e1638e7aba226b60b2a3ac3ce55e5676459b71eab6470055b3bf2c
  The final complete handoff SHA-256 and all five final hashes are recorded in
  /private/tmp/redisdb-v2-root-correction-iucrxd95/SHA256SUMS.txt
  and review.txt in the same directory. The final handoff hash is external to
  avoid a self-referential hash changing the bytes it identifies.
- Actual corrected-source local macOS reruns: Debug configure/build PASS,
  focused 6/6 PASS (7.64 s), full 83/83 PASS (28.33 s); Release configure/build
  PASS, focused 6/6 PASS (3.90 s), full 83/83 PASS (20.26 s); preserved
  build/stage4b-ubsan configure/build PASS, focused 6/6 PASS (11.56 s), full
  84/84 PASS (34.35 s), including parser corpus smoke. UBSan cache remains
  byte-identical, preserving compiler, Debug, fuzz ON and existing sanitizer flags.
  Each focused run reports 23 positive, 57 rejection, 67 syscall/boundary and
  20 reached allocation-failure scenarios. No count change.
- Initial full-suite sandbox attempts failed on networking loopback bind access;
  final full suites passed through the authorized outside-sandbox route. Both
  attempts and final logs are retained in the external review export. No tests
  were skipped. These macOS results do not establish Linux runtime or CI success.
- Standalone-header and both new translation units pass strict C++20 with
  Wall/Wextra/Wpedantic/Wconversion/Wshadow/Werror. clang-format dry-run/Werror,
  tracked/new-file whitespace and CMake registration checks pass: 83/83/84 total
  tests, exactly one unit.v2_root each, timeout 60 seconds. A whitespace audit
  helper was corrected to accept git diff --no-index's normal new-file exit 1
  only with empty diagnostics; no source change was needed for that helper.
- Exactly the same five authorized paths remain; since the prior review only
  the fixture pathname and this additive checkpoint changed. No production
  persistence artifacts were accessed or modified; test fixtures/build outputs
  are local harness outputs. Nothing staged or committed; no push, PR or merge.
  No 10D.1B–F, P1–P7, R4 or later work was implemented. Feature publication
  remains unauthorized pending corrected-source independent acceptance.

# Stage 6B / 10D.1A — local V2 root and identity checkpoint (2026-10-27)

**LOCAL, UNCOMMITTED AND UNPUBLISHED; STOP FOR INDEPENDENT FULL-SOURCE REVIEW.**
This checkpoint supersedes current-action instructions below. All prior handoff
text remains verbatim. No original roadmap item or all of Stage 6B is closed.

- Accepted starting main / unchanged HEAD:
  `667a9c047d913375263ec8145142bb01854ea5d5`; accepted tree:
  `df7ef6fce9c54f4053655cfdf8885244f645e101`. The preceding read-only audit found
  10C.1 technically ready for closure with no mandatory missing pure-codec seam.
  This does not equate serialized validity with selected-file or sync authority.
- Fresh preflight verified exact repository/origin, Git identity and authenticated
  GitHub account `sinthetaaa`, clean main/index/worktree, no unfinished Git operation,
  normal fetch, HEAD/main/origin/main/actual remote main equality and zero divergence.
  Proposed branch was absent locally/remotely (local quiet show-ref absence code 1).
  Created LOCAL `fasttrack/v2-root-identity-validation` directly from accepted main.
- Exact manifest: NEW `include/redisdb/persistence/V2Root.hpp`,
  `src/persistence/V2Root.cpp`, `tests/unit/v2_root_test.cpp`; MODIFIED
  `CMakeLists.txt` (six registration lines; one core source and `unit.v2_root`,
  timeout 60 seconds) and this additive handoff. All accepted V1/V2 source/tests,
  UniqueFd, server/CLI/storage/networking, presets, frozen architecture/format,
  ROADMAP, workflows/Docker/benchmarks/README remain unchanged. Original 20 phases
  and 169 unique checklist identifiers remain preserved; Phase 16 stays EXCLUDED.
- Public ownership API: move-only `RootLease::open_existing(const filesystem::path&,
  shared_ptr<RootIo> = {}) noexcept`, `valid()`, `error()`, `recheck()`,
  `pin_current()`, `pin_generation_file(ArtifactRole, GenerationId)` and
  `close_checked()`. Move-only `PinnedFile` provides `valid()`, `error()`,
  `identity()`, `recheck()`, `read_exact(uint64_t, span<byte>)` and
  `close_checked()`, all noexcept. Identity is a borrowed historical observation,
  not a validated selector/generation/EOF/durable boundary. No raw FD escape.
- Acquisition normalizes only trailing slash/dot components, preserving trusted
  ancestors and '..'. Opens the existing root once with directory/CLOEXEC/nofollow
  flags, fstats directory identity, opens existing root-relative LOCK read-only
  with CLOEXEC/nofollow/nonblocking and NO creation/truncation flags, validates
  regular single-link bounded metadata, acquires LOCK_EX|LOCK_NB, then rechecks
  held root/LOCK and named LOCK via fstatat(AT_SYMLINK_NOFOLLOW). Failed acquisition
  closes partial resources immediately; it never retries using another LOCK.
  Zero-byte LOCK is valid; no LOCK payload format is invented.
- Typed artifact names only: CURRENT, base-g.snap, tail-g.aof, tail-g.commit.
  Generation uses accepted checked GenerationId, allocation-free decimal to_chars
  with roundtrip and name-buffer bounds. No arbitrary filename, directory scan,
  selector decode or generation reservation. Existing artifact opens are read-only,
  CLOEXEC/nofollow/nonblocking. Held/named type/link/dev/ino/size checks require
  CURRENT/marker exactly 112, base >=204, tail >=96, and nonnegative extents <=
  min(INT64_MAX, native off_t max). Junk correctly sized content can be pinned:
  this is intentionally preliminary identity validation, not byte decoding.
- Shared private RootState retains root and stable LOCK until the last dependent
  file/lease is destroyed. Each pin owns exactly its artifact FD and owned name.
  File teardown closes artifact before releasing its state reference; last state
  teardown closes LOCK then root. Root move/assignment cannot invalidate a retained
  pin's lock lifetime. No threading/reentrancy guarantee: caller serializes owner
  calls/destruction; trusted adapters cannot reenter or retain borrowed buffers.
  A closed pin conservatively retains its lease reference until destruction/move-away.
  Explicit root close with dependent pins fails terminally without releasing LOCK.
- Rechecks verify held root identity, held/named LOCK and held/named artifact,
  regular/single-link rules and captured size. First failure latches the entire
  shared state, invalidates all peers/identity access and prevents further open/read
  attempts. No repinning, fallback, repair or reopening after failure. Legitimate
  future marker replacement requires a separately coordinated new pin.
- Reads use caller storage only, no allocation by file size. Subtraction-first
  range checks precede addition/narrowing; actual pread requests <=64 KiB and
  SSIZE_MAX. Retry actual EINTR and accumulate short reads; zero progress before
  completion is Truncated; invalid counts/returns or permanent errors fail.
  Identity checks surround even empty reads. Partially filled output on failure
  must be discarded. Successful read is not actual EOF confirmation or content
  integrity proof; future validators and coordinator must establish both.
- RootIo owns no global failure controls. Its protected virtual customization seam
  covers exactly open/openat/fstat/fstatat/flock/pread/close and scalar before/after
  events. Defaults invoke POSIX. RootError owns category, operation, role, stage
  and immediately captured errno without paths/data. Expected allocation/length
  failures become ResourceLimit; programmer callbacks are noexcept by contract.
  There are no write/fsync/rename/unlink/creation wrappers.
- Close relinquishes the FD number before the single attempt, never retries EINTR,
  and still attempts cleanup if a Before observer rejects. Checked errors latch;
  destructors do not throw. Destructor-time errors can be queried only through
  surviving peers or observed in the adapter trace. Tests safely close the REAL
  descriptor before injecting EIO/EINTR, including immediately reopening a fixture
  at the SAME number and verifying it survives all later teardown. This proves
  no unsafe retry for that controlled outcome, not perfect recovery of a real
  platform close that might leave its kernel FD open. Ambiguous OS close can leak
  a kernel resource; no guessed second close or false successful cleanup claim.
- Deterministic fault matrix (every armed point reached and context asserted):

  | Operation/point | Injected result | Expected state |
  |---|---|---|
  | Root open, before/call/after | EIO; call EACCES | no usable owner; partial FDs closed; InvalidRoot on actual failure, IoFailure on hooks |
  | LOCK openat, before/call/after | EIO; actual missing ENOENT | no owner; IoFailure / MissingLock; no file creation |
  | Root/LOCK fstat, before/call/after | EIO, including second LOCK stat AFTER flock | no owner; IoFailure; acquired resources released |
  | LOCK named fstatat and flock, before/call/after | EIO; flock EWOULDBLOCK | no owner; IoFailure / LockUnavailable; no alternate inode |
  | Artifact openat/fstat/fstatat/read, before/call/after | EIO | root and pins terminal; errno remains owned after global errno changes |
  | pread progress | EINTR then success, short reads, zero, permanent error, negative/oversized return | valid retry/accumulation or terminal Truncated/IoFailure |
  | Metadata and invalid syscall returns | negative/max fixed size, wrong identity/type/link/size, non-POSIX return | InvalidLength/identity rejection/IoFailure; no accepted pin |
  | Close of artifact/LOCK/root, before/call/after | EIO and EINTR; invalid positive return | one attempt, terminal IoFailure, no reused-number close |
  | Construction allocations | budget sweep through success, default adapter/state allocation failure | ResourceLimit, no leaked partial owner |

- Actual new-test counters on this Mac: **23 positive cases, 57 rejection cases,
  67 syscall/boundary cases, 20 reached allocation failures**. These count scenario
  cases, not every assertion/syscall, and additional ownership checks are not
  inflated into the totals. Covered valid generation 1/UINT64_MAX, opaque fixed
  files, binary NUL/high-bit 200,000-byte reads, >20,000 seven-byte short reads,
  EINTR, moves, retained dependent lock, independent child opens denied/allowed,
  deterministic teardown order, terminal moved-from/failed access and premature
  root-close rejection. Invalid roots/LOCK/artifacts include missing, symlink
  with slash/dot variants, hardlink, directory, FIFO, inode replacement/removal,
  length/link/type changes, bad fixed/minimum sizes, invalid generation/role,
  negative/max metadata and overflow/beyond/crossing read endpoints. Actual inode
  truncation during read and post-read metadata change cannot be hidden.
- Fixture safety: only harness-created unique /private/tmp/redisdb-v2-root-* roots,
  original root dev/ino verified before cleanup; flat nofollow exact-entry cleanup,
  no arbitrary recursive/user-path deletion. Harness creates/changes/corrupts the
  fixtures; V2Root does not. Membership, metadata, FNV-1a fingerprints AND exact
  bytes are compared before/after relevant read-only/failure cases (atime excluded).
  Per-adapter FD acquire/close accounting and process descriptor inventory checks
  pass. Child lock contention always independently opens LOCK; inherited fork
  descriptions are never used as the contender. Only owned children are waited/killed.
- Actual FINAL local gates: Debug configure/build PASS; focused root/tail/base/
  fixed/AOF/Snapshot **6/6 PASS (9.92 s)**; full **83/83 PASS (23.88 s)**.
  Release configure/build PASS; focused **6/6 PASS (5.16 s)**; full **83/83 PASS
  (20.06 s)**. Preserved stage4b-ubsan compiler/cache, Debug and fuzz-target ON,
  flags `-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`;
  rebuild PASS; focused **6/6 PASS (12.46 s)**; full **84/84 PASS (41.79 s)**,
  including the actual 50-seed parser smoke. Full suites used the established
  authorized outside-sandbox loopback route; no tests skipped/weakened. Registration
  counts are 83/83/84 with exactly one unit.v2_root. No new Linux CI, Docker or
  ASan execution, dependency installation, remote workflow trigger or fuzz claim.
- Final clang-format dry-run/Werror and strict -std=c++20 TU/header standalone
  checks with Wall/Wextra/Wpedantic/Wconversion/Wshadow/Werror PASS. Actual core
  compile commands use c++20 and test targets retain established gnu++20 CMake
  convention; strict manual compilation passed for both new TUs. Warnings and
  UBSan instrumentation preserved. No new local warnings or sanitizer diagnostics.
  Known accepted Linux GCC Marker shadow, ListCommands redundant move, historical
  allocation-hook warnings and Docker Node deprecations remain unchanged.
- Self-review reread full new source/tests: corrected immediate failed-acquisition
  cleanup before execution; first focused persistence run was 5/6 because the
  test's wrong-root-type injection still supplied S_IFDIR. Corrected that fixture;
  root retry passed and all final focused/full gates passed. Later replaced a
  duplicate fault with the second post-flock LOCK stat, added exact teardown-order
  and protected-backend API assertions, then rebuilt and reran all gates. Earlier
  full Debug/Release also passed 83/83; final figures above include final tests.
  An audit helper initially required c++20 on the existing gnu++20 test convention;
  corrected the helper, without changing build flags. No frozen contradiction,
  hidden V1 interaction, raw-FD escape or blocking unresolved finding remains.
- Final scope/whitespace/source-integrity checks and review export cover all three
  complete new files (untracked files are not in ordinary git diff), full tracked
  CMake/handoff diff, actual test results, Git status and five SHA-256 values.
  No staged change, commit, push, PR, merge, source publication, evidence archive,
  live persistence-root access or live/selected artifact mutation occurred.
- **Next gate:** independent full-source review BEFORE separately authorized
  feature publication and exact-feature CI. STOP LOCAL AND UNCOMMITTED.
  **10D.1B–F, all P1–P7, R4/R5, 10C.2 and later stages remain deferred.** No
  CURRENT publication, candidate creation/reservation, fsync, writer activation,
  import/recovery, Database mutation, compaction or reclamation was implemented.
  No Stage 6C or infrastructure work; original Phase 16 remains EXCLUDED.

# Stage 6B / 10C.1B.2B — independent review acceptance (2026-10-27)

Independent full-source review **PASSED** on 2026-10-27 with no blocking source
changes requested. Reviewed SHA-256 values:

- `V2TailStream.hpp`: `76ee12acc336bdeb4ec528e7620aa877fe4e1a244d7c637330594ec4f8b851bd`
- `V2TailStream.cpp`: `5e5c3a2ce245148e077ca51b00aa301d6894808f6367476bbe0bfe9be710dc71`
- `v2_tail_stream_test.cpp`: `d0eeb03cd247c0ca6dea2bb14ab7627796dd51aa1c13296330668f8f28e93c87`

Reported local Debug 82/82, Release 82/82 and UBSan/fuzz 83/83 passed.
The authorized next gate is one guarded feature commit, normal feature push and
exact-feature Linux CI verification. No GitHub CI outcome is predeclared.
Main merge, 10D.1, filesystem persistence integration and later stages remain
unauthorized. All previous handoff text follows verbatim.

# Stage 6B / 10C.1B.2B — local bounded V2 tail streaming checkpoint (2026-10-27)

**LOCAL, UNCOMMITTED AND UNPUBLISHED; STOP FOR INDEPENDENT FULL-SOURCE REVIEW.**
This additive checkpoint supersedes current-action instructions below. All prior
historical text is preserved verbatim. Neither all 10C.1B, original Phase 10C nor
Stage 6B is declared complete.

- Accepted starting main and unchanged HEAD:
  `f6852b40161b4a724e5751686f255cc68b25f925`. Stage 10C.1B.2A is formally
  accepted on main, with exact-main Networking 40505023576 (81/81 Debug and
  Release), ASan/UBSan/fuzz 40505023361 (82/82, 50 seeds, 10,000 fuzz runs) and
  Docker 40505023657 accepted in the prior merge task. These are predecessor
  evidence, not tests or remote CI executions of this new slice.
  Fresh preflight verified repository/origin, GitHub identity `sinthetaaa`, clean
  main/worktree/index, no in-progress operation, normal fetch, HEAD/main/fetched
  origin/main/actual remote main equality and zero ahead/behind. Proposed branch
  absence was checked locally (absence exit code interpreted correctly) and
  remotely. Created LOCAL `fasttrack/v2-tail-streaming-validation` directly from
  that accepted main. No commit, push, PR, merge or history rewrite in this task.
- Exact five-path manifest: NEW `include/redisdb/persistence/V2TailStream.hpp`,
  `src/persistence/V2TailStream.cpp`, `tests/unit/v2_tail_stream_test.cpp`;
  MODIFIED `CMakeLists.txt` (six registration lines only) and this additive handoff.
  Accepted fixed V2/base code and tests, V1 AOF/Snapshot, storage/accounting,
  existing tests/fixtures, frozen architecture/format, ROADMAP, server/RESP/CLI,
  CMake presets, CI/Docker/benchmarks and other tracked paths remain unchanged.
  Original 20 phases and all 169 unique roadmap identifiers remain preserved.
- Public API: `TailStreamValidator(TailHeaderFields expected, MarkerFields marker,
  uint64_t declared_extent, TailStreamLimits limits = {})`, with noexcept
  `feed(ByteView)`, `finish()`, `status()`, `error()`, `result()`,
  `bytes_received()`, `buffered_bytes()` and `buffer_capacity()`. Reuses existing
  StreamStatus/ByteView without changing accepted headers. Inputs are owned
  copies of untrusted claims; marker fields are revalidated even if caller-built.
  Mandatory extent is 96..INT64_MAX on supported macOS/Linux. Caller serializes
  calls, supplies all bytes in order, and calls finish only after actual EOF at
  the pinned extent. Chunks are borrowed solely for each feed call.
- Private-construction `ValidatedTailScan` owns immutable header identity and
  metadata, final observed generation-qualified W/L/R/P, a matched generation-
  qualified marker checkpoint, exact tail length and validated record count.
  It owns no effects history. No result exists during NeedMore or Error; even
  complete header/records and an already matched watermark remain NeedMore until
  explicit successful finish. Finish is idempotent after success. Any feed after
  success, including empty, fails terminally and invalidates the accessible result.
  All failures retain their first error and discard the buffer/result. Result
  references are borrowed until destruction or rejected feed; a copied result
  owns its metadata independently of validator/input lifetime.
- Trust boundary: this proves complete serialized-tail structure, inherited
  effects semantics and actual encoded boundary claims only. It does NOT verify
  the referenced base's complete bytes, selected files/inodes, CURRENT/LOCK,
  permissions, fsync completion, durable acknowledgment, private-state replay
  correctness, recovery readiness or authority to activate a writer/serve.
  No callbacks, clocks, filesystem interfaces or live Database mutation exist.
- Actual 96-byte tail header is decoded with unchanged `decode_tail_header()`;
  all base fields must equal expected store/g/S/B/base CRC/base-header CRC/model.
  Stored/full actual tail-header CRCs agree with derived TailHeaderIdentity and
  marker binding. Constructor checks untrusted expected/marker structure with
  existing pure fixed helpers; header completion independently checks actual bytes
  and `validate_marker_header(marker, actual)`. Matching claims alone never finish
  a scan. Header-only tails initialize W=S/L=96/R=0/P=CRC(all actual header bytes),
  including positive S and S=UINT64_MAX after compaction.
- Exact inherited TXN1 framing: tag(4), BE sequence(8), BE payload length(4),
  payload n<=67,108,864, BE record CRC(4), literal END!(4); total n+24,
  maximum 67,108,888. Record CRC covers the first 16+n bytes; prefix CRC covers
  the complete record including CRC/trailer. Prelude validates tag/next sequence,
  cap/remaining extent/native size/budgets before allocating. First sequence is
  S+1, then exactly consecutive; exhaustion is checked before addition. A final
  record at UINT64_MAX is readable; another record is rejected without wrap.
- Each fully framed/checksummed record calls unchanged V1 `decode_effects()`
  exactly once, then destroys the temporary owned effects before advancing
  observed W/L/R/P. Tombstones, all four replacement types, binary/empty keys and
  strings, list order/duplicates, unique unsorted inner hash/set items, signed
  absolute deadlines and inherited counts/blob/native-clock limits remain V1.
  No integer normalization, expiration, policy accounting, recency touch or replay.
  Journal LRU validates grammar/uniqueness, not the strict base permutation rule;
  a unique order containing keys outside this record's changed keys is accepted.
  Configured-policy/keyspace replay validation remains a later coordinator task.
- Actual watermark algorithm: only header completion and successful complete
  record decode are candidate boundaries. All four actual D/O/R/P values must
  simultaneously equal the marker. Reaching D with wrong coordinates or crossing
  O without a match fails; an interior offset cannot be blessed by its own valid
  prefix CRC. D=S must be S/96/0/actual full-header CRC. A matched boundary never
  stops validation. Every later complete record remains scanned, with final W/L
  possibly beyond D/O and no claim that the suffix was synchronized. Corrupt or
  partial suffix, missing boundary, incomplete header/prelude/payload/CRC/trailer,
  wrong extent or extra bytes rejects the whole scan. No truncation/repair/fallback.
- Memory/complexity: one lazily allocated owned char array, initially 96 bytes,
  then at most 67,108,888 bytes. Growth copies only the constant 16-byte prelude,
  frees the old array BEFORE allocating its exact checked replacement, and never
  geometrically over-reserves or holds two record buffers. Prefix is extended only
  at accepted boundaries. Chunks copy once into available storage; effects decode
  once per completed record, with constant framing/CRC passes. No whole-tail buffer, historical
  vector or allocation by aggregate extent/count exists. Temporary decoded V1
  objects may require additional bounded per-record memory: no constant-RSS or
  universal resource-admission claim. Checked extent bounds prove received/end
  addition safety; n+24/native narrowing and record/sequence counter increments
  are checked before execution. Expected allocation/container failures latch
  ResourceLimit, with allocation-free internal rejection and nonthrowing result
  moves statically checked. Programmer errors are not interpreted as success.
- Independent forward-polynomial CRC32C/reference byte builders reproduce frozen
  E.tail=96/dbfbc819 (stored header CRC cc38e707), T.record=48 with R=ad924c74,
  T.tail=144/04430f74 and T.marker=1/144/ad924c74/04430f74 (full marker CRC
  7a05287b). C.tail=96/28b640b9, S=1, stored header CRC ad63e9bb;
  U.record=56, R=e91dbec0, full record CRC f18ecbb6, U.tail=200/a33c606b;
  L.tail=96/dc793392, S=2, stored header CRC c1721669. Complete record/checkpoint
  values are independently asserted, not solely production roundtrips.
- Coverage on this macOS clock: **21 positive helper scans**, **1,046 partition
  scans** (every possible two-chunk split plus irregular caller-overwrite scans
  for seven frozen header/tail/boundary fixtures), **1,170 rejection cases**, of
  which **1,032 are all-byte truncations**, **nine explicit budget/overflow
  cases**, and **24 reached injected allocation failures** through first success.
  Additional direct ownership/resource/finalization assertions are not inflated
  into those helper counts. Positive checks include markers at initial/intermediate/
  final boundaries, valid suffixes, positive/max base sequence, sequence ending at
  UINT64_MAX, binary/empty data, every type, tombstones, unsorted unique hash/set,
  list duplicates, LRU present/absent/empty, negative/positive/past deadlines and
  both native clock extremes, bytewise/chunked input, copied metadata after input
  destruction, and terminal feed-after-finish. Actual observed/matched checkpoints
  are compared in every partition scan.
- Negative cases rebuild enclosing CRCs for intended semantic layers: malformed
  tail headers/immutable identities and resealed foreign markers; invalid initial
  claims; unknown tag, impossible/oversized lengths, CRC/END! errors; every sequence
  gap/repeat/decrease/reset/exhaustion; zero/excessive effects/items, invalid tags/
  timed/LRU flags, empty collections, duplicate/unordered outer keys, duplicate
  hash/set/LRU keys, forbidden LRU data, oversized blobs and payload suffixes;
  D below S/beyond final W, O before header/beyond extent/at EVERY interior byte of
  a real record, individually wrong R/P, and corrupt/partial suffix after matched
  initial/intermediate watermarks. Native deadline scaling overflow is conditional
  on ticks/us >1: all i64 microsecond deadlines are representable on this Mac,
  while the two unrepresentable extremes reject on nanosecond hosts. Astronomical
  counter/offset overflow is prevented by caps/extent guards rather than allocating
  an impossible-size file. No check/test/format was weakened.
- Bounded stress: **67,268,728-byte** streamed tail with one exact-limit
  **67,108,864-byte effects payload / 67,108,888-byte record**, made from eight
  individually legal <=8 MiB list blobs, in **4093-byte chunks**. Largest observed
  validator allocation is exactly **67,108,888 bytes**; 4,096 later records keep
  the live allocation count constant and are included in the final CRC/checkpoint.
  Whole-chunk versus bytewise medium input performs identical **four allocations /
  16,644 requested bytes**. Oversized u32 length claims under INT64_MAX declared
  extent fail with no allocation above the initial 96-byte header buffer.
- Actual local macOS outcomes: Debug configure/build PASS; focused tail/base/
  fixed/AOF/Snapshot **5/5 PASS (8.71 s)**; full **82/82 PASS (40.43 s)**.
  Release configure/build PASS; focused **5/5 PASS (6.47 s)**; full **82/82 PASS
  (58.41 s)**. Existing `build/stage4b-ubsan` cache/toolchain preserved, reconfigured
  and built with `-fsanitize=undefined -fno-sanitize-recover=all
  -fno-omit-frame-pointer`, fuzz targets ON; focused **5/5 PASS (13.37 s)** and
  full **83/83 PASS (65.58 s)**, including actual frozen 50-seed parser smoke.
  Full suites ran through the established authorized outside-sandbox loopback
  route. All builds/new-file strict C++20 warning/Werror syntax checks, standalone
  header compile and clang-format dry-run/Werror passed without new local compiler
  warnings or sanitizer diagnostics. Existing Linux GCC allocation-hook/ListCommands
  warning baselines were not edited; no new Linux/ASan CI or Docker execution is
  claimed for this uncommitted slice. No dependency installation or fuzz-CI change.
- Self-review reread the complete implementation/tests and challenged ownership,
  state transitions, subtraction-first bounds, allocation capacity, prefix coverage,
  actual boundary proof, corrupt suffix handling, sequence exhaustion, native
  deadlines, inherited collection/LRU permissiveness and exception-safe terminal
  states. Added explicit U record/boundary checks, positive-sequence decrease and
  successive-growth allocation failures after a real watermark match. Before
  execution, adjusted two negative fixtures' marker/extent setup to reach their
  intended Length/Truncated checks. No executed test failure or blocking source/
  specification contradiction remains. No accepted source/API/bytes were modified.
- Final review checks include exact five-file scope, no staged work, HEAD/main/
  origin/main at accepted starting SHA, unchanged protected paths and all 169
  identifiers, preserved prior handoff suffix, registered/instrumented CTest,
  tracked/new-file whitespace and git diff checks. Review deliverables are the
  three complete new source files, complete CMake/handoff diff, executed test
  report and final Git status; any local plain-text review export is outside the
  repository and is not a commit, archive, persistence fixture or publication.
- **Next gate:** independent full-source review against frozen format and accepted
  V1/base APIs; explicitly accept this local 10C.1B.2B slice or request corrections
  BEFORE any separately authorized feature publication. STOP uncommitted.
  **10D.1 and every filesystem/locking/fsync/rename/unlink/publication task remain
  deferred**, followed by independently gated **10C.2 → 10D.2 → 10E → 10F →
  10G.1 → 10G.2**. No initialization, V1 import, selected recovery/private-state
  replay, serving/writer activation, compaction or reclamation was implemented.
  Original Phase 16 remote VPS deployment remains EXCLUDED; no Stage 6C or
  infrastructure work was begun. No V2 persistence directory or selected artifact
  was touched; new V2 fixtures exist only in memory, while existing regressions
  use their established ordinary disposable V1 test fixtures.

# Stage 6B / 10C.1B.2A — independent acceptance and feature publication (2026-10-27)

Independent code review PASSED, as reported by the user, with no blocking source
issue or format contradiction. The three implementation/test files match the
reviewed SHA-256 copies; the reviewer independently reproduced E/C/L full-base
lengths and CRCs. This accepts the bounded base-streaming slice for feature
publication only, not filesystem safety, durability, serving or live recovery.

The current authorized gate is one normal feature commit and push on
`fasttrack/v2-base-streaming-validation`, followed by inspection of all three
exact-feature-head Linux workflows. Actual commit identities, CI run IDs and
execution outcomes belong in the publication report. STOP before main merge;
that requires separate authorization, unchanged accepted main, exact reviewed/
tested feature head and five-file scope, ordered merge parents, tested-tree
equality and successful exact post-merge main CI. Tail streaming remains separately
gated 10C.1B.2B; neither all 10C.1B nor original Phase 10C is complete. All prior
checkpoints below remain verbatim.

# Stage 6B / 10C.1B.2A — local bounded base streaming checkpoint (2026-10-27)

This additive checkpoint supersedes current-action instructions below; every prior
record remains verbatim. **LOCAL, UNCOMMITTED; STOP FOR INDEPENDENT CODE REVIEW.**
This is not acceptance of all 10C.1B or original Phase 10C.

- Accepted main and unchanged HEAD: `756d64c3058705f62175a62d6708bea69ff16802`.
  **10C.1B.1 is formally complete**, independently accepted, merged into main and
  validated by the required Linux workflows, as reported by the user: Networking
  40495943071, ASan/UBSan/libFuzzer 40495943163, Docker 40495943084. Those results
  validate the fixed-metadata predecessor, not this new base-stream implementation.
  Entry checks verified exact repository/origin, GitHub identity `sinthetaaa`,
  clean main/index, fresh fetch, HEAD/main/origin/main/remote main equality, fixed
  metadata source/tests, and absent local/remote proposed branch. Created local
  `fasttrack/v2-base-streaming-validation` directly from accepted main.
- Exact five-file manifest: new `include/redisdb/persistence/V2BaseStream.hpp`,
  `src/persistence/V2BaseStream.cpp`, `tests/unit/v2_base_stream_test.cpp`;
  modified `CMakeLists.txt` (one core source and one unit target/test, six added
  lines) and this additive handoff. Accepted V2Format.hpp/.cpp, V1 AofLog/Snapshot,
  Database/MemoryAccounting, existing tests, architecture/format documents, CI,
  Docker, benchmarks and ROADMAP are preserved. No reusable fixed-codec adjustment
  was necessary. All original 169 roadmap identifiers remain unchanged.
- Public pure API in `redisdb::persistence::v2`: `BaseStreamValidator` accepts
  owned copies of untrusted `BaseReferenceFields` or `BaseIdentity`, including
  exact complete extent and checksum claims, plus optional per-frame/entry/charge
  resource limits. `feed(ByteView)` borrows each span only during the call;
  `finish()` explicitly confirms caller-established EOF. NeedMore persists through
  DONE; only successful finish exposes a const `ValidatedBase` through `result()`.
  That object owns identity/header, physical entries, recomputed charges, pre-DONE
  CRC, DONE frame CRC and LRU indices into its entries. No borrowed chunk storage
  survives. Result views remain valid until destruction or a rejected subsequent
  feed. Repeated finish is idempotent; any feed after success, including empty,
  invalidates the result and fails. All failures latch a stable error and discard
  private state. Caller serializes calls and establishes actual EOF against pinned
  input extent; serialized-content validation proves neither filesystem identity,
  selection, fsync durability nor permission to serve.
- Grammar implemented: exact accepted 184-byte header, exactly K KEY1 frames,
  exactly J raw-key LRU1 frames, one four-byte-payload DONE frame, immediate EOF.
  Frames are tag/u32 length/payload/CRC32C/END!, n+16 bytes, unchanged coverage.
  KEY1 <=67,108,864 bytes; LRU1 <=8,388,608 bytes. Header claims are validated
  with existing fixed codecs and exact immutable binding. Full-file CRC32C is
  extended incrementally over every byte including DONE. Prefix before DONE,
  its payload checksum, DONE's own frame checksum/trailer, final byte length,
  header CRC, store/generation/sequence, complete CRC and explicit EOF all match.
  No matching naked checksum claim is treated as complete-file proof.
- KEY1 calls accepted V1 `decode_effects` unchanged, then requires exactly one
  replacement, no tombstone/LRU and strictly increasing unsigned-byte outer keys.
  Empty/binary keys and values, list order/duplicates, unique unsorted hash fields
  and set members, integer-looking strings and absolute deadlines survive.
  Reuses pure checked model-1 accounting over every physical entry, including
  already-past TTLs; verifies Q before success. No clock is sampled. LRU is an
  exact oldest-to-newest permutation regardless of future serving policy; no
  policy argument can bypass validation. Without history the owned entries retain
  binary sorted cold-start order and `lru_order()` remains empty. Empty compacted
  LRU permutations are valid. Pure `encode_key_frame`, `encode_lru_frame` and
  `encode_done_frame` return owned frame strings and reject unrepresentable input;
  KEY encoding also checks inherited decoder validity (including empty collections).
  Framing errors use BaseFrameError; inherited encoder errors remain PersistenceError;
  allocation failures propagate from encoders and latch ResourceLimit in the stream.
- Memory/complexity review: one reusable incomplete frame buffer, plus owned
  decoded values/entries and an owned-key balanced-tree permutation index; no
  whole-base buffer, aggregate K/J reserve or database-sized encoder. Header
  feasibility uses checked minimum-byte bounds. Frame lengths are capped and
  compared with remaining supplied extent/native max_size before n+16/reserve;
  streamed counts are bounded by checked header counts. Charge uses accepted
  checked size_t operations, with size_t <=u64 statically asserted. Existing
  inherited count/blob checks precede decode loops/accesses. State/permutation
  storage is O(K + decoded bytes); index operations take O(log K) comparisons;
  bounded frame storage does not mean constant total memory or RSS admission.
  Final result moves are statically checked nonthrowing. Resource failures never
  become empty state. Tests compare allocation work for one chunk versus bytes,
  inject every allocation failure on the typed/LRU fixture through first success,
  and show feasible K=1,048,577 metadata does not reserve or inherit the per-payload
  one-million-item cap. A complete million-key base was not materialized locally.
- Positive tests: independent forward-polynomial CRC32C/reference framing and
  frozen section-15 E/C/L complete lengths/header CRCs/full CRCs; fresh empty E,
  g=3/S=1 C and g=4/S=2 L, binary/NUL/high-bit/empty keys and values, all four
  types, unsorted unique inner collections, duplicate list elements, byte-exact
  `0007`, negative/positive/past TTLs and both native deadline extremes, physical
  ledger 1096, LRU on/off and empty permutation, caller-byte destruction, V1 frame
  encoder agreement, BaseIdentity overload, EOF-only completion and stable finish.
  **784 partition cases** include whole/one-byte/7-byte chunks, every possible
  two-chunk split of E/C/L and deterministic irregular sizes. Every fixed header
  field/checksum/trailer boundary is split.
- Negative tests: **829 rejection cases on this macOS clock**, including all
  **764 E/C/L truncations**, empty input, header identification/CRC errors,
  immutable identity/extent/whole-CRC mismatches, missing/duplicate DONE/trailing
  bytes, unknown/extra/misordered frames, malformed DONE sizes, KEY/LRU caps,
  UINT32_MAX frame/key lengths and UINT64_MAX extent/count claims, payload/CRC/
  trailer truncations/corruption, wrong DONE prefix, forged K/J/Q, inflated tiny
  files, tombstone/multiple effects/LRU inside KEY1, duplicate/descending/prefix/
  high-bit outer order, invalid value/timed flags, duplicate hash/set items,
  empty collections, inherited count/blob bounds/suffixes, duplicate/foreign/
  missing LRU references and LRU without flag. Semantic cases have independently
  rebuilt enclosing CRCs and assert error categories. Native unrepresentable
  i64 deadlines are conditionally rejected when clock ticks/us >1; macOS uses
  microseconds, so all i64 microsecond deadlines are representable here. There
  are also three explicit resource-budget cases, six invalid encoder cases and
  **34 reached injected allocation failures**. All error states reject later
  feeds/finish and expose no result. Feasible native endpoint arithmetic cannot
  overflow after the wire-length cap; hostile maximum lengths reject before add.
  A physically realizable charge-overflow dataset was not allocated; checked
  accounting and charge mismatch/budget rejection are exercised.
- Large evidence: **75,498,000-byte** base from nine separately bounded 8 MiB
  values, constructed/tested in two per-frame passes without a whole-base buffer,
  streamed in **4093-byte** pieces. Largest observed validator allocation:
  **8,388,648 bytes**, including allocator rounding. A separate exactly
  **67,108,864-byte KEY1 payload** (eight-element list) passes in **8191-byte**
  pieces. Both cases run deterministically in the single 120-second CTest budget;
  no benchmark threshold or separate large-resource configuration was needed.
- Actual local macOS validation: Debug configure/build PASS; focused new+fixed
  tests **2/2 PASS**, full Debug **81/81 PASS (13.40 s)**. Release configure/build
  PASS, focused **2/2 PASS**, full **81/81 PASS (36.76 s)**. Existing
  `build/stage4b-ubsan` configured/built with `-fsanitize=undefined
  -fno-sanitize-recover=all -fno-omit-frame-pointer` and fuzz targets ON; focused
  **2/2 PASS**, full **82/82 PASS (41.87 s)**, including actual replay of the
  frozen **50-seed** corpus. No sanitizer diagnostics or final compiler warnings.
  Full suites use the established authorized out-of-sandbox loopback procedure;
  no test was skipped or weakened. No local Docker, ASan, 10,000-run fuzz or
  GitHub CI execution is claimed. Existing predecessor Linux outcomes remain
  historical evidence only.
- Validation/self-review: complete new implementation/tests reread; challenged
  chunk/CRC/END!/EOF transitions, unsigned ordering, byte ownership, checked
  remaining lengths/counts/charge, expired physical entries, policy-independent
  permutation, native deadlines, unsorted inherited collections, terminal errors,
  repeated finish/feed and absence of filesystem operations. No frozen-format
  contradiction found. Corrected a test signedness warning; adjusted tombstone
  and missing-LRU fixtures so minimum-size checks do not preempt intended semantic
  checks; corrected a multiple-effect fixture's byte slice and native-deadline
  expected charge (113 ->114). Initial focused runs failed during these fixture
  corrections; one early full Debug run was **80/81**, then corrected full Debug
  passed **81/81**. No production format/semantic relaxation was needed. Header
  syntax validation initially compiled the header as a main file, triggering
  pragma-once/Werror; corrected to a standalone including translation unit.
  Final clang-format dry-run/Werror, strict C++20 standalone-header and both
  translation-unit warning/Werror checks, CMake registration/instrumentation,
  tracked/new-file whitespace, source scope and diff checks pass.
- **No persistence filesystem I/O or live recovery implemented.** No paths/FDs,
  locking, fsync, rename/unlink, import/init, tail TXN1 scan, commit boundary scan,
  CURRENT publication, writer activation, compaction or deletion. New tests keep
  V2 bytes in memory; existing regressions use their established disposable V1
  fixtures. No persistence data artifacts or archives are added to the repository.
  No commit/push/merge/PR/history rewrite; local and remote main remain accepted.
  Stage 6C is not begun; original Phase 16 remote VPS deployment remains EXCLUDED.
- **Exact independent review gate:** inspect all five uncommitted files against
  the frozen specification and unchanged accepted codecs; independently challenge
  framing/bounds, owned state, resource failures, full CRC/extent/EOF identity,
  physical accounting and policy-independent permutation. Explicitly accept
  10C.1B.2A or request corrections before any publication. STOP here.
  Remaining **10C.1B.2B** is separately gated bounded tail TXN1 streaming and
  commit-marker boundary validation. Subsequent order remains **10D.1 → 10C.2 →
  10D.2 → 10E → 10F → 10G.1 → 10G.2** with independent acceptance and separate
  authorization; 10G.2 additionally requires its own deletion review. Neither all
  10C.1B nor original Phase 10C/Phase 10 is marked complete.

# Stage 6B / 10C.1B.1 — independent acceptance and feature publication (2026-10-27)

This status clarification supersedes current-action instructions below; the local
implementation checkpoint and every historical section remain verbatim.

Independent implementation review passed, as reported by the user, with no
blocking findings. The fixed-metadata implementation is accepted for feature
publication only. Supplemental GCC 14, Clang 17 and ASan/UBSan focused Linux
checks passed using a compatibility stub for the accepted V1 CRC interface;
these are supplemental review evidence, not full-repository Linux integration.
The existing macOS Debug 80/80, Release 80/80 and UBSan 81/81 integration evidence
remains applicable to the unchanged implementation. Publication rechecked all
three focused V2 tests, warning/header/format checks and exact five-file scope.

Current authorized task: one normal feature commit/push on
`fasttrack/v2-fixed-metadata-codecs`, followed by actual exact-feature-head Linux
CI inspection. Commit identities and CI outcomes belong in the publication report;
no future SHA or anticipated success is asserted here. Main merge remains a
separately authorized guarded task after successful feature CI: unchanged accepted
main, exact reviewed/tested feature head, five-file scope, ordered merge parents,
merge-tree equality and successful exact post-merge main CI.

No V2 filesystem, streaming, recovery, import, publication, writer activation or
server functionality exists in this slice. Original Phase 10C remains incomplete;
10C.1B.2 and subsequent milestones remain separately gated. STOP after feature CI.

# Stage 6B / 10C.1B.1 — local fixed metadata implementation (2026-10-27)

This additive checkpoint supersedes current-action instructions below. All earlier
checkpoint text remains verbatim. **LOCAL, UNCOMMITTED; INDEPENDENT IMPLEMENTATION
REVIEW REQUIRED.** No implementation publication or acceptance is claimed.

- Accepted starting main: `e6b8e7a191b2ba9d8c25d5d5f132645af2a68e67`.
  10C.1A independently accepted and merged, exact-main Networking **40469997848**,
  ASan/UBSan/fuzz **40469997797**, Docker **40469997838** successful. These remain
  previous-implementation baseline evidence, not tests of these new V2 codecs.
  Correct path/origin, GitHub identity `sinthetaaa`, clean main, fresh fetch,
  HEAD/local main/origin/main/remote main equality and new-branch absence verified.
  Created `fasttrack/v2-fixed-metadata-codecs` directly from accepted main.
- Exact scope: new `include/redisdb/persistence/V2Format.hpp`,
  `src/persistence/V2Format.cpp`, `tests/unit/v2_format_test.cpp`; modified only
  `CMakeLists.txt` (one core source and one unit target/test registration) and this
  additive handoff. Accepted format/architecture/ROADMAP, V1 persistence/storage,
  existing tests, server, CI and Docker remain unchanged.
- `redisdb::persistence::v2` adds checked nondefault StoreId/GenerationId/
  LogicalSequence/TailOffset/Crc32c domains, generation-qualified BaseIdentity,
  TailHeaderIdentity and GenerationCheckpoint, owned untrusted field structs,
  categorized FormatError exceptions, checked generation/sequence advancement,
  canonical decimal generation parsing/formatting and pure CRC reuse. Integer
  wrappers require the exact unsigned underlying type, preventing implicit
  signed/floating/narrowing construction; no V1 checkpoint conversion exists.
- Pure fixed codecs implement base header **184**, tail header **96**, marker
  **112**, CURRENT **112** bytes, exact BE fields/CRCs/trailers/reserved values,
  source kinds 0/1/2, structural counts/offsets and immutable cross-header binding.
  Full-header CRC is distinct from stored tail-header CRC. Empty markers require
  generation-local S/96/0/CRC(full header); supplied-header checks distinguish
  empty/nonempty claims. No nonempty record boundary is asserted from metadata.
  Raw marker codec cannot establish S without the supplied tail-header validator.
- No ValidatedSelector, DurableTailBoundary, PreparedGeneration, streaming base
  frames/tail records, complete-file CRC verification, recovery/import, publication,
  locking, writer activation or server integration. Header consistency is explicitly
  separate from complete-file/semantic validation, file identity and mandatory R4.
  No V2 filesystem calls or persistence mutation; new tests operate entirely in
  memory. Existing regression tests use their own disposable V1 test data/roots.
- Dedicated `unit.v2_format` compares **13 fixed portions** from accepted E/C/L/T
  vectors (12 initial family headers/markers plus nonempty T marker). The accepted
  in-memory recipe also reproduced all 23 documented outputs during development.
  Positive tests include BE high-bit fields, CRC standard/chunk extension, identity
  equality, provenance 0/1/2, V1 observed/durable/empty tuples, nonzero-S markers
  and exhausted-but-readable sequence. Negative cases cover all **504 short-input
  lengths**, trailing bytes, V1/wrong magics, versions/flags/lengths/accounting,
  every reserved byte, checksum/trailer, zero/foreign IDs, provenance/count/size/
  boundary errors, canonical decimal and exhaustion, inconsistent encode arguments
  and CRC-correct cross-header mismatches. Matching complete-base CRC claims are
  deliberately not treated as verification of the base's absent body bytes.
- Actual macOS local results: CMake Debug configure/build PASS; full Debug CTest
  **80/80 PASS**; Release configure/build and full CTest **80/80 PASS**; existing
  `build/stage4b-ubsan` reconfigured/built with undefined sanitizer and fuzz targets,
  full CTest **81/81 PASS**, including the 50-seed corpus smoke. Final focused
  `unit.v2_format` **1/1 PASS in Debug, Release and UBSan**. After adding the final
  direct raw-field/BE cases, full Debug was rerun: **80/80 PASS** (11.96 seconds).
  Release/UBSan full runs also include those final cases. No ASan or 10,000-run
  local fuzz claim.
  Full suites ran outside the socket-restricted sandbox as authorized. No local
  Docker or GitHub CI execution. CTest discovery verifies V2 exactly once in each
  configuration; actual compile commands retain warning/UBSan instrumentation.
- Validation: clang-format dry-run/Werror PASS for all three new C++ files;
  strict `-std=c++20` standalone-header and both translation-unit syntax checks
  with all repository warnings plus `-Werror` PASS; tracked/new-file whitespace
  and `git diff --check` PASS. Initial build found a missing test-helper closing
  brace; corrected. A prematurely attempted focused CTest was Not Run because
  that failed build had no test executable; subsequent builds/focused/full runs
  passed. Self-review tightened integer construction and locally knowable empty-
  marker invariants before full regression runs; checksum-correct negative tests
  retain the intended binding checks. An audit helper expected an absent UBSan
  compile_commands.json; corrected to inspect actual Ninja compilation commands.
  No accepted assertion, warning flag, format field or existing test was weakened.
- **No commit, push, merge, PR, evidence archive or history rewrite.** HEAD stays
  at accepted main on the local implementation branch. Original Phase 10C and
  Phase 10 remain incomplete; original Phase 16 remote deployment remains
  **EXCLUDED**, never completed.
- **Exact next gate:** independent reviewer examines all five uncommitted paths
  against the frozen specification/source and local evidence, challenges bounds,
  provenance, checksums, strong types and header-only validation limits, then
  explicitly accepts this slice or requests corrections. STOP before publication.
  Remaining **10C.1B.2**: separately authorized base KEY1/LRU1/DONE and tail TXN1
  bounded streaming codecs/validators, semantic/complete-file checks and tests.
  Subsequent **10D.1 → 10C.2 → 10D.2 → 10E → 10F → 10G.1 → 10G.2** remain
  pending, with independent predecessor acceptance/separate authorization;
  10G.2 still requires its own deletion review and stopped physical reduction.
  No Stage 6C or infrastructure work has begun.

# Stage 6B / 10C.1A — independent format acceptance and publication (2026-10-27)

This additive checkpoint supersedes current-action instructions below. Every
previous handoff checkpoint is preserved verbatim as historical evidence.

- **Stage 10C.1A independently ACCEPTED at byte-format specification level**,
  as reported by the user; no blocking format contradiction. Accepted entry main:
  `6233f42e35dc5243314f1425cbc0e7d3c5cfe969`. Correct repository/origin and GitHub
  identity `sinthetaaa`, branch `fasttrack/aof-v2-format-spec`, HEAD/local main/
  fetched origin/main/remote main equality, absent remote feature branch and exact
  two-document worktree scope verified before edits/publication.
- [Accepted format specification](V2_PERSISTENCE_FORMAT.md): base header 184 bytes
  (27 contiguous fields), tail header 96 (14), commit marker 112 (18), CURRENT
  112 (17). Independent review reproduced all 23 CRC32C/SHA-256 artifact outputs,
  standard CRC32C check E3069283, empty/nonzero-S boundaries, mutation progression,
  generations 3/4, binary/LRU examples, cross-file bindings and malformed cases.
  V1 TXN1/KEY1/LRU1/DONE compatibility and P0–P8/R0–R5, including mandatory R4,
  remain intact. LRU reachability and future implementation-test obligations are
  editorial clarifications; no layout/magic/checksum/recipe bytes change.
- **No V2 code implementation or runtime validation.** Original Phase 10C is not
  formally complete. Existing main CI 40456025900 / 40456025929 / 40456025919
  remains accepted V1 baseline evidence. New exact-feature-head workflows exercise
  that existing implementation; they cannot dynamically validate an absent V2 codec.
- **Next step: guarded format-document publication** of exactly this handoff and
  the specification, one normal commit/push on the feature branch, then inspection
  of actual exact-feature-head Networking, ASan/UBSan/libFuzzer and Docker execution.
  Commit/parent/tree, actual run IDs/counts/conclusions and remote synchronization
  belong in the publication report, not a future self-referential SHA here.
- **After successful feature CI, STOP before merge.** A separate authorized guarded
  main merge requires unchanged accepted main, exact reviewed/tested feature head,
  clean two-document scope, ordered merge parents and merge-tree equality to the
  tested feature, then successful exact post-merge main Linux CI.
  After accepted merge, obtain separate **10C.1B codec/model implementation**
  authorization; it is NOT STARTED and is not authorized by this publication.
- Subsequent **10D.1, 10C.2, 10D.2, 10E, 10F, 10G.1 and 10G.2 remain pending**
  in accepted dependency order. 10G.2 still requires its own deletion review,
  stopped backup/pins/inventory and accepted physical-reduction evidence.
  No PR, main merge, filesystem feature, import, compaction, reclamation, Stage 6C
  or infrastructure work. **Original Phase 16 remote deployment remains EXCLUDED,
  never completed.**

# Stage 6B / 10C.1A — byte-exact V2 format design (2026-10-27)

This additive checkpoint supersedes current-action instructions below; every
historical handoff section is preserved verbatim.

- **Accepted main:** `6233f42e35dc5243314f1425cbc0e7d3c5cfe969`.
  Correct path/origin, GitHub identity `sinthetaaa`, clean tree, fresh fetch,
  HEAD/local main/origin/main/remote main equality and branch absence verified.
  Created `fasttrack/aof-v2-format-spec` directly from that SHA. Changes remain
  unstaged and uncommitted; main remains unchanged.
- Stage 6A is formally closed; Stage 6B architecture is independently accepted,
  feature `5a090e87c3ae4aaf38d16007b4484e6a08a15b9e`, now on accepted main.
  [Accepted architecture](AOF_COMPACTION_DESIGN.md) remains unchanged.
  User-supplied successful exact-main CI: Networking **40456025900**,
  ASan/UBSan/fuzz **40456025929**, Docker Linux **40456025919**. Historical V1
  results: Debug/Release 79/79 each, sanitizer 80/80, 50 seeds/10,000 fuzz runs,
  15 container/10 Compose checks, parity 54 cases/40 routes, 10 operational cases,
  benchmark 24 profiles/48 runs. No new CI execution or V2 validation is claimed.
- **10C.1A is design only, awaiting independent format acceptance.** Exact draft:
  [V2_PERSISTENCE_FORMAT.md](V2_PERSISTENCE_FORMAT.md). Proposed fixed headers:
  base 184 bytes, tail 96, selector/marker 112 each; BE fields, distinct V2 magics,
  exact CRC32C ranges, per-entry KEY1/per-key LRU1/DONE, unchanged TXN1/effects,
  store/generation-qualified immutable binding and mandatory full-EOF validation.
  Generation begins at 1; logical sequence survives compaction. CURRENT excludes
  mutable tail boundaries. P0–P8 and mandatory restart R4 remain unchanged.
- Review questions: exact layouts/dual header CRC identity; observed/durable source
  provenance and derived V1 durable prefix CRC; inherited hash/set decoder order
  and noeviction journal LRU semantics; portable bounds/exhaustion policy; strong
  validated types versus actual durability/activation. These are concrete proposed
  decisions in section 13, not architecture-level byte-format acceptance.
- Documentation calculations exercised the in-memory golden recipe, independently
  cross-checked CRC32C conventions, header offsets/widths, binary/LRU and empty/
  nonzero-S tail boundaries. Four independent width/table calculations and all
  23 artifact outputs agree; 36 local links/anchors, fences/table columns and
  tracked/new-file whitespace passed. ROADMAP is byte-identical with all 169
  identifiers; historical handoff is preserved verbatim. External historical URLs
  were not re-fetched. Documentation-only self-audit is not independent
  review or a working V2 codec test. Only this handoff and the new specification
  are authorized changes. No source/codec/test/build/CI/Docker implementation,
  runtime persistence fixture/data operation, commit/push/PR/merge or archive.
- **Next action:** independent reviewer reads the entire uncommitted format draft
  against accepted source/architecture, reproduces vectors and field/CRC ranges,
  resolves section 13 and explicitly accepts or requests documentation revisions.
  STOP before coding. Original Phase 10C is not marked complete. Obtain separate
  10C.1 implementation authorization after format acceptance.
- Remaining Stage 6B dependency order: **10C.1 → 10D.1 → 10C.2 → 10D.2 →
  10E → 10F → 10G.1 → 10G.2 → formal Phase 10 closure**. Each predecessor needs
  independent acceptance and the next slice separate authorization. 10G.2 still
  needs its own deletion review and stopped physical-reduction evidence. No
  10D.1/10C.2, Stage 6C, reclamation or infrastructure work has started.
  **Original Phase 16 remote VPS deployment remains EXCLUDED, never completed.**

# Stage 6B.0C — architecture acceptance and documentation publication (2026-10-27)

This checkpoint supersedes current-action instructions in the preserved 6B.0B,
6B.0A and Stage 6A records below. All earlier handoff text remains verbatim.

- **Accepted starting main:** `65a495a42df1e1bc73268137c3124852d76f71e5`.
  Fresh origin fetch, authenticated GitHub identity `sinthetaaa`, correct repository,
  branch `fasttrack/aof-compaction-design`, and HEAD/local main/origin/main/remote
  main equality verified. No conflicting remote design branch existed. Exactly
  the two intended documentation changes were present.
- The user reports independent acceptance of the revised Stage 6B.0B architecture.
  [AOF compaction design](AOF_COMPACTION_DESIGN.md) publishes accepted constraints:
  dedicated opt-in V2 root; existing valid CURRENT for ordinary startup; explicit
  init/offline import; mutually exclusive V1/V2 options; V2 SIGUSR1 compaction on
  the existing event loop; unchanged V1 SIGUSR1 snapshots; continuous stable root
  lock; checksummed generation-selected base/tail/marker; P0–P8 and R0–R5;
  fail-closed file/sync errors; no automatic deletion or recovery fallback.
  R4 is mandatory on every V2 serving restart before any startup write/readiness;
  the modeled double-crash scenario remains required future validation.
- Import clarification: omitting `--source-snapshot-dir` explicitly selects
  AOF-only source recovery. The importer cannot discover an arbitrary separately
  configured snapshot directory; an unprovided corrupt snapshot is not validated.
  Provided snapshot directories require full validation. Source V1 files remain
  read-only; original V1 formats/recovery remain unchanged.
- Acceptance is **architecture only**, not byte-exact format, production-code
  authorization or implemented/tested V2 behavior. Original 10A/10B and 10C–10G
  remain incomplete. Retain-all is intermediate, not storage-lifecycle completion.
  Exact-byte format review is required before implementing 10C.1; separate
  independent deletion review and authorization are required before 10G.2 code.
- Required order: **10C.1 → 10D.1 → 10C.2 → 10D.2 → 10E → 10F → 10G.1 →
  10G.2 → formal Phase 10 closure**. Each needs separate authorization, focused
  tests, exact-feature-head CI and an independently accepted predecessor.
  Mandatory 10G.2 requires stopped physical reclamation safety/reduction evidence.
- Publication review confirms consistent P/R visibility/durability, mandatory R4,
  no pre-R4 startup writes, double-crash model, V1/source preservation, no retired
  fallback, explicit stopped reclamation gates and acyclic dependencies. Validation
  passed: exact two-file scope, preserved historical handoff, byte-identical ROADMAP
  with all 169 identifiers, 100 Markdown links (53 unchanged source-line targets;
  historical external URLs retained), local targets/anchors, fences, numbered
  protocol/dependency ordering and tracked/new-file whitespace. No source/tests/CMake/workflows/Docker changes or local C++ tests.
  Historical test/CI results below remain historical evidence.
- One normal Conventional Commit and push are authorized after all guards pass.
  Exact commit/parent/tree, remote equality and triggered Linux run IDs/results
  belong in the external publication report, avoiding self-referential commit
  metadata or claims of future CI success. Existing workflows validate unchanged
  implementation regressions; they do not dynamically validate V2 design.
- **STOP after feature CI.** No PR, merge, implementation, deletion, infrastructure
  or Stage 6C. Original Phase 16 remains **EXCLUDED, never completed**.
  Next guarded merge requires separate authorization, fresh identity/origin/ref
  checks, accepted main unchanged, exact reviewed/tested feature head, clean
  two-document scope, normal non-fast-forward merge with exactly accepted-main
  then feature parents, tree equality to tested feature, and successful exact
  post-merge main Linux CI. Any failing guard stops merge. Then separately
  authorize 10C.1 scope and accept its byte format before coding.

# Stage 6B.0B — current independent-review corrections (2026-10-27)

This additive checkpoint supersedes current-action instructions in the preserved
6B.0A/Stage 6A record below. Those historical sections remain unchanged.

- **Accepted main remains `65a495a42df1e1bc73268137c3124852d76f71e5`.**
  HEAD/local main/origin/main and branch `fasttrack/aof-compaction-design` verified
  at entry; correct origin retained. Existing work was exactly the two uncommitted
  docs. No new fetch, identity-network check, CI run or C++ test is claimed in 6B.0B.
  Stage 6A remains formally closed; its final main CI metadata/results are retained
  in the 6B.0A checkpoint: 40384462761 / 40384462817 / 40384462849.
- Independent reviewer feedback supplied by the user **conditionally accepts the
  general architectural direction**, with mandatory corrections. The revised
  [AOF compaction design](AOF_COMPACTION_DESIGN.md) records findings/resolutions in
  section 13. This revision awaits independent re-review; byte-exact V2 format,
  deletion protocol and implementation scope are not accepted by this checkpoint.
- Critical correction: restart R0–R3 validates only the selected unit read-only;
  R4 pins/rechecks CURRENT/LOCK/selected identities, successfully fsyncs the root
  containing observed CURRENT, then rechecks before any R5 baseline/marker/expiry
  write, readiness or acknowledgment. Failed sync/identity check is fatal. Applies
  after P6-before-P7 process death and first initialization/import selector rename.
  Modeled second power loss cannot roll back selection after successful R4; tests
  distinguish always durability from weak-policy unsynced suffix and from SIGKILL.
- Proposed precise modes: explicit `--persistence-dir` V2 serving with appendonly
  yes; separate offline `--persistence-init` and `--persistence-import-v1`; V1/V2
  persistence options mutually exclusive. V2 SIGUSR1 runs generation compaction on
  existing single event loop; V1 SIGUSR1 snapshots/defaults remain unchanged.
  No source V1 AofLog constructor/recover or SnapshotStore constructor/publication
  on import; ordered existing source locks, full prefix/suffix validation, source
  hashes/membership preserved on success/failure, physical TTL/LRU fidelity.
- Selected CURRENT/base/tail/marker and stable LOCK must be regular single-link
  artifacts. Root-relative operations, continuous exclusive lock, exact owned-temp
  cleanup/collision rules and fatal unsupported directory sync; trusted directory
  assumption remains. No hostile-directory/anti-rollback guarantee is claimed.
- **Required dependency order:** accepted 6B.0B re-review → 10C.1 format/model →
  10D.1 tested durable publication primitive → 10C.2 init/import/recovery including
  R4 → 10D.2 live compaction/handoff → 10E interruption acceptance → 10F full
  compatibility/correctness → 10G.1 recovery/active-history measurements →
  **10G.2 Explicit Stopped Generation Reclamation** → formal Phase 10 closure.
  Basic correctness/fault tests accompany every slice. Each requires accepted
  predecessors and separately authorized scope; no implementation has started.
- Supplemental 10G.2 requires its own independent deletion review: stopped server,
  stable root lock, selected R0–R4 validation, verified independent stopped backup,
  explicit retention/pins, exact dry-run inventory and operator approval, identity
  check before every root-relative unlink and directory fsync after each unlink,
  safe partial retry and owned disposable test roots. Never delete selected,
  CURRENT/LOCK, pinned or unapproved files; reject ambiguity. No automated startup
  or background GC. Physical source-disk reduction must be measured/accepted before
  formal Phase 10 closure; retain-all and 10G.1 alone cannot establish reclamation.
- Documentation validation passed: revised sections fully read; 99 Markdown
  links/anchors, 53 source-line targets against accepted files, table columns/
  fences and whitespace checked; historical external links not re-fetched. The
  exact two-document scope, no staged/unrelated untracked files, preserved historic
  handoff, unchanged ROADMAP/169 original identifiers and acyclic dependencies
  also passed. Historical run/test results remain labeled historical. Only these two
  docs were written; no persistence files created/modified, production/tests/CMake/
  CI/Docker changes, compilation/tests/benchmark rerun, evidence archive or commit/
  push/merge. Changes remain uncommitted on `fasttrack/aof-compaction-design`.
- Original Phase 10A/10B and implementation tasks are **not marked complete**.
  Remaining stages 6C–6E are untouched. **Original Phase 16 actual remote Linux
  deployment remains EXCLUDED, never completed.**
- **STOP for independent re-review** of revised P0–P8/R0–R5 ordering, source V1
  compatibility, mode/signal boundaries, file identities, dependency graph and
  supplemental reclamation. Resolve section 13.3, then obtain separate 10C.1 scope
  authorization and byte-format review before coding. No commit/push/merge or
  Stage 6C work is authorized here.

# Stage 6B.0A — current design-only checkpoint (2026-10-27)

This additive checkpoint supersedes dated next-action instructions below; all
historical Stage 6A sections are preserved.

- **Last accepted main:** `65a495a42df1e1bc73268137c3124852d76f71e5`.
  Stage 6A is **formally closed**; original Phases 11 and 12 are accepted.
  Local main, fetched origin/main and remote main matched this SHA at task entry;
  GitHub identity `sinthetaaa`, correct origin and clean starting worktree verified.
- Final exact-main CI metadata rechecked: [Networking 40384462761](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40384462761),
  [ASan/UBSan/fuzz 40384462817](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40384462817),
  [Docker Linux 40384462849](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40384462849)
  all completed successfully at that SHA. Accepted baseline evidence is Debug
  79/79, Release 79/79, sanitizer 80/80 and 10,000 fuzz executions plus Redis/Docker
  validation; this documentation audit did not rerun those workloads.
- **Stage 6B.0A started as a design-only checkpoint.** Branch
  `fasttrack/aof-compaction-design` was absent locally/remotely and created from
  exact accepted main. HEAD remains at that SHA; main is unchanged. Draft changes
  remain unstaged/uncommitted in exactly two documentation paths: this handoff and
  [AOF_COMPACTION_DESIGN.md](AOF_COMPACTION_DESIGN.md).
- Draft covers source contracts/line references, Phase 10A growth evidence,
  alternatives, proposed opt-in V2 base + generation journal + watermark selected
  by one manifest, publication/recovery/failure state machine, explicit read-only
  V1 import, retention limits, validation matrix and 10C–10G slices. Its separate
  adversarial self-review is not independent design acceptance. **10A/10B are not
  marked complete. No persistence implementation has started.**
- Validation: all 94 Markdown links checked, including relative paths/anchors/
  source line bounds and API resolution of all 15 distinct historical/current
  GitHub run links (historical failures remain recorded as failures);
  `git diff --check` passed; only the two authorized docs changed, nothing staged;
  production/source, tests, CMake, CI and Docker files unchanged. No compilation,
  CTest, benchmark, Docker, sanitizer/fuzz rerun or evidence archive. No persistence
  data was removed, truncated or altered; no commit/push/PR/merge performed.
- **Exact next gate:** independent reviewer examines the uncommitted draft against
  accepted source and resolves section 13, explicitly accepting architecture,
  backward compatibility/V1 recovery and P0–P8 crash protocol before any coding.
  Revisions remain documentation-only. Then obtain separate implementation
  authorization for 10C.1 byte-format/model review; 10C–10G remain unstarted.
- Remaining: 6B Phase 10; 6C Phases 13–14; 6D roadmap/docs/demo; 6E final audit/
  portfolio closure. **Original Phase 16 actual remote deployment is EXCLUDED,
  never completed.** STOP before implementation, commit, push, merge or Stage 6C.

# Continuation checkpoint — Stage 6A documentation closure (2026-10-27)

Repository: https://github.com/sinthetaaa/redis-compatible-in-memory-database
Local path: `/Users/sinthetaaa/Desktop/Projects/SDE-Projects/RedisCompatibleDB`.

Authority: [ROADMAP](ROADMAP.md), [PROJECT_STATE](PROJECT_STATE.md),
[ARCHITECTURE](ARCHITECTURE.md), [DECISIONS](DECISIONS.md), [AOF](AOF.md),
[SNAPSHOTS](SNAPSHOTS.md), [BENCHMARKS](BENCHMARKS.md).
Dated records are historical evidence; current checkpoint notes supersede their
future-work statements. Do not delete or rewrite historical engineering records.

## Current accepted baseline and documentation closure

**Stage 6A engineering: COMPLETE AND ACCEPTED**, including all original Phase 11
items 11A–11H and Phase 12 items 12A–12G. Acceptance is supported by actual
post-merge execution on main, not merely test discovery or the earlier feature CI.

- Accepted main merge: `bf1f19f2961544b6524a3e31b4250ce4e9ff0952`.
- Accepted feature: `fasttrack/network-observability` at `6f11ff21eeb5725e6b23cb5e45ecb57aaebe5059`.
- Exact feature history: `208df581a8eaacf02faa9f74608092e4697e6289` followed by
  `6f11ff21eeb5725e6b23cb5e45ecb57aaebe5059`; both commits are preserved.
- Normal non-fast-forward merge, exactly two parents in order:
  `832631ecec6afb55869514d2f3fa6fdd8155dc77` and `6f11ff21eeb5725e6b23cb5e45ecb57aaebe5059`.
- Merge tree `48b95456f18df6f35938c68e3f0177881277d3a0` exactly equals the accepted feature tree;
  only the reviewed 22 implementation/documentation/test/CI paths were merged.
- Clean main at task entry; fetched local main, origin/main and remote main all
  matched the accepted merge. GitHub identity `sinthetaaa` verified; feature branch
  preserved locally and remotely. No conflicting closure branch existed.
- Current task/branch: strictly documentation-only closure on
  `fasttrack/stage6a-closure`, created directly from accepted main. These six-document
  changes were uncommitted and unpublished at preparation. Review and a normal
  feature commit/push are now authorized; a closure merge is **not authorized**.
  Main remains at the accepted baseline. Determine the eventual feature SHA from
  Git and exact-head CI externally; this document has no self-referential SHA.

| Workflow on accepted main | Exact verified execution |
|---|---|
| [Networking 40375036254](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40375036254) | Debug 79/79; Release 79/79; networking repeat gates passed |
| [ASan/UBSan/libFuzzer 40375036340](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40375036340) | Sanitizer 80/80; 10,000 bounded fuzz executions |
| [Docker Linux 40375036503](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40375036503) | 15 container tests; 10 private Compose checks; Redis 8.10.2 parity 54 cases/40 routes plus 10 operational cases; 24 benchmark profiles/48 timed runs |

Both `unit.operations` and `integration.resp_operations` executed and passed in
Linux Debug, Release and sanitizer suites. Existing networking, command, TTL,
LRU/memory-limit, AOF, snapshot, recovery and transaction rollback regressions
passed. The benchmark matrix has no throughput acceptance threshold; these counts
are execution evidence, not new performance claims. Local Docker was not run.

Stage 6A accepted startup-only idle timeout, active-output/deferred-work protection,
bounded shutdown drain, JSON lifecycle/error diagnostics, saturating counters,
read-only INFO/CONFIG GET subsets and bounded concurrent/slow-reader validation.
The detailed audit, counter definitions, limitations and persistence invariants
below remain authoritative. Phase 11H is complete to its documented bounded test
scope; no indefinite soak or production-capacity guarantee is claimed.

Original Phase 16 is **EXCLUDED BY APPROVED SCOPE REVISION**, not completed.
No VPS was provisioned, no remote deployment occurred, and no card, SSH host or
permanently running public server is required. Docker/Linux validation and private
host publication remain mandatory; all other original phases remain in scope.

Remaining engineering: **6B** safe AOF compaction (Phase 10); **6C** correctness
and performance gates (Phases 13–14); **6D** roadmap reconciliation, final docs and
demo (Phases 6–9, 15, 17); **6E** final engineering audit and portfolio closure
(Phases 18–19). Stage 6B starts with detailed architecture and crash-safety design
review before any persistence-code modification; no Stage 6B work has begun.

Current closure file manifest: [README.md](../README.md),
[CONTINUATION_HANDOFF.md](CONTINUATION_HANDOFF.md),
[PROJECT_STATE.md](PROJECT_STATE.md), [ROADMAP.md](ROADMAP.md),
[ARCHITECTURE.md](ARCHITECTURE.md), [DECISIONS.md](DECISIONS.md).

Preparation-time documentation-only validation (2026-10-27): exactly the six authorized paths
changed, no staged files or untracked project artifacts, and all implementation,
test, CI, build and Docker files unchanged. The original 20 phases and 169 unique
checklist items are preserved; only 11A–11H and 12A–12G checkboxes changed. Phase
10 and excluded Phase 16 remain unchecked. All 43 relative Markdown links/anchors
resolve; git diff --check passes. Accepted SHAs/run IDs, consistent completion and
scope statements, design-first 6B–6E plans, historical milestones, failed/skipped
CI attempts and unchanged ADR/persistence contracts were checked. No new C++ build,
CTest, benchmark, Docker or CI execution was performed for this documentation-only
closure; acceptance counts above belong to the previously verified main runs.

Publication review (2026-10-27) authorizes one normal documentation commit/push
only after all guards pass. Then verify exact-feature-head Linux workflows and
STOP for separate guarded merge authorization. Engineering acceptance on main
is already established; documentation closure is not yet merged. No Stage 6B
work is authorized. Preparation-time statements above do not assert the eventual
published branch is uncommitted; inspect Git and CI for its current publication state.

## Original scope and historical Stage 6A entry checkpoint

The following entry/publication record is preserved as historical evidence.
Its old accepted SHA, branch and pending-acceptance instructions are superseded by
the current checkpoint above; they are not instructions for the current task.

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

Every row is COMPLETE AND ACCEPTED through the exact-main evidence in the current
checkpoint. The table distinguishes reused baseline functionality from Stage 6A
additions; 11H acceptance is limited to the documented bounded workloads. The
documentation closure is separate from this engineering acceptance.

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
A blocked stderr sink can stall lifecycle reporting; there is no background logger.

## Files, environment and historical implementation validation

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

Historical local implementation validation (2026-10-26):

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
| Linux Debug/Release/epoll, ASan/UBSan/libFuzzer, Docker/Compose | NOT RUN locally; subsequent accepted remote CI is recorded above |

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
No local performance benchmark or local Linux Docker execution is claimed here;
accepted GitHub-hosted Linux execution is recorded in the current checkpoint.

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

### Historical publication review and CI corrections (2026-10-26)

The pending statements in this historical record describe its original checkpoint;
corrected feature and post-merge main CI subsequently passed as recorded below.

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

### Corrected feature and guarded main acceptance

Corrected feature `6f11ff21eeb5725e6b23cb5e45ecb57aaebe5059` passed Networking
[40371833161](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40371833161),
ASan/UBSan/fuzz [40371833225](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40371833225)
and Docker [40371833198](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40371833198).
The guarded main merge then passed all three exact-main runs in the opening
checkpoint. Earlier failed or skipped steps above are retained as such; they are
not counted as successful execution. This documentation closure changes none of
the accepted code, tests, CI, build or Docker behavior.

### Historical baseline CI and implementation file manifest

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

Historical Stage 6A implementation manifest: six added — this handoff,
`include/redisdb/server/Operations.hpp`, `src/server/Operations.cpp`,
`tests/unit/operations_test.cpp`, `tests/integration/resp_operations_test.py`,
`tests/differential/operations_parity.py`. Sixteen modified — `.github/workflows/docker-linux.yml`,
`tests/differential/linux_container.py`, `CMakeLists.txt`,
`README.md`, `docs/{ROADMAP,PROJECT_STATE,ARCHITECTURE,DECISIONS}.md`,
`include/redisdb/net/ConnectionManager.hpp`,
`include/redisdb/server/RespCommandServer.hpp`, `src/main.cpp`,
`src/net/ConnectionManager.cpp`, `src/server/RespCommandServer.cpp`,
`tests/integration/{connection_dispatch_test.cpp,resp_hardening_test.py,
resp_pipeline_scheduler_hardening_test.cpp}`. That 22-file scope belongs to the accepted engineering feature. The current
six-document manifest is listed in the opening checkpoint; no implementation
paths are changed.

## Remaining milestones and exact next step

- **Stage 6B — Phase 10, NOT STARTED:** 10A AOF growth analysis; 10B
  rewrite/compaction design; 10C generation and offset coordination; 10D safe file
  replacement; 10E interrupted rewrite recovery; 10F compaction correctness tests;
  10G recovery-time measurements. All seven tasks remain pending.
- **Stage 6C:** complete original correctness/performance gates, Phases 13–14.
- **Stage 6D:** roadmap reconciliation, final documentation/demo, Phases 6–9, 15, 17.
- **Stage 6E:** final engineering audit/portfolio closure, Phases 18–19.
- **Original Phase 16:** excluded by approved scope revision, never marked complete.

Exact publication workflow: complete the authorized six-document review, create
one guarded Conventional Commit directly from accepted main, push the feature
normally and verify all triggered workflows at that exact documentation HEAD.
After feature CI, STOP. A separately authorized guarded merge must recheck main
and feature identities, two merge parents and tested-tree equality, then require
exact post-merge main CI. Main merge and Stage 6B work are not authorized here.

Exact next engineering task, only after separate Stage 6B authorization: begin
10A/10B with detailed architecture and crash-safety design review. Inspect AOF
growth, journal/watermark and snapshot/CURRENT associations, sequence/offset
coordination and interruption boundaries before modifying persistence code. Preserve
acknowledgment, rollback and recovery invariants; no compaction implementation or
format change is part of the present task.

### Historical publication next action (superseded)

The instructions below applied before the now-accepted guarded merge and are
retained only to preserve the earlier continuation record:

Exact next action after authorized feature publication: monitor all three workflows
at exact feature HEAD, fix relevant failures via ordinary follow-up commits and
normal pushes, and revalidate corrected exact HEAD. After successful feature CI,
STOP and obtain separate guarded merge authorization. Recheck accepted main and
feature ancestry, review the tested tree, merge only if authorized, then require
successful exact post-merge main Linux CI before recording Stage 6A accepted.
Do not begin Stage 6B in this task.
