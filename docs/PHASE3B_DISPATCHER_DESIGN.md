# Phase 3B — Command Dispatcher: Design Boundary and Acceptance Matrix

**Checkpoint:** 3B.0B — DESIGN ONLY; implementation NOT STARTED\
**Date:** 2026-10-22\
**Feature branch:** `phase3b/command-dispatcher` (local only)\
**Immutable baseline (`main`):** `bb9610e4422dac7fa383a3473abf2e67dbe1766b`\
**Phase 3A final status Linux CI:** #39792311541 SUCCESS (Debug and Release, 19/19 each)\
**Scope:** command-name routing and owned handler outcomes only; no production dispatch integration.

## 1. Reviewed Existing Contracts

- `commands::Command` owns an uppercase-ASCII name and ordered, binary-safe `std::vector<std::string>` arguments. Its constructor is private; only structural `decode_command(const protocol::RespValue&)` creates validated values. Names may contain unusual nonempty bytes and must not be silently rejected during decoding.
- The decoder accepts a nonempty RESP2 array of bulk strings and returns `CommandDecodeResult { optional<Command>, CommandDecodeError }` with no side effects. It has no registry or RESP reply responsibilities.
- `protocol::RespValue` owns all seven RESP2 alternatives; a `RespValue` can represent a handler's *future* response. `protocol::RespSerializer` serializes RESP responses independently of socket lifecycle.
- Production server is **transport-only TCP echo**. Existing RESP pipeline reply factories are test seams, not a working command server. They must remain untouched during 3B.
- ADR-002 plans single-threaded command execution; the 3B test-only dispatcher remains independent of networking, event loops, connections, keyspace, and persistence.
- The approved Phase 3 roadmap separates **3B routing**, **3C argument validation**, **3D PING/ECHO/QUIT behavior**, **3E redis-cli smoke**, and **3F unknown-command wire semantics**.

## 2. Narrow Dispatcher Responsibilities

1. Build a deterministic registry from an explicit set of command-name-to-handler registrations. **No giant command switch, process-wide singleton, global mutable registry, implicit built-in commands, or network/server hookup**.
2. Route an already-decoded `const Command&` using `Command::name()`. Do not decode RESP requests again, transform arguments, or inspect socket state.
3. Invoke the matching handler **exactly once**. Unknown commands must produce a typed routing result with **no fabricated RESP error**; RESP error wording, compatibility and wire encoding belong to 3F/3E.
4. Transfer each handler's **owned** response value and optional future connection disposition as typed metadata; the dispatcher itself must not serialize, enqueue, write, close, or flush sockets.
5. Treat registry-construction defects (duplicate canonical names, invalid registration names, empty handler) as programmer/configuration errors, distinct from client-caused unknown-command outcomes. Never silently replace a registered handler.
6. Propagate handler exceptions; **never catch-and-retry** handler invocation or map handler exceptions to an unknown-command response. Existing pipeline exception/close policy must be preserved when a future integration is explicitly approved.

## 3. Proposed C++20 API (to be reviewed before implementation)

Files proposed **for the later 3B.1 implementation**, not created by this checkpoint:

- `include/redisdb/commands/CommandDispatcher.hpp`
- `src/commands/CommandDispatcher.cpp`
- `tests/unit/command_dispatcher_test.cpp`
- `CMakeLists.txt` registration of `unit.command_dispatcher` (normal CTest count expected to increase from 19 to **20**; fuzz-enabled discovery from 20 to **21**).

Conceptual types (exact spelling can be refined during 3B.1 review):

```cpp
namespace redisdb::commands {

enum class ConnectionDisposition { KeepOpen, CloseAfterReply };

struct HandlerResult {
    protocol::RespValue reply;  // owns reply, no serialization or I/O
    ConnectionDisposition disposition = ConnectionDisposition::KeepOpen;
};

using CommandHandler = std::function<HandlerResult(const Command&)>;

struct CommandRoute {
    std::string name;         // registry configuration, ASCII-normalized on build
    CommandHandler handler;  // supplied explicitly; no real built-ins in 3B
};

enum class DispatchError { None, UnknownCommand };

struct DispatchResult {
    std::optional<HandlerResult> handled;
    DispatchError error = DispatchError::None;
};

class CommandDispatcher final {
  public:
    explicit CommandDispatcher(std::vector<CommandRoute> routes);
    [[nodiscard]] DispatchResult dispatch(const Command& command) const;
  private:
    // Owned std::unordered_map<std::string, CommandHandler> registry;
    // populated exactly once in constructor, then no mutation API.
};

} // namespace redisdb::commands
```

This is **design sketch, not compilable implementation**. It deliberately avoids `std::expected` (C++23) and does not claim the CloseAfterReply lifecycle is implemented. When QUIT is added in 3D, a later server-integration milestone must interpret `CloseAfterReply` **only after queued bytes are flushed**; no early close in 3B.

## 4. Registry Name and Error Policy

- Canonicalize **ASCII** `a`–`z` to `A`–`Z` for registration keys, without locale or Unicode conversion; leave `Command::arguments()` completely unchanged.
- Registration name must be nonempty printable ASCII, with **no whitespace, control bytes, or non-ASCII bytes**. Printable punctuation is allowed to avoid unnecessarily limiting later command naming. A decoded client command with unusual bytes is still structurally valid but ordinarily resolves to `UnknownCommand`.
- Constructor rejects any duplicate after canonicalization (e.g., `ping` + `PING`), invalid name, or empty `std::function`. Use `std::invalid_argument` for misconfiguration; `std::bad_alloc` can propagate. Build the map privately during construction and expose **no runtime registration mutator**.
- An empty registry is valid and routes all commands as `UnknownCommand`. No built-ins are registered in 3B production because no production dispatcher instance is wired yet.
- `DispatchError::None` **iff** `handled.has_value()`; `UnknownCommand` **iff** `!handled.has_value()`. A matching handler's returned `RespValue` is an owned value; no implicit success response is invented.
- An unknown handler must not be invoked, registry state must be unchanged after lookups, and no user-controlled bytes are interpolated into wire errors in this phase.

