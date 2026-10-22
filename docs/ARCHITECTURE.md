# System Architecture

## Redis-Compatible In-Memory Database Server

**Language:** C++20
**Development platform:** macOS ARM64
**Deployment target:** Linux
**Protocol target:** Documented RESP2 subset
**Architecture state:** Phase 1 COMPLETE; RESP2 library phases 2A-2F implemented and macOS/Linux post-merge validated; executable remains transport-smoke-only

This document describes the intended system.

The build foundation, descriptor ownership, TCP listener,
portable EventPoller, native kqueue and epoll backends,
and ConnectionManager are implemented and tested.

The networking library provides bounded nonblocking I/O,
readiness dispatch, admission control, idle eviction,
backpressure and per-client failure isolation.

The executable has a continuous transport-only TCP
smoke loop. RESP2 processing exists in library/test
integrations, not the production executable; database
command execution remains unimplemented.

Do not confuse architecture plans with implemented
or validated functionality.

---

# 1. System Purpose

Build a standalone in-memory database server that:

- Accepts TCP connections.
- Understands a documented RESP2 subset.
- Supports multiple concurrent clients.
- Executes database commands atomically.
- Stores active keys and values in memory.
- Provides TTL and memory-bounded eviction.
- Supports configurable persistence.
- Recovers state after restarts and crashes.
- Works on macOS and Linux.
- Can be operated through standard Redis clients.
- Produces reproducible correctness and benchmarks.

The project prioritizes:

Correctness -> Reliability -> Measured Performance
-> Deployment -> Portfolio Presentation

# 2. High-Level Architecture

```text
                  Redis CLI / Redis Clients
                              |
                              | TCP + RESP2
                              v
                  +-----------------------+
                  | TCP Listener          |
                  | Non-blocking Sockets  |
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | Event Loop            |
                  | macOS: kqueue         |
                  | Linux: epoll          |
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | Connection Manager    |
                  | Input / Output Buffers|
                  | Backpressure          |
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | RESP2 Protocol Layer  |
                  | Parser / Serializer   |
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | Command Dispatcher    |
                  | Validation / Semantics|
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | In-Memory Database    |
                  | Types / TTL / Memory  |
                  | Eviction              |
                  +-----------------------+
                              |
                              | Mutations
                              v
                  +-----------------------+
                  | Persistence Manager   |
                  | AOF / Snapshots       |
                  | Recovery / Compaction |
                  +-----------------------+
                              |
                              v
                  +-----------------------+
                  | Filesystem            |
                  | Persistent Data       |
                  +-----------------------+
```

Supporting components:

- Configuration.
- Structured logging.
- Operational metrics.
- Shutdown and lifecycle management.
- Unit and integration test harnesses.
- Differential-test harness.
- Benchmark suite.

The arrows illustrate responsibilities, not a
finalized mutation-commit ordering.

The persistence coordinator must participate
in command execution at precisely defined
boundaries once Phase 8 begins.

# 3. Process and Thread Model

## Initial Architecture

One event-loop thread handles:

- Socket readiness notifications.
- Incoming request processing.
- Command dispatch.
- Keyspace mutation.
- Response scheduling.
- Bounded maintenance work.

Multiple sockets remain active concurrently.

Individual commands execute sequentially.

## Command Atomicity

No other command may observe an intermediate
keyspace mutation during one command's execution.

This is an in-process ordering property.

It does not imply durable transactions, rollback
after crashes, or multi-command isolation.

## Blocking Operations

Large commands, synchronous filesystem I/O
and expensive maintenance tasks may delay
other clients.

Measure event-loop stalls and tail latency.

Dedicated persistence workers may be evaluated
later with explicit ordering guarantees.

# 4. Networking Subsystem

Proposed modules:

```text
include/redisdb/net/
src/net/

TcpListener
Connection
EventLoop
KqueuePoller
EpollPoller
SocketUtils
```

## Implemented Foundation — Phase 1A

`redisdb::net::UniqueFd` provides a move-only,
RAII-owned POSIX file descriptor.

Implemented files:

- `include/redisdb/net/UniqueFd.hpp`
- `src/net/UniqueFd.cpp`
- `tests/unit/unique_fd_test.cpp`

Supported operations:

- `get()` — inspect the borrowed descriptor.
- `release()` — relinquish ownership without closing.
- `reset()` — close or replace an owned descriptor.
- Move construction and move assignment.
- Automatic cleanup during destruction.

Copy construction and assignment are disabled.
The wrapper assumes exclusive ownership of adopted FDs.
Close errors cannot be reported from the destructor.
The TCP listener is implemented in Phase 1B.
No event loop or connection manager exists yet.

## Implemented Networking — Phase 1B

`redisdb::net::TcpListener` owns an IPv4 listening
socket using `UniqueFd`.

Implemented files:

- `include/redisdb/net/TcpListener.hpp`
- `src/net/TcpListener.cpp`
- `tests/integration/tcp_listener_test.cpp`

Validated behavior:

