# Phase 3C — Argument Validation: Design Boundary and Acceptance Matrix

**Checkpoint:** 3C.0B — DESIGN ONLY; no validator implementation started.
**Date:** 2026-10-22
**Local branch:** `phase3c/argument-validation` (not pushed)
**Immutable Phase 3B closure baseline:** `73fd219fee136a4993f2f8312a50ef036e3ea229`
**Phase 3B exact-closure Linux CI:** `39811900890`, SUCCESS (20/20 Debug, 20/20 Release).
**Scope:** validate the count of ordered, already-decoded command arguments with caller-supplied policy; no command-specific execution, transport, RESP2 wire errors, storage, or persistence.

## 1. Contracts Reviewed Before Design

- `include/redisdb/commands/Command.hpp`: `Command::name()` is a nonempty ASCII-uppercase owned string, and `Command::arguments()` is an owned, ordered `const std::vector<std::string>&`. Arguments can include zero bytes, non-UTF-8 bytes, empty strings and duplicate strings. `decode_command` only checks RESP2 array shape, not command arity.
- `include/redisdb/commands/CommandDispatcher.hpp`: an explicit constructor-built registry maps canonical command names to injected `CommandHandler` callbacks. `dispatch(const Command&)` returns either an owned handler result or typed `UnknownCommand`; it does not know argument counts or produce Redis wire errors. Handler exceptions propagate.
- `docs/ROADMAP.md`: 3C argument validation is separate from 3D PING/ECHO/QUIT implementation, 3E redis-cli interoperability testing, and 3F unknown-command wire behavior. The Phase 3 acceptance gate remains unmet.
- The production `redisdb-server` executable is **transport-only TCP echo**. Existing pipeline response factories are tests, not a production Redis request path.
- ADR-002's future single-threaded execution plan is unchanged. Phase 3C must not claim shared-state thread safety.

## 2. Decision: Policy-Driven, Count-Only Validator

Implement a pure, deterministic **count-only** validation function in the command-library layer, supplied an explicit immutable arity rule by its caller. Do **not** add a second command-name registry or change the Phase 3B dispatcher. Do not perform a lookup by `Command::name()`: a future handler may supply its own arity rule after routing. There are no built-in command policies installed in Phase 3C.

The validator reads only `command.arguments().size()`. It must not parse, inspect, copy, mutate, canonicalize or reinterpret argument bytes; it must not consume wire input, serialize a reply, invoke a handler, enqueue output, or alter `ConnectionDisposition`. It must neither change the number of arguments nor conflate malformed RESP2 with an arity mismatch.

### Proposed C++20 interface (conceptual; to approve at 3C.1A)

```cpp
namespace redisdb::commands {

struct ArityRule {
    std::size_t minimum_arguments = 0;
    std::optional<std::size_t> maximum_arguments; // nullopt = unbounded
    std::size_t group_size = 1; // count above minimum must be a multiple of this
};

enum class ArityValidationError {
    None,
    TooFewArguments,
    TooManyArguments,
    InvalidArgumentGroup,
};

struct ArityValidationResult {
    ArityValidationError error = ArityValidationError::None;
    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] ArityValidationResult validate_arity(
    const Command& command, const ArityRule& rule);

} // namespace redisdb::commands
```

This is a design sketch, not yet compiled or installed. Exact names and helper ergonomics may be refined in a separately accepted implementation checkpoint. Use `std::size_t` for counts and an `std::optional<std::size_t>` upper bound; C++20 only (no `std::expected`). The return result owns no borrowed argument memory or mutable state.

## 3. Rule Semantics, Examples and Error Precedence

Let `n = command.arguments().size()`, `min = minimum_arguments`, `max = maximum_arguments`, `step = group_size`.

