# TCP performance baselines

Stage 4A is merged and Linux-validated at `5e3b3cceaf1fd9c9522567b0a4807f2f7cf604cc`;
see [Docker acceptance evidence](DOCKER.md). The tables here are **native macOS**
measurements, kept separate from Linux container results. Stage 4B is local and
unpublished; its new Linux Docker/ASan checks await feature publication. Neither
set of measurements establishes Redis throughput parity or production capacity.

## Method and reproduction

`benchmarks/tcp_baseline.py` uses only Python's standard library and the existing
bounded RESP2 codec. It launches and cleans up its own server processes or
labelled containers/volumes; it cannot target an arbitrary existing database.
Each repeat creates a fresh database, preloads 100 or 1,000 keys through actual
TCP MSET batches, performs warm-up, then measures successful GET/SET replies.
SET values change on every operation so AOF costs cannot disappear as no-ops.
Keys are 9 bytes; values are 64 bytes. GET requests are 28 bytes; SET requests
99 bytes. Counts, total request bytes and actual read/write counts are recorded.

The 30-profile matrix combines: GET/SET/80%-read/80%-write, one/four client
threads, pipeline one/16 with persistence off; and GET/write-heavy scaling at
100/1,000 keys under off/always/everysec/no. It is deliberately not a full
Cartesian product. Random keys use deterministic per-client seeds. No eviction,
expiration, background traffic or changing key count is introduced during load.
Dataset raw values occupy 6,400/64,000 bytes and logical V1 charges 13,700/137,000
bytes; temporary staging, buffers and RSS are outside that logical charge.

Each client is closed-loop: send one bounded pipeline batch, then drain/validate
all replies. Publication review strengthened single-client GET validation to
exact bytes using an oracle carried across warmup/load, including pipeline order;
SET replies must be exact OK. Multi-client shared-key GET checks remain type/length,
with concurrency correctness covered separately. The historical native figures
below predate this added oracle overhead; future before/after runs must use the
same driver revision. CI asserts all 24 scaling profiles and 48 uncapped repeats.
A shared barrier starts clients together. Throughput divides all
completed operations by actual time through final outstanding reply completion.
Each request's latency starts at its batch send and ends after its own parsed
reply, including server queueing and preceding reply decoding. Percentiles use
nearest rank over every measured operation; median across repeats is explicitly
labelled rather than presenting a median-of-percentiles as a pooled percentile.
Latency is not divided by pipeline depth. This methodology has closed-loop
coordinated omission and Python/client-CPU limits; it is not server-only capacity.

Windows, warm-up and repeats are configurable within fixed bounds. Default:
2 seconds + 0.15-second warm-up, two repeats. Everysec baselines require at least
two measured seconds so scheduled synchronization occurs; `--smoke` is explicitly
a shorter four-policy functionality profile. The per-client million-operation
safety cap is recorded if hit. No raw latency archive or large framework.

```sh
cmake --preset release
cmake --build --preset release --parallel 2
python3 benchmarks/tcp_baseline.py --server build/release/redisdb-server \
  --seconds 2 --warmup 0.25 --repeats 2 \
  --output benchmark-results/native-new.json
# Container counterpart, after a real image build:
docker build --build-arg VCS_REF="$(git rev-parse HEAD)" -t redisdb:stage4a .
python3 benchmarks/tcp_baseline.py --image redisdb:stage4a \
  --seconds 2 --warmup 0.25 --repeats 2 \
  --output benchmark-results/linux-new.json
```

Use a fresh output path; existing reports are never overwritten. Partial runs
are written with `complete: false` and exit nonzero. Compare only complete runs
with matching environment, build, dataset, policy and load settings. Run on a
quiet host without simultaneous test/build/benchmark jobs. Native baselines
require an adjacent CMakeCache proving Release. Container mode requires this project's Release/source image labels. Runs override the
image command with explicit workload policy arguments (no logical maxmemory
limit; helper still imposes 512 MiB cgroup memory).

