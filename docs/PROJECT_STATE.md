# PROJECT STATE — Redis-Compatible In-Memory Database Server

## 1. Snapshot

- Updated: 2026-10-24. Phase 6D formally closed at `281d044f4f09ac4b33f4323067ef4bc20c46bbbe`. Phase 6E WRONGTYPE test/CMake/docs implementation merged into `main` at `37aaad4068ca0cf4452635737bd219a11d120d4c`; green post-implementation Linux Debug/Release and Clang 18 sanitizer/fuzz. Formal Phase 6E documentation-only closure still pending.
- Current phase: Phase 6 — Collection Data Types IN PROGRESS. Phases 0–5 and 6A–6D formally complete. Phase 6E implementation ACCEPTED but NOT formally closed; Phase 6F–6G NOT STARTED.
- Current subphase: Phase 6E.2B — guarded five-document closure draft on `phase6e/closure-audit`, branched from accepted implementation merge. Do not commit, push or merge the draft until independently reviewed and its local regression accepted.
- Last completed engineering milestone: exact cross-product WRONGTYPE unit + real TCP integration suites; 26 commands x 3 incompatible stored types = 78 rejection cases per suite, with TTL/value integrity, missing keys, MGET nil, SET/MSET replacement, and pipeline recovery. Feature commit `a66128b077cfbfc22fbe6daca1e053e5fdaa111e`, feature tree and main merge tree `2ff6ff4be49c6b81cd787a8c5a4ec4b992b10712`, two-parent direct main merge `37aaad4068ca0cf4452635737bd219a11d120d4c`. Only six CMake/test/doc files changed; runtime source untouched. macOS Debug/Release 64/64 each, fuzz-enabled 65/65. Exact-head feature Linux `40004381493` / `40004381506` and post-merge main `40005159337` / `40005159401` all green.
- Exact next task: Verify the five-document draft locally, independently review its exact bytes, commit/push only `phase6e/closure-audit`, require both exact-head documentation-branch Linux workflows, perform a guarded two-parent docs-only merge into `main`, and verify both exact-head post-closure main Linux workflows. Synchronize the final status snapshot with verified closure before any Phase 6F engineering begins.
- Current command subset: PING/ECHO/QUIT; documented string/integer, key and TTL operations; LPUSH/RPUSH/LLEN/LPOP/RPOP/LRANGE; HSET/HGET/HEXISTS/HLEN/HDEL/HMGET/HGETALL; SADD/SREM/SISMEMBER/SCARD/SMEMBERS/SMISMEMBER. The Phase 6E matrix is a test milestone and adds no commands.
- Scope limits: Unordered hash/set collections; 8,192-byte reply bound and command-specific reply-safe member limits. Redis server compatibility remains intentionally partial. Phase 6F memory accounting, Phase 6G differential testing, eviction, AOF/snapshot persistence, crash recovery, deployment and benchmarks remain incomplete.
- Current-status authority: This top snapshot supersedes older local-only Phase 6E continuation instructions below; the following dated Phase 6D records remain valid historical evidence. See `docs/ROADMAP.md` for future gates.

### Phase 6E wrong-type cross-product acceptance — implementation merged (2026-10-24)

- **Baseline:** Final Phase 6D five-document closure `281d044f4f09ac4b33f4323067ef4bc20c46bbbe`; CI `39999397205` / `39999397202` successful.
- **Feature:** Retained `phase6e/wrongtype-acceptance` at `a66128b077cfbfc22fbe6daca1e053e5fdaa111e`, one parent equal to the Phase 6D closure. Feature-head Linux networking `40004381493` and Clang 18 sanitizer/fuzz `40004381506`, all jobs and steps green.
- **Merge:** Direct no-ff merge `37aaad4068ca0cf4452635737bd219a11d120d4c`, two parents Phase 6D closure and the feature commit, tree `2ff6ff4be49c6b81cd787a8c5a4ec4b992b10712`. Exactly four modified files (CMake, README, PROJECT_STATE, ROADMAP) and two new tests; production code and workflow files unchanged.
- **Tests:** `unit.wrongtype_matrix` and `integration.resp_wrongtype_matrix`; 26 commands against three wrong stored types, 78 rejection cases in each suite. Assertions include exact WRONGTYPE, preserved value/type/TTL on errors, binary-safe data, missing keys, MGET nil for non-strings, SET/MSET replacement, and TCP pipeline recovery. Full macOS regression Debug/Release 64/64 and fuzz-enabled 65/65. Post-merge main networking `40005159337` and Clang 18 sanitizer/fuzz `40005159401` green.
- **Next acceptance:** Independent five-document-only closure, doc-feature CI, direct two-parent docs-only merge into main, and independent final main exact-head CI. Do not mark Phase 6E formally complete before that final CI and status sync. Phase 6F/6G have not started.

