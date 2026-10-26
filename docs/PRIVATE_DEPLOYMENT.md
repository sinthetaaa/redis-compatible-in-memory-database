# Private Linux deployment runbook — Stage 5A preparation

**Remote deployment is not accepted yet.** Runtime source baseline:
`d69b3ede7405b4fe843ffb1c3bbfe4d957810615` (Stage 4B, merged and Linux-validated).
Stage 5A adds configuration/tooling, locally prepared on `fasttrack/private-deployment`.
New Compose acceptance must run in Linux CI after authorized publication. Stage 5B
must then test an explicitly approved real host, persistent disk and SSH tunnel.
No host, credentials, registry upload or paid infrastructure is assumed here.

## Host and access prerequisites

Use a dedicated, supported Linux host; Ubuntu 24.04 amd64 matches CI. Start with
2 vCPUs, 2 GiB RAM and at least 10 GiB free persistent SSD space for a small demo;
prefer 4 GiB RAM when compiling on that same host. These are preparation budgets,
not measured production capacity. The container is limited to one CPU/512 MiB/64
PIDs, 64 clients and 64 MiB **logical** data. RSS, buffers, staging and disk history
are outside the logical ledger. Sustained writes require a separate disk budget.
Use persistent storage supporting advisory locks, atomic rename and file/directory
fsync; an ephemeral VM disk or tmpfs is unsuitable. Keep wall time synchronized for
absolute persisted TTL deadlines. Actual filesystem/device durability needs host validation.