- Binds specifically to `127.0.0.1`.
- Defaults to port `6380` with backlog `128`.
- Accepts port `0` for OS-assigned test ports.
- Configures `O_NONBLOCK` and `FD_CLOEXEC`.
- Uses `SO_REUSEADDR` on the listening socket.
- Provides `fd()`, `port()` and `accept_one()`.
- Returns `std::nullopt` for no pending connection.
- Returns an owning `UniqueFd` for accepted clients.
- Retries interrupted and aborted accepts.
- Rejects invalid backlogs and invalid listener state.
- Propagates unexpected socket errors as exceptions.
- Cleans up listening and accepted descriptors via RAII.

The listener is integrated with EventPoller through
ConnectionManager as of Phase 1F. Phase 1G adds
the executable's continuous, test-only TCP serving loop.
No command handling, RESP2 parsing or persistent data
storage occurs in the executable.

## 4.1 TCP Listener

Responsibilities:

- Open sockets through RAII wrappers.
- Configure non-blocking mode.
- Apply appropriate socket options.
- Bind and listen.
- Accept ready client connections.
- Handle transient errors.
- Enforce connection limits.
- Close descriptors safely.

Planned development defaults:

- Address: 127.0.0.1
- Port: 6380

## 4.2 Event-Poller Interface — Implemented in Phase 1C

Header: `include/redisdb/net/EventPoller.hpp`

Automated tests: `tests/unit/event_poller_test.cpp`

The abstract `EventPoller` interface exposes:

- `add(fd, interest, token)`
- `modify(fd, interest)`
- `remove(fd)`
- `wait(std::chrono::milliseconds timeout)`

`EventMask` is a type-safe flag enum containing:

- Read
- Write
- Error
- Hangup

Read and Write are supported interest masks.
Error and Hangup are output-only readiness flags.

`ReadyEvent` carries a 64-bit registration token
and the reported event flags. Token zero is invalid.

The caller must assign a fresh nonzero token for
every registration lifetime. A token must not be
reused when the operating system reuses an FD number.

A native poller does not own registered descriptors.
The owning component must unregister a descriptor
before closing it.

Registration operations are single-threaded under
the current contract.

`wait()` accepts:

- Zero milliseconds for a non-blocking check.
- A positive timeout for a finite wait.
- Negative one millisecond for an infinite wait.

Other negative timeout values are invalid.

Removal is idempotent; modification requires an
existing registration. Duplicate live registrations
are invalid.

An event in a previously returned batch may become
stale after a connection closes. The event consumer
must compare tokens against current live registrations
before processing the event.

A fake poller validates the interface contract,
argument handling, event representation and
simulated stale-event filtering.

The fake implementation does not observe actual
kernel readiness, block for real elapsed time,
or establish kqueue/epoll compatibility.

The macOS kqueue backend was completed in Phase 1D,
the Linux epoll backend in Phase 1E, and connection
lifecycle management through Phase 1F.

The continuous transport-only executable loop was
added in Phase 1G. Production command serving remains
future work.

## Implemented Kernel Poller — Phase 1D

`redisdb::net::KqueuePoller` implements the portable
`EventPoller` interface on macOS.

Implemented files:

- `include/redisdb/net/KqueuePoller.hpp`
- `src/net/KqueuePoller.cpp`
- `tests/integration/kqueue_poller_test.cpp`

CMake includes the backend and native tests only when
`APPLE` is true.

The poller owns its `kqueue()` descriptor through
`UniqueFd`, with close-on-exec enabled.

Registered socket descriptors remain caller-owned.

The implementation uses level-triggered `EVFILT_READ`
and `EVFILT_WRITE` filters with explicit enable and
disable behavior.

Registration and modification use `EV_RECEIPT` to
inspect kernel registration results.

The backend retains a mapping from file descriptors
to interest masks and nonzero 64-bit tokens.

Tokens travel through the opaque `kevent.udata` field.
The field is treated as an identity value and is
never dereferenced.

On `wait()`, the backend converts native filters
and flags into `ReadyEvent` values, including
read/write readiness and EOF/error indications.

Supported timeouts are immediate, positive finite,
and indefinite, following the EventPoller contract.
Interrupted waits are retried with remaining time
accounted for on finite waits.

Registration removal is idempotent at the API level.
A poller does not close the registered socket.

A failed registration attempts filter cleanup.
A failed interest modification attempts rollback.
If rollback cannot restore a reliable kernel state,
the poller rejects further operations.

Phase 1D tests passed on macOS in both Debug and
Release configurations, including real Unix socket
and TCP loopback readiness.

The tests do not cover every injected syscall failure
or guarantee future connection-manager correctness.

The Linux epoll backend is implemented in Phase 1E.

## Implemented Kernel Poller — Phase 1E

`redisdb::net::EpollPoller` implements `EventPoller`
on Linux using the native epoll API.

Files:

- `include/redisdb/net/EpollPoller.hpp`
- `src/net/EpollPoller.cpp`
- `tests/integration/epoll_poller_test.cpp`

CMake selects this backend only for Linux, while
retaining KqueuePoller on macOS.