### Phase 6D formal closure — VERIFIED PASS (2026-10-23)

- **Final main:** `281d044f4f09ac4b33f4323067ef4bc20c46bbbe`; exact two parents `d21421ad910a179ea877fc7f1e555688ca8a81a8` and `c030abe0d6339a78bad62542f715581b1f1b5f3e`; tree `287bbc48c8f2ed93288eded5a65f433cf9e1e28e` equals the reviewed five-document feature tree.
- **Final exact-head push CI:** Linux Debug/Release `39999397205` SUCCESS; Linux Clang 18 ASan+UBSan/fuzz `39999397202` SUCCESS; all jobs and steps green. The former documentation feature `phase6d/closure-audit` and implementation feature `phase6d/set-storage-commands` remain retained.
- **Read-only Phase 6E.0A independent audit:** PASS with all ten Phase 6D exact-head feature/merge workflows accepted, macOS Debug/Release 62/62, fuzz-enabled 63/63, synchronized clean main. No source, workflow, or history changes made by the audit.
- **Scope boundary:** Six implemented set commands are accepted; 6E comprehensive WRONGTYPE cross-product verification, 6F memory accounting hooks and 6G differential tests are separate incomplete stages. This record supersedes the draft-time snapshot that appears in older milestone notes.

### Phase 6D set storage and six RESP2 commands — implementation accepted (2026-10-23)

- **Baseline:** Final Phase 6C docs closure `201c3f1f3036ad70e69d8a15b84fc5202782fc97`, with exact-head Linux networking `39987655559` and fuzz `39987655669` successful.
- **Feature history:** `phase6d/set-storage-commands` retained. First commit `743261636c2926f40b56f61856bcb9725e8e9b72` (five storage primitives and foundation unit test), second `40657326e93631d8851c780424a730389b1f2cbb` (six RESP2 routes, unit test and live TCP test). Both feature commits passed local and Linux exact-head CI; second feature run IDs `39995306323` and `39995306807`.
- **Implementation merge:** Direct no-ff merge `d21421ad910a179ea877fc7f1e555688ca8a81a8`, two parents `201c3f1f3036ad70e69d8a15b84fc5202782fc97` and `40657326e93631d8851c780424a730389b1f2cbb`. Merge tree equals feature tree `e1cadc1b2c86b24d37ce6819bd3926c87eda507d`. Exactly nine source/build/test paths changed; documentation and CI workflow files unchanged.
- **Semantics:** Binary-safe unique members; SADD counts newly inserted distinct members; SREM counts removed distinct members and deletes empty set keys plus expiry index; SISMEMBER/SCARD return integer 0/1/count; SMEMBERS returns unordered owned RESP2 bulk array; SMISMEMBER returns positional integer array. Missing-key reads produce 0 or an empty array; invalid types produce WRONGTYPE. Existing TTL survives nonempty mutation. Request/reply-size limits are explicitly bounded.
- **Verification:** macOS Debug 62/62, Release 62/62, fuzz-enabled 63/63; permanent tests `unit.set_storage_foundation`, `unit.set_commands`, `integration.resp_set_commands`; live networking tests cover fragmentation, pipelining, concurrent clients and error recovery. Feature Linux networking `39995306323` (2 jobs) and fuzz `39995306807` (1 job) successful. Exact post-implementation main `39996097334` (Linux Debug/Release) and `39996097046` (Clang 18 libFuzzer ASan/UBSan) successful with every job and step green.
- **Scope/operations:** No PR, force push, history rewrite or branch deletion. Document closure is separate and NOT YET MERGED. Phase 6E wrong-type matrix, 6F memory accounting, 6G differential testing and later persistence/eviction/deployment are future work.
- **Next subphase:** 6D.3B docs draft and test, 6D.3C docs commit and exact-head feature CI, 6D.3D guarded direct docs merge and final exact-head main CI. Then begin 6E on a newly accepted clean main.

### Phase 6C hash storage and RESP2 commands — accepted implementation (2026-10-23)

