# RedisCompatibleDB — Phase 6G.0B Frozen Differential-Testing Contract

**Decision:** ACCEPTED — reference Redis version and core comparison semantics frozen on 2026-10-24. **Implementation remains NOT STARTED.** The Linux OCI artifact digest and execution adapter must be verified before Linux differential CI can be accepted; they are implementation gates, not already-proven facts. This contract is an external reviewed artifact: it has **not** been added to the repository.

**Frozen project baseline:** `main` = `00c6f46098c16dfddbe1cad547a8ac43cefca115`, tree = `f7f1ea4923f17c3bb1ed9033744d7da4ca44c035`. Phases 0–5 and 6A–6F formally complete. Phase 6G baseline/contract research only; no Phase 6G source, test, commit, or branch changes.

## 1. Evidence and immutable reference identity

**1A — Project evidence.** The read-only Phase 6G.0A evidence archive has SHA-256 `b7d8c825876f12c65ce43e87b54432d851694f198e791189a7ce3c4abb7e1f9f`. It confirms exact `main`, feature-history references, five accepted documents and five source SHA pins, clean working tree/index, and CTest inventories of Debug **67**, Release **67**, fuzz-enabled **68** (inventory only; they were not rerun). Exact final-main CI on this SHA passed: networking Linux run `40072439651` (Debug + Release) and Clang 18 ASan/UBSan/libFuzzer run `40072439616`. Independently verified again through GitHub in this review.

**1B — Reference Redis evidence.** User-provided Phase 6G.0B.1 fingerprint archive SHA-256:
`b6c1017429fd352e6bbb5dc9da9bf95032098f3c907b4628492c03a076ce19b8`.
The archive was inspected, not merely accepted from the Terminal output. It contains the text version outputs, machine and Homebrew information, `fingerprint.json`, and an empty observational port-6379 file.

| Item | Exact verified value |
|---|---|
| Reference Redis version | **8.10.2**, `Redis server v=8.10.2 sha=00000000:1 malloc=libc bits=64 build=764b3e45065a66fb` |
| Reference CLI version | `redis-cli 8.10.2` |
| Homebrew formula revision | `redis 8.10.2_1` |
| Native execution platform | macOS ARM64 (`uname` Darwin 25.5.0; Python platform `macOS-26.5.2-arm64-arm-64bit-Mach-O`) |
| Server command | `/opt/homebrew/bin/redis-server` |
| Resolved server path | `/opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server` |
| Server binary SHA-256 | `8640ea31c7ece0c823fdf24ea53b8d74fda99d2a129dd69b3cd043cbf6c08176` |
| Server binary size | 2,366,752 bytes |
| CLI command | `/opt/homebrew/bin/redis-cli` |
| Resolved CLI path | `/opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-cli` |
| CLI binary SHA-256 | `593152500bc7cc529889b981d04b435d6173154bffe3a83a884f4f9d7aad1a98` |
| CLI binary size | 460,672 bytes |
| Default port 6379 observation | No listener was observed at check time; **not** an enduring guarantee, and harness never uses 6379 |
| Fingerprint script side effects | No Redis process launch, no Redis TCP connection, no Git mutation |

At local harness startup, rehash the **resolved executable bytes** and assert both the stored SHA-256 and `redis-server --version` exactly identify the approved binary. A Homebrew update invalidates the existing binary pin, even if its reported version remains 8.10.2; require explicit reviewed repinning rather than silently proceeding. `redis-cli` is diagnostic tooling only, **not** the response oracle; the harness uses direct RESP2 TCP framing.

**1C — Linux reference version and supply chain.** Set `REFERENCE_REDIS_VERSION=8.10.2` for both macOS and Linux. The official `redis:8.10.2` image exists (also tagged `redis:8.10.2-trixie`). For CI, use a **dedicated disposable Redis container**, or an equivalently verified native 8.10.2 binary, never the distro-default unversioned `apt install redis-server` as proof of 8.10.2. Resolve and review the **complete OCI manifest digest for `linux/amd64`**, record the immutable `redis@sha256:<approved-digest>` reference in the implementation and CI configuration, then fail closed if its digest, platform, or in-container `redis-server --version` differs. Avoid floating tags (`latest`, `8`, `8.10`), and never invent a digest from a shortened UI prefix. The macOS executable hash **must not** be reused as a Linux binary hash. Before the first accepted CI run, preserve the actual pulled manifest digest, platform, container image ID, and runtime version in test artifacts. If Docker is unavailable on a runner, the task fails as `HARNESS_INFRA_FAILURE` until a separately reviewed pinned native 8.10.2 provisioning route is supplied; do not silently substitute a different Redis version.

