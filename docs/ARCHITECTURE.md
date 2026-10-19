# System Architecture

## Redis-Compatible In-Memory Database Server

**Language:** C++20
**Development platform:** macOS ARM64
**Deployment target:** Linux
**Protocol target:** Documented RESP2 subset
**Architecture state:** Approved design; networking foundations through Phase 1D implemented

This document describes the intended system.

The build foundation, descriptor-ownership wrapper,
loopback TCP listener, portable event-poller interface
and native macOS kqueue backend are implemented.
Linux epoll, the serving loop, protocol engine and
database execution path remain unimplemented.

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

The listener is not yet integrated with a polling loop.
No command handling, RESP2 parsing or persistent data
storage occurs in the server executable.

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

The native macOS backend was implemented in Phase 1D.
The Linux backend remains Phase 1E.
The running server loop and client lifecycle
remain future work.

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

The Linux epoll backend is not yet implemented.

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

Limits will be selected and tested in Phase 11.

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