An `epoll_create1(EPOLL_CLOEXEC)` descriptor is
managed through `UniqueFd`. Registered sockets are
owned by their callers, not the poller.

The implementation uses level-triggered notifications
through `epoll_ctl()` and `epoll_wait()`.

Read and write interest map to `EPOLLIN` and `EPOLLOUT`.
Hangup maps from `EPOLLHUP` or `EPOLLRDHUP`.
Kernel errors map from `EPOLLERR`.

A registration token is stored in `epoll_event.data.u64`
and returned with its corresponding readiness event.
Tokens must be unique for each registration lifetime.

The backend validates duplicate registration, invalid
interest masks and missing registrations. Failed ADD
operations discard their inserted bookkeeping.
Failed MOD and DEL operations propagate kernel errors.

Waiting supports zero, positive finite and infinite
timeouts. Interrupted system calls are retried.

Native integration tests cover real socket readiness,
read/write modification, multiple tokens, cleanup,
failed registration, timeout behavior, peer half-close
and TCP listener readiness.

The Ubuntu 24.04 x86-64 Debug and Release GitHub Actions
jobs passed all six automated tests each, including
15 repeated epoll tests per configuration.

The native Linux backend does not run the server loop,
parse RESP2, execute database commands or manage the
full client connection lifecycle.

## 4.3 Connection Ownership

Each connection conceptually owns:

- Socket descriptor.
- Incremental receive buffer.
- Pending response buffer.
- Parser state.
- Lifecycle state.
- Last-activity time.
- Buffer accounting.

Required behaviors:

- Partial reads and writes.
- Multiple commands in one read.
- Commands spanning multiple reads.
- Strict per-connection response ordering.
- EOF and reset handling.
- EAGAIN and EINTR handling.
- Bounded client resource usage.

## 4.4 Backpressure

Planned policies include:

- Maximum input size.
- Maximum queued output size.
- Bounded read processing.
- Read suspension when appropriate.
- Disconnection at hard safety limits.

Preliminary buffer limits and output watermarks
were implemented and tested in Phase 1F.
Phase 11 will address broader robustness and tuning.

## Implemented Connection Lifecycle — Phase 1F

`Connection` owns a nonblocking socket, bounded input
and output buffers, and activity timestamps.

`ConnectionManager` owns the listener and accepted
connections, and borrows a native EventPoller.

Implemented invariants:

- Unique tokens identify registration lifetimes.
- Poller removal precedes socket destruction.
- Accept, read and write operations use finite budgets.
- Duplicate readiness events are coalesced by token.
- Stale registration tokens are ignored.
- Admission limits suspend listener registration.
- Listener resumption uses a fresh registration token.
- Output high watermarks suspend client reads.
- Low watermarks allow reads to resume.
- EOF connections drain queued output before closing.
- Configured idle deadlines bound indefinite polling.
- Callback and unexpected client socket failures are isolated.
- Shared listener or poller failures remain visible.

These behaviors are tested with macOS kqueue and
Linux epoll integration tests.

The input handler is transport-level, not a Redis
protocol parser or command dispatcher.

Phase 1G added the executable's native TCP smoke
serving loop and real external-process multi-client
integration testing. This is not a Redis protocol
implementation.

## Implemented TCP Smoke Runner — Phase 1G

Files:

- `include/redisdb/server/SmokeServer.hpp`
- `src/server/SmokeServer.cpp`
- `src/main.cpp`
- `tests/integration/server_smoke_test.py`

`redisdb-server --smoke-echo` starts a loopback-only
TCP server using the native kqueue/epoll backend,
`TcpListener` and `ConnectionManager`.

The smoke callback consumes incoming bytes and queues
the exact same bytes for return. No RESP2 framing,
command dispatch or database execution takes place.

Options:

- `--port PORT`: 6380 by default; 0 selects an ephemeral port.
- `--max-clients COUNT`: 1024 by default.

The server emits `SMOKE_READY` after initialization
and `SMOKE_STOPPED` after its event loop exits.
SIGINT and SIGTERM request controlled loop termination.

Each poll is bounded to 100 milliseconds so shutdown
does not require an indefinitely interrupted native wait.

A peer reset during macOS accepted-socket setup can
cause `SO_NOSIGPIPE` configuration to fail. A typed
peer-disconnection exception allows the manager to
discard that accepted socket while preserving other
connections. Unrelated setup errors continue to
propagate.

Automated external-process tests exercise concurrent
clients, binary payloads, half-close, admission limits,
listener recovery, connection churn, abrupt resets and
shutdown signals.

The test runner does not establish Redis compatibility.
Broader resource and shutdown guarantees remain for
later dedicated hardening phases.

# 5. RESP2 Protocol Subsystem

Proposed modules:

```text
protocol/RespValue
protocol/RespParser
protocol/RespSerializer
protocol/ProtocolError
```

Supported RESP2 value categories:

- Simple strings.
- Error responses.
- Integers.
- Bulk strings.
- Arrays.
- Null bulk strings.
- Null arrays.

## Phase 2A Value Model — COMPLETE

Files:

