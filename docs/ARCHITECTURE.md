# System architecture

The production server is a feature-frozen C++20 Redis-compatible subset using
V1 append-only persistence and snapshots. macOS uses kqueue; Linux uses epoll.
The [README](../README.md) contains build and quick-start examples.

## Components and execution model

```text
TCP listener -> ConnectionManager -> Connection buffers
                    kqueue / epoll         |
                                    RESP2 parser / pipeline
                                           |
                                    owned Command decoder
                                           |
                                  dispatcher / typed handlers
                                           |
                               shared Database + TTL + LRU ledger
                                           |
                               optional V1 AOF + SnapshotStore
```

One event-loop thread handles readiness, command execution, keyspace mutation,
response scheduling and maintenance. Multiple clients have active sockets, but
commands execute sequentially against one authoritative database. No command
observes another command's intermediate mutation. This is an in-process ordering
property, not Redis MULTI/EXEC, multi-command isolation or exactly-once execution.
Database is not a synchronized multiwriter container.

Large collection operations, journal writes, `fsync`, snapshot capture and startup
replay are synchronous. They can stall all clients; bounded network work does not
bound filesystem latency. No persistence worker or thread per client is used.

Implementation modules live in [net](../src/net), [protocol](../src/protocol),
[commands](../src/commands), [storage](../src/storage),
[persistence](../src/persistence) and [server](../src/server).

## Networking and resource limits

`TcpListener` accepts nonblocking IPv4 sockets. `UniqueFd` owns descriptors;
`ConnectionManager` registers them with the native level-triggered poller and
unregisters before close. Registration tokens distinguish connection lifetimes,
including when the OS reuses a descriptor. Stale events and deferred tokens must
not act on a new connection.

Each `Connection` owns input/output byte strings, lifecycle state and activity
time. Reads and writes handle partial progress, EAGAIN, EINTR, EOF and resets.
Queued replies stay ordered across fragmentation, pipelines and half-closes.
Malformed frames and ordinary client-handler failures isolate that client.
`QUIT` queues OK and closes after its reply is flushed.

The default profile in the networking/protocol headers is:

| Resource | Default |
|---|---:|
| Clients | 1,024 |
| Connection input/output buffer | 64 KiB each |
| Read/write work per client event | 8,192 bytes each |
| Frames per pipeline drain | 16 |
| Serialized reply | 8,192 bytes |
| Deferred clients per poll | 32 |
| Production parser frame/bulk budget | 64 KiB each |
| Parser array elements / nesting depth | 65,536 / 32 |

The standalone `RespParser` default allows 1 MiB frames/bulks; it is not the
production pipeline profile. Parser, reply and connection limits must agree.
These are explicit C++ configuration bounds, not additional public CLI flags.

The pipeline peeks at a frame and checks room for the maximum allowed reply
before invoking its reply callback. It queues one serialized reply before
consuming the request bytes. Insufficient output capacity prevents execution,
even when a particular reply might be smaller. Exceptions after side effects
require client closure; retrying that request could duplicate a mutation.

A FIFO deferred queue and membership set prevent duplicate scheduling.
`AwaitRead`, `Continue` and `AwaitWrite` distinguish incomplete input, scheduled
CPU work and output pressure. `Continue` must make progress; write progress
resumes blocked buffered requests without another client write. Input that fills
its cap without an eligible continuation closes the client instead of spinning.
Backpressure suspends reading as output fills and resumes it as output drains.

## RESP2 and command dispatch

The parser returns Complete, NeedMore or Invalid. Complete owns one decoded
`RespValue` and reports exact consumed bytes. NeedMore and Invalid consume zero.
`RespFramedInput` reuses the existing connection buffer; it does not allocate a
second receive buffer. Canonical length syntax, framing, nesting and aggregate
frame limits are checked before trusting declared input sizes.

Commands require a nonempty RESP2 array of bulk strings. The decoder copies an
owned command name and ordered, binary-safe arguments; only ASCII name letters
are uppercased. Protocol values remain separate from stored values. The explicit
registry maps names to handlers; a reusable arity validator handles argument
counts while handlers validate options, numbers, types and reply bounds.
Unknown commands return safe errors. Wrong-type access returns WRONGTYPE rather
than reinterpreting a variant. Structural decoding, routing and execution are
separate from socket ownership.

## Supported command subset

Only these forms and their documented behavior are implemented. Brackets below
mean an optional argument; ellipses mean repeated arguments, not additional Redis
options. All forms remain subject to request and response budgets.

