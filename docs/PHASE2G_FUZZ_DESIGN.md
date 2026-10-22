# Phase 2G.0C — RESP2 Parser Fuzzing Architecture and Acceptance Design

**Project:** Redis-Compatible In-Memory Database Server (C++20)
**Repository:** `sinthetaaa/redis-compatible-in-memory-database`
**Design checkpoint:** 2026-10-22; documentation-only design, not implementation acceptance
**Local feature branch:** `phase2g/resp2-parser-fuzz-foundation`
**Immutable Phase 2G starting baseline:** `3406b14babd60406b226d109be82d10e36a8e8b0`
**Current status:** Phase 2F formally closed; Phase 2G.0A and 2G.0B complete; Phase 2G.0C design record prepared.

## 1. Verified inputs and scope

The baseline has a C++20 `redisdb-core` library; `RespParser::parse(std::string_view)` returns `ParseResult` with `Complete`, `NeedMore`, or `Invalid`, an optional owned `RespValue`, a consumed-byte count, and a string error. The parser is stateless and accepts configurable byte/bulk/array/depth/metadata limits. `RespValue` supports binary bulk strings and nested arrays. Phase 2F tightened noncanonical bulk/array lengths, malformed bulk terminators, and parser/connection buffer compatibility. Existing unit tests cover grammar, all value types, resource limits, fragmentation, and round-trip behavior.

The default CMake Debug/Release presets each register 18 CTests on macOS; the existing Ubuntu 24.04 workflow builds Debug and Release and repeats protocol, TCP smoke, lifecycle, and epoll tests. It must remain operational without a fuzzing compiler. The production binary remains **transport-only TCP echo**; no Redis command execution, data storage, persistence, or public deployment is in scope.

The completed macOS Phase 2G.0A toolchain audit found: `/usr/bin/clang++` (Apple Clang 17) can compile/link ASan+UBSan, but **cannot link libFuzzer** because the fuzzer runtime archive is missing. Phase 2G.1A-R1 later showed that this host's ASan runtime hangs during initialization, before the corpus runner enters `main()`. No working local libFuzzer was demonstrated. LLVM documents the `LLVMFuzzerTestOneInput` entry point, `-fsanitize=fuzzer` and its runtime linking requirements. Ubuntu 24.04 provides Clang 18 and `libfuzzer-18-dev`; successful compilation and running in our GitHub workflow must be verified, not assumed.

### In-scope

- One coverage-guided libFuzzer target for the **actual production `RespParser` and `RespValue` implementation**, not a mock parser.
- A deterministic standalone corpus runner that uses the **same invariant-check function** without requiring libFuzzer, including on macOS.
- Bounded corpus generation, timeout/memory/input limits, ASan+UBSan checks, and reproducible failure artifacts.
- An **opt-in** CMake fuzz configuration and a separate Ubuntu 24.04 GitHub Actions fuzzing workflow.
- Full regression of the existing default Debug/Release builds and CI; no change to production behavior.

### Out-of-scope

- Fuzzing network sockets, `ConnectionManager`, `RespPipeline`, serializer, Redis commands, persistence or Docker in 2G.
- Changing accepted RESP2 syntax or `ParseResult` semantics merely to satisfy the harness.
- Unbounded fuzzing on shared CI, full compatibility assertions, coverage percentage guarantees, benchmark claims, or silent exception suppression.

## 2. Compilation architecture and build isolation

Proposed paths (subject to guarded implementation review):

| Path | Responsibility |
|---|---|
| `tests/fuzz/RespParserFuzzChecks.hpp` | Declare one `fuzz_one(std::string_view)` invariant-check function |
| `tests/fuzz/resp_parser_fuzz_checks.cpp` | Run production parser with fixed bounds and verify invariants |
| `tests/fuzz/resp_parser_fuzz_entry.cpp` | Tiny `extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t)` bridge; no `main` |
| `tests/fuzz/resp_parser_seed_smoke.cpp` | `main` for deterministic replay of every seed in a directory |
| `tests/fuzz/generate_resp2_corpus.py` | Standard-library-only deterministic seed generation, fixed names and manifest hashes |
| `cmake/FuzzTargets.cmake` | Isolate opt-in test targets and sanitizer/fuzzer flags |
| `.github/workflows/phase2g-fuzz.yml` | Ubuntu Clang libFuzzer+ASan+UBSan CI without replacing existing workflow |
| `docs/PHASE2G_FUZZ_DESIGN.md` | This design record in the feature branch |