JSON captures host/OS/CPU/architecture/Python, Git HEAD and dirty state, source
fingerprint, Release flags/compiler and binary SHA (or container image ID,
architecture and build labels, with image revision required to match HEAD), duration, clients, pipeline, key/value counts,
operation/request byte counts, p50/p95/p99, throughput, AOF growth, and observed
snapshot time. Linux VmHWM provides peak server-process RSS up to the end of
load, including preload/warm-up; Docker cgroup memory is never mislabeled RSS.
On this macOS host peak RSS is null because no reliable per-child peak source
is used. Snapshot duration is signal-to-published-message observation including
100 ms event-loop polling (and Docker CLI/log polling in containers), not an
isolated snapshot disk-write benchmark. All writes/flushes during preload and
warm-up are excluded from throughput; AOF byte growth covers measured commands.
The final snapshot can force pending weak-policy data to durable storage after
timing; it does not retroactively make their timed replies synchronous.

## Measured local figures (2026-10-26)

Host: Apple M4, 10 logical CPUs, arm64, macOS 26.5.2. Driver: Python 3.9.6.
Native Apple Clang 17 Release, `-O3 -DNDEBUG`; Git HEAD baseline
`798010ff34ec0c094aa37f7821524277edbb75dd` with Stage 4A working-tree changes.
Measured server SHA-256:
`fa994fadf00161b7884cb2e36e9455810551e4309427d65dcd9002e79f833d95`.
The raw JSON retains its exact source fingerprint and dirty flag; these are not
claimed to be measurements of a published Stage 4A commit.

Two repeats/profile. Off/always/no figures below use 0.15-second warm-up and
0.5-second measured windows (actual 0.5000–0.5004 seconds), an exploratory bounded
baseline. Everysec figures use a separate 0.25-second warm-up + two-second run
with the same binary. No sample cap was hit in these final measured runs.
An earlier exploratory run shortened fast pipelines at a lower cap and is not
used here. No repeated unchanged local regression suites were needed for timing.

| Policy | Keys | Workload | Clients | Pipeline | Median ops/s | Median p50 ms | Median p95 ms | Median p99 ms |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| off | 1,000 | GET | 1 | 1 | 35,216 | 0.0248 | 0.0276 | 0.0301 |
| off | 1,000 | GET | 1 | 16 | 145,756 | 0.0554 | 0.0808 | 0.0876 |
| off | 1,000 | GET | 4 | 16 | 155,586 | 0.3505 | 0.5589 | 0.6807 |
| off | 1,000 | SET | 1 | 1 | 35,271 | 0.0240 | 0.0269 | 0.0293 |
| off | 1,000 | 80% writes | 1 | 1 | 35,293 | 0.0240 | 0.0272 | 0.0298 |
| always | 100 | 80% writes | 1 | 1 | 6,865 | 0.1653 | 0.1970 | 0.2073 |
| always | 1,000 | 80% writes | 1 | 1 | 4,287 | 0.2620 | 0.3057 | 0.3299 |
| everysec | 100 | 80% writes | 1 | 1 | 26,080 | 0.0350 | 0.0434 | 0.0484 |
| everysec | 1,000 | 80% writes | 1 | 1 | 10,369 | 0.1071 | 0.1181 | 0.1259 |
| no | 100 | 80% writes | 1 | 1 | 26,739 | 0.0344 | 0.0415 | 0.0446 |
| no | 1,000 | 80% writes | 1 | 1 | 10,347 | 0.1062 | 0.1202 | 0.1274 |

The 1,000-key write-heavy AOF growth was 200,332/202,304 bytes per 0.5-second
`always` repeat; 486,736/485,228 for `no`; 1,933,488/1,935,228 per two-second
`everysec` repeat. Across every complete final profile, growth was exactly
116 bytes per changed-value SET in this no-TTL/no-LRU workload. Reads and off
mode added zero journal bytes. This is not a general record-size guarantee for
collections, expirations or LRU metadata.

Native signal-to-snapshot-publication observations for these write-heavy
profiles were approximately 91–103 ms; the polling interval dominates at this
small dataset, so no pure snapshot-write speed is inferred. Local reports live
under ignored `benchmark-results/`; measured figures above are the compact,
reviewable record. No peak RSS, image size or Linux throughput is invented.

## Historical Stage 4A bottleneck observations