| Forms | Behavior |
|---|---|
| `PING [message]`, `ECHO message`, `QUIT` | PONG or echoed bytes; graceful close after OK for QUIT. |
| `SET key value [EX seconds or PX milliseconds]` | One optional positive relative expiry pair; replaces any stored type. No NX, XX, GET or KEEPTTL. |
| `GET key`, `STRLEN key`, `APPEND key value` | String operations; missing GET is nil, missing STRLEN is zero. |
| `MSET key value [key value ...]`, `MGET key [key ...]` | Atomic MSET with last duplicate destination winning; MGET preserves order and returns nil for missing or non-string entries. |
| `INCR key`, `DECR key`, `INCRBY key amount`, `DECRBY key amount` | Checked signed 64-bit integer strings; malformed numbers and overflow reject without changing the value. |
| `DEL key [key ...]`, `EXISTS key [key ...]`, `TYPE key` | DEL counts each removed key once; EXISTS counts repeated existing arguments. TYPE reports none/string/list/hash/set. |
| `EXPIRE key seconds`, `PEXPIRE key milliseconds` | Set a relative deadline; nonpositive values delete a live key. No conditional-expiry options. |
| `TTL key`, `PTTL key`, `PERSIST key` | Remaining seconds/milliseconds; -2 for missing, -1 for persistent. PERSIST removes an existing deadline. |
| `LPUSH key element [element ...]`, `RPUSH key element [element ...]`, `LLEN key` | Ordered lists with duplicate elements; pushes return length. |
| `LPOP key`, `RPOP key`, `LRANGE key start stop` | Single-element pops; inclusive ranges with negative indices. No pop-count or blocking forms. |
| `HSET key field value [field value ...]`, `HGET key field`, `HEXISTS key field`, `HLEN key`, `HDEL key field [field ...]` | Typed hash operations; HSET counts newly inserted fields. |
| `HMGET key field [field ...]`, `HGETALL key` | Ordered positional results for HMGET; unspecified field iteration order for HGETALL. |
| `SADD key member [member ...]`, `SREM key member [member ...]`, `SISMEMBER key member`, `SCARD key` | Binary-exact unique members; add/remove counts reflect distinct changes. |
| `SMEMBERS key`, `SMISMEMBER key member [member ...]` | Unordered members or positional 0/1 membership results. |
| `INFO [section]`, `CONFIG GET pattern [pattern ...]` | Restricted read-only operational views, described below. |

String writes and collection element/field/value admission use reply-safe size
checks. The string value limit is 8,176 bytes, leaving space within the 8,192-byte
bulk-reply budget. Aggregate MGET/LRANGE/HMGET/HGETALL/SMEMBERS replies must also
fit their encoded reply budgets; a large collection does not imply every range
or full-collection reply can be returned. Persistence codec limits are separate
and larger; they are not advertised TCP command limits.

## Keyspace, expiration and memory

The keyspace uses `std::unordered_map<std::string, Entry>`. Entries own one
`std::variant` value and an optional absolute wall-clock deadline:

| Value | Container | Tradeoff |
|---|---|---|
| String | `std::string` | Binary bytes; integer commands store decimal strings, not a separate TYPE. |
| List | `std::list<std::string>` | Ordered duplicates and constant-time end operations, with node allocation/cache costs. |
| Hash | `std::unordered_map<std::string, std::string>` | Unique fields; no iteration-order promise. |
| Set | `std::unordered_set<std::string>` | Unique binary members; no iteration-order promise. |

Keys and nested values own their bytes, including NUL and non-UTF8. Copies are
independent. Collection mutations stage replacements for exception safety;
existing-key pushes/updates can copy the affected collection. Nonempty collection
mutations preserve TTL. Removing the last item removes the key and expiry index
record. SET/MSET replace types and clear old TTL unless SET supplies EX/PX;
APPEND and integer updates preserve existing expiration.

Lazy checks hide expired keys on access. A deadline-ordered index supports at
most 32 active cleanup checks on each nominal 100 ms monotonic maintenance turn.
Clock injection makes boundary testing deterministic. Scheduling uses monotonic
time; persisted expiry uses signed Unix microseconds, never steady-clock time
points. Downtime does not renew TTL. Wall-clock changes still affect absolute
expiry, and filesystem stalls can delay physical cleanup.