## 5. Handler Semantics and Forward Compatibility

- Handler receives `const Command&`; the dispatcher does not copy or reorder the arguments, and does not borrow the returned value.
- Tests use **injected, side-effect-controlled stub handlers** that return owned RESP2 values; these are test fixtures, not live `PING`, `ECHO`, `QUIT`, `GET`, or other Redis implementations.
- Registration is one-time constructor configuration; `dispatch` is logically read-only. No claim of thread-safe handler state or parallel execution is made. Future mutable state must follow ADR-002 and later storage design.
- Phase 3C will define command-specific arity and argument-error behavior **outside the routing lookup**. The 3B registry must not infer arity or reject arbitrary binary arguments.
- Phase 3D will introduce real connectivity handlers and decide response/disposition behavior; Phase 3F will map unknown outcomes into documented RESP2 errors. Neither behavior is part of 3B.
- Return `ConnectionDisposition::CloseAfterReply` as inert metadata in stub tests only. A future integration must coordinate reply ordering, backpressure, flushing and close without losing pipelined replies.

## 6. 3B.1 Unit-Test Acceptance Matrix

1. Empty registry returns exactly `UnknownCommand` and no reply.
2. A single registered stub routes a mixed-case `pInG` decoded by the 3A decoder to the canonical `PING` key exactly once (the name itself was normalized by 3A).
3. Multiple routes select only their matching handlers, never call unmatched handlers, and preserve deterministic results regardless of registration order.
4. Dispatcher passes original argument strings **byte-for-byte**, including empty argument, embedded `\0`, CR/LF and bytes `0x80`/`0xff`, preserving order and duplicates.
5. Returned RESP2 value is fully owned and survives destruction of the original stub's temporary data; no serializer or socket operation is performed.
6. A stub can return `KeepOpen` or `CloseAfterReply` metadata; dispatcher forwards it without acting on it.
7. Unknown printable commands, non-ASCII/control-verb commands structurally decoded by 3A, and a known command against an empty registry all return typed `UnknownCommand`, without fabricated RESP2 error bytes.
8. Constructor rejects blank name, whitespace/control/non-ASCII registration names, and a `std::function` without a target.
9. Constructor rejects duplicate names both identically spelled and after ASCII canonicalization, never silently overwriting earlier handlers.
10. Handler throws deliberate test exception: exception propagates exactly once; dispatcher does not retry, mutate registry, or invent an unknown-command outcome.
11. Repeated independent dispatch calls do not mutate route selection; dispatcher makes no hidden copies of binary command argument buffers or hidden global state.
12. Existing `unit.command`, RESP2 parser/serializer/pipeline regressions and external TCP echo smoke remain unchanged.

**Negative-scope tests:** do *not* test live `redis-cli`, execute PING/ECHO/QUIT, enforce arity, implement unknown-command RESP message text, accept inline RESP, mutate keyspace, or close real sockets here.

## 7. Validation and Integration Gates

- `3B.0A` **COMPLETE**: pinned Phase 3A closure baseline, final Linux CI accepted, clean local dispatcher branch created; macOS Debug/Release 19/19; immutable 50-seed UBSan smoke passed.
- `3B.0B` **DESIGN ONLY (CURRENT)**: one new local untracked design file; no C++ changes or tracked-file modifications; review the registry API and acceptance matrix.
- `3B.1A` **NEXT AFTER DESIGN ACCEPTANCE**: implement the isolated dispatcher, test-only stub registrations, unit target and CMake wiring. No production networking/server integration.
- `3B.1B`: format and compiler warnings; Debug/Release full CTest expected 20 tests, dispatcher repetition tests, protocol regressions; fuzz-enabled discovery check must tolerate 21 total with required named smoke/command tests.
- `3B.1C+`: guarded commit/push on feature branch, draft PR, PR-head Linux Debug/Release and ASan+UBSan/libFuzzer (if workflow paths trigger), review, controlled merge, post-merge CI, and separate documentation closure.
- Complete **Phase 3B only** after its acceptance/closure checkpoints. Leave 3C–3F, storage, durability and production RESP2 command serving unstarted until their explicit milestones.

## 8. Design Trade-offs and Risks

- An immutable constructor-built `std::unordered_map` provides modular routing and prevents accidental mutation mid-dispatch. Hash iteration order is irrelevant: only exact key lookup is observable.
- `std::function` supports injected stub handlers and future module registration, at the cost of indirect invocation/possible allocation; correctness comes before micro-optimization (ADR-007).
- Typed `UnknownCommand` in 3B keeps routing separate from Redis error-string compatibility in 3F. Invalid RESP request *shape* remains a **3A decode error**, not unknown dispatch.
- Carrying inert `CloseAfterReply` metadata avoids prematurely coupling dispatcher code to sockets and makes a later QUIT contract possible. The actual flush-and-close mechanism requires explicit later design.
- No duplicate-handler override avoids changing command meaning based on registration order. Strict canonicalization avoids locale-dependent behavior.
- Future C++ code should not introduce global state, `std::string_view` ownership bugs, thread-safety promises, or unbounded handler retries.

**Design acceptance status:** Proposed for review; no implementation, commit, push, remote feature branch, or PR is authorized by this document alone.