With sync disabled, increasing key count from 100 to 1,000 reduced the write-heavy
median from about 26.7k to 10.3k ops/s; off mode stayed near 35k. The Stage 4A
persistent transaction copied the whole database and computed effects. That
known O(database size) path is consistent with the observed persistence scaling
penalty; this experiment does not isolate all allocation/encoding/filesystem costs.
`always` also pays checked journal/watermark/directory synchronization, and is
slower than the weaker policies. Weaker policies retain their documented loss
windows; these numbers are not a recommendation to weaken durability.

Stage 4B below profiles and replaces noeviction staging. Remaining follow-ups: measure
snapshot blocking time on substantially larger datasets; use a lower-overhead
load generator and longer controlled Linux runs to identify the Python-driver
ceiling; define a private deployment memory/stop-grace budget. AOF growth and
retained generations remain operational limits. Compaction/retention are separate
work, not implemented here. No replication or cluster changes.

## Stage 4B: measured local hardening (2026-10-26)

Same Apple M4/10 logical CPUs/arm64/macOS 26.5.2, Apple Clang 17 Release
`-O3 -DNDEBUG`, Python 3.9.6, native loopback TCP. Before: preserved executable
built from accepted main `5e3b3cceaf1fd9c9522567b0a4807f2f7cf604cc` before
storage edits. After: local working tree on `fasttrack/performance-hardening`.
Both reports show that HEAD and `dirty_worktree: true`; their source fingerprints
identify the workspace at measurement time, not the preserved baseline build.
Binary SHA-256 is the executable identity:

- Before: `fa994fadf00161b7884cb2e36e9455810551e4309427d65dcd9002e79f833d95`
- After: `8504d00e9bf3ee452682c9dcf177aefb548e6f565c24db191e2643d63739a179`

Comparable matrices: 24 profiles each (48 timed repeats), SET and 80%-write/20%-GET
at 100/1,000/10,000 keys, off/always/everysec/no persistence, one client,
pipeline one, 9-byte keys, 64-byte values, deterministic uniform keys, 0.25-second
warmup, two seconds/repeat. Dataset value bytes: 6,400/64,000/640,000;
logical bytes: 13,700/137,000/1,370,000. No maxmemory, TTL or eviction in this
throughput experiment. Both matrices completed ~96 measured seconds with no caps.
No builds/tests/profilers ran during the selected TCP measurement windows.
An initial diagnostic overlap was excluded. Figures are medians across repeats;
latencies are median per-run nearest-rank percentiles, in milliseconds.

| Policy | Keys | Workload | ops/s before → after | Ratio | p50 before → after | p95 before → after | p99 before → after |
|---|---:|---|---:|---:|---:|---:|---:|
| off | 100 | set | 35,217 → 35,295 | 1.00× | 0.0241 → 0.0243 | 0.0278 → 0.0275 | 0.0293 → 0.0292 |
| off | 1,000 | set | 35,629 → 35,754 | 1.00× | 0.0235 → 0.0236 | 0.0271 → 0.0270 | 0.0288 → 0.0283 |
| off | 10,000 | set | 35,173 → 35,635 | 1.01× | 0.0238 → 0.0235 | 0.0271 → 0.0269 | 0.0288 → 0.0285 |
| off | 10,000 | write80 | 35,288 → 35,689 | 1.01× | 0.0240 → 0.0237 | 0.0272 → 0.0271 | 0.0290 → 0.0289 |
| always | 100 | set | 5,648 → 5,928 | 1.05× | 0.1690 → 0.1605 | 0.2003 → 0.1915 | 0.2111 → 0.1999 |
| always | 1,000 | set | 3,442 → 5,860 | 1.70× | 0.2808 → 0.1610 | 0.3187 → 0.1945 | 0.3327 → 0.2047 |
| always | 10,000 | set | 921 → 5,887 | 6.39× | 1.0775 → 0.1609 | 1.1078 → 0.1928 | 1.1343 → 0.2047 |
| always | 10,000 | write80 | 1,129 → 7,051 | 6.25× | 1.0764 → 0.1592 | 1.1133 → 0.1923 | 1.1279 → 0.2026 |
| everysec | 100 | set | 23,847 → 31,227 | 1.31× | 0.0363 → 0.0265 | 0.0447 → 0.0342 | 0.0499 → 0.0407 |
| everysec | 1,000 | set | 8,751 → 31,337 | 3.58× | 0.1091 → 0.0266 | 0.1189 → 0.0343 | 0.1252 → 0.0390 |
| everysec | 10,000 | set | 1,088 → 31,026 | 28.52× | 0.9160 → 0.0272 | 0.9426 → 0.0345 | 0.9623 → 0.0396 |
| everysec | 10,000 | write80 | 1,312 → 31,709 | 24.16× | 0.9144 → 0.0266 | 0.9516 → 0.0340 | 0.9726 → 0.0378 |
| no | 100 | set | 23,689 → 31,693 | 1.34× | 0.0369 → 0.0264 | 0.0446 → 0.0338 | 0.0508 → 0.0387 |
| no | 1,000 | set | 8,723 → 31,230 | 3.58× | 0.1095 → 0.0271 | 0.1196 → 0.0341 | 0.1263 → 0.0386 |
| no | 10,000 | set | 1,081 → 30,895 | 28.57× | 0.9169 → 0.0274 | 0.9489 → 0.0344 | 0.9685 → 0.0397 |
| no | 10,000 | write80 | 1,329 → 31,710 | 23.87× | 0.9047 → 0.0267 | 0.9370 → 0.0340 | 0.9559 → 0.0377 |