1. Validate **rule configuration first**. `step == 0` or a provided `max < min` is a programmer/configuration defect: throw `std::invalid_argument`. These are **not** client arity results. No overflow-prone arithmetic is needed to validate the rule.
2. If `n < min`, return `TooFewArguments`.
3. If `max` exists and `n > max`, return `TooManyArguments`.
4. If `(n - min) % step != 0`, return `InvalidArgumentGroup`. Subtraction is safe because step 2 ensured `n >= min` and step 1 ensured `step > 0`.
5. Otherwise return `None`. `valid()` is true exactly when `error == None`.

The order is deliberate: out-of-range errors take precedence over group alignment, yielding stable typed results. `group_size` imposes alignment relative to the minimum and does not impose a count of tokens or inspect values. Bounded rules can have valid counts separated by a stride; the policy need not include every possible count in the bound.

| Rule kind | Policy example (min, max, group) | Accepted counts | Scope note |
|---|---|---|---|
| Exactly zero | `(0, 0, 1)` | `0` | Future QUIT illustration only |
| Exactly one | `(1, 1, 1)` | `1` | Future ECHO illustration only |
| Zero or one | `(0, 1, 1)` | `0, 1` | Future PING illustration only |
| At least two | `(2, nullopt, 1)` | `2, 3, 4, ...` | Count only; no options grammar |
| Two to four | `(2, 4, 1)` | `2, 3, 4` | Inclusive maximum |
| Nonzero argument pairs | `(2, nullopt, 2)` | `2, 4, 6, ...` | Future MSET-like illustration only |
| Fixed three, grouped | `(3, 3, 2)` | `3` | Group condition vacuous at min |

Examples show possible future handler policies; **no PING/ECHO/QUIT/MSET behavior or actual registry entry is created or promised in 3C**. Parsing integers, `SET` option grammar, UTF-8, duplicate-key meaning, error string formatting, Redis compatibility and storage semantics are deferred to the relevant subsequent phases.

## 4. Error Taxonomy and Layer Ownership

- `CommandDecodeError` (3A) remains exclusively responsible for structural RESP2-to-Command shape errors. Decoder behavior is not modified.
- `DispatchError::UnknownCommand` (3B) remains exclusively responsible for a missed registry lookup. Unknown commands do not accidentally receive a fabricated `TooFewArguments` result from the dispatcher.
- `ArityValidationError` (3C) describes a **known caller-selected** arity rule's count mismatch. A well-formed `Command` is never altered by a failure. No RESP2 error value is synthesized here.
- Invalid rule configuration throws `std::invalid_argument`. Handler exceptions and the Phase 3B typed dispatch result remain unchanged. This helper must not catch, retry or invoke handlers.
- Redis-compatible error formatting and network response sequencing belong to later milestones (3D–3F and explicit production integration); 3C must not assume a response gets sent.

## 5. Ownership, Complexity and Numeric Safety

- Inputs: borrowed `const Command&` and `const ArityRule&`; both only observed during the call. No retained views/pointers or heap allocation in ordinary validation.
- Time `O(1)` and additional storage `O(1)` relative to argument count: read vector size, compare bounds, perform one modulo. **Do not traverse argument strings**.
- Use `std::size_t` without casting counts down to signed types. Avoid `count + 1`, `min + step`, multiplying group sizes or casting extreme user-supplied numeric bounds. Test rules with `SIZE_MAX` without creating huge arrays.
- No global mutable registry, no locale transforms, no parsing of binary arguments, no state in repeated calls. Pure validation is safe to call on immutable data, but makes **no promises** about concurrent mutation of handlers or future executor state.

## 6. Implementation Scope Proposed for Later 3C.1

Allowed new files after explicit approval:

- `include/redisdb/commands/ArityValidator.hpp` (or equivalent reviewed public name)
- `src/commands/ArityValidator.cpp`
- `tests/unit/arity_validator_test.cpp`
- Strictly additive `CMakeLists.txt` changes to compile core source and register a single new `unit.arity_validator` CTest, normal discovery expected **21** tests, fuzz build **22** tests.

Existing files **must remain functionally untouched**: `Command.hpp/.cpp`, `CommandDispatcher.hpp/.cpp`, RESP2 protocol types/parser/serializer/framing/pipeline, TCP server, networking, persistence, CI workflow files and test seed corpus. If build dependencies reveal unavoidable changes, stop and review them as a separate scope decision.

