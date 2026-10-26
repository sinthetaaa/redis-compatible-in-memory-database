# Stage 4A: Linux container packaging and private demo

Implementation is local on `fasttrack/docker-benchmarks`, based on accepted main
`798010ff34ec0c094aa37f7821524277edbb75dd`. Container execution is **not yet
validated**: Docker CLI/Compose are installed locally, but the Desktop daemon
socket is absent. No daemon was started or software installed. The new
`docker-linux.yml` workflow requires a subsequent authorized branch publication
before GitHub can execute it. No Linux image-build, persistence, parity or
performance success is claimed from static inspection or native macOS tests.

## Image and runtime design

The multi-stage Dockerfile pins the Debian bookworm-slim OCI index to
`sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8`
(resolved 2026-10-26). Builder: GCC, CMake >=3.25, Ninja, C++20, Release, tests off,
stripped server. Runtime: matching Debian base, libstdc++/runtime dependencies and
one executable. No compiler, source tree or build cache is copied. Exact image
size remains unmeasured until a real build; the container test reports bytes.
Apt packages follow that Debian suite's updates; base identity is fixed, but
bit-for-bit builds across different apt repository dates are not promised.
`.dockerignore` allows only build inputs and excludes host data/credentials.

The runtime is UID/GID **10001:10001**. `/data/snapshots` is created and owned
by this UID in the image; a new Docker named volume is populated with this
layout. Existing host bind mounts are not automatically chowned. If using one,
prepare its ownership and snapshot directory explicitly for UID 10001 before
starting. Do not mount unrelated user directories or run the service as root to
bypass permission errors.

Exec-form ENTRYPOINT runs the server directly as PID 1. SIGTERM/SIGINT follow the
existing checked shutdown; SIGUSR1 only sets the existing safe snapshot request
flag. Publication happens on the serialized event loop. The default image CMD
selects AOF `always`, `/data/appendonly.aof`, `/data/snapshots`, port 6380, explicit
`0.0.0.0`, and 64 MiB **logical** maxmemory with noeviction. Compose additionally
uses read-only rootfs, all capabilities dropped, no-new-privileges, 64 PIDs,
512 MiB cgroup memory and 30-second stop grace. Logical charge is not RSS;
full-database staging and temporary buffers can still hit the cgroup limit.
The image does not manage retention, backups, log rotation or remote deployment.

## Safe networking

Native `--resp2-server` still binds **127.0.0.1** unless explicitly configured.
`--bind` accepts only the exact IPv4 literals `127.0.0.1` and `0.0.0.0`; no DNS,
IPv6, whitespace, shorthand addresses or arbitrary interfaces. Smoke echo remains
loopback-only. Descriptor ownership/nonblocking/close-on-exec behavior is unchanged.