CMake opt-in gate: `REDISDB_ENABLE_FUZZ_TARGETS=OFF` by default. Normal `cmake --preset debug/release` must continue to build and discover **exactly the existing 18 CTests**. Under the opt-in flag, compile separate parser-specific executables from `src/protocol/RespParser.cpp`, `src/protocol/RespValue.cpp`, and shared fuzz-check sources, with the repository include path. This intentionally recompiles the **real parser sources with instrumentation**, rather than linking an uninstrumented `redisdb-core` and losing meaningful sanitizer/coverage instrumentation.

- **Standalone seed runner:** compile/link the shared parser/check sources with `-fsanitize=address,undefined -fno-sanitize-recover=all` on Linux Clang. On the audited Apple Clang 17 host, use `-fsanitize=undefined -fno-sanitize-recover=all` until a functioning ASan runtime is independently demonstrated. The runner provides its own `main` and **does not use `-fsanitize=fuzzer`**.
- **Linux fuzz executable:** compile/link the same production parser/check sources plus the entry point using `clang++-18` with `-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`. libFuzzer supplies `main`. Gate this target behind Linux + verified Clang runtime/link availability, and produce an explanatory failure if missing.
- Use an isolated build directory such as `build/fuzz-sanitize` on macOS and `build/fuzz-linux` on Linux. This is ignored by the existing `/build/` rule.
- Keep these instrumentation flags off `redisdb-core`, `redisdb-server`, the existing CTest executables, and the two default presets. Do **not** globally alter `CMAKE_CXX_FLAGS`, replace the compiler for the existing build directories, or change existing workflow behavior.
- Make the seed runner a CTest target **only in the opt-in build**: `fuzz.resp_parser_corpus_smoke`, with finite test timeout. Run the true fuzz loop separately; never invoke an unbounded fuzzer through ordinary CTest.

Ubuntu 24.04 planned package probes: check `clang++-18`, `libclang-rt-18-dev`, `libfuzzer-18-dev`, compiler version and actual compile/link. Package names exist in Ubuntu Noble; install only inside the separate GitHub Actions job after implementation. No Homebrew LLVM installation is a Phase 2G prerequisite.

## 3. Harness contract and acceptance invariants

Entry point: `LLVMFuzzerTestOneInput(data, size)` converts arbitrary bytes into a **binary-safe** `std::string_view(data, size)`, with safe treatment of `(nullptr, 0)` where applicable, then calls the shared `fuzz_one`. Never interpret input as NUL-terminated text or normalize embedded `\0`, CR or LF.

**Bounds chosen for the foundation:** `max_input_len=4096` bytes. A single constant parser profile suffices initially:

| Parser limit | Foundation setting |
|---|---:|
| `max_frame_bytes` | 4096 |
| `max_bulk_bytes` | 2048 |
| `max_array_elements` | 32 |
| `max_nesting_depth` | 8 |
| `max_inline_bytes` | 256 |

These are **fuzz-harness test settings**, not changes to production defaults. Reject/skip input exceeding the harness's 4096-byte cap. This bounds parse time, allocations, and recursion while still probing invalid declared lengths and exact-budget framing. libFuzzer also receives `-max_len=4096`.

Every completed invocation must satisfy:

1. `Complete`: optional value is present; error string is empty; consumed bytes are **positive** and **no greater** than both actual input length and configured frame budget.
2. `NeedMore`: no value, `consumed_bytes == 0`, and empty error string.
3. `Invalid`: no value, `consumed_bytes == 0`, and nonempty error reason.
4. For `Complete`, reparsing the **exact consumed prefix** with the same parser must return `Complete`, consume that entire prefix, and produce an equal owned `RespValue`. This checks one-frame consumption when arbitrary bytes follow the first frame.
5. For `Complete`, sample up to eight deterministic proper prefix lengths (rather than all 4095 prefixes) and require `NeedMore`; this checks incremental framing semantics without quadratic test-work explosion. Include a zero-length prefix if input is nonempty.
6. The same parser instance should remain reusable for subsequent invocations; do not retain views into fuzz input or pass references to freed storage. All parser exceptions caused by arbitrary input should remain visible as failures unless an exception is **explicitly documented as an expected API outcome** (none are expected for fixed valid `Limits`).
7. Do not use `assert(...)` as the only invariant enforcement: optimized builds can disable it. Use a failure path that always terminates (e.g. `std::abort` or `__builtin_trap`) so libFuzzer records the counterexample.

Do not assume `parse` consumes the entire input on `Complete`: it is deliberately a **single-frame parser** and may leave a suffix. Also do not claim queued network replies are delivered—networking is outside the fuzz target.

## 4. Versioned, reproducible seed-corpus specification