For live keys, TTL rounds remaining seconds to the nearest integer; PTTL truncates
to whole milliseconds. Duration parsing and deadline arithmetic reject overflow.
SET EX/PX requires a positive duration, while nonpositive EXPIRE/PEXPIRE deletes
a live key. These differences are intentional parts of the supported subset.

Each physical entry caches a deterministic V1 logical charge; a cached database
total supports constant-time inspection. The accounting policy is:

- Entry: 64 plus key byte length.
- String: payload length.
- List: 48 plus byte length per element.
- Hash: 80 plus field and value byte lengths per field.
- Set: 56 plus member byte length per member.
- Expiration: an additional 48 plus key byte length.

Checked arithmetic prevents wrapping. Expired entries remain charged until
physically removed. These coefficients are policy units, not measured allocator
costs, bucket capacity, temporary clones, network buffers or process RSS.
`--maxmemory 0` is unlimited. `noeviction` rejects writes that cannot be admitted;
`allkeys-lru` uses exact recency ordering rather than random sampling. Memory
admission and mutation are coordinated so a rejected write does not publish
partial data changes. Recovered live state exceeding the configured budget fails
startup instead of silently evicting it.

## Persistence and recovery boundary

Native persistence is off by default. When enabled, commands execute tentatively
under an undo transaction (`noeviction`) or private database clone (exact LRU).
Actual changed-key postimages and tombstones, including eviction, form one V1
journal transaction. Failed commands roll back; required append/synchronization
precedes allocation-free memory publication and a successful response.

Affected-key staging avoids a whole-database copy under noeviction. Exact LRU
retains the O(database size) copy and full-order record; large collection
postimages also remain costly. Read-only recency changes become durable only at
a later data commit; a snapshot captures recency as of its own checkpoint.

The checksummed AOF and atomic `.commit` watermark form one recovery unit.
`always`, `everysec` and `no` have different acknowledgment/durability boundaries.
A complete unacknowledged record can recover, while an incomplete tail rejects
startup. Persistence failure stops serving. CRC32C is accidental-corruption
detection, not authentication. Full guarantees: [AOF](AOF.md).

SIGUSR1 requests an optional synchronized snapshot after readiness. Immutable V1
generations are selected solely through CURRENT, after file/directory syncs.
Recovery validates the selected snapshot and full associated journal history,
then applies only effects after the checkpoint. No fallback to an arbitrary
snapshot, automatic salvage or journal truncation occurs. Snapshots do not prune
history or bound recovery time. See [snapshots](SNAPSHOTS.md).

Isolated V2 codec/staging prerequisites exist in the source, but the production
server has no V2 initialization/import, generation replacement/recovery,
compaction or reclamation pipeline. That research is frozen and is not a usable
server persistence mode.

## CLI and operational visibility

Configuration is startup-only, with no configuration-file parser or CONFIG SET.
Use `redisdb-server --help` for accepted modes. `--smoke-echo` is raw loopback
transport testing, not a RESP2 endpoint; `--version` reports the binary version.

| RESP2 option | Default and accepted values |
|---|---|
| `--port` | 6380; 0 requests an ephemeral port. |
| `--bind` | 127.0.0.1; only the exact IPv4 literals 127.0.0.1 and 0.0.0.0. |
| `--max-clients` | 1024; positive count. |
| `--idle-timeout-ms` | 0 disables; otherwise up to 31,536,000,000 ms. |
| `--maxmemory` | 0 unlimited logical bytes. |
| `--maxmemory-policy` | noeviction or allkeys-lru; default noeviction. |
| `--appendonly` | no; accepts yes/no. |
| `--aof-file` | ./appendonly.aof; parent directory must exist. |
| `--appendfsync` | always; accepts always/everysec/no. |
| `--snapshot-dir` | disabled; existing directory, requires AOF. |