No actual built-in commands or handlers, no command execution, no global arity table, no response text, and no `redis-cli` smoke tests should be introduced in this phase.

## 7. Mandatory Unit Acceptance Matrix

1. Exactly-zero: `n=0` passes; `n=1` returns TooMany. Exactly-one: `n=0` TooFew, `n=1` passes, `n=2` TooMany.
2. Inclusive 0–1 range: both endpoints pass; `n=2` TooMany.
3. Lower-bounded: `n=min-1` TooFew, `n=min` passes, `n>min` passes with group 1; test min 0.
4. Closed range: below/inside/above return correctly; min==max behaves exactly.
5. Grouped arguments with `min=2, step=2`: `n=0/1` TooFew, `n=2/4/6` valid, `n=3/5` InvalidArgumentGroup.
6. Grouped bounded: test invalid group within range and TooMany precedence above range, TooFew precedence below range.
7. Misconfiguration: `step=0` and `max<min` throw `std::invalid_argument`; test both on commands with differing argument counts; no client-result masquerade.
8. Extreme policies: `SIZE_MAX` as min/max, `SIZE_MAX` as group size, `(min=0,max=0)`, and `group=1`. No overflow, allocation explosion, signed conversion or undefined behavior.
9. Binary and empty arguments: include `""`, embedded NUL, CR/LF, `0x80`, `0xFF`, duplicate arguments; validation never reads/mutates/reorders them; compare original `Command` and argument memory where feasible.
10. ASCII-case canonicalization remains decoder-only: validator does not inspect name; use both registered and unrelated verb names in fixtures, including unusual verb bytes decoded by 3A.
11. Determinism/reentrancy: repeated calls with same immutable input and rule produce same result; no shared state, handler calls or side effects.
12. Error independence: dispatcher continues producing `UnknownCommand` for unmatched names; protocol structural errors remain 3A; no fabricated RESP2 wire error.
13. Regression: all existing 20 normal CTests continue to pass on macOS Debug and Release; bounded repeats for `unit.command`, `unit.command_dispatcher`, `unit.arity_validator` and pipeline smoke; sanitizer corpus smoke unchanged.
14. Cross-platform: Linux Debug and Release complete 21 CTests each after PR trigger, Clang 18 ASan/UBSan fuzz build discovers 22; current Linux fuzz workflow has a minimum-discovery guard and must be evaluated before CI—no silent workflow edits.

## 8. Sequenced Milestones and Release Gates

- **3C.0A (COMPLETE):** pinned accepted Phase 3B closure, repeated baseline tests and created clean **local-only** feature branch `phase3c/argument-validation` at `73fd219fee136a4993f2f8312a50ef036e3ea229`.
- **3C.0B (CURRENT):** create and review only this design document with exact code-layer boundaries, numeric rules, error precedence and negative-case matrix. No implementation, staging, commits, push or PR.
- **3C.1A:** after design acceptance, implement isolated validator and unit test with narrow CMake additions (design/API subject to final review).
- **3C.1B:** formatting, complete Debug/Release regression, repeats, fuzz-enabled sanitizer smoke and exact changed-file audit.
- **3C.1C:** guarded feature commit and non-force push, no `main` change.
- **3C.1D–1E:** draft PR, exact-feature-head Linux Debug/Release CI and Clang 18 ASan/UBSan corpus/fuzz validation; verify 50 immutable corpus seeds and 10,000 bounded libFuzzer executions; resolve any failures with separately verified corrections.
- **3C.1F–1G:** controlled PR merge, exact-merge Linux CI, local `main` synchronization and macOS regression acceptance.
- **3C.2A–2C:** five-document acceptance and formal closure with exact-head Linux CI before Phase 3D starts. Detailed numbering may be refined while preserving these gates.

**Current acceptance:** design-only artifact, no implementation or execution; next step after this document passes its guarded scope check is an explicit design review before 3C.1A.