- `include/redisdb/protocol/RespValue.hpp`
- `src/protocol/RespValue.cpp`
- `tests/unit/resp_value_test.cpp`

`RespValue` stores a `std::variant` of the seven
supported RESP2 value alternatives.

Bulk data uses owned `std::string` storage,
preserving embedded NUL bytes and arbitrary octets.
Arrays contain owned `std::vector<RespValue>` elements.

Null bulk strings and null arrays are distinct
from empty bulk strings and empty arrays.

Simple strings and error responses reject CR/LF.
Integers use `std::int64_t`.

Copy and move operations are supported. Nested array
and binary-buffer ownership are covered by unit tests.

Linux Debug and Release validation passed in
workflow 39297170492 for commit 7981c8c.

Final PR-head CI: 39298460768 SUCCESS.
Merge commit: 861f0491fdb8af490ed6da820a8024541f26f9fa.
Post-merge CI: 39304412144 SUCCESS.

Phase 2A is complete. At its closure checkpoint,
Phase 2B response serialization had not started.

This value model does not implement RESP2 wire
serialization, incremental parsing or command
execution. Those remain separate later milestones.

## Phase 2B Response Serializer — COMPLETE

Files:

- `include/redisdb/protocol/RespSerializer.hpp`
- `src/protocol/RespSerializer.cpp`
- `tests/unit/resp_serializer_test.cpp`

`RespSerializer` encodes an immutable `RespValue`
into an owned binary-safe `std::string`.

The implementation first measures the exact
serialized output size and validates configured
resource limits. It then reserves output storage
and writes the RESP2 wire framing.

Integer and length fields use `std::to_chars`.

Initial serializer limits:

- Maximum encoded output: 1 MiB.
- Maximum elements per array: 65,536.
- Maximum nested array depth: 32.

Output-size checks use subtraction-based bounds
checks to prevent arithmetic overflow.

All seven RESP2 value alternatives are supported,
including distinct null arrays and null bulk strings.

The serializer has no socket, parser, database or
command execution dependencies.

Implementation: 7835524.
PR #5: MERGED.
Initial Linux CI: 39311216460 SUCCESS.
Final PR-head CI: 39314875934 SUCCESS.
Merge commit: 0fa0269257a9c34f7aa4bb5b909bb0a5d976882c.
Post-merge CI: 39320475360 SUCCESS.

Phase 2B is complete.
Phase 2C incremental parsing is NEXT, NOT STARTED.

## Phase 2C Single-Frame RESP2 Parser — COMPLETE

Files:

- `include/redisdb/protocol/RespParser.hpp`
- `src/protocol/RespParser.cpp`
- `tests/unit/resp_parser_test.cpp`

`RespParser` provides stateless, bounded decoding
of one RESP2 frame from a `std::string_view`.

It reports:

- Complete: owned RespValue and exact consumed bytes.
- NeedMore: incomplete valid prefix, zero consumption.
- Invalid: malformed or limited frame, zero consumption
  and an error explanation.

Parsed values own their strings and arrays.
No borrowed input data is exposed through RespValue.

The parser uses std::from_chars for integer and
declared-length conversion. Bulk payloads remain
binary safe and require exact CRLF framing.

Configured defaults:

- Maximum frame bytes: 1 MiB.
- Maximum bulk bytes: 1 MiB.
- Maximum array elements: 65,536.
- Maximum nested array depth: 32.
- Maximum inline metadata bytes: 64 KiB.

The parser does not allocate arrays directly from
untrusted declared element counts.

The parser consumes at most one complete value,
leaving trailing input to the caller.

Implementation: 82b7dc9.
PR #6: MERGED.
Initial Linux workflow: 39327039720 SUCCESS.
Final PR-head CI: 39332004304 SUCCESS.
Merge commit: 68dd83137ca27a35c78907907b731ef8c1fedf4a.
Post-merge CI: 39370627026 SUCCESS.

Phase 2C is complete.
Persistent receive buffers and fragmented network
input belong to Phase 2D. Multi-frame pipelining
belongs to Phase 2E.

At Phase 2C closure, Phase 2D was next.
The following Phase 2D record captures its
subsequent adapter and TCP-test implementation.

No production parser-to-socket integration
exists yet.

## Phase 2D Fragmented Input Adapter — COMPLETE

Files:

- `include/redisdb/protocol/RespFramedInput.hpp`
- `src/protocol/RespFramedInput.cpp`
- `tests/integration/resp_framed_input_test.cpp`
- `tests/integration/resp_fragmented_dispatch_test.cpp`

The adapter receives an existing Connection
reference and uses its owned input buffer.

No new per-connection input buffer is introduced.

For each explicit poll call:

- Complete: returns an owned RESP2 value and
  consumes the exact completed frame.
- NeedMore: retains every buffered byte.
- Invalid: preserves input and reports an error
  for the caller to apply its close policy.

The adapter defaults to a 64 KiB frame limit,
coordinated with Connection's default input limit.

The core RespParser retains its separate 1 MiB
default profile when instantiated independently.

Custom Connection and adapter limits must be
configured consistently.