Use `tests/fuzz/generate_resp2_corpus.py` with a deterministic source table mapping stable file names to explicit Python `bytes`. Generate seed files into ignored `build/.../corpus` (not the source tree), validate a deterministic SHA-256 manifest, and reject unexpected collisions. Never require network access or external Python dependencies.

**Required seed categories (minimum 25 distinct cases):**

- Complete RESP2 values: `+OK\r\n`, `+\r\n`, `-ERR\r\n`, `:0\r\n`, `:+12\r\n`, `:-42\r\n`, `$0\r\n\r\n`, `$-1\r\n`, `*0\r\n`, `*-1\r\n`.
- Scalar and structured: `$5\r\nhello\r\n`, `*2\r\n+OK\r\n:2\r\n`, nested `*1\r\n*1\r\n:7\r\n`, valid first frame followed by second frame.
- Binary-safe: bulk containing embedded `\x00`, `\r\n`, `\x80` and `\xff` with correct declared length.
- Incomplete prefixes: `b''`, `b'+'`, `b'+abc\r'`, `b'$3\r\nab'`, `b'$3\r\nabc\r'`, partial nested array.
- Malformed: unsupported marker, bare LF, mismatched bulk CRLF (`$3\r\nabcX`), malformed second CRLF byte, negative bulk lengths other than `-1`.
- Noncanonical lengths: `$01`, `*00`, `$-0`, `*-01`, `$+1` (all with framing appropriate to test the error), and oversized `*999999` or `$999999` declarations.
- Boundary-adjacent: a seed exactly at selected harness metadata/bulk budget and one just over it, with documented expected status.

The generator should print the stable seed count and manifest hash. The standalone runner must enumerate the directory in **stable sorted filename order**, run every seed, report count, and reject empty corpus directories (while allowing an intentionally empty-input seed file). Retain source seeds immutable: libFuzzer mutations should go to a **separate ignored output corpus**, seeded from a copied initial directory.

### Phase 2G.1A-R1 runtime diagnosis and platform policy (2026-10-22)

The first opt-in macOS binary compiled with `-fsanitize=address,undefined`, but
all no-arguments, missing-corpus and corpus invocations exceeded bounded
process timeouts. A macOS process sample identified its main thread inside
`__asan::InitializeShadowMemory()` before the runner's `main()` function.
Disabling leak detection did not change this behavior. An uninstrumented
control replayed all 18 initial seeds in approximately 0.62 seconds.
This is evidence of a local sanitizer startup failure, **not** proof of
parser correctness or proof that ASan will fail on Ubuntu.

For Phase 2G.1A-R2, a temporary UBSan-only build must successfully replay
the exact 18-seed corpus **before** the opt-in macOS CMake target is changed.
Apple Clang receives UBSan-only for this seed runner, while Ubuntu Clang must
retain ASan+UBSan and a separate libFuzzer target in Phase 2G.2. No reported
Linux sanitizer success is implied until the actual Linux CI run passes.

## 5. Bounded runtime, CI, crash artifacts and reproducibility

**Local macOS:** build the standalone runner with working UBSan-only instrumentation on this Apple Clang 17 host. ASan+UBSan currently hangs in sanitizer shadow-memory initialization and is **not an accepted local test result**. Run the exact seed corpus and (optionally) a limited deterministic mutation-driver smoke. Do not claim local coverage-guided libFuzzer is available without a successful local runtime test.

**Linux GitHub Actions:** create a new, separate workflow triggered on `pull_request` to `main` and `push` to `main` (the normal networking workflow remains unchanged). Job on Ubuntu 24.04, `timeout-minutes: 10` initially; install/check Clang 18/compiler-rt/libFuzzer; configure isolated opt-in CMake; compile the instrumented standalone/fuzzer targets; run corpus seed smoke; then run libFuzzer with a **fixed seed** and a finite iteration cap, e.g.:

```bash
./build/fuzz-linux/redisdb-resp-parser-fuzzer \
  -runs=10000 -seed=1337 -max_len=4096 \
  -timeout=5 -rss_limit_mb=1024 \
  -artifact_prefix="$RUNNER_TEMP/resp2-fuzz-artifacts/" \
  "$RUNNER_TEMP/resp2-fuzz-mutated-corpus"
```

Before launch, generate the versioned initial corpus in the build directory, copy it to a dedicated mutable corpus directory under `RUNNER_TEMP`, and create a writable artifacts directory. Use `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` where runtime support is demonstrated. The CI job must **fail** on abnormal exit, sanitizer report, harness assertion, compiler/runtime failure, zero seed count, or missing/undiscovered dedicated CTest. Allow artifacts on failure via `actions/upload-artifact@v4` containing the **locally generated fuzzing reproducer inputs and logs only**, subject to normal repository security review.