- **History:** Phase 6B formal closure `81798723a794f261682844a4fe01dc5adf9eeb35`, green Linux networking/fuzz `39975274770` / `39975274694`. Phase 6C feature `phase6c/hash-storage-commands` retained at final commit `88817b205b511f7bf4799ea2a675a74845427f16`, with first hash foundation commit `0a6a560dc6bc9746617bd8da7d020cd921993916`.
- **Implementation merge:** Two-parent non-fast-forward commit `7b0abaa20b65b6095dd432784aced4a16fc70d32`, parents `81798723a794f261682844a4fe01dc5adf9eeb35` and `88817b205b511f7bf4799ea2a675a74845427f16`. Merge and accepted feature tree `1fdac28176454971c28281da8ded2ed0a2108d2e`. Exactly nine implementation files changed; neither workflow nor documentation altered in the implementation merge.
- **Storage:** `hash_set`, `hash_get`, `hash_exists`, `hash_length`, `hash_delete`, `hash_items` on binary-safe owned hash fields. HSET last-wins and counts distinct new fields; the stored hash is unordered. Existing TTL and expiry index survive field mutation; deleting the final field removes the key and corresponding expiry metadata; lazy expiration remains consistent.
- **RESP2 and compatibility:** Seven live routes HSET/HGET/HEXISTS/HLEN/HDEL/HMGET/HGETALL; HGET/HMGET return nil for missing fields, HGETALL produces an unordered alternating field/value reply, reply size at most 8,192 bytes. Non-hash storage reports WRONGTYPE; string GET/APPEND/arithmetic reject hash, MGET treats hash as nil; SET/MSET replace hash and clear TTL. Tests include binary bytes, malformed arity, large replies, fragmentation, pipelines, multi-client correctness and optional redis-cli.
- **Tests:** macOS Debug 59/59, Release 59/59 and fuzz-enabled 60/60; newly added unit `unit.hash_storage_foundation`, `unit.hash_commands` and live `integration.resp_hash_commands`. First storage feature CI `39979350475` / `39979350423` and second feature CI `39983170325` / `39983170439` accepted. Post-implementation main Linux `39984084722` (Debug/Release) and `39984084843` (Clang 18 ASan/UBSan/fuzz) completed success at exact merge SHA with all jobs and steps passing.
- **Workflow:** Feature branch preserved; no PR, forced push, rebasing or deletion. Formal closure is a distinct five-document-only feature, reviewed CI gate, direct merge, and post-closure CI gate. A prior checkpoint must not be treated as current instructions.
- **Next exact subphase:** Phase 6C.4A docs-only draft and validation, then 6C.4B docs commit/push plus feature Linux CI; then 6C.4C two-parent docs merge and final CI; only afterward Phase 6D.


### Phase 6B list storage and RESP2 commands — accepted implementation (2026-10-23)

- **Feature branch:** `phase6b/list-storage-commands` retained remotely; first commit `18e27a0995071acfed579aa52d23167f15e16a09` (six-file list storage and cross-type hardening); second commit `00dddb0db60913c0a9d8e990c4dd49b71c6b8eb6` (eight-file wire commands/tests). Both commits follow clean Phase 6A closure baseline `e895e7cd06474a0ae2e1079591409eace86f517c`.
- **Implementation merge:** `7df2e08209468ae3e8a619e25451abe9196bda11`, parents `e895e7cd06474a0ae2e1079591409eace86f517c` and `00dddb0db60913c0a9d8e990c4dd49b71c6b8eb6`, with merge/feature tree `1acc6498c99f007fa4f747e578ebc43ca85ee814` (matching). Exactly eleven combined code/test/CMake files, with no documentation or CI workflow modifications.
- **Storage:** `Database::type_of`, `list_push`, `list_length`, `list_pop`, `list_range`; owned binary-safe list members, left/right insertion order, duplicate preservation, TTL retained during mutation, last-pop removal from keyspace and expiry index, lazy-expiration consistency, wrong-type rejection without partial mutation.
- **String and key interop:** GET/APPEND/STRLEN/INCR and related numeric routes return `WRONGTYPE` on lists, `MGET` returns nil for non-string values, `TYPE` returns `list`, SET/MSET replace prior typed entries with strings and clear expiration.
- **RESP2 wire subset:** LPUSH/RPUSH, LLEN, LPOP/RPOP single-element forms, LRANGE with negative-inclusive indexes, checked signed parsing, 8,176-byte element and 8,192-byte LRANGE reply-safe limits. No count-form pop, blocking queue commands, hashes or sets.
- **Verification:** Local Debug 56/56, Release 56/56, fuzz-enabled 57/57; permanent `unit.list_storage_foundation`, `unit.list_commands`, `integration.resp_list_commands` plus all older tests. Live-TCP tests cover 8 clients, fragmentation, pipelines, redis-cli (when installed), binary bytes, indexes, wrong-type and TTL. Feature-head Linux `39972085606` and `39972085386`: success. Post-merge main Linux `39973111123` (Debug/Release) and `39973111117` (Clang 18 ASan/UBSan/fuzz): each run is `push`, `completed`, `success` at exact `7df2e08` merge with all jobs/steps green.
- **Delivery:** No PR, force push, history rewrite or feature deletion. This implementation is ACCEPTED; formal five-document closure on its own branch and its own exact-head/after-merge CI remain pending at the time of this record.
- **Next exact stage:** Validate, merge and post-closure verify documentation branch `phase6b/closure-audit`; then begin Phase 6C on clean synchronized `main`.

### Phase 6A type-safe value representation — accepted implementation (2026-10-23)

