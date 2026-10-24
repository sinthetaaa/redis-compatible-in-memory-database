# Phase 6G.1A — Differential harness foundation (local draft)

Source-of-truth contract: `docs/PHASE6G_DIFFERENTIAL_CONTRACT.md` (Redis 8.10.2).
Route matrix: `tests/differential/command_matrix.csv` (40 registered commands).

This checkpoint deliberately adds only **stdlib Python** infrastructure, not a
new Redis command, C++ source modification, CMake/CTest registration or CI job.
Its eight offline codec unit tests and 16-case native, owned-process smoke are
an engineering *foundation*, not the 40-route differential acceptance suite.

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
Linux 8.10.2 OCI digest resolution and isolated reference-container adapter
remain future separate checkpoints.

Local offline check: `python3 -m unittest discover -s tests/differential -p test_foundation.py -v`.

Native smoke after a verified Debug build:
`python3 tests/differential/native_smoke.py --custom build/debug/redisdb-server --reference /opt/homebrew/Cellar/redis/8.10.2_1/bin/redis-server --report /path/outside/repo/smoke.json`.

**Gate:** Passing a smoke alone does not certify parity across 40 commands,
long randomized traces, TTL races, Linux, or mismatch replay; those remain
Phase 6G follow-on milestones. All files are kept local and uncommitted at
6G.1A until the independent evidence review.
