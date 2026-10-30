# Redis-Compatible In-Memory Database

A Redis-inspired, **C++20 in-memory database server** with an event-driven TCP networking stack, binary-safe RESP2 command handling, typed collections, expiration and memory policies, and optional append-only/snapshot persistence. Built for exploring systems programming, storage consistency, and measurable backend engineering on macOS and Linux.

The server implements a **documented subset of Redis commands**; it is not intended to be a full Redis replacement.

## Features

- **Event-driven networking:** non-blocking sockets, native `kqueue` on macOS and `epoll` on Linux, multi-client connections, buffered I/O, pipelined requests, backpressure and graceful shutdown.
- **RESP2 protocol:** incremental binary-safe parsing and serialization, partial-frame handling, bounded request/reply processing and malformed-input isolation.
- **Data structures:** strings, integers, lists, hashes and sets, with key inspection, type checking and Redis-style command errors.
- **Expiration and memory:** absolute TTL metadata, lazy/active expiration, logical memory accounting, `noeviction` and `allkeys-lru` policies.
- **Persistence:** opt-in append-only journal (`always`, `everysec`, `no` synchronization), checksummed commit watermark, operator-triggered snapshots and strict startup recovery.
- **Validation and packaging:** unit/TCP/integration tests, sanitizer and parser-fuzz testing, Redis-subset differential checks, reproducible benchmarks, GitHub Actions and non-root Docker packaging.

## Architecture

```text
Redis CLI / RESP2 clients
         |
   TCP listener
         |
  kqueue / epoll
         |
 Connection manager  --  backpressure / timeouts
         |
   RESP2 pipeline
         |
 Command dispatcher
         |
  In-memory keyspace
  |  typed values
  |  TTL + LRU + logical accounting
         |
  Optional V1 persistence
  |  append-only journal + commit watermark
  |  checksummed snapshots + CURRENT selection
         |
   Startup recovery
```

The server handles multiple TCP clients while processing database commands in a serialized event-loop context. It does not use one thread per client.

## Supported commands

| Category | Implemented commands |
|---|---|
| Connection | `PING`, `ECHO`, `QUIT` |
| Strings and integers | `SET`, `GET`, `MSET`, `MGET`, `APPEND`, `STRLEN`, `INCR`, `DECR`, `INCRBY`, `DECRBY` |
| Keys | `DEL`, `EXISTS`, `TYPE` |
| Expiration | `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`, `SET EX/PX` |
| Lists | Selected list commands including `RPUSH`, `LRANGE` |
| Hashes | `HSET`, `HGET`, `HEXISTS`, `HLEN`, `HDEL`, `HMGET`, `HGETALL` |
| Sets | `SADD`, `SREM`, `SISMEMBER`, `SCARD`, `SMEMBERS`, `SMISMEMBER` |
| Introspection | Documented read-only subsets of `INFO`, `CONFIG GET` |

Only the documented commands and options are supported. Binary-safe keys and values are subject to bounded sizes; full Redis protocol/command parity is not claimed.

## Build from source

Requirements: a C++20-capable compiler, **CMake 3.25+**, Ninja and Python 3. macOS and Linux are supported; platform-specific network polling is selected by the build.

```bash
git clone https://github.com/sinthetaaa/redis-compatible-in-memory-database.git
cd redis-compatible-in-memory-database

cmake --preset release
cmake --build --preset release
```

To build the Debug configuration instead, use `debug` in place of `release`.

## Run locally

Start a RESP2 server with in-memory-only storage:

```bash
./build/release/redisdb-server --resp2-server --port 6380
```

In another terminal (requires `redis-cli`):

```bash
redis-cli -h 127.0.0.1 -p 6380 PING
redis-cli -h 127.0.0.1 -p 6380 SET greeting hello
redis-cli -h 127.0.0.1 -p 6380 GET greeting
redis-cli -h 127.0.0.1 -p 6380 RPUSH tasks first second
redis-cli -h 127.0.0.1 -p 6380 LRANGE tasks 0 -1
redis-cli -h 127.0.0.1 -p 6380 HSET profile language cpp
redis-cli -h 127.0.0.1 -p 6380 HGET profile language
redis-cli -h 127.0.0.1 -p 6380 SADD tags backend systems
```