- **Previous baseline:** `d9bbcaa611f85a8e5e356e7a1fc8a2f44f24cc41`, verified post-Phase-5 closure.
- **Implementation feature branch:** `phase6a/type-safe-value-representation`, retained remotely. Feature commit `94f7b8d626c5c470101972b3296e68dbb7f67e8f`, a single commit parented directly on prior baseline.
- **Two-parent implementation merge on main:** `5043cc7cee3124baf809434479e2812c366fba2f`; first parent `d9bbcaa611f85a8e5e356e7a1fc8a2f44f24cc41`, second parent `94f7b8d626c5c470101972b3296e68dbb7f67e8f`. The merge tree and feature tree are both `4cfbf7c0f88cefe0cf0156d0f17962da32f1b898`.
- **Exact three-file scope:** `include/redisdb/storage/Value.hpp`, new `tests/unit/storage_value_test.cpp`, and `CMakeLists.txt`. No public Database API, command handler, RESP2, network, expiration, CI workflow, or immutable parser corpus changes.
- **Containers:** `std::string` for StringValue, `std::list<std::string>` for ListValue, `std::unordered_map<std::string,std::string>` for HashValue, and `std::unordered_set<std::string>` for SetValue. All owned and binary-safe; list order/duplicates, hash field overwrite, and set uniqueness are tested. Deep copies, move/no-throw properties, Entry aggregate construction, and independent optional wall-clock TTL metadata are tested. Node-based list allocation is deferred to benchmark-based evaluation.
- **Local macOS acceptance:** Debug 53/53, Release 53/53, fuzz-enabled 54/54, including `unit.storage_value`; repeated successfully before the guarded implementation merge.
- **Feature exact-head CI:** Linux Debug/Release `39953114572` PASS; Clang 18 ASan+UBSan/fuzz `39953114723` PASS; push events at the full feature SHA.
- **Implementation-merge exact-head CI:** Linux Debug/Release `39962981311` PASS and Clang 18 ASan+UBSan/fuzz `39962981227` PASS; both `push`, `completed`, `success` at the full merge SHA. The CI sanitizer workflow verifies 50 immutable parser corpus seeds and performs 10,000 bounded libFuzzer executions.
- **Guarded delivery:** No PR, force-push, or history rewrite; implementation feature branch retained; local and remote main synchronized at the implementation merge according to guarded Terminal trace.
- **Phase boundary:** `storage::Value` and `protocol::RespValue` are distinct. No public API can insert a non-string database entry in 6A; existing `std::get<StringValue>` accesses are not reachable with wrong type. Actual collection command semantics and general WRONGTYPE are Phase 6B–6E work. Phase 6A status is accepted as implementation, with formal five-document closure pending its own CI and merge.
- **Next:** Phase 6B List storage and commands only after the documentation closure is integrated and its new main SHA passes post-merge Linux CI.

### Phase 5 formal closure record (2026-10-23)

- **Audit verdict:** PASS; implementation baseline `479fdf46481f1d5b8e3966ae2e56483ab4560db3`, the correct two-parent merge of Phase 5G (`5c1cf93`) and Phase 5H (`d26a39d`). No PR was used for the Phase 5H merge.
- **History:** All eight direct two-parent merges have their expected first and feature parents; each merge's tree equals its feature tree; all are ancestors of the audited baseline.
- **Evidence:** All 16 post-merge push-triggered GitHub Actions workflows for Phases 5A–5H ended in success at their exact merge SHAs.

| Subphase | Delivered | Two-parent main merge | Linux Debug/Release CI | Clang ASan+UBSan/fuzz CI |
|---|---|---|---|---|
| 5A | Clock abstraction | `05d0ca4` | `39920666650` PASS | `39920666241` PASS |
| 5B | Expiration metadata | `f51d8ad` | `39923103272` PASS | `39923103331` PASS |
| 5C | Lazy expiration | `11b1326` | `39925610242` PASS | `39925610067` PASS |
| 5D | EXPIRE/PEXPIRE | `7169576` | `39928058802` PASS | `39928058720` PASS |
| 5E | TTL/PTTL/PERSIST | `7782dfc` | `39931900587` PASS | `39931900483` PASS |
| 5F | SET EX/PX | `2756f01` | `39938093896` PASS | `39938094657` PASS |
| 5G | Bounded active expiration | `5c1cf93` | `39941780831` PASS | `39941780912` PASS |
| 5H | Deterministic acceptance | `479fdf4` | `39945756231` PASS | `39945756252` PASS |

