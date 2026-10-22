# Phase 3A — Command Representation: Proposed Design Boundary

**Checkpoint:** Phase 3A.0B, design only (no implementation)
**Date:** 2026-10-22
**Branch:** `phase3a/command-representation` (local only)
**Immutable Phase 2 baseline:** `e7dfbbf5d64795e225c69e5813f75101d0c90067`
**Precondition:** Phase 2G.5C final Linux Debug/Release CI #39774474442 SUCCESS.

## 1. Existing verified interfaces

- `protocol::RespParser::parse(std::string_view)` returns one `ParseResult` with a fully owned `RespValue` for `Complete`, its exact `consumed_bytes`, and no input consumption for `NeedMore` or `Invalid`.
- `protocol::RespValue` is a seven-alternative owned `std::variant`: `SimpleString`, `Error`, `Integer`, `BulkString`, `NullBulkString`, `Array`, and `NullArray`; `BulkString::bytes` is an owned binary-safe `std::string`.
- `protocol::RespFramedInput` peeks or consumes within the existing connection input buffer. It performs no command interpretation.
- `protocol::RespPipeline::drain` invokes `PipelineReplyFactory` with `const RespValue&`, reserves output capacity before invoking the callback, serializes one reply per completed frame, and consumes the request only after queuing its reply. An exception during callback/reply processing requires caller-level close policy; do not retry side effects.
- `protocol::RespSerializer` owns only RESP response encoding. The production executable remains `--smoke-echo` transport-only and must not be changed by Phase 3A.

## 2. Request-to-command boundary (Phase 3A)

```text
RESP2 wire bytes -> RespParser / RespPipeline -> owned RespValue
   -> Command decoder (3A: structural conversion)
   -> owned Command{name, arguments}
   -> CommandDispatcher (3B: not implemented yet)
   -> per-command validation (3C: not implemented yet)
   -> PING / ECHO / QUIT semantics (3D: not implemented yet)
```

Only a **nonempty top-level RESP2 array of bulk strings** is accepted as a command request in the initial documented subset. The first element is the command verb; remaining elements are arguments. This matches the multibulk form used by modern Redis clients. RESP2 values of all seven categories remain valid in the generic parser, but not every RESP2 value is a valid command request.

Examples (conceptual):

- `*1\r\n$4\r\nPING\r\n` -> `Command{name="PING", arguments=[]}`.
- `*2\r\n$4\r\nECHO\r\n$0\r\n\r\n` -> `Command{name="ECHO", arguments=[""]}`.
- `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n` -> `Command{name="GET", arguments=["key"]}`; implementation of GET is **not** part of Phase 3.
- `$4\r\nPING\r\n`, `*0\r\n`, a null array, and arrays containing non-bulk-string elements -> command *shape* errors, not parser errors.

RESP inline command syntax, nested argument arrays, and accepting RESP `SimpleString`/`Integer` elements as command tokens are outside the initial compatibility subset. They require a deliberate later decision; do not silently enable them.

## 3. Command ownership and normalization invariants

1. A decoded command owns an independent `std::string` for its name and an ordered `std::vector<std::string>` for arguments. There are **no `std::string_view` members or pointers into the parser, socket, input buffer, or `RespValue`**.
2. The name must not be empty. Its canonical form performs **ASCII a-z -> A-Z only**. Non-ASCII bytes are not decoded or locale-transformed. Unknown or unusual nonempty names are still representable and can be rejected later by dispatch/validation; no implicit Unicode or locale casing.
3. Argument bytes are copied exactly and are binary-safe: preserve NUL, CR/LF, `0xff`, non-UTF-8 sequences, and empty strings. Preserve argument order and duplication. The command verb is *not* repeated among arguments.
4. A successful decoder does not mutate `RespValue` and is unaffected if its source `RespValue` or socket buffer is subsequently destroyed or altered. Copies and moves of `Command` must have normal value semantics.
5. Reject top-level non-array, null array, empty array, non-bulk verb, empty verb, and any non-bulk argument with a **typed decode failure**, not a fabricated command. A null bulk is not an empty bulk string.
6. No request bytes are consumed, no response is serialized, and no `Connection` state changes in command representation code.
7. Parser/connection limits already bound request size and RESP nesting. The 3A decoder should not introduce unreviewed global caps, database state, network state, or side effects.
8. API failures caused by malformed client command *shape* return a result instead of throwing. Resource allocation failures may propagate exceptions; future pipeline integration must retain its existing fail-close, no-retry-after-side-effect contract.