The image explicitly chooses `0.0.0.0` **inside its network namespace** so Docker
port publishing can work. This is not a host-side firewall or authentication
boundary. Compose publishes **only `127.0.0.1:6380:6380`**, on a project-specific
bridge, with no host networking or privileged mode. Never replace it with
`6380:6380`, `0.0.0.0:6380:6380`, or public cloud ingress for this unauthenticated,
unencrypted database. A trusted Docker host operator can access containers.
Use Docker Engine **>=28**: older engines have a documented localhost-publication
L2 isolation caveat. The automated adapter rejects older/non-Linux engines.
See [Docker port publishing](https://docs.docker.com/engine/network/port-publishing/).

## Build and demonstration

Prerequisites: running Linux Docker Engine >=28 (Docker Desktop's Linux VM is
acceptable), Docker Compose, and `redis-cli` on the host for these demo commands.
No reference Redis server on the host or public network is needed.

```sh
# Local working-tree build; the label explicitly distinguishes local edits.
REDISDB_VCS_REF="$(git rev-parse HEAD)-local" docker compose build
docker compose up -d
docker compose logs redisdb   # wait for RESP_READY 0.0.0.0:6380
redis-cli -h 127.0.0.1 -p 6380 PING
redis-cli -h 127.0.0.1 -p 6380 SET interview:message hello
redis-cli -h 127.0.0.1 -p 6380 GET interview:message
redis-cli -h 127.0.0.1 -p 6380 RPUSH interview:list a b
redis-cli -h 127.0.0.1 -p 6380 LRANGE interview:list 0 -1
redis-cli -h 127.0.0.1 -p 6380 HSET interview:hash field value
redis-cli -h 127.0.0.1 -p 6380 SADD interview:set one two

# Capture a synchronized generation; wait for SNAPSHOT_PUBLISHED in logs.
docker compose kill -s SIGUSR1 redisdb
docker compose logs redisdb
redis-cli -h 127.0.0.1 -p 6380 SET interview:after-snapshot newer

# Checked graceful shutdown, followed by a new container using the same volume.
docker compose stop -t 30
docker compose logs redisdb   # expect RESP_STOPPED and exit 0
docker compose up -d --force-recreate
redis-cli -h 127.0.0.1 -p 6380 GET interview:after-snapshot

# Process-crash demonstration on this demo data only; not a power-loss test.
docker compose kill -s SIGKILL redisdb
docker compose up -d --force-recreate
redis-cli -h 127.0.0.1 -p 6380 GET interview:message

# Stops/removes project containers/network; preserves its named data volume.
docker compose down
```

Compose's `redisdb-stage4a_redisdb-data` volume survives container recreation and
`down`. Do not add `-v`, prune volumes, or manually remove recovery files to get
past errors. Retain the AOF, `.commit`, and selected snapshot/CURRENT from the
same stopped database. See [AOF](AOF.md) and [snapshots](SNAPSHOTS.md) for the full
recovery contract. Corrupt/incomplete selected state or required history rejects
startup; there is no automatic reset or salvage. SIGKILL during a journal append
can leave a partial tail which correctly fails startup. Docker stop can escalate
to SIGKILL if a blocking write/sync exceeds the grace period.

Equivalent direct run, after building/tagging the image:

```sh
docker build --build-arg VCS_REF="$(git rev-parse HEAD)-local" -t redisdb:stage4a .
docker volume create redisdb-stage4a-demo-data
docker run -d --name redisdb-stage4a-demo --read-only --cap-drop ALL \
  --security-opt no-new-privileges --memory 512m --pids-limit 64 \
  -p 127.0.0.1:6380:6380 \
  --mount type=volume,src=redisdb-stage4a-demo-data,dst=/data redisdb:stage4a
docker kill --signal USR1 redisdb-stage4a-demo
docker stop --time 30 redisdb-stage4a-demo
```

## Repeatable container checks and Linux reference

```sh
python3 tests/docker/container_test.py --image redisdb:stage4a \
  --report benchmark-results/container.json
python3 tests/differential/linux_container.py --image redisdb:stage4a \
  --report benchmark-results/linux-parity.json
# Benchmark identity requires a build labelled with exact HEAD (no -local suffix).
docker build --build-arg VCS_REF="$(git rev-parse HEAD)" -t redisdb:stage4a .
python3 benchmarks/tcp_baseline.py --image redisdb:stage4a --seconds 2 \
  --warmup 0.25 --repeats 2 --output benchmark-results/linux-benchmark.json
```

Reports are bounded JSON, not acceptance evidence until these commands actually
pass. Helpers own UUID-named, labelled containers, volumes and a private bridge;
fixture mutation and cleanup apply only to resources created by that invocation.
Cleanup verifies labels and removes exact resources, never global prune or user
volumes. Corruption/missing-file tests copy the helper's stopped database into
new disposable volumes first. Non-root helpers use no network for fixture edits.

The container suite covers runtime hygiene, host-localhost Redis CLI access,
PING/strings/lists/hashes/sets and binary values, snapshot signal/publication,
snapshot plus journal suffix, clean SIGTERM, acknowledged `always` writes after
SIGKILL, repeated recovery, independent empty volumes, and rejected missing
snapshot/journal/watermark and corrupt snapshot/incomplete AOF fixtures. Other
AOF policy semantics remain covered by the existing actual-process suite; the
benchmark also runs all three policies inside the container when CI executes.

`docker/reference.json` separately freezes the official Redis **8.10.2-alpine**
OCI index, per-platform amd64/arm64 manifests/config identities, binary-containing
layers and executable hashes. These were independently obtained from Docker Hub
and verified against SHA-256 of the downloaded OCI bytes, without execution.
The Linux adapter pulls an exact platform digest, checks Linux architecture,
RepoDigest, image/config ID, executable hash, `--version`, and owned PID 1's
`INFO SERVER` version. It reuses the 54-case/40-route curated trace with two
clients. Replies compare exact wire bytes except the existing duplicate-checked
unordered SMEMBERS/HGETALL semantics. Raw request/reply hex and failures are
recorded, and mismatch/infrastructure/identity errors exit nonzero. No floating
fallback or unverified distro Redis server. The macOS executable pin and harness
are untouched. Version/digest/hash changes require reviewed repinning, never an
automatic floating-tag refresh. Linux parity remains **unexecuted** until this adapter passes.

The new GitHub-hosted Ubuntu workflow builds the image, runs these checks and
records image size, exact reference identity and bounded baseline JSON/job summary.
The existing Debug/Release and ASan/UBSan/libFuzzer workflows are unchanged.
No images are pushed, deployments provisioned, or paid resources created.

## Local validation checkpoint

On 2026-10-26 the complete native Debug suite passed **75/75**, Release **75/75**,
and full-program Apple Clang 17 UBSan **76/76**, including the pinned parser seed
corpus. The new listener test checks actual `getsockname` addresses and descriptor
flags; the CLI test exercises default loopback, explicit wildcard, invalid forms
and graceful shutdown. Five offline Docker-tooling identity/safety/method checks passed.
The existing pinned macOS Redis differential trace passed **54 cases / 40 routes**.
Compose rendered successfully with the sole host binding `127.0.0.1`.
The existing documented local Apple ASan runtime limitation remains; Linux
ASan/UBSan/fuzz execution is pending publication alongside Docker execution.