- **Final exact-head CI:** Linux Debug 52/52 and Release 52/52 in run `39945756231`; Clang 18 ASan+UBSan 53/53 in run `39945756252`; SHA-256 manifest verified 50 immutable RESP2 corpus seeds, with 10,000 bounded libFuzzer iterations.
- **Final macOS acceptance:** 52/52 Debug, 52/52 Release, 53/53 fuzz-enabled tests on Phase 5H feature; independent Linux tests passed again after merge.
- **Deterministic acceptance:** fixed-seed 4,096-step reference model, 513-key backlog completed in 17 strictly 32-record sweeps, no premature deletion after backward clock jumps, and a two-client 192-reply RESP2 integration matrix.
- **Correctness architecture:** one authoritative binary-safe hash map; wall-clock expiration deadlines; monotonic 100 ms scheduling; 32-record limit per maintenance turn; ordered deadline index updated by every deletion, overwrite, expiration change and MSET clone-and-swap; lazy expiration remains authoritative between sweeps.
- **Deliberate exclusions:** No disk persistence or crash-recovery guarantees; no eviction, collection types, full Redis compatibility, SET NX/XX/GET/KEEPTTL or EXPIRE NX/XX/GT/LT; no remote deployment. Existing per-value 8,176-byte and per-reply 8,192-byte limits still apply; MSET still clones the keyspace before committing.
- **Repository policy:** Documentation closure occurs on isolated feature branch and merges directly only after its own exact-head Linux CI succeeds; do not start Phase 6 until the documentation closure merge has post-merge green CI and the working tree is clean and synchronized.

### Phase 4 formal closure record (2026-10-23)

- **Status:** ACCEPTED. All Phase 4A–4H commits are in main's ancestry; Phase 4H has correct two-parent merge history.
- **Exact audited main commit:** `31914ae7127b432f2fb38f3f07ac66a4ae6b435b`, parents `d7877afc2fc95f75e4271ada1736fcaba45e2336` and `589a1fd81cf54405a52351bbb9ae17f49b9d2947`.
- **Linux networking CI:** run `39915994311`, push event at audited SHA, Debug 39/39 and Release 39/39 PASS.
- **Linux sanitizer/fuzz CI:** run `39915994295`, push event at audited SHA, Clang 18 ASan+UBSan 40/40 PASS; 50 SHA-256-verified RESP2 corpus seeds; 10,000 libFuzzer iterations.
- **macOS acceptance:** Debug 39/39, Release 39/39 and fuzz-enabled 40/40 PASS on Phase 4H feature SHA `589a1fd81cf54405a52351bbb9ae17f49b9d2947`.
- **Command coverage:** SET, GET, DEL, EXISTS, TYPE, MSET, MGET, APPEND, STRLEN, INCR, DECR, INCRBY and DECRBY, plus PING, ECHO, QUIT. Binary-safe strings, duplicate key semantics, strict int64 overflow rejection, atomic MSET and cross-client visibility confirmed.
- **Accepted limits:** 8,176-byte individual stored string value limit and 8,192-byte serialized single-reply limit. MSET full-map clone-and-swap incurs O(keyspace + batch) memory/time. These are documented deviations or optimization targets, not defects resolved by Phase 4.
- **Not yet implemented:** TTL/expiration, eviction, AOF/snapshots, collection types, full Redis command/SET option compatibility and remote deployment. Phase 5A clock abstraction is NEXT, NOT STARTED.
- **Repository policy:** no PR for Phase 4H or documentation closure; guarded direct non-force merges, retained feature branches, clean main and exact-head CI evidence.

### Phase 5A clock design checkpoint (2026-10-23)

- **Status:** Feature implementation and test validation pending exact-head CI and merge.
- **Starting clean main SHA:** `188e00ca40f48273e789598f3e3a7fc86249a2ba`.
- **Decision:** ADR-009 dual time domains; monotonic (`steady_clock`) for in-process scheduling and wall (`system_clock`) for future persistent deadlines. Never serialize raw monotonic time points.
- **API:** `Clock` nonthrowing virtual `monotonic_now()` / `wall_now()`; `SystemClock` production implementation, test-only `ManualClock` injected as `const Clock&`.
- **Scope:** New clock header/source and clock unit tests; no database entry mutation, TTL, EXPIRE, AOF or networking changes.
- **Expected suite:** macOS and Linux Debug/Release 40/40; fuzz-enabled 41/41; existing 50-seed immutable parser corpus and Linux 10,000-run libFuzzer gate unchanged.
- **Next:** Verify feature CI, direct merge, then Phase 5B expiration metadata.

### Phase 5B expiration metadata checkpoint (2026-10-23)