## 4. Proposed narrow C++20 API (final names can be refined in 3A implementation review)

- `include/redisdb/commands/Command.hpp`: an owned `redisdb::commands::Command` with read-only name/argument accessors; no public mutable borrowing interfaces.
- `src/commands/Command.cpp`: ASCII-only name canonicalization, command construction, and RESP2 value-to-command decoding.
- `enum class CommandDecodeError { None, NotArray, EmptyArray, VerbNotBulk, EmptyVerb, NonBulkArgument }` to distinguish structural errors without embedding transport/Redis error strings.
- `struct CommandDecodeResult` with `std::optional<Command>` and a typed error; success must have exactly one command and no error, failure must have no command and a non-`None` error.
- `[[nodiscard]] CommandDecodeResult decode_command(const protocol::RespValue&)` (or equivalently a `CommandDecoder` static function), with no socket or parser dependency beyond the `RespValue` type.
- Construction must ensure command-name normalization and the nonempty-name invariant. Prefer private state or otherwise prevent mutable access that would bypass these invariants.

Avoid `std::expected` because this repository targets **C++20**, not C++23. No global command registry is needed for 3A.

## 5. Unit acceptance cases for 3A implementation

- `PING` with zero args; case-insensitive `pInG` canonicalization; `ECHO` with one empty bulk argument; `GET` with a normal binary-safe key; arg order and duplicates.
- Embedded NUL, CR/LF, high bytes, and empty string in an argument survive byte-for-byte; caller can destroy/reassign source `RespValue` after decoding without changing the command.
- Structural negative matrix: simple string, error, integer, bulk and null bulk as top-level requests; null and empty arrays; empty command verb; non-bulk verb; non-bulk arguments including null bulk, nested array, integer, and simple string.
- Verify input `RespValue` is unchanged after success/failure; verify returned success/failure invariants, independent copies and moves, and name casing without locale dependence.
- Test no dispatch, `PING` replies, arity validation, socket integration, keyspace operations, or `redis-cli` claims in 3A. Such tests belong to 3B-3F or Phase 4.

## 6. Scope, integration, and staged checkpoints

- 3A.0A **COMPLETE**: clean final Phase 2 baseline, Linux final CI receipt, Mac Debug/Release and UBSan seed tests, new local-only branch.
- 3A.0B **DESIGN CHECKPOINT**: preserve this architecture and negative-case matrix with a clean one-file documentation diff; no source implementation yet.
- 3A.1A **NEXT AFTER DESIGN ACCEPTANCE**: implement `Command`/decoder and isolated unit test with CMake registration. Expect default CTest discovery to rise from 18 to 19 on macOS and Linux when that unit test is added.
- 3A.1B: compiler warnings, clang-format, complete Debug/Release suites, binary ownership/negative-case validations.
- 3A.2+: commit, push, PR-head Linux CI, PR review and merge, post-merge/closure documentation following the established guarded workflow.
- Preserve `RespParser`, `RespValue`, `RespPipeline`, server smoke, and fuzz instrumentation semantics; do not route the live server into commands in 3A.
- 3B dispatcher, 3C command-specific argument validation, 3D PING/ECHO/QUIT, 3E redis-cli smoke, and 3F unknown-command behavior remain **NOT STARTED**.

## 7. Engineering rationale and trade-offs

- Separate `Command` from `RespValue`: the RESP AST also encodes replies and nested RESP values; application command semantics must not depend on arbitrary wire-type alternatives.
- Keep decoder structural, dispatcher semantic: avoids conflating malformed request frames with unknown commands and keeps 3A independent of future storage/persistence.
- Copy from owned RESP input instead of storing views: a small extra allocation per token avoids lifetime/use-after-free bugs when connection buffers compact or are consumed.
- Canonicalize ASCII command verbs, not binary argument bytes: Redis command names are ASCII-oriented while values/keys are binary-safe.
- Do not mutate production transport before 3B-3F and a separate explicit server-integration checkpoint: preserve all passed Phase 2 acceptance and smoke behaviors.

**Design status:** Prepared for local review on the Phase 3A branch; it is not an implementation or formal completion claim.