AOF path/sync/snapshot options without enabled AOF reject startup. The native
listener defaults to loopback; the container deliberately binds 0.0.0.0 inside
its namespace while Compose publishes only host loopback. No authentication,
TLS, replication or clustering is implemented. See [Docker security](DOCKER.md#safe-networking).

### INFO and CONFIG GET

INFO supports server, clients, stats, memory and persistence sections. Omission,
`default` or `all` selects these sections; an unknown section returns an empty
bulk string. More than one section argument is rejected. Fields include
`redisdb_version`, `connected_clients`, `total_connections_received`,
`total_commands_processed`, `redisdb_commands_succeeded`,
`redisdb_commands_rejected`, `total_net_input_bytes`, `total_net_output_bytes`,
`redisdb_connections_closed`, `redisdb_parser_errors`, `redisdb_transport_errors`,
`redisdb_logical_memory_bytes`, `aof_enabled` and `redisdb_aof_sequence`.

CONFIG GET returns alternating bulk name/value elements for these nine settings:
`port`, `bind`, `maxclients`, `maxmemory`, `maxmemory-policy`, `appendonly`,
`appendfsync`, `timeout`, `idle-timeout-ms`. Names are canonical lowercase;
patterns match exact names or one trailing `*`. `timeout` is integer seconds
from the millisecond timeout. Private paths are not exposed.

These views sample cached event-loop state before the command clock/transaction
path. They do not scan keys, expire data or change LRU. Transport counters cover
completed polls; command counters exclude the current inspection. Counters reset
on restart and saturate at UINT64_MAX. Written AOF sequence is not proof of
weaker-policy durability, and logical memory is not RSS.

Idle reaping exempts pending output, AwaitWrite and deferred CPU work. Incomplete
input may expire. SIGINT/SIGTERM stops admission, drains observed pending work for
up to one monotonic second, then checks journal finish before RESP_STOPPED.
Unread future kernel requests have no completion guarantee; blocking filesystem
stalls are outside the network drain deadline.

Static lifecycle/error categories are JSON lines on stderr, without stored
values, keys or private paths. Failed diagnostic write/flush exits 74. Readiness,
snapshot and shutdown markers remain on stdout. A signal is not proof of a
published snapshot; wait for SNAPSHOT_PUBLISHED.

## Validation and engineering tradeoffs

Unit tests cover protocol, typed storage, expiration, accounting, command errors,
transaction rollback and V1 recovery. Actual-process TCP tests cover fragments,
pipelines, multiple clients, malformed peers, shutdown and restart. Snapshot and
AOF tests include corrupt/truncated data and injected failures; process-crash
tests do not prove host power-loss durability.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The Linux workflows run GCC Debug/Release and Clang 18 ASan/UBSan, immutable
50-seed parser replay and 10,000 bounded libFuzzer executions. Fuzz targets are
opt-in with `REDISDB_ENABLE_FUZZ_TARGETS=ON`; mutation uses a temporary corpus copy
and preserves the original SHA-256 manifest. Inputs are bounded to 4,096 bytes.
The documented Apple Clang 17 development host could not run ASan reliably before
main or link libFuzzer; its deterministic corpus used UBSan. This is a recorded
host limitation, not a claim about every macOS toolchain.

### Differential comparison boundaries

The differential harness uses a pinned Redis 8.10.2 reference. The route matrix
in `tests/differential/command_matrix.csv` covers the 40 registered data/connectivity
commands above. INFO and CONFIG GET have a separate operational comparison;
40 registered names do not imply complete Redis command or option support.

Comparison preserves RESP2 types, binary bytes, nil versus empty, exact integer
values and positional arrays. SMEMBERS compares unique unordered bulk members;
HGETALL compares unique binary field/value mappings without depending on order.
Duplicates in an allegedly unique reply are failures, not silently discarded.
Only errors/forms supported by both servers belong to a shared parity trace;
unknown routes, advanced options, oversized data and protocol fuzzing are separate
from this constrained oracle. QUIT belongs to a connection-termination check,
not a midstream mixed trace.

TTL/PTTL missing/persistent codes compare exactly. Positive TTL and natural expiry
need bounded-time invariants and explicit boundary handling, not naive equality
between commands sampled at different times. Deterministic mixed traces use plain
SET and zero/negative expiry to avoid that ambiguity. Raw request/reply bytes,
reference fingerprints and replay metadata distinguish semantic mismatches from
infrastructure or identity failures.

The native harness owns new child processes on ephemeral loopback ports; it does
not target an existing user database. Linux helpers own labelled containers,
volumes and a private bridge. Cleanup applies only to those exact owned resources.
Native and Linux reference hashes are platform-specific; a changed binary/image
fails identity checks and must not silently fall back to a floating version.
See [differential harness examples](../tests/differential/README.md) for native
commands and [Docker validation](DOCKER.md#repeatable-container-checks-and-linux-reference)
for the Linux image/reference adapter.

Benchmarks retain workload, binary, environment and measurement limitations in
[BENCHMARKS.md](BENCHMARKS.md). No Redis-wide performance or production-capacity
claim follows from the test counts or the benchmark driver.

Architecture rationale is recorded in [DECISIONS.md](DECISIONS.md).