- **Starting clean main SHA:** `05d0ca40b0f86dd85e722834e93cd39f2dbea20b`.
- **Status:** Feature implementation and local acceptance; Linux CI and direct merge pending.
- **Scope:** `Entry` owns optional absolute wall-clock deadline from 5A. `Database` exposes setter, getter and clear operations with no implicit key creation. No lazy removal or TTL wire commands until later subphases.
- **Lifecycle:** New keys have no expiration; SET and MSET clear previous deadline; APPEND and signed arithmetic preserve it; DEL/clear erase it. Rejected mutations never change deadline or value. Full-map MSET staging unchanged.
- **Time domain:** Absolute wall-clock deadline for future persistence compatibility, never serialized steady time. Test-only fake clock controls wall-time observations without sleeping.
- **Regression expected:** macOS/Linux Debug and Release 41/41; fuzz-enabled 42/42; existing immutable corpus and bounded Linux libFuzzer unchanged.
- **Next:** Verify exact-head feature CI, merge directly, then begin Phase 5C lazy expiration.

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
- Branch: main (Phase 5 audited baseline `479fdf4`; Phase 5A–5H feature branches retained)
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
- Current development branch: main (Phase 5H audited merge `479fdf4`; historical Phase 3 branches retained).
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
| 2 | RESP2 Protocol Engine | COMPLETE |
| 3 | Command Framework | COMPLETE (see authoritative roadmap; older notes are dated) |
| 4 | Core String Database | COMPLETE |
| 5 | TTL and Expiration | COMPLETE AND ACCEPTED (5A–5H) |
| 6 | Collection Data Types | IN PROGRESS (6A closed; 6B implementation accepted; 6C next after docs closure) |
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

### Phase 5 Detail

- 5A–5H: COMPLETE AND ACCEPTED — all eight implementation and acceptance subphases merged, all exact-head post-merge Linux CI successful, formal closure record above.
- Phase 6A: COMPLETE. Phase 6B: IMPLEMENTED, post-merge CI PASS, documentation closure pending. Phase 6C: NEXT after formal closure.

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

### Phase 3–4 functionality (accepted)

- Production loopback RESP2 command server with shared process-local database,
  binary-safe value ownership and serialized command execution.
- RESP2 incremental parsing, bounded FIFO pipelining, serializer and limits,
  with kqueue on macOS and epoll on Linux.
- SET/GET, DEL/EXISTS/TYPE, atomic MSET/ordered MGET, APPEND/STRLEN,
  INCR/DECR/INCRBY/DECRBY and PING/ECHO/QUIT.
- Signed 64-bit integer parsing and overflow safety; rejected operations
  preserve existing bytes or missing-key absence.
- Cross-client state visibility, atomic MGET snapshots, 640 shared counter
  increments, 360 APPEND writes, QUIT/churn/restart regressions.
- Linux Debug/Release 39/39 tests each; Clang 18 ASan+UBSan 40/40,
  50 immutable RESP2 seeds and 10,000 libFuzzer iterations at the audited
  Phase 4H main merge SHA `31914ae7127b432f2fb38f3f07ac66a4ae6b435b`.

### Phase 5 functionality (accepted)

- Injectable wall and monotonic clocks; optional wall-clock deadlines and TTL metadata.
- Lazy expiration and RESP2 EXPIRE, PEXPIRE, TTL, PTTL and PERSIST.
- Atomic SET EX/PX options (positive relative deadlines) and all-or-nothing timeout rejection.
- Deadline-ordered active expiration with a bounded 32-record event-loop maintenance turn.
- Cross-client correctness, binary keys, exact-clock fake-time tests and deterministic model-based acceptance.

### Not Yet Implemented

- Collection data types (Phase 6).
- Memory-bounded eviction (Phase 7).
- Append-only persistence (Phase 8).
- Snapshotting and recovery (Phase 9).
- Production observability, remote Linux deployment and demo closure.

## 7. Supported Commands

**Implemented and validated RESP2 command subset (Phases 4 and 5 accepted):**

- Connectivity: `PING`, `ECHO`, `QUIT`.
- Strings: `SET`, `GET`, `MSET`, `MGET`, `APPEND`, `STRLEN`.
- Integers: `INCR`, `DECR`, `INCRBY`, `DECRBY`.
- Keys: `DEL`, `EXISTS`, `TYPE` (string or none).
- Expiration: `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`; `SET` also accepts EX/PX.

Commands are case-insensitive; keys and string values are binary-safe.
All live clients share the same process-local in-memory database. MSET is
atomic and MGET preserves argument order, including repeated and missing keys.
Integer mutations use strict signed 64-bit parsing and reject overflow without
corrupting state. QUIT closes only the requesting connection after its reply.

**Current executable modes and options:** `--version`, `--help`,
`--smoke-echo`, `--resp2-server`, `--port PORT`, `--max-clients COUNT`.
Both server modes bind to loopback `127.0.0.1`; port 0 requests an ephemeral port.
`--smoke-echo` is raw TCP echo and does not use RESP2; `--resp2-server`
accepts the implemented RESP2 command subset.

**Intentional limits:** 8,176-byte stored string values and 8,192-byte
serialized single replies. MSET copies the keyspace before committing.
This is not a complete Redis server; eviction, disk persistence,
collection commands, conditional EXPIRE flags and advanced Redis SET
options remain future work.

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

### Phase 2G — 2026-10-22 (COMPLETE — FINAL STATUS RECORDED)