ConnectionManager already supplies bounded,
nonblocking TCP reads and per-client isolation.

Native integration tests install a test-specific
input handler and exercise real loopback TCP
through kqueue on macOS and epoll on Linux.

The integration tests reject malformed frames
through the manager's existing handler-failure
cleanup path. The adapter itself does not close
sockets or send protocol error responses.

Exactly one complete frame is extracted per
adapter call. A second buffered frame is left
for an explicit later call.

Automatic multi-frame draining and response
ordering belong to Phase 2E.

No production Redis command-serving endpoint
is implemented in this phase.

The --smoke-echo executable remains unchanged.

Implementation: 4f2535a.
PR #7: MERGED.
Initial Linux CI: 39376269629 SUCCESS.
Final PR-head CI: 39380341041 SUCCESS.
Merge commit: f1bc679ed707404cbc8d0a569c2eadb9af4d6687.
Post-merge CI: 39381045190 SUCCESS.

Phase 2D is complete and validated in
macOS/Linux Debug and Release.

At Phase 2D closure, pipelining was the next milestone.
The Phase 2E implementation is documented below.

## Phase 2E Bounded Pipelining — COMPLETE

Implementation files:

- `include/redisdb/protocol/RespPipeline.hpp`
- `src/protocol/RespPipeline.cpp`
- `include/redisdb/protocol/RespFramedInput.hpp`
- `src/protocol/RespFramedInput.cpp`
- `include/redisdb/net/ConnectionManager.hpp`
- `src/net/ConnectionManager.cpp`
- `include/redisdb/net/Connection.hpp`

Three dedicated integration targets cover POSIX socketpairs,
real native TCP dispatch and scheduler safety. The CI workflow
`.github/workflows/phase1e-linux.yml` repeats these targets.

### Protocol processing and output admission

`RespPipeline::drain(Connection&, PipelineReplyFactory)` handles
several complete frames from the existing connection input buffer
in request order. `RespFramedInput::peek` inspects the next frame
without consuming it. For each frame, the processor checks that
up to `max_reply_bytes` can fit in the connection output buffer
before invoking the reply factory. It then serializes and queues
one reply before consuming the corresponding frame bytes.

Processing stops with one of four outcomes:

- `NeedMore`: preserve the incomplete buffered suffix.
- `BudgetExhausted`: stop after a bounded number of frames.
- `Backpressured`: retain the next request until output drains.
- `Invalid`: preserve invalid input; caller applies close policy.

Default pipeline limits are 16 frames per drain, 8192 maximum
reply bytes and a 64 KiB parser frame budget. The connection
has 64 KiB default input and output caps. These budgets must
be coordinated when changing configuration. Output admission
is conservative: insufficient reserved capacity prevents
request execution, even if an eventual reply might be smaller.

The synchronous protocol processor performs no socket reads,
writes, native polling or independent input-buffer allocation.
`PipelineReplyFactory` is a test integration seam rather than
an implemented Redis command dispatcher. Exceptions in reply
processing require caller-level client closure; the pipeline
does not promise retry-safe side effects.

### Resumable native event-loop scheduling

`ConnectionManager` preserves the legacy `InputHandler` for
smoke echo and exposes an exclusive `ResumableInputHandler`
for protocol-aware test integrations. Its actions are:

- `AwaitRead`: wait for more socket input.
- `Continue`: enqueue deferred work for a later bounded turn.
- `AwaitWrite`: resume when pending output makes progress.

A FIFO token queue plus pending-token membership set prevents
duplicate scheduling. The manager defaults to at most 32
deferred clients per poll. Closed clients are removed from the
queue and scheduling sets; stale registration tokens cannot
be reused. The callback must consume input or enqueue output
when it requests `Continue`, preventing a no-progress CPU spin.
A full input buffer with no eligible continuation is closed
rather than left indefinitely unable to receive additional data.

Read interest and write interest remain governed by the native
kqueue/epoll poller and the connection output watermarks.
Deferred processing does not require a fresh client write,
and write-progress callbacks re-arm blocked request processing.
EOF handling preserves queued replies and remaining complete
requests where applicable. Exceptions isolate the affected
client rather than terminating the manager's event loop.

### Validation and current serving boundary

Phase 2E implementation commits: 861ebb4, 169d915, 84d4ebc.
Linux regression CI commit: 3e4bc6f.
PR #8: MERGED on 2026-10-21.
Merge commit: ba97b8bcbb11edacbbb7fb823cce02fa765118e8.
Phase 2E.5 closure checkpoint 3315a3b: COMPLETE.
Post-closure Linux CI #39629733366: SUCCESS (Debug/Release).

Local macOS Debug and Release: 18/18 tests each.
Linux Debug and Release: 18/18 tests each.
CI #39420606587: SUCCESS (initial).
CI #39422679453: SUCCESS (20 additional repetitions
per pipeline integration test in each build).
Final PR-head CI #39625859355: SUCCESS.
Post-merge Linux CI #39627679647: SUCCESS.
Post-merge macOS and Linux Debug/Release: 18/18 tests each.
Linux repeat gates: three pipeline tests 20 times each,
TCP smoke 10 times, connection tests 20 times and epoll 15 times,
in both Debug and Release.