Use a fixed run count rather than time alone for a reproducible work budget. A successful 10,000-run job proves **only that bounded run**; it is not a guarantee that the parser is free of vulnerabilities. Repeat with additional seeds and prolonged separate campaigns in later engineering phases.

**Reproduction:** save the exact libFuzzer artifact file, compiler version, binary build configuration and exit reason. Replay with the same instrumented fuzzer binary and the artifact path; then minimize with standard libFuzzer tooling under a separate guarded task. A discovered parser crash is a **blocked acceptance** until fixed with a deterministic regression test. Do not suppress sanitizer failures or silently exclude crashing seeds.

## 6. Stage-by-stage execution plan

| Gate | Work | Acceptance evidence |
|---|---|---|
| 2G.0A | Toolchain/architecture audit | COMPLETE; ASan+UBSan compiles/links on Apple Clang but runtime initialization later proved blocked; Apple libFuzzer missing; repository clean |
| 2G.0B | Guarded local branch creation | COMPLETE; `phase2g/resp2-parser-fuzz-foundation` at exact `3406b14`; no push |
| 2G.0C | Design decision and acceptance contract | This document reviewed; design-only diff, no code, no commit before audit |
| 2G.1A | Shared invariant harness + standalone runner | Generated corpus replays deterministically; opt-in build; no default-target change |
| 2G.1B | Seed generator + manifest/reproducibility gates | At least 25 seeds, stable paths/hashes, corrupt/missing manifest detection |
| 2G.2 | Linux Clang libFuzzer executable + CI | Instrumented parser+value sources; clean Ubuntu compile, seed replay and bounded mutation success |
| 2G.3 | Stress and correctness audit | ASan/UBSan green; representative malformed cases; default macOS 18/18 Debug/Release unchanged; Linux existing workflow green |
| 2G.4 | Documentation, review and PR | Exact file scope, clean feature branch, draft PR, Linux CI both workflows |
| 2G.5 | Two-parent merge, post-merge validation, formal closure | PR merged without squash/rebase; `main` synchronized, CI validated, docs updated, clean repo |

Within each gate: guarded exact SHA and clean-tree preflight; no destructive Git commands; terminal-driven change script; formatting/checks; narrow commit and push only **after** evidence; preservation of Phase 2F branches and normal build. CI duration, seed count, compiler version and fuzz run count must be recorded as observed values, not fabricated outcomes.

## 7. Explicit remaining questions/gates before implementation

1. **Compile/link proof:** Ubuntu 24.04 Clang 18 package availability is sourced, but this repository's libFuzzer build/run has not yet occurred.
2. **Shared check behavior:** validate proper-prefix property against all generated valid seeds; if one is legitimately non-NeedMore, investigate semantics rather than weaken the assertion silently.
3. **Resource behavior:** confirm the chosen 4096/2048/32/8/256 limits are accepted by constructor and CI runtime is comfortably bounded.
4. **Implementation isolation:** inspect CMake target-source, include and link behavior; enforce the 18-test default CTest count and preserve the networking workflow unchanged.
5. **PR gate:** do not mark Phase 2G COMPLETE until Linux fuzz CI, existing Linux Debug/Release regression, macOS validation, merge and post-merge acceptance pass.

**Next exact engineering task:** prepare Phase 2G.1A guarded implementation of the shared parser invariant checks, an opt-in standalone seed runner and a minimal deterministic corpus, without a libFuzzer dependency on macOS. Linux libFuzzer integration follows as a distinct checkpoint.

## 8. Source references

- Repository at baseline `3406b14`: `CMakeLists.txt`, `CMakePresets.json`, `include/redisdb/protocol/RespParser.hpp`, `src/protocol/RespParser.cpp`, `src/protocol/RespValue.cpp`, `tests/unit/resp_parser_test.cpp`, `.github/workflows/phase1e-linux.yml`, `docs/PROJECT_STATE.md` and `docs/ROADMAP.md`.
- User-supplied Phase 2G.0A Terminal audit, 2026-10-22; Phase 2G.0B branch-creation Terminal log, 2026-10-22.
- LLVM libFuzzer documentation: https://llvm.org/docs/LibFuzzer.html
- Ubuntu 24.04 libFuzzer package record: https://packages.ubuntu.com/noble/amd64/libdevel/libfuzzer-18-dev
- Ubuntu 24.04 Clang runtime record: https://packages.ubuntu.com/libclang-rt-18-dev