Initial baseline: `3406b14babd60406b226d109be82d10e36a8e8b0`.
Feature branch: `phase2g/resp2-parser-fuzz-foundation`.
Foundation: `ee87032d1ea590f32f8feb3567553ebb9170cd16`.
Linux target/CI: `b7edd7540ec3d46008ee7878fb55063c26334e9f`.
Feature docs: `c089890d1dda45bc7eef0591bfb21cab21ca0cf4`.
PR #10: MERGED as true two-parent commit
`36ec43d8ce247d50c81bf90d60641d4db9b802b8`.

Test-only parser fuzzing includes production parser/model sources,
a shared invariant checker, opt-in CMake test targets, 50 pinned
binary-safe RESP2 seeds, a standalone deterministic replay runner,
and a Linux Clang 18 libFuzzer ASan/UBSan executable.

Feature CI #39754741830: SUCCESS (19/19 + 10,000 fuzz runs).
Final PR-head CI #39767524677: Linux Debug and Release SUCCESS.
Final PR-head fuzz CI #39767524405: SUCCESS, 19/19 CTests,
50-seed integrity PASS, 10,000 bounded libFuzzer runs.
Post-merge main CI #39770284668: Linux Debug and Release
18/18 tests each and repeated network/protocol regressions PASS.
On macOS synchronized merge HEAD: Debug/Release 18/18 each,
UBSan 50-seed corpus replay PASS, worktree clean.

The macOS Apple Clang ASan startup hang remains documented;
Linux ASan+UBSan passed. The main-push fuzz workflow is not
configured to trigger automatically: verified fuzz evidence is
from the final PR head, whose tree matches the merge tree.
Production `redisdb-server` remains transport-only TCP echo;
Redis commands, database storage, persistence and a production
RESP2 serving path are NOT IMPLEMENTED.

Phase 2G.5A closure documentation was locally validated.
Phase 2G.5B documentation commit
`09f1fc77f14dfd3b4f12dfb17148b60bbfe0a94b` was pushed
on main; its Linux Debug/Release CI #39772960967 passed
18/18 CTests in each configuration and all existing repeat gates.
Phase 2G and the entire Phase 2 RESP2 Protocol Engine are
formally recorded as COMPLETE. The exact Phase 2G.5C final-status
commit `e7dfbbf5d64795e225c69e5813f75101d0c90067` passed
main Linux Debug/Release CI #39774474442 before Phase 3A began.

### Phase 3A — 2026-10-22 (COMPLETE; FINAL STATUS CI VERIFIED)

Baseline `e7dfbbf5d64795e225c69e5813f75101d0c90067`;
feature branch `phase3a/command-representation` (retained).
Design: `docs/PHASE3A_COMMAND_DESIGN.md`.
C++20 implementation: binary-safe owned `commands::Command` and
structural RESP2 request-to-command decoder; ASCII-only command-name
normalization; typed errors for invalid request structure;
isolated unit tests and CMake registration.

Feature implementation commit:
`6c402b8a929b695f9abd51949c5d3967f1ce95c9`.
CI-only discovery repair:
`9d64403925b62b108ee1172c06ad9b866592d509`.
PR #11: MERGED as true two-parent commit
`4b493ac7984b4c3bdf47a24d41b517930cd689b3`;
merged tree equals final reviewed feature tree.
PR-head Linux CI #39784944527: Debug and Release SUCCESS
(19/19 each, repeat gates). PR-head Clang 18 ASan+UBSan
fuzz CI #39784944465: 20/20, 50 seeds, 10,000 runs SUCCESS.
Post-merge Linux main CI #39786009239: Debug/Release
19/19 each, network and protocol repeats SUCCESS.
Local synchronized main: macOS Debug and Release 19/19 each;
clean worktree, feature branch preserved.

Phase 3A.2A five-document draft and 3A.2B documentation-only
commit `c369fdf3c223361bfd7a7966dc3228d16b8ec2b7` are
COMPLETE; exact-head Linux CI #39789969583 SUCCESS (Debug and
Release 19/19 each, repeated validation). Phase 3A.2C status
commit `bb9610e4422dac7fa383a3473abf2e67dbe1766b` passed
exact-head Linux Debug/Release CI #39792311541 (19/19 each).
The 3A completion gate was satisfied before Phase 3B started.
The production `redisdb-server` executable remains transport-only
TCP echo: no production RESP2 command-serving mode, executable
command routing, Redis command execution, keyspace or persistence.

### Phase 3B — 2026-10-22 (COMPLETE STATUS RECORDED; FINAL STATUS CI PENDING)

