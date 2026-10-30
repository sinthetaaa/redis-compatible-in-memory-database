# Differential testing

Comparison contract: [differential comparison boundaries](../../docs/ARCHITECTURE.md#differential-comparison-boundaries)
(Redis 8.10.2).
Route matrix: [command_matrix.csv](command_matrix.csv) (40 registered commands).

The **stdlib Python** harness includes offline codec tests, native owned-process
smoke and curated comparisons, bounded mixed traces, TTL checks and a separate
Linux container adapter. The native smoke is a limited check; the curated trace
exercises the constrained 40-route subset. Neither implies complete Redis parity.

The RESP2 module preserves `bytes`, nil-vs-empty and type distinctions. It
compares positionally except for duplicate-checked `SMEMBERS` and `HGETALL`
order. The smoke launches a **new custom server child** on ephemeral loopback
port and a **new reference Redis 8.10.2 child** on a checked ephemeral loopback
port, then checks reference `INFO SERVER`'s PID/version against the owned child.
The INFO probe is **reference-only**. Both subprocesses are terminated using
their exact `Popen` handles. No `FLUSHALL`, `FLUSHDB`, `SHUTDOWN`, global Redis
service, port 6379, repo source or user data is touched. The custom process
uses its existing `--resp2-server --port 0` readiness line.

The approved native reference executable is a Homebrew Redis 8.10.2 binary
with SHA-256 `8640ea31c7ece0c823fdf24ea53b8d74fda99d2a129dd69b3cd043cbf6c08176`.
A changed binary/version fails closed. The native smoke is designed for the
user's macOS ARM64 host; do not invoke this exact Mac binary pin on Linux.
The implemented Linux adapter verifies separate platform-specific OCI/image,
executable and runtime identities from [docker/reference.json](../../docker/reference.json).
It does not reuse the macOS executable hash. See the
[Docker validation guide](../../docs/DOCKER.md#repeatable-container-checks-and-linux-reference)
for image build and Linux comparison commands. The adapter owns its disposable
containers, volumes and private bridge; cleanup is restricted to those exact
labelled resources. It runs the curated command trace and a separate operational
comparison against the pinned Redis 8.10.2 reference.

Local offline check: `python3 -m unittest discover -s tests/differential -p test_foundation.py -v`.

Native smoke after a verified Debug build:
`python3 tests/differential/native_smoke.py --custom build/debug/redisdb-server --reference /opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server --report /path/outside/repo/smoke.json`.

Passing a smoke alone does not certify the curated subset, longer mixed traces,
TTL boundaries, Linux execution or replay. Run the corresponding suites and
inspect their reports; the presence of a harness is not execution evidence.

## Bounded mixed-command campaign (native macOS)

`mixed_campaign.py` reuses the owned-process runner, pinned Redis and exact
RESP2 replay format. Generator `phase6g-mixed-v1` runs seeds
`0, 1, 7, 42, 99, 123, 256, 1024, 2026, 65535`, each with exactly 100 commands:
8 setup, 60 mixed operations and 32 complete final-content probes. Two clients
alternate sequentially; each seed covers all 39 midstream command names.
Binary input, duplicate arguments, wrong types and type replacement are included.
Requests and worst-case replies stay below 4096 bytes.

Run with a fresh output directory outside the repository:

```sh
python3 tests/differential/mixed_campaign.py --custom build/debug/redisdb-server --reference /opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server --report-dir /tmp/redisdb-mixed-run
python3 tests/differential/mixed_campaign.py --custom build/debug/redisdb-server --reference /opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server --report-dir /tmp/redisdb-mixed-replay --replay /tmp/redisdb-mixed-run/seed-42.json
python3 -m unittest discover -s tests/differential -p 'test_*.py' -v
```

Each seed report preserves the full raw trace on success or failure, seed,
generator version, trace digest, Git HEAD, platform, reference fingerprint,
raw replies, first failing index and owned-child cleanup. Replay reads stored
request bytes without calling the generator; existing reports are never replaced.
Failure exits nonzero and stops the campaign without reducing its planned budget.

Exclusions are recorded separately in every seed report: `QUIT` belongs to the
curated termination suite; positive TTLs and natural expiry use `negative_ttl.py`
clock policies. Mixed traces use plain `SET` and zero/negative expiry only, so
`TTL`/`PTTL` state codes compare exactly. Redis-only routes, advanced options,
unapproved numeric error cases, oversized data and protocol fuzzing are excluded.
Native mixed-campaign results do not by themselves validate Linux execution;
the separate container adapter and Linux CI cover their own stated workloads.
No complete Redis or full engineering-roadmap acceptance is claimed here.