No production Redis request/command-serving endpoint is wired
into `redisdb-server`. The `--smoke-echo` path still echoes raw
TCP bytes. Phase 2F malformed-input and resource-limit
hardening is merged and validated as documented below.
Phase 2G parser fuzz-test foundation is NEXT, NOT STARTED.

## Phase 2F Malformed-Input and Framing-Limit Hardening

Phase 2F parser changes recognize invalid bulk terminator bytes
as soon as the declared payload and first terminator byte are
available. Incomplete valid prefixes still return NeedMore.
Bulk/array length headers permit canonical nonnegative decimal
spelling or exactly `-1` for null. RESP signed integer values
retain their preexisting grammar. Binary payloads remain opaque.

`RespParser::max_frame_bytes()` and
`Connection::max_input_bytes()` expose read-only capacities.
`RespFramedInput::peek` rejects a parser frame budget larger
than the owning connection input cap before parsing or consuming
buffered input; `poll` uses the same check through `peek`.

Native kqueue/epoll TCP dispatch tests check malformed-client
isolation, healthy fragmented binary frames, canonical length
failures, and valid-prefix/malformed-suffix processing exactly
once. A reply queued before the offending peer is closed is not
guaranteed to reach that peer; the tests do not claim delivery.

Feature commits: `9781a7a`, `4dcab5f`, `d555b47`, `b86ee47`.
PR #9 merged 2026-10-22 as `ce65649` (two parents).
PR-head Linux CI #39735651885: SUCCESS.
Post-merge Linux CI #39737223938: SUCCESS.
Post-merge macOS Debug and Release: 18/18 tests each,
140 protocol repetitions and 10 native kqueue repetitions each.
Formal Phase 2F closure record committed as `b54ab7d`;
post-closure Linux Debug/Release CI #39739519389: SUCCESS.
Phase 2F is formally COMPLETE; Phase 2G remains NOT STARTED.

The production executable remains transport-smoke-only.
Phase 2G will provide parser fuzz-test foundations separately.

## Parsing Model

The parser must distinguish:

- Complete message.
- Need more input.
- Invalid message.

Parsing must be incremental and binary-safe.

The parser must not assume one read equals
one complete request.

## Resource Protection

Enforce configurable or explicit bounds for:

- Bulk-string length.
- Array element count.
- Nesting depth.
- Buffered request size.
- Integer conversion.
- Aggregate memory consumed per client.

No untrusted frame length may trigger an
unbounded allocation.

## Required Tests

- Complete request.
- Byte-by-byte fragmentation.
- Pipelined requests.
- Empty strings.
- Embedded NUL bytes.
- Invalid framing.
- Oversized declared lengths.
- Premature disconnects.
- Deep nesting.
- Truncated requests.

Parser fuzzing begins in Phase 2 and is expanded
during comprehensive validation.

# 6. Command Execution Layer

Proposed modules:

```text
commands/Command
commands/CommandDispatcher
commands/StringCommands
commands/KeyCommands
commands/ExpirationCommands
commands/ListCommands
commands/HashCommands
commands/SetCommands
```

## Request Lifecycle

Conceptual flow:

1. Read bytes from a ready socket.
2. Feed bytes into the RESP parser.
3. Obtain zero or more complete commands.
4. Validate command syntax and arguments.
5. Evaluate command semantics.
6. Coordinate mutations and persistence.
7. Encode typed RESP responses.
8. Queue responses in request order.
9. Flush output through non-blocking writes.

The mutation/persistence step is deliberately
high-level until Phase 8 defines its exact order.

## Command Registry

Prefer a modular registry or equivalent clean
dispatch design.

Avoid a single giant command switch.

## Error Handling

Unsupported commands must return clear errors.

Malformed arguments must not corrupt the keyspace.

Wrong-type errors must match documented semantics.

Fatal persistence errors must not be disguised
as successful durable writes.

# 7. Supported Command Groups

## Connectivity

PING, ECHO, QUIT.

## Strings

SET, GET, MSET, MGET, APPEND, STRLEN,
INCR, DECR, INCRBY, DECRBY.

## Key Operations

DEL, EXISTS, TYPE.

## Expiration

EXPIRE, PEXPIRE, TTL, PTTL, PERSIST.

SET supports the documented EX and PX options.

## Lists

LPUSH, RPUSH, LPOP, RPOP, LRANGE.

## Hashes

HSET, HGET, HDEL, HGETALL.

## Sets

SADD, SREM, SMEMBERS, SISMEMBER.

## Introspection

A documented INFO subset and selected
read-only CONFIG GET options.

## Compatibility Policy

Each implemented command must document:

- Syntax and accepted options.
- Argument-count validation.
- Input types and constraints.
- Response type and values.
- Error cases.
- TTL behavior.
- Atomicity.
- Persistence representation.
- Intentional compatibility differences.

Additional client-handshake support may be added
after testing real clients.

# 8. Storage Subsystem

Proposed modules:

```text
storage/Database
storage/Entry
storage/Value
storage/ExpirationManager
storage/MemoryTracker
storage/EvictionPolicy
```

## Keyspace

Candidate primary container:

```cpp
std::unordered_map<std::string, Entry>
```

## Values

Planned type-safe representation:

```cpp
using Value = std::variant<
    StringValue,
    ListValue,
    HashValue,
    SetValue
>;
```

This is a design sketch, not a final declaration.

## Entry Metadata

May include:

- Typed value.
- Optional expiration deadline.
- Accounted memory estimate.
- Eviction access metadata.
- Generation/version metadata if necessary.

## Fundamental Invariants

- A key maps to at most one live entry.
- Expired entries behave as absent.
- Wrong-type commands cannot reinterpret values.
- Failed writes must not leave partial mutations.
- Deletion clears associated metadata.
- Accounted memory is updated consistently.
- Sequential command execution preserves ordering.

# 9. Expiration

Use both lazy and active expiration.

## Lazy Expiration

Check expiration during key access.

If expired, remove the entry before returning
a result to the caller.

## Active Expiration

Perform bounded periodic cleanup.

Do not monopolize the event loop with unbounded
expiration scans.

## Clock Semantics

Use an injectable clock for deterministic tests.

Use wall-clock expiration deadlines when persistence
must preserve absolute expiry times.

Never serialize raw steady_clock time points.

# 10. Memory Accounting and Eviction

Track estimated data memory separately from
network-buffer allocations.

Account for:

- Key bytes.
- Value bytes.
- Entry metadata.
- Relevant auxiliary structures.
- Container overhead estimates.

Estimated logical memory differs from process RSS.

## Initial Policy

noeviction:

Reject eligible memory-increasing operations that
would exceed the configured data-memory limit.

A rejected operation must preserve existing values.

## Secondary Policy

Implement one LRU-style policy.

Choose exact or approximate LRU deliberately,
then document its semantics.

Eviction must coordinate with persistent mutations.

# 11. Append-Only Persistence

Proposed modules:

```text
persistence/AofWriter
persistence/AofReader
persistence/PersistenceCoordinator
```

The AOF contains replayable logical mutations.

The final record representation is decided
during Phase 8.

## Policies

- always: synchronize required records before
  successful durable acknowledgment.
- everysec: periodically synchronize; recent
  acknowledged writes may be lost.
- no: rely on OS writeback with weaker guarantees.

## Mutation Ordering

The coordinator must define the transitions among:

- Validation.
- Resource reservation.
- Mutation preparation.
- Log append.
- Required synchronization.
- In-memory publication.
- Client acknowledgment.

Neither a failed append nor a failed synchronization
may be falsely reported as a successful durable write.

Commands involving relative expiration, eviction
or existing numeric values require deterministic
replay semantics.

## Failure Paths

Test:

- Short writes.
- Interrupted syscalls.
- Disk exhaustion.
- Synchronization failures.
- Incomplete final records.
- Corrupt records.
- Unexpectedly missing files.

# 12. Snapshotting

Proposed modules:

```text
persistence/SnapshotWriter
persistence/SnapshotReader
```

Use a versioned, portable binary representation.

Planned metadata includes:

- Magic identifier.
- Format version.
- Record framing.
- Key and value data.
- Expiration deadlines.
- Integrity checks.
- AOF generation/offset relationship.

Do not serialize native C++ object memory.

## Publication Requirements

- Write into a temporary file.
- Validate and synchronize file contents.
- Publish a completed snapshot safely.
- Synchronize directory metadata where required.
- Retain a valid recovery generation.

The exact snapshot strategy must be reviewed
against the target filesystems and guarantees.

# 13. Recovery and Compaction

Proposed modules:

```text
persistence/RecoveryManager
```

## Startup Recovery

1. Validate configuration.
2. Discover persistent files.
3. Validate snapshot metadata and integrity.
4. Restore a valid snapshot.
5. Replay the necessary AOF generation.
6. Reconstruct expiration deadlines.
7. Apply recovery consistency checks.
8. Start accepting TCP clients only when ready.

Unrecoverable corruption must produce a clear error.

Never silently replace corrupted persistent data
with an empty database.

## Compaction

Prevent unbounded AOF growth through coordinated
snapshotting and log rewrite/rotation.

Compaction must preserve acknowledged state,
TTL semantics and recoverability across interruption.

## Crash Testing

Use controlled fault injection around append,
sync, snapshot publication, compaction and replay.

SIGKILL tests alone do not establish
power-loss durability guarantees.

# 14. Configuration and Security

Centralize configuration of:

- Bind address and port.
- Maximum clients.
- Input and output limits.
- Protocol size limits.
- Data-memory limit.
- Eviction policy.
- AOF enablement.
- AOF synchronization mode.
- Data directory.
- Snapshot settings.
- Logging level.

Invalid settings must fail clearly before serving.

Do not silently allow public database exposure.

Initial local default: 127.0.0.1:6380.

Container port publication must preserve restricted
host access even if internal container binding
requires a different interface.

# 15. Observability