Install Docker Engine **>=28** and Compose plugin **>=2.24** following the
[official Ubuntu repository instructions](https://docs.docker.com/engine/install/ubuntu/).
Use Python 3, `iproute2` (`ip`, `ss`), Git and `redis-tools` (client only).
Do not install a competing Redis server. Confirm a local Unix-socket Docker context;
the checker refuses remote contexts and requires execution on the actual Linux host.
Docker access grants substantial host privileges; allow only trusted operators.

Configure SSH key access, verify the host-key fingerprint through a trusted channel,
and restrict TCP 22 to trusted source CIDRs at the provider and host firewall.
Only disable password access after verifying key access and retaining console recovery.
**Never open TCP 6380 in any public security group/firewall, including IPv6.**
Docker publishing can bypass ordinary host firewall rules; localhost publication
must be explicit. See [Docker port publishing](https://docs.docker.com/engine/network/port-publishing/).
Do not enable direct routing, routed gateway modes, host networking or public
forwarding to the bridge. Trusted host/Docker users can still access this service.

## Architecture and immutable identity

`deploy/compose.private.yaml` runs one server on an isolated project bridge (IPv4
NAT). It binds `0.0.0.0` **inside the container**, publishes **127.0.0.1:6380 only**,
and mounts an explicitly named, existing external volume at `/data`. Native default
binding remains loopback. User/group 10001, read-only rootfs, dropped capabilities
and no-new-privileges apply. A real RESP PING healthcheck uses existing runtime tools;
an unhealthy container is diagnostic, not a reason to erase data or auto-repair.
Logs rotate at 3 × 5 MiB. SIGTERM has 60 seconds; stalled storage can still exceed it.

The command explicitly selects AOF `always`, snapshots, noeviction and bounded
clients/data. The `unless-stopped` restart policy supports host/daemon restart while
respecting an operator stop; it does not cure invalid persistence. Stop a repeatedly
failing service and inspect logs. See [restart policy semantics](https://docs.docker.com/engine/containers/start-containers-automatically/).
Startup still validates existing V1 files before readiness; no initialization script
deletes, truncates, chowns or repairs an existing recovery set.

Images built by CI have not been published to a registry. There is no remote image
tag to pull. Build the reviewed runtime source with the existing digest-pinned
Dockerfile, then record its immutable local image ID. Apt dependencies can change;
the source SHA alone does not promise a bit-identical rebuild. Never invent/reuse
an ephemeral CI image ID as if it were available on another host.

On the **later approved host**, use a reviewed checkout containing these tools:

```sh
set -eu
CONTROL_SHA=REVIEWED_STAGE5A_COMMIT  # replace with the authorized full Git SHA
git clone https://github.com/sinthetaaa/redis-compatible-in-memory-database.git redisdb-control
cd redisdb-control
git switch --detach "$CONTROL_SHA"
export REDISDB_REVISION=d69b3ede7405b4fe843ffb1c3bbfe4d957810615
git worktree add --detach ../redisdb-runtime "$REDISDB_REVISION"
test -z "$(git -C ../redisdb-runtime status --porcelain)"
docker build --build-arg VCS_REF="$REDISDB_REVISION" \
  -t "redisdb:$REDISDB_REVISION" ../redisdb-runtime
export REDISDB_IMAGE_ID="$(docker image inspect "redisdb:$REDISDB_REVISION" --format '{{.Id}}')"
```

Build success, image identity, startup and recovery are distinct gates. The checker
validates the local image ID and its full source/Release labels; it does not build
an image or authenticate arbitrary operator-supplied labels. Use the trusted build
above. CI independently builds and tests its own exact workflow SHA.

## Disposable acceptance before using real data

```sh
python3 tools/private_deployment.py smoke --image-id "$REDISDB_IMAGE_ID" \
  --revision "$REDISDB_REVISION" --report benchmark-results/host-compose-acceptance.json
```

This uses the actual private Compose file, a UUID project/owned external volumes
and an assigned **loopback-only** host port. It checks health, PID 1 identity,
limits, image/revision, PING, strings/lists/hashes/sets and binary values, snapshot
plus AOF suffix, clean stop/restart, recreation, SIGKILL, stopped copy/restore and
corrupt/missing/incomplete recovery rejection without file changes. Fixture mutations
apply only to fresh owned copies. Crash/error cases temporarily disable automatic
restart on their disposable container to observe exit status, then recreate with
the configured policy. Cleanup verifies owner labels and removes exact disposable
resources; it never prunes or adopts a user volume. A failed cleanup exits nonzero.

Read-only inspection checks Docker mappings, host `ss` listeners, loopback PING and
refused TCP connections to the host's own non-loopback IPv4 addresses. This is not
an external firewall/IPv6 routing audit, nor an SSH-tunnel test. The JSON explicitly
keeps `remote_deployment_validated: false`; remote acceptance needs the evidence below.

## Initial dedicated demo deployment

Use a **fresh dedicated demo volume** for the following mutating demonstration.
For a service holding real user data, use read-only inspection and the disposable
smoke instead; do not run the demo writes, crash or corruption procedure on it.

```sh
set -eu
test ! -e ../redisdb-private.env  # do not replace an existing deployment's settings
export REDISDB_DATA_VOLUME="redisdb-private-$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
export REDISDB_HOST_PORT=6380
docker volume create --label io.redisdb.purpose=private-demo "$REDISDB_DATA_VOLUME"
python3 tools/private_deployment.py config --image-id "$REDISDB_IMAGE_ID" \
  --revision "$REDISDB_REVISION" --volume "$REDISDB_DATA_VOLUME"
# Persist these four public settings outside the checkout for the next login.
umask 077
(set -C; printf 'REDISDB_IMAGE_ID=%s\nREDISDB_REVISION=%s\nREDISDB_DATA_VOLUME=%s\nREDISDB_HOST_PORT=6380\n' \
  "$REDISDB_IMAGE_ID" "$REDISDB_REVISION" "$REDISDB_DATA_VOLUME" > ../redisdb-private.env)
dc() { docker compose --env-file ../redisdb-private.env --project-name redisdb-private \
       --file deploy/compose.private.yaml "$@"; }
dc up --no-build --pull never -d --wait --wait-timeout 120
python3 tools/private_deployment.py inspect --image-id "$REDISDB_IMAGE_ID" \
  --revision "$REDISDB_REVISION" --volume "$REDISDB_DATA_VOLUME"
```

New named volumes receive `/data` and `/data/snapshots` ownership from the image
(10001:10001). Existing volumes must already have compatible trusted ownership;
failure never triggers a recursive chown or reset. Do not substitute a host bind
mount or start as root to bypass errors. An absent external volume fails startup
instead of silently allocating an empty database. Retain the environment file;
never generate a new volume name as an attempted recovery fix.
An older AOF-only volume also needs a planned, stopped snapshot-directory migration
following [SNAPSHOTS.md](SNAPSHOTS.md); this deployment does not initialize it automatically.
On later logins, return to this control checkout and load only your own trusted
operator settings with `set -a; . ../redisdb-private.env; set +a`, then define `dc`
as above. No environment file or credentials are checked into Git.

## SSH-tunnel demonstration (Stage 5B evidence)

On the operator's laptop, with its SSH agent/key and verified host key:

```sh
ssh -N -o ExitOnForwardFailure=yes -o StrictHostKeyChecking=yes \
  -L 127.0.0.1:16380:127.0.0.1:6380 USER@HOST
# Short form with default local-loopback forwarding:
# ssh -N -L 16380:127.0.0.1:6380 USER@HOST
```

In another laptop terminal (fresh dedicated demo data only):

```sh
redis-cli -h 127.0.0.1 -p 16380 PING
redis-cli -h 127.0.0.1 -p 16380 SET demo:message before-snapshot
redis-cli -h 127.0.0.1 -p 16380 GET demo:message
redis-cli -h 127.0.0.1 -p 16380 RPUSH demo:list a b
redis-cli -h 127.0.0.1 -p 16380 LRANGE demo:list 0 -1
redis-cli -h 127.0.0.1 -p 16380 HSET demo:hash field value
redis-cli -h 127.0.0.1 -p 16380 HGET demo:hash field
redis-cli -h 127.0.0.1 -p 16380 SADD demo:set member
redis-cli -h 127.0.0.1 -p 16380 SISMEMBER demo:set member
python3 -c 'import sys; sys.stdout.buffer.write(bytes([0,255,13,10]))' | \
  redis-cli -h 127.0.0.1 -p 16380 -x SET demo:binary
python3 - <<'PY'
import subprocess
assert subprocess.check_output(['redis-cli', '-h', '127.0.0.1', '-p', '16380',
                                '--raw', 'GET', 'demo:binary']) == bytes([0,255,13,10,10])
print('PASS binary-safe tunnel value')
PY
```

On the approved host, `dc kill -s SIGUSR1 redisdb`; wait for a **new**
`SNAPSHOT_PUBLISHED` in `dc logs --tail 20 redisdb`. A sent signal alone is not success.
Through the tunnel SET `demo:after-snapshot newer`. Then on the host:

```sh
dc stop --timeout 60 redisdb     # require RESP_STOPPED and exit 0 in logs/inspect
dc up --no-build --pull never -d --wait --wait-timeout 120
dc up --no-build --pull never -d --force-recreate --wait --wait-timeout 120
dc kill -s SIGKILL redisdb       # dedicated demo only, after acknowledged writes
dc up --no-build --pull never -d --wait --wait-timeout 120
```

After each restart/recreation, recheck both messages, all collection values and
the binary value through port 16380. Check the same volume/image IDs on the host.
SIGKILL is a process crash, not power loss. If an incomplete tail rejects startup,
stop and preserve it; never truncate/repair it to make the demonstration pass.
From an approved external client, `nc -vz -w3 HOST 6380` must fail; also review
provider IPv4/IPv6 rules. Do not open the port to make this probe or tunnel work.

## Logs, capacity and stopped backups

Use `dc ps --all`, `dc logs --tail 100 redisdb`, `docker inspect "$(dc ps -q redisdb)"`
and `docker volume inspect "$REDISDB_DATA_VOLUME"`. `inspect` mode above performs
the identity/network/PING check without writing keys or changing lifecycle.
Read disk usage with a non-root helper (safe even while serving; not a backup):

```sh
docker run --rm --network none --read-only --user 10001:10001 --cap-drop ALL \
  --security-opt no-new-privileges --cpus .25 --memory 64m --pids-limit 16 \
  --mount "type=volume,src=$REDISDB_DATA_VOLUME,dst=/data,readonly" \
  --entrypoint /bin/sh "$REDISDB_IMAGE_ID" -ec 'df -h /data; df -i /data; du -sh /data /data/snapshots'
```

Monitor free bytes **and inodes**, host/container RSS, health, restart count and
latency. Establish alerts and a stop-before-exhaustion threshold (for example 20%
free disk plus room for the next snapshot and backup). AOF, old generations and
crash leftovers grow without automatic compaction/deletion. Log rotation does not
bound database disk growth. Do not remove generations or AOF bytes as maintenance.

For a consistent backup: stop all writers, require exit 0/RESP_STOPPED and verify
the container is stopped. Copy the **entire** `/data` tree while stopped: AOF,
`.commit`, snapshot `CURRENT`, its selected generation, all retained history and
other recovery files together. Never copy changing files from a running server.
Choose a new trusted absolute backup directory; the helper must write as UID 10001:

```sh
set -eu
dc stop --timeout 60 redisdb
CID="$(dc ps --all --quiet redisdb)"
test "$(docker inspect "$CID" --format '{{.State.Running}}')" = false
test "$(docker inspect "$CID" --format '{{.State.ExitCode}}')" = 0
test "$(docker logs "$CID" | tail -n 1)" = RESP_STOPPED
BACKUP_DIR=/srv/redisdb/backups/UNIQUE_STOPPED_BACKUP
test ! -e "$BACKUP_DIR"
sudo install -d -m 0700 -o 10001 -g 10001 "$BACKUP_DIR"
docker run --rm --network none --read-only --user 10001:10001 --cap-drop ALL \
  --security-opt no-new-privileges --cpus .25 --memory 64m --pids-limit 16 \
  --mount "type=volume,src=$REDISDB_DATA_VOLUME,dst=/data,readonly" \
  --mount "type=bind,src=$BACKUP_DIR,dst=/backup" --entrypoint /bin/bash "$REDISDB_IMAGE_ID" \
  -ec 'set -o pipefail; cp -a /data/. /backup/; cd /backup; find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS'
```

Record image ID, full source/control SHAs, command/options, volume name and backup
time alongside the backup. Keep an independent protected/off-host copy under an
approved backup policy; disk persistence on one host is not a backup strategy.

Restore into a **new** disposable named volume and different project, never over
the original. With `RESTORE_VOLUME` a fresh UUID name, create it, then:

```sh
RESTORE_TOKEN="$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
RESTORE_VOLUME="redisdb-restore-$RESTORE_TOKEN"
RESTORE_PROJECT="redisdb-restore-$RESTORE_TOKEN"
docker volume create --label io.redisdb.purpose=restore-validation "$RESTORE_VOLUME"
docker run --rm --network none --read-only --user 10001:10001 --cap-drop ALL \
  --security-opt no-new-privileges --cpus .25 --memory 64m --pids-limit 16 \
  --mount "type=bind,src=$BACKUP_DIR,dst=/backup,readonly" \
  --mount "type=volume,src=$RESTORE_VOLUME,dst=/data" --entrypoint /bin/sh "$REDISDB_IMAGE_ID" \
  -ec 'cd /backup; sha256sum -c SHA256SUMS; cp -a /backup/. /data/'
REDISDB_DATA_VOLUME="$RESTORE_VOLUME" REDISDB_HOST_PORT=0 docker compose \
  --env-file ../redisdb-private.env --project-name "$RESTORE_PROJECT" \
  --file deploy/compose.private.yaml up --no-build --pull never -d --wait --wait-timeout 120
python3 tools/private_deployment.py inspect --image-id "$REDISDB_IMAGE_ID" \
  --revision "$REDISDB_REVISION" --volume "$RESTORE_VOLUME" --port 0 --project "$RESTORE_PROJECT"
```

Use the reported loopback port for read-only sentinel/value checks. Stop/down that
exact restore project afterward; its external volume is retained. The disposable
smoke also validates stopped-copy restore and corruption handling on its own data.
Use the same `REDISDB_DATA_VOLUME="$RESTORE_VOLUME" REDISDB_HOST_PORT=0` prefix,
environment file, Compose file and `--project-name "$RESTORE_PROJECT"` for that
project's `stop --timeout 60 redisdb` and `down`; never substitute the live project.

## Updates, rollback and shutdown

Retain the previous image ID/revision, volume setting and stopped backup. Build
the new reviewed source separately, run disposable acceptance, stop the service,
take a consistent backup, then change only the image ID/revision in the operator
environment file. Keep the same volume, snapshot directory and AOF policy. Start
with `--pull never --wait`, verify health/identity/recovered sentinels, then admit clients.

If startup fails, stop retries (`dc stop`) and retain logs plus all current recovery
files. Test the previous image against a stopped **copy** of the current failed
recovery set before selecting it for rollback. Version 1 is unchanged here, but
future backward compatibility must be demonstrated. Do not silently restore an
older backup: it can discard acknowledged writes. If validation fails, remain
stopped for an explicit operator recovery decision. No automatic salvage exists.

For planned shutdown: stop clients/tunnel, `dc stop --timeout 60`, verify clean
exit, then `dc down` to remove service/network while retaining its external volume.
Do not use global prune, volume deletion or recovery-file removal. Only the smoke
automatically removes resources carrying its fresh UUID ownership label.

## Evidence required before accepting real deployment

Record an approved host/OS/architecture and cost authorization (if any), trusted
SSH user/key **reference** and host-key fingerprint (never the private key), ingress
rules, persistent disk/volume ownership/free space, exact source/control SHAs and
actual image ID. Run the Linux CI and disposable acceptance, real service inspection,
SSH PING/type/binary demo, same-volume restart/recreation/crash recovery, external
non-exposure check and stopped backup/restore validation on dedicated demo data.
Retain the successful outputs and operator recovery/cleanup plan. CI is not remote
host acceptance. No Stage 5B/Phase 16 completion is claimed until that evidence exists.

Local Stage 5A checkpoint: 17 offline deployment guards and five existing Docker
tooling tests passed; Compose rendered for fixed and assigned loopback ports.
The health command's exact wire request, bad-reply rejection and actual PING against
an owned native server passed. Workflow/runbook shell and embedded Python syntax
were checked. Docker startup, new Compose recovery/restore and remote SSH checks
are **not executed locally**: the daemon is unavailable. Linux CI is prepared but
cannot exercise these unpublished files before authorized commit/push.