- Immutable Phase 3A baseline: `bb9610e4422dac7fa383a3473abf2e67dbe1766b`.
- Retained feature branch: `phase3b/command-dispatcher` at `e92aaf54cd00e090620be64ede21f32cde7e0549`.
- Design boundary: `docs/PHASE3B_DISPATCHER_DESIGN.md` (historical 3B.0B design checkpoint).
- Implementation: `CommandDispatcher.hpp`, `CommandDispatcher.cpp`, `command_dispatcher_test.cpp`, additive CMake integration; registry-owned handler routing with typed unknown outcome and owned `RespValue` reply, optional `CloseAfterReply` metadata. No executable command routing, argument arity or built-in commands.
- 3B.0A: verified immutable Phase 3A final CI and created guarded local feature branch.
- 3B.0B: design-only document accepted; SHA-256 after harmless Markdown whitespace repair `eaaf42179a7b77e92ca133fca3c01f92d9e68d0945136b5a096122bb70e859af`.
- 3B.1A and 3B.1B: isolated C++ implementation; macOS Debug/Release 20/20 each, repeated unit/protocol/TCP regressions, UBSan dispatcher test and immutable 50-seed RESP2 parser corpus PASS.
- 3B.1C: feature commit `e92aaf54cd00e090620be64ede21f32cde7e0549`; non-force push, exact five-file scope; staged-index/whitespace recovery completed safely.
- 3B.1D and 3B.1E: PR #12 drafted and then marked ready. Exact feature-head networking Linux CI #39805019741: Debug/Release 20/20 each; Clang 18 ASan+UBSan fuzz CI #39805019351: 21/21, 50 verified seeds, 10,000 libFuzzer runs.
- 3B.1F: PR #12 merged as true two-parent `84d95500545eb4c0a417cd95cc98c62e30384424`; merged tree matches feature head; branches preserved. Exact-merge-head Linux CI #39806894796: Debug/Release 20/20 each plus network/protocol repeat gates SUCCESS.
- 3B.1G: local `main` fast-forwarded to exact merge, clean and synchronized; macOS Debug/Release 20/20 each, 20 dispatcher repeats, 10 command repeats, 5 protocol repeats, 3 TCP smoke repeats per configuration. UBSan dispatcher test and 50-seed manifest/replay PASS.
- 3B.2A: five-document acceptance draft created and locally validated,
  without a commit or push.
- 3B.2B: five-document documentation-only commit
  `cc88f88948acd96574479750eefc7ede6445c70a` accepted by exact-head
  Linux CI #39810737185: 20/20 Debug and Release plus repeat gates.
- 3B.2C: final COMPLETE status recorded in five documents. The new
  status-commit SHA is determined at commit time; its own exact-head
  Linux CI must pass before Phase 3B closure is fully accepted.

**Strict scope boundary:** 3B provides an isolated command registry, not a Redis command-serving socket endpoint. Command-specific arity (3C), PING/ECHO/QUIT (3D), redis-cli smoke (3E), unknown-command wire semantics (3F), database storage and persistence remain NOT STARTED.

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

**Current handoff (2026-10-23):** Phases 0–4 are implemented and
accepted. Phase 4A–4H are already merged into `main` at the audited
Phase 4H checkpoint `31914ae7127b432f2fb38f3f07ac66a4ae6b435b`.
Phase 5 is NOT STARTED; **Phase 5A — Clock abstraction is NEXT**.

1. If the Phase 4 closure-documentation feature branch is not yet
   merged, verify its exact-head Linux Debug/Release and Clang 18
   sanitizer/fuzz CI, then merge directly into `main` without a PR.
2. Verify the final closure merge's two parents, remote `main` equality,
   clean working tree, and **post-merge** Linux validation at the exact
   new main SHA. Retain the feature branch and external logs.
3. Begin Phase 5A — Clock abstraction in a fresh feature branch from
   the verified clean `main`. Make clocks injectable and testable;
   do not implement expiration semantics ahead of 5B–5H.
4. Preserve the documented 8,176-byte stored-value cap, 8,192-byte
   single-reply cap and MSET full-map staging until explicitly redesigned.
   Do not silently expand Redis compatibility or claim TTL/persistence.

### Superseded Phase 3C continuation checkpoint (historical only)

The following archived checkpoint was correct when Phase 3C was active,
but is **not a current instruction** and does not describe the current
RESP2 database server:

Phase 3B formal closure CI #39811900890 passed.

Phase 3C feature `38c5354` merged directly as
`d733daa13cb9c1d8741c4c8d1e1080fe593d32b7`.

Post-merge acceptance:
- macOS Debug and Release: 21/21 each.
- macOS fuzz-enabled UBSan: 22/22.
- Linux CI #39828953490: Debug/Release 21/21 each.
- Clang 18 #39829737702: 22/22, 50 seeds,
  10,000 bounded libFuzzer executions.

1. Validate the consolidated documentation/CI branch
   with Linux CI and merge directly.
2. Start Phase 3D — PING/ECHO/QUIT.
3. Use feature branches, direct merges and no new PRs.
4. Preserve tests, historical branches and external logs.

The production server remains transport-only TCP echo.

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