For 10,000-key SET, median server CPU seconds per two-second window fell from
1.865 to 0.755 (`no`), 1.870 to 0.755 (`everysec`), and 1.845 to 1.595 (`always`).
Persistence-off remained 0.545 seconds. These are coarse cumulative `ps` CPU
boundary differences, including kernel time. End-of-load current RSS was
7,176,192 → 5,586,944 bytes (`no`), 7,192,576 → 6,225,920 (`everysec`), and
7,208,960 → 5,308,416 (`always`). RSS is an instantaneous allocator-dependent
sample, not peak transaction memory or a capacity bound. All measured persistent
runs still added exactly 116 journal bytes per changed-value SET.

### Profiling and design

The small opt-in C++ phase profiler calls the real storage/AOF code, counts
ordinary C++ allocation requests and times storage begin/mutation/effects,
standalone encoding, append (including encoding/CRC), and commit/destruction.
I/O callbacks separately time actual writes/fsync, including final flush.
Standalone encoding is an extra diagnostic pass; do not sum it into real-path
latency. The profiler uses short `kN` keys, 64-byte values and 200 operations,
SET and deterministic write80, all three AOF policies, and both eviction policies
at 100/1,000/10,000 keys. Before links the preserved baseline library and matching
baseline headers; after links the changed library. It excludes TCP/dispatcher
and is not an independent server-throughput result. TCP runs include decode,
argument copies, dispatch and reply construction; these paths were inspected
and remain unchanged.

At 10,000 keys under `no`, SET begin/effects/destruction averaged
396.137/264.916/204.405 µs before, versus 0.056/0.069/0.100 µs after.
Mutation rose from 0.238 to 0.329 µs because it now prepares the undo records;
encoding stayed small (0.161 → 0.111 µs); append was 3.169 → 2.596 µs.
Begin+mutation allocation requests fell from 2,125,568 to 736 bytes per write.
These requested-byte totals distinguish staging allocations from the logical
ledger; they are not simultaneous live bytes or RSS. Collection/value copying
still scales with the affected value, and bucket growth may allocate/retain
capacity. Per-phase timer overhead and short-run noise matter at sub-µs scales.

A separate deterministic expiry profile marks the dataset due and journals one
bounded indexed deletion per turn (100/200/200 turns). At 10,000 keys, before
begin/effects cost 1,187.020/270.737 µs while actual sweep cost 0.256 µs;
after they were 0.078/0.062 µs with 0.386 µs sweep+undo preparation. The earlier
expiry cost was dominated by full staging, not the ordered deadline lookup.
Exact LRU retains its fallback: at 10,000 keys under `no`, before/after begin
was 568.921/575.744 µs, effects 304.737/304.558 µs and standalone V1 LRU encoding
114.563/113.907 µs. No LRU speedup is claimed.

