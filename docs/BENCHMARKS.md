# Stage 4A TCP performance baseline

These are **native macOS observations**, not Linux/container results, Redis
throughput parity, a capacity guarantee, or deployment sizing advice. The local
Docker daemon is unavailable; a separate Linux container workflow is prepared
but cannot run until the branch is authorized for publication.

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
all replies. A shared barrier starts clients together. Throughput divides all
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

## Bottlenecks and Stage 4B candidates

With sync disabled, increasing key count from 100 to 1,000 reduced the write-heavy
median from about 26.7k to 10.3k ops/s; off mode stayed near 35k. The existing
persistent transaction copies the whole database and computes effects. That
known O(database size) path is consistent with the observed persistence scaling
penalty; this experiment does not isolate all allocation/encoding/filesystem costs.
`always` also pays checked journal/watermark/directory synchronization, and is
slower than the weaker policies. Weaker policies retain their documented loss
windows; these numbers are not a recommendation to weaken durability.

Suggested measured follow-ups: profile allocation, cloning/effect comparison and
sync time separately; replace whole-database staging with bounded per-command
transaction preparation while preserving rollback/publication semantics; measure
snapshot blocking time on substantially larger datasets; use a lower-overhead
load generator and longer controlled Linux runs to identify the Python-driver
ceiling; define a private deployment memory/stop-grace budget. AOF growth and
retained generations remain operational limits. Compaction/retention are separate
work, not implemented here. No replication or cluster changes.
