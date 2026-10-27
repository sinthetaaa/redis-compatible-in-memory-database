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