**Provenance boundary:** the fingerprint archive proves the native macOS installed binaries and versions, not a running server, reference-command parity, Docker availability, a specific Linux digest, or successful Linux differential tests. Those claims require new tests.

## 2. Exact command eligibility: 40 registered names, not complete Redis command support

| Family | Count | Registered commands |
|---|---:|---|
| Connectivity | 3 | `PING`, `ECHO`, `QUIT` |
| String/integer | 10 | `SET`, `GET`, `MSET`, `MGET`, `APPEND`, `STRLEN`, `INCR`, `DECR`, `INCRBY`, `DECRBY` |
| Key/TTL | 8 | `DEL`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST` |
| List | 6 | `LPUSH`, `RPUSH`, `LLEN`, `LPOP`, `RPOP`, `LRANGE` |
| Hash | 7 | `HSET`, `HGET`, `HEXISTS`, `HLEN`, `HDEL`, `HMGET`, `HGETALL` |
| Set | 6 | `SADD`, `SREM`, `SISMEMBER`, `SCARD`, `SMEMBERS`, `SMISMEMBER` |
| **Total** | **40** | No other command routes are claimed |

The implementation source checked at the frozen SHA is `src/commands/{Connectivity,String,Key,List,Hash,Set}Commands.cpp` plus `src/server/RespCommandServer.cpp` and existing real-TCP integration tests. The adjacent `phase6g_0b_command_matrix.csv` is the machine-readable route matrix and must match all 40 exactly.

**Eligible shared forms:** `PING`, `ECHO`, and `QUIT` at supported arities (`QUIT` in its dedicated connection-close suite); `SET key value` and exactly one `SET key value EX seconds` or `PX milliseconds` option; all other routes only with arity, numeric range, and syntax implemented by both servers. Include duplicate arguments/fields/members, case-insensitive ASCII command spelling, binary keys and values (`\x00`, `\xff`, CRLF, empty bytes), cross-client visibility and pipelining. Bound generated requests and responses **strictly below** custom transport/response limits; default normal-case target <= 4,096 serialized reply bytes, and compute exact worst-case RESP2 framing for variable-size reads. Data stores must be per-test isolated; no reuse of user keys.

**Excluded from the strict parity denominator:** Redis-only commands (`HELLO`, `SELECT`, `FLUSHDB`, `INFO`, `CONFIG`, `MULTI/EXEC`, `AUTH`, `SCAN`, etc.); `SET` options `NX`, `XX`, `GET`, `KEEPTTL`, `EXAT`, `PXAT`; `EXPIRE/PEXPIRE NX|XX|GT|LT`; count-form `LPOP/RPOP`; oversized output/input triggering custom 8,192-byte reply or 8,176-byte per-element/value bound; protocol-parser fuzz/malformed noncanonical requests; integer-text edge forms not proven equivalent; concurrency races with no deterministic ordering. These may be separately described as **negative compatibility cases** with expected divergence, not silently counted as successes.

**Errors:** within an eligible case, compare RESP Error category (especially `WRONGTYPE`) and subsequent unchanged state. Do **not** normalize away arbitrary error text or unexpected type differences. Non-WRONGTYPE error messages require a per-command approved exact-equivalence rule before inclusion. Any unexplained divergence in an eligible sample is `PARITY_MISMATCH`, never an automatic exclusion.

## 3. Typed, binary-safe RESP2 oracle

- Send **identical raw RESP2 arrays of binary bulk strings** to two separately controlled endpoints; each command within an ordered trace is sent in identical order. The reference Redis TCP connection uses RESP2 (default) and no `HELLO` is sent to the custom server.
- Implement a bounded incremental decoder for Simple String, Error, Integer, Bulk String, Null Bulk, Array and Null Array. Preserve exact byte strings and RESP wire type; distinguish `b''` from null, `+OK` from `$2\r\nOK`, and ordered arrays from unordered container semantics. Reject invalid lengths, framing, truncation, unexpected EOF, extra replies, and resource-limit violations. Never UTF-8-decode user payloads for comparison.
- **Strict by default:** array length, position, scalar type and byte content must match. `MGET`, `HMGET`, `SMISMEMBER`, `LRANGE`, multi-command pipeline responses, and error/recovery boundaries remain positional.
- **Only two order-independent commands:** for `SMEMBERS`, enforce arrays of unique binary bulk elements then compare *sets of bytes*; for `HGETALL`, enforce even-length flat arrays with unique bulk-string fields and bulk-string values then compare exact field-to-value maps. Reject repeated elements/fields rather than masking bugs with conversion to sets/maps. All other arrays remain ordered.
- After mutations, use only shared read commands (`EXISTS`, `TYPE`, `GET`, `MGET`, `LLEN`, `LRANGE`, `HLEN`, `HMGET`, `HGETALL`, `SCARD`, `SMISMEMBER`) to compare observable state. Reads are bounded to safe response sizes. Errors must not corrupt state; validate pipeline recovery.

## 4. Time, expiry, and clock ambiguity

- Main deterministic parity streams use keys without expiry or with deliberately long TTLs that do not approach the deadline during the trace. Positive `TTL` and `PTTL` values from **different server clocks cannot be required to be identical**.
- Compare `TTL`/`PTTL` states `-2` (missing) and `-1` (no expiry) exactly. For positive results, assert version-appropriate mathematical bounds and monotonic nonincrease when the key is left untouched; use client-side monotonic timestamps and explicitly recorded tolerance windows. `TTL` seconds rounding differs from `PTTL` milliseconds; no exact same-millisecond assertion.
- Compare zero and negative `EXPIRE`/`PEXPIRE` semantics and later key absence, with supported syntax only. Natural expiry gets a distinct bounded-time eventual-invisibility track, without comparing internal physical deletion timing or the Phase 6F accounting ledger.
- If a probe straddles a deadline, label `TIMING_AMBIGUOUS` and replay in a stable window; it cannot silently pass. Repeated ambiguity is an acceptance blocker.

## 5. Process and network ownership

**macOS:** run the currently SHA-pinned native Redis 8.10.2 executable as an owned child. Start the custom server as another owned child with `redisdb-server --resp2-server --port 0 --max-clients 16` and parse its exact `RESP_READY 127.0.0.1:<port>` marker. For native Redis, note **`redis-server --port 0` disables TCP and is not an ephemeral port request**: reserve/probe an available nonzero local port, release the probing socket and retry on bind races. Use loopback-only bind, protected mode, `--daemonize no`, persistence disabled (`--save ''` and `--appendonly no`), and a harness-owned temporary working directory. Never connect to or stop another Redis instance.

**Linux CI:** approved image reference must be immutable (digest pinned) and Redis 8.10.2 at `linux/amd64`. Own a fresh disposable container by ID, publish **only** its Redis TCP port on host `127.0.0.1` using a Docker-assigned nonzero host port, and assert the matched container ID/port before connecting. Docker-container processes are managed by the Docker daemon, not ordinary direct descendant PIDs; **container-ID ownership and cleanup replace the native-PID rule** in this adapter. The custom C++ process remains harness-owned and loopback-bound. Docker availability is a preflight check, not an assumed property of macOS or every runner.

Both adapters implement a single teardown contract: bounded socket/process/container waits, cleanup in `finally` and on signals, stop **only** owned PIDs/container IDs, collect bounded diagnostics, and delete **only** harness-created temp directories. Never use `FLUSHALL`, `SHUTDOWN`, `CONFIG`, `killall`, `pkill`, wildcard Docker deletion, or administrative commands against persistent services. Do not mutate port 6379 or Homebrew configs. Confirm teardown by checking owned PID/container exits. Fail closed on unexpected active endpoints.

## 6. Differential suites and required diagnostics

1. **Lifecycle + RESP2 decoder self-tests.** Start two controlled endpoints; validate successful health/handshake, invalid startup, port conflicts, reference version/digest mismatch, malformed/short RESP replies, timeout, and guaranteed owned-resource cleanup. Must not touch unrelated processes.
2. **Curated command-family parity.** Valid-type normal behavior, missing keys, `DEL` unique-deletion counting, `EXISTS` repeated-key counting, wrong-type categories, `MGET` null on non-string, atomic type replacement via `SET`/`MSET`, list order, hash duplicate-field updates, set duplicate-member counting, last-element/field removal, embedded binary bytes and two-client consistency.
3. **Dedicated TTL invariants.** Immediate expiry, `SET EX/PX`, `PERSIST`, `TTL/PTTL` state codes, eventually invisible expired keys, time windows recorded.
4. **Deterministic fixed-seed mixed traces.** Explicit generator version, a small documented seed set, bounded scenario budget, 2+ clients per server (sequential interleaving to avoid unspecified races), and identical request streams across endpoints; later larger randomized campaigns require separate resource budget approval.
5. **Standalone replay.** Any mismatch must save an immutable, binary-safe artifact containing the exact raw command stream and original seed, case index, reference version and binary/digest, target Git SHA, platform, port mapping, raw + parsed replies, state snapshots/probes, timestamps, classification, and subprocess diagnostics. Reproduction cannot depend on the generator's current version. Minimization is optional and must never destroy the original.

Classification values: `PARITY_PASS`, `PARITY_MISMATCH`, `EXPECTED_EXCLUSION`, `REFERENCE_OR_PROTOCOL_FAILURE`, `HARNESS_INFRA_FAILURE`, `TIMING_AMBIGUOUS`. Report denominator and counts for every class; exclusions never count as passed parity. Unexpected failures and repeated infrastructure/timing issues block acceptance. A harness that records a mismatch but continues to exit successfully is not acceptable. Include both a human-readable report and machine-readable JSON summary.

## 7. Quantitative acceptance (proposed minimum and conservative gates)

These are minimum engineering gates for the **new** 6G harness, not claims of completed tests:

- **All 40 command names** must have at least one explicit eligible-case or justified exclusion/negative-suite entry in the version-pinned coverage matrix; valid normal-case parity is required for the implemented shared form of every route. `QUIT` is tested as a termination/response protocol interaction, not reused mid-trace.
- **Deterministic curated cases:** 100% pass in eligible cases on both macOS native Redis and the verified Linux reference; no parity mismatch, incomplete test, or parser failure.
- **Mixed traces:** at least **10 fixed seeds × 100 commands** on each platform, constrained to eligible cases and with reproducible request byte streams. These counts are a planned minimum and may be increased after observing costs; never silently reduce the minimum on failure.
- **Stability:** repeat the deterministic curated suite and the same fixed seeds twice on each supported platform; compare outcomes and diagnostics. No unexplained flakiness, output-order false positives, leaked owned process/container, or unexpected Git mutation.
- **Failure path:** demonstrate a deliberately injected oracle mismatch and an independent harness startup/timeout failure both produce nonzero exit, sufficient replay/diagnostic evidence, and reliable cleanup.
- **Existing tests after changes:** rerun full macOS Debug and Release (>= 67 each) and fuzz-enabled (>= 68) CTests; their result must be all green, and any added tests must be discovered. Preserve existing Linux networking Debug/Release and Clang 18 ASan/UBSan/libFuzzer checks. Do not claim historical test totals were rerun.
- **Git acceptance:** independent review of exact changed-file set and hashes, normal nonforced feature push, successful exact-feature-head Linux CI, separately guarded two-parent non-fast-forward main merge, exact-new-main Linux CI, and separately verified final documentation/status closure before checking off Phase 6G. Retain branches and do not rewrite prior history.

## 8. Planned development boundaries

1. **Phase 6G.1A (provisional checkpoint):** read-only targeted harness design and owned-process lifecycle skeleton, bounded RESP2 wire codec/decoder tests, *no* bulk randomized parity yet.
2. **6G.1B / 6G.2 (provisional):** reference-image digest proof and safe Linux adapter; curated command-family differential cases, exclusions and TTL suite.
3. **6G.3 (provisional):** fixed-seed generation, binary replay, failure-injection and acceptance campaigns.
4. **6G.4 (provisional):** reproducible GitHub Linux CI; source review, feature/main acceptance and final documentation sync.

These checkpoint labels are planning names, **not** assertion that official ROADMAP IDs already exist. Preserve the approved roadmap's Phase 6G and Phase 13C distinct scopes. The tests may reveal genuine implementation discrepancies: any production fix requires its own explicit review and guarded commit; do not sneak in new command support, a Redis protocol expansion, memory limits, eviction, persistence, networking exposure, or changes to accepted Phase 6F ledger behavior.

## 9. Evidence closure and decision

**Phase 6G.0A baseline:** ACCEPTED, read-only.

**Phase 6G.0B.1 macOS reference fingerprint:** ACCEPTED, archive-verified, SHA-pinned Redis 8.10.2 and CLI 8.10.2.

**Phase 6G.0B comparison design:** ACCEPTED and version-frozen to Redis **8.10.2**. The official Redis Linux image availability is externally verified, **but an immutable Linux image manifest digest has not yet been captured by an execution environment**. That missing runtime proof is a mandatory **Phase 6G.1/CI infrastructure gate**, not a claim that the version or comparison semantics remain undecided. Do not mark Linux provisioning as tested until an actual digest-pinned pull and runtime version verification succeed.

**Next exact action:** guarded local-only Phase 6G.1A harness foundation design/implementation on a fresh feature branch **after** recording the proposed changed-file scope. First demonstrate owned native processes and typed RESP2 oracle on a limited deterministic smoke trace; no main merge, no CI changes, and no mass random test campaign in that first step.

**Repository at contract signing:** clean local `main` reported by Mac audit; remote `main` verified at `00c6f46098c16dfddbe1cad547a8ac43cefca115`. No Phase 6G test, source, commit, push, or merge has occurred as a result of this contract.