`noeviction` now saves each changed key's first owned preimage and expiry node
before any physical mutation, including lazy/active expiration and expired
capacity victims. Effects compare only touched keys, emit canonical postimages/
tombstones and preserve MSET last-wins atomicity. Rollback erases tentative nodes,
transfers already allocated backup nodes into retained bucket capacity and
restores the original ledger; it neither allocates nor throws. The historical
noexcept internal `clear()` retains its first whole-table state by swapping.
Publication still follows successful append and required sync, before any reply.
Validation/allocation/encoding/write/required-sync failures restore memory;
a journal failure remains fatal and prohibits subsequent acknowledgments.
A complete unacknowledged record may still recover after a failed sync, as before.

Exact allkeys-lru uses the original full-clone path and V1 full-order effects.
Read-only recency remains volatile until the next data commit. Reads that can
expire data and maintenance use the existing frozen-clock transaction boundary;
snapshots remain serialized with commands. AOF/snapshot/watermark formats and
replay policy are unchanged. Remaining limits: LRU full-keyspace cost, large
collection postimages, fsync/watermark latency, Python/closed-loop driver overhead,
and blocking snapshots/I/O. No O(1), Redis-parity or production-throughput claim.

### Reproduction and operational boundaries

```sh
# Preserve an accepted-baseline Release build before editing storage sources.
cmake -S . -B build/stage4b-before -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/stage4b-before --parallel 2
# Run before; rebuild the changed source in build/release, then run after.
python3 benchmarks/tcp_baseline.py --server build/stage4b-before/redisdb-server \
  --scaling --seconds 2 --warmup 0.25 --repeats 2 --output benchmark-results/before-new.json
cmake --preset release -DREDISDB_BUILD_PROFILES=ON
cmake --build --preset release --parallel 2
python3 benchmarks/tcp_baseline.py --server build/release/redisdb-server \
  --scaling --seconds 2 --warmup 0.25 --repeats 2 --output benchmark-results/after-new.json
build/release/redisdb-transaction-profile 10000 200 no noeviction set
build/release/redisdb-transaction-profile 10000 200 no noeviction expire
```

Local raw reports are ignored under `benchmark-results/stage4b-*`; concise figures
above are the reviewable record. The golden V1 fixture includes its accepted
source commit, executable SHA and per-file hashes; it is test data generated by
the old binary, not a newly encoded substitute.

The operational test applies all three AOF policies, five seconds of mixed
four-client load per policy with 512 bounded reconnects, then reused clients,
16-command pipelines and a concurrent
non-reader. It checks sampled RSS/FD bounds where available, malformed/failed
commands, immediate SIGTERM after acknowledged pipelined writes, recovery and
repeated SIGKILL/restart. This short check cannot prove absence of long-term leaks.
Docker now configures one CPU alongside 512 MiB and 64 PIDs, with runtime assertions;
new enforcement and Linux performance/ASan must be validated after publication.
Stage 4A Linux figures used the older CPU configuration and are not directly
comparable. No Docker daemon was started or system configuration changed.

Local validation: Debug's 77 tests passed across the full run and targeted retry
of the new hardening test; Release passed 77/77 in one run; full-program UBSan
passed 78/78 including the pinned 50-seed parser smoke. The transaction test
exhausted 2,115 allocation/rollback trials, including rehash, expiry metadata and
both policies. Two test-harness issues were corrected: an APPEND input exceeded
the codec's own bound, and unlimited connection churn exhausted host TIME_WAIT
ports. Successful unchanged tests were not rerun to address those harness issues.
The 16-case native smoke and 54-case/40-route curated parity with candidate
AOF `always` passed against the pinned Redis 8.10.2 binary; all 41 differential
harness tests passed. The Linux adapter now also enables candidate AOF `always`.
Reproduce the persistent comparison with:

```sh
python3 tests/differential/curated_parity.py --custom build/release/redisdb-server \
  --reference /opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server --appendfsync always \
  --report benchmark-results/persistent-parity-new.json
```

AOF and snapshot generations still grow without automatic compaction/deletion.
Monitor actual disk/RSS and stop cleanly before exhaustion; preserve the matching
AOF, watermark and selected snapshot/CURRENT as one stopped recovery set. A failed
or partial journal is fail-closed: never delete/truncate it to force readiness.
SIGKILL tests process crashes, not power loss; weaker sync policies retain their
loss windows. Backup/retention and deployment capacity remain operator duties.