## Logs

Record startup, configuration summary, listener
state, recovery, snapshots, persistence failures,
evictions, shutdown and fatal errors.

Avoid logging actual stored values by default.

## Metrics

Planned measurements:

- Active clients.
- Total accepted connections.
- Commands processed.
- Errors by category.
- Key count.
- Expired and evicted keys.
- Accounted data memory.
- Network-buffer memory.
- AOF bytes and sync latency.
- Snapshot duration.
- Recovery duration.

Expose a documented subset through INFO.

# 16. Testing Architecture

## Unit Tests

Protocol parser, serializer, storage, commands,
expiration, memory accounting, persistence formats,
snapshots and recovery.

## TCP Integration Tests

Launch the actual server process.

Exercise multiple clients, fragmentation, pipelining,
disconnects, large payloads and restart recovery.

## Differential Tests

Execute supported commands against this server
and a real Redis instance.

Compare response types, values, errors and behavior.

## Sanitizers

- AddressSanitizer.
- UndefinedBehaviorSanitizer.

ThreadSanitizer may be considered if a multithreaded
component is introduced and the platform supports it.

## Fuzzing

Prioritize untrusted RESP parser input.

## Fault Injection

Target AOF, snapshots, recovery and compaction.

# 17. Performance Engineering

Record:

- Operations per second.
- P50, P95 and P99 latency.
- CPU utilization.
- RSS.
- Accounted data memory.
- Memory per key.
- Concurrent client count.
- Pipeline depth.
- Persistence overhead.
- Recovery duration.

Benchmark GET, SET, mixed workloads, pipelining,
TTL-heavy and eviction-heavy behavior.

Any Redis comparison must state machine, OS,
compiler, versions, workload and configuration.

# 18. Deployment Architecture

## Mandatory Local Linux/Docker Milestone

- Linux compilation.
- Multi-stage Docker build.
- Minimal runtime image.
- Non-root execution.
- Persistent volume.
- Container health checks.
- Graceful shutdown.
- Restart recovery.
- Reproducible demo.

## Private Remote Linux Milestone

- Approved infrastructure.
- Restricted network exposure.
- Persistent storage.
- SSH tunnel access.
- Remote command verification.
- Restart and recovery tests.
- Cleanup and cost documentation.

An always-on publicly accessible database
service is not required.

# 19. Initial Repository Structure

Implemented foundation:

```text
RedisCompatibleDB/
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── .gitignore
├── include/
│   └── redisdb/server/Version.hpp
├── src/
│   ├── main.cpp
│   └── server/Version.cpp
└── tests/
    └── unit/version_test.cpp
```

Target future modules:

```text
include/redisdb/
├── config/
├── net/
├── protocol/
├── commands/
├── storage/
├── persistence/
├── metrics/
└── server/

src/
├── config/
├── net/
├── protocol/
├── commands/
├── storage/
├── persistence/
├── metrics/
└── server/

tests/
├── unit/
├── integration/
├── differential/
├── persistence/
└── fuzz/
```

The final repository will also include documentation,
benchmarking, scripts, Docker and CI files.

Do not create empty implementation modules merely
to match the target structure.

# 20. Architecture Evolution Policy

Implement in the approved roadmap order.

When a design changes:

1. Record the technical motivation.
2. Identify affected components.
3. Document migration and compatibility risks.
4. Add or revise tests.
5. Update the relevant ADR.
6. Update this architecture document.
7. Update PROJECT_STATE.md.
8. Commit the validated change.

An architectural idea becomes an implemented feature
only when working code and verification support it.

# 21. Phase 2G Parser Fuzzing Validation Architecture

**Implementation status: feature-branch CI validated; merge pending.**
Feature commits: `ee87032d1ea590f32f8feb3567553ebb9170cd16`
and `b7edd7540ec3d46008ee7878fb55063c26334e9f`.

Opt-in CMake flag `REDISDB_ENABLE_FUZZ_TARGETS=ON` builds
standalone instrumentation of the real `RespParser` and
`RespValue` sources together with a shared invariant checker.

On macOS with Apple Clang 17, the deterministic seed runner
uses UBSan alone because the observed ASan runtime hangs during
initialization before `main()`. The 50 binary-safe, bounded
RESP2 corpus files are verified against an external SHA-256
manifest before any replay.

On Ubuntu 24.04 with Clang 18, a separate
`LLVMFuzzerTestOneInput` target enables libFuzzer coverage,
ASan and UBSan on the production parser source. Inputs are
bounded to 4096 bytes. The seed corpus is copied into a
separate writable temporary directory for mutations; the
original SHA-256 corpus remains unchanged.

GitHub Actions run #39754741830 passed all 19 Linux CTests
and 10,000 libFuzzer executions on feature commit `b7edd754`.
No sanitizer failures were reported. Existing default Debug
and Release builds continue to use their normal test suites.

This is a test-only architecture addition. No production
RESP2 server execution, database commands, storage, wire
syntax changes or persistence behavior has been introduced.
Formal Phase 2G completion requires the PR merge and
post-merge verification on main.