Stop with **Ctrl+C**. The separate `--smoke-echo` mode is a raw transport test, not a RESP2 endpoint.

### Enable persistent storage

Persistence is **disabled by default** when starting the native server. Create the data directories and explicitly enable the journal and optional snapshots:

```bash
mkdir -p data/snapshots
./build/release/redisdb-server --resp2-server --port 6380 \
  --appendonly yes \
  --aof-file data/appendonly.aof \
  --appendfsync always \
  --snapshot-dir data/snapshots
```

After the server reports readiness, `SIGUSR1` requests a snapshot; check its output for `SNAPSHOT_PUBLISHED` rather than assuming the signal itself means success. Preserve the journal, `.commit` watermark, snapshot files and selector together when restarting.

See [AOF semantics](docs/AOF.md) and [snapshot/recovery semantics](docs/SNAPSHOTS.md) for the synchronization contracts, failure modes and limitations.

## Run with Docker Compose

Requires a running **Docker Engine 28+** Linux backend, Docker Compose and `redis-cli` for client examples. The Compose service maps its port to **host loopback only** and stores data in a persistent named volume.

```bash
REDISDB_VCS_REF="$(git rev-parse HEAD)-local" docker compose build
docker compose up -d

docker compose logs redisdb   # wait for RESP_READY
redis-cli -h 127.0.0.1 -p 6380 PING
redis-cli -h 127.0.0.1 -p 6380 SET demo:key persistent

docker compose stop -t 30
docker compose up -d --force-recreate
redis-cli -h 127.0.0.1 -p 6380 GET demo:key

docker compose down           # retain the named data volume
```

Do **not** run `docker compose down -v` if you want to preserve database files. Do not publish port 6380 on a public interface; the server does not implement authentication or TLS. More detail: [Docker guide](docs/DOCKER.md).

## Tests and benchmarking

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Linux GitHub Actions validates GCC Debug/Release, Clang ASan/UBSan, a fixed RESP2 parser corpus and coverage-guided fuzzing. Additional private Docker tests compare the implemented command subset against a pinned Redis 8.10.2 reference and run repeatable multi-client/persistence benchmarks.

The accepted current-main Linux baseline completed **87/87 Debug tests, 87/87 Release tests and 88/88 Clang sanitizer tests**, along with 50 pinned parser seeds and 10,000 libFuzzer executions. [Networking CI](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40905360659) · [Sanitizer CI](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40905360660) · [Docker CI](https://github.com/sinthetaaa/redis-compatible-in-memory-database/actions/runs/40905360517).

See [benchmark methodology](docs/BENCHMARKS.md) for reproducible workloads and measurement limitations. Benchmarks are workload-specific, not claims of Redis-wide performance parity.

## Scope and limitations

- The implementation is a **Redis-compatible subset**, not a complete Redis replacement.
- Memory limits refer to **logical accounted database bytes**, not an exact bound on process RSS.
- AOF and snapshots implement the documented **V1 persistence format**. There is **no active AOF compaction, automatic log pruning or V2 generation-replacement/recovery pipeline** in the production server.
- `everysec` and `no` do not promise the same crash-durability boundary as `always`; even `always` depends on filesystem/device guarantees.
- No replication, clustering, client authentication or TLS. Keep access private.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Append-only journal and sync policies](docs/AOF.md)
- [Snapshots and recovery](docs/SNAPSHOTS.md)
- [Docker and private localhost demo](docs/DOCKER.md)
- [Benchmark methodology](docs/BENCHMARKS.md)

---

This project is a systems-engineering implementation intended for learning, testing and portfolio demonstration. Its supported commands, durability boundaries and benchmark assumptions are intentionally explicit.
