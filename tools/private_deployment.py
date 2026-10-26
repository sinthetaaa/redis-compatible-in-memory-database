"""Private Compose checks; inspect is read-only, smoke mutates only UUID-owned data."""
from __future__ import annotations

import argparse
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests/docker')]
from docker_support import DockerResources, LABEL, docker, inspect
from container_test import exchange, snapshot
from resp2_wire import RespValue

COMPOSE = ROOT / 'deploy/compose.private.yaml'
SOURCE = 'https://github.com/sinthetaaa/redis-compatible-in-memory-database'
COMMAND = ['--resp2-server', '--bind', '0.0.0.0', '--port', '6380', '--max-clients', '64',
           '--maxmemory', '67108864', '--maxmemory-policy', 'noeviction', '--appendonly', 'yes',
           '--aof-file', '/data/appendonly.aof', '--appendfsync', 'always',
           '--snapshot-dir', '/data/snapshots']


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def settings(image, revision, volume, port=6380, owner='operator-managed'):
    require(bool(re.fullmatch(r'sha256:[0-9a-f]{64}', image)), 'require immutable local image ID')
    require(bool(re.fullmatch(r'[0-9a-f]{40}', revision)), 'require full 40-hex source revision')
    require(bool(re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_.-]{0,127}', volume)), 'invalid named volume')
    require(port in (0, 6380), 'only production port 6380 or disposable port 0 is allowed')
    env = dict(os.environ)
    env.update(REDISDB_IMAGE_ID=image, REDISDB_REVISION=revision, REDISDB_DATA_VOLUME=volume,
               REDISDB_HOST_PORT=str(port), REDISDB_OWNER_TOKEN=owner)
    return env


def compose(env, project, *args, check=True, timeout=120):
    require(bool(re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,100}', project)), 'invalid Compose project')
    # Explicit file/project; no implicit override files or caller COMPOSE_FILE.
    result = subprocess.run(['docker', 'compose', '--env-file', os.devnull,
                             '--project-name', project, '--file', str(COMPOSE),
                             *args], env=env, text=True, capture_output=True, timeout=timeout)
    if check:
        require(result.returncode == 0, 'Compose failed: ' + result.stderr.strip())
    return result


def validate_config(config, env):
    require(set(config['services']) == {'redisdb'}, 'exactly one service required')
    s = config['services']['redisdb']
    require(s['image'] == env['REDISDB_IMAGE_ID'] and s['pull_policy'] == 'never', 'image pin/pull policy')
    require('build' not in s and s['user'] == '10001:10001', 'build/runtime user mismatch')
    require(s['entrypoint'] == ['/usr/local/bin/redisdb-server'] and s['command'] == COMMAND,
            'server command/persistence boundary changed')
    p = s['ports']
    require(len(p) == 1 and p[0]['host_ip'] == '127.0.0.1' and p[0]['target'] == 6380
            and str(p[0]['published']) == env['REDISDB_HOST_PORT']
            and p[0].get('protocol', 'tcp') == 'tcp', 'unsafe host publication')
    require(s.get('read_only') and not s.get('privileged') and not s.get('cap_add'), 'privileges/rootfs')
    require(s['cap_drop'] == ['ALL'] and s['security_opt'] == ['no-new-privileges:true'], 'capabilities')
    require(not any(k in s for k in ('network_mode', 'pid', 'ipc', 'devices', 'volumes_from',
                                    'configs', 'secrets', 'environment')), 'unexpected host access')
    require(float(s['cpus']) == 1 and int(s['mem_limit']) == 512 * 1024 * 1024
            and s['pids_limit'] == 64, 'resource limits')
    require(s['restart'] == 'unless-stopped' and s['stop_signal'] == 'SIGTERM'
            and s['stop_grace_period'] == '1m0s', 'restart/stop policy')
    require(len(s['volumes']) == 1 and s['volumes'][0]['type'] == 'volume'
            and s['volumes'][0]['source'] == 'data' and s['volumes'][0]['target'] == '/data', 'data mount')
    require(set(config['volumes']) == {'data'} and
            config['volumes']['data'] == {'name': env['REDISDB_DATA_VOLUME'], 'external': True},
            'must use the explicit existing external volume')
    require(set(config['networks']) == {'default'} and set(s['networks']) == {'default'}, 'extra network')
    n = config['networks']['default']
    require(n['driver'] == 'bridge' and not n.get('enable_ipv6')
            and n['driver_opts'] == {'com.docker.network.bridge.gateway_mode_ipv4': 'nat'}, 'network routing')
    require(s['labels']['io.redisdb.deployment.revision'] == env['REDISDB_REVISION']
            and s['labels'][LABEL] == n['labels'][LABEL] == env['REDISDB_OWNER_TOKEN'], 'ownership/revision')
    require(s['healthcheck']['test'][:3] == ['CMD', '/bin/bash', '-ec']
            and s['healthcheck']['timeout'] == '3s', 'bounded real PING healthcheck required')
    require(s['logging'] == {'driver': 'json-file', 'options': {'max-file': '3', 'max-size': '5m'}},
            'bounded logs required')


def preflight(image, revision):
    # Refuse remote Docker contexts before contacting their daemon. Runtime
    # probes must run on the Linux Docker host, not on a laptop controlling it.
    require(platform.system() == 'Linux', 'runtime acceptance requires the actual Linux Docker host')
    endpoint = os.environ.get('DOCKER_HOST')
    if not endpoint or os.environ.get('DOCKER_CONTEXT'):
        endpoint = json.loads(docker('context', 'inspect', '--format', '{{json .Endpoints.docker.Host}}').stdout)
    require(endpoint.startswith('unix://'), 'refusing remote Docker endpoint')
    version = json.loads(docker('version', '--format', '{{json .Server}}').stdout)
    require(version['Os'] == 'linux' and int(version['Version'].split('.')[0]) >= 28,
            'Linux Docker Engine >=28 required')
    v = re.match(r'v?(\d+)\.(\d+)', docker('compose', 'version', '--short').stdout.strip())
    require(v is not None and tuple(map(int, v.groups())) >= (2, 24), 'Compose >=2.24 required')
    metadata = json.loads(docker('image', 'inspect', image).stdout)[0]
    labels = metadata['Config'].get('Labels') or {}
    require(metadata['Id'] == image and metadata['Os'] == 'linux'
            and metadata['Architecture'] == version['Arch'], 'local image identity/OS/architecture mismatch')
    require(labels.get('org.opencontainers.image.revision') == revision
            and labels.get('org.opencontainers.image.source') == SOURCE
            and labels.get('io.redisdb.build.configuration') == 'Release'
            and labels.get('io.redisdb.build.cxx_standard') == '20', 'image source/Release identity mismatch')
    require(metadata['Config']['User'] == '10001:10001', 'image must be non-root')
    print('PASS image availability and exact Release/revision identity (build is a separate step)')
    return {'image_id': image, 'revision': revision, 'image_architecture': metadata['Architecture'],
            'docker_version': version['Version'], 'build': 'external; pinned image inspected'}


def container(env, project):
    ids = compose(env, project, 'ps', '--all', '--quiet', 'redisdb').stdout.split()
    require(len(ids) == 1, 'expected exactly one Compose service container')
    state = inspect(ids[0])
    labels = state['Config']['Labels']
    require(labels.get('com.docker.compose.project') == project
            and labels.get('com.docker.compose.service') == 'redisdb'
            and labels.get(LABEL) == env['REDISDB_OWNER_TOKEN'], 'container ownership mismatch')
    return ids[0], state


def validate_runtime(state, env):
    c, h = state['Config'], state['HostConfig']
    require(state['Image'] == env['REDISDB_IMAGE_ID'] and c['User'] == '10001:10001', 'runtime image/user')
    require(c['Entrypoint'] == ['/usr/local/bin/redisdb-server'] and c['Cmd'] == COMMAND, 'runtime command')
    require(c['Labels'].get('io.redisdb.deployment.revision') == env['REDISDB_REVISION'], 'runtime revision')
    require(h['ReadonlyRootfs'] and not h['Privileged'] and not h.get('CapAdd')
            and h['CapDrop'] == ['ALL'] and set(h['SecurityOpt']) in
            ({'no-new-privileges'}, {'no-new-privileges:true'}), 'runtime privileges')
    require(h['Memory'] == 512 * 1024 * 1024 and h['NanoCpus'] == 1_000_000_000
            and h['PidsLimit'] == 64, 'runtime resource limits')
    require(h['RestartPolicy']['Name'] == 'unless-stopped' and c['StopTimeout'] == 60, 'runtime lifecycle')
    mounts = state['Mounts']
    require(len(mounts) == 1 and mounts[0]['Type'] == 'volume' and mounts[0]['Destination'] == '/data'
            and mounts[0]['Name'] == env['REDISDB_DATA_VOLUME'] and mounts[0]['RW'], 'runtime volume')
    require(set(h['PortBindings']) == {'6380/tcp'}, 'unexpected published container ports')
    hp = h['PortBindings']['6380/tcp']
    require(len(hp) == 1 and hp[0]['HostIp'] == '127.0.0.1', 'unsafe configured publication')
    p = state['NetworkSettings']['Ports']['6380/tcp']
    require(len(p) == 1 and p[0]['HostIp'] == '127.0.0.1', 'unsafe actual publication')
    port = int(p[0]['HostPort'])
    require(0 < port <= 65535 and (env['REDISDB_HOST_PORT'] == '0' or port == 6380), 'unexpected host port')
    require(state['State']['Running'] and state['State'].get('Health', {}).get('Status') == 'healthy',
            'service must be running with a successful real PING healthcheck')
    return port


def network_check(port):
    listeners = subprocess.run(['ss', '-H', '-ltn', f'sport = :{port}'], capture_output=True,
                               text=True, timeout=5, check=True).stdout
    for line in listeners.splitlines():
        require(line.split()[3] == f'127.0.0.1:{port}', 'non-loopback host listener detected')
    interfaces = json.loads(subprocess.run(['ip', '-j', '-4', 'address', 'show', 'scope', 'global'],
                                           capture_output=True, text=True, timeout=5, check=True).stdout)
    addresses = {a['local'] for interface in interfaces for a in interface.get('addr_info', [])
                 if a.get('family') == 'inet' and not ipaddress.ip_address(a['local']).is_loopback}
    require(0 < len(addresses) <= 16, 'need a bounded non-loopback host-address probe')
    for address in sorted(addresses):
        try:
            connection = socket.create_connection((address, port), timeout=1)
        except (ConnectionRefusedError, TimeoutError, socket.timeout):
            continue
        connection.close()
        raise RuntimeError(f'database port reachable on host interface {address}')
    return {'host_interface_probes': sorted(addresses), 'non_loopback_connections': 'blocked_or_refused',
            'ss_listeners': listeners.splitlines(), 'external_firewall': 'not tested',
            'ssh_tunnel': 'not tested'}


def inspect_service(env, project):
    cid, state = container(env, project)
    port = validate_runtime(state, env)
    networks = state['NetworkSettings']['Networks']
    require(set(networks) == {project + '_default'}, 'unexpected attached runtime network')
    network_state = json.loads(docker('network', 'inspect', project + '_default').stdout)[0]
    require(network_state['Driver'] == 'bridge' and not network_state['EnableIPv6']
            and network_state['Options'].get('com.docker.network.bridge.gateway_mode_ipv4') == 'nat'
            and network_state['Labels'].get('com.docker.compose.project') == project
            and network_state['Labels'].get(LABEL) == env['REDISDB_OWNER_TOKEN'], 'runtime network isolation')
    status = docker('exec', cid, 'cat', '/proc/1/status').stdout
    for key in ('Uid:', 'Gid:'):
        require(next(line.split()[1:] for line in status.splitlines() if line.startswith(key))
                == ['10001'] * 4, 'PID 1 identity mismatch')
    require(exchange(port, b'PING') == RespValue('simple', b'PONG'), 'host-loopback PING failed')
    network = network_check(port)
    print('PASS healthy startup, PID 1 identity, resource limits and private host publication')
    return cid, port, network


def up(env, project, recreate=False):
    flags = ['up', '--no-build', '--pull', 'never', '-d', '--wait', '--wait-timeout', '60']
    if recreate:
        flags.append('--force-recreate')
    compose(env, project, *flags, 'redisdb')
    return inspect_service(env, project)


def stop(env, project):
    cid, _ = container(env, project)
    compose(env, project, 'stop', '--timeout', '60', 'redisdb', timeout=75)
    require(inspect(cid)['State']['ExitCode'] == 0 and 'RESP_STOPPED' in docker('logs', cid).stdout,
            'SIGTERM did not finish cleanly')


def check_data(port, number):
    binary = b'\x00\xff\r\n'
    for command, reply in [((b'GET', binary), RespValue('bulk', binary)),
                           ((b'GET', b'n'), RespValue('bulk', str(number).encode())),
                           ((b'LRANGE', b'l', b'0', b'-1'), RespValue('array', (RespValue('bulk', binary),))),
                           ((b'HGET', b'h', binary), RespValue('bulk', binary)),
                           ((b'SISMEMBER', b's', binary), RespValue('integer', 1))]:
        require(exchange(port, *command) == reply, 'persistent value/type mismatch')


def collect_owned(owned, project):
    # Discover even partially created Compose resources. Both labels must match;
    # DockerResources.close rechecks ownership before each exact deletion.
    filters = ['--filter', f'label={LABEL}={owned.owner}',
               '--filter', f'label=com.docker.compose.project={project}']
    ids = docker('ps', '--all', '--quiet', *filters).stdout.split()
    owned.containers.extend(cid for cid in ids if cid not in owned.containers)
    networks = docker('network', 'ls', '--quiet', *filters).stdout.split()
    require(len(networks) <= 1, 'unexpected extra owned Compose networks; refusing broad cleanup')
    if networks:
        owned.network = networks[0]


def owned_volume(owned):
    name = owned.volume()
    metadata = json.loads(docker('volume', 'inspect', name).stdout)[0]
    require(metadata.get('Labels', {}).get(LABEL) == owned.owner,
            'refusing to adopt an existing foreign volume')
    return name


def smoke(image, revision, report):
    with DockerResources() as owned:
        project = 'redisdb-accept-' + owned.owner
        # Fail on even an improbable collision rather than adopting resources.
        filters = ['--filter', f'label=com.docker.compose.project={project}']
        require(not docker('ps', '--all', '--quiet', *filters).stdout.strip()
                and not docker('network', 'ls', '--quiet', *filters).stdout.strip(), 'project collision')
        env = settings(image, revision, owned_volume(owned), 0, owned.owner)
        validate_config(json.loads(compose(env, project, 'config', '--format', 'json').stdout), env)
        try:
            first, port, report['network'] = up(env, project)
            report['cases'].append('actual private Compose startup/health/identity/limits/network')
            binary = b'\x00\xff\r\n'
            for command, reply in [((b'SET', binary, binary), RespValue('simple', b'OK')),
                                   ((b'SET', b'n', b'1'), RespValue('simple', b'OK')),
                                   ((b'RPUSH', b'l', binary), RespValue('integer', 1)),
                                   ((b'HSET', b'h', binary, binary), RespValue('integer', 1)),
                                   ((b'SADD', b's', binary), RespValue('integer', 1))]:
                require(exchange(port, *command) == reply, 'fixture command failed')
            check_data(port, 1)
            report['cases'].append('PING/strings/lists/hashes/sets/binary values')
            report['snapshot_seconds'] = snapshot(first)
            require(exchange(port, b'INCR', b'n') == RespValue('integer', 2), 'AOF suffix failed')
            stop(env, project)
            same, port, _ = up(env, project)
            require(same == first, 'restart unexpectedly recreated the container')
            check_data(port, 2)
            report['cases'].append('SIGUSR1 snapshot plus AOF suffix; clean SIGTERM/restart')
            newer, port, _ = up(env, project, recreate=True)
            require(newer != first, 'container was not recreated')
            check_data(port, 2)
            require(exchange(port, b'INCR', b'n') == RespValue('integer', 3), 'crash fixture write failed')
            report['cases'].append('container recreation preserves owned external volume')
            # Only this disposable container: expose exit 137 deterministically,
            # then recreate with the unchanged production restart policy.
            docker('update', '--restart', 'no', newer)
            docker('kill', '--signal', 'KILL', newer)
            require(docker('wait', newer, timeout=20).stdout.strip() == '137', 'SIGKILL not observed')
            _, port, _ = up(env, project, recreate=True)
            check_data(port, 3)
            report['cases'].append('acknowledged always-policy write survives SIGKILL/recreation')
            stop(env, project)
            source = env['REDISDB_DATA_VOLUME']
            restored = owned_volume(owned)
            mount = [f'type=volume,src={source},dst=/source,readonly']
            owned.helper(image, restored, 'cp -a /source/. /data/', extra_mounts=mount)
            env['REDISDB_DATA_VOLUME'] = restored
            _, port, _ = up(env, project, recreate=True)
            check_data(port, 3)
            stop(env, project)
            report['cases'].append('stopped consistent copy restores into separate owned volume')
            for label, mutation in [('corrupt snapshot', 'printf bad > /data/snapshots/generation-1.snap'),
                                    ('incomplete AOF', 'printf bad >> /data/appendonly.aof'),
                                    ('missing watermark', 'mv /data/appendonly.aof.commit /data/marker.fixture')]:
                fixture = owned_volume(owned)
                owned.helper(image, fixture, 'cp -a /source/. /data/; ' + mutation, extra_mounts=mount)
                hashes = 'cd /data; find . -type f -exec sha256sum {} + | sort'
                before = owned.helper(image, fixture, hashes)
                env['REDISDB_DATA_VOLUME'] = fixture
                compose(env, project, 'create', '--no-build', '--pull', 'never', '--force-recreate', 'redisdb')
                failed, _ = container(env, project)
                docker('update', '--restart', 'no', failed)
                docker('start', failed)
                require(docker('wait', failed, timeout=20).stdout.strip() == '1', label + ' did not fail closed')
                require('RESP_READY' not in docker('logs', failed).stdout, 'invalid state reached readiness')
                require(owned.helper(image, fixture, hashes) == before, 'failed startup changed recovery files')
                report['cases'].append(label + ' rejected without repair or file changes')
            print('PASS Compose persistence/snapshot/restart/crash/backup and isolated corruption checks')
        finally:
            collect_owned(owned, project)
    report['cases'].append('exact owned-resource cleanup')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    for mode in ('config', 'inspect', 'smoke'):
        p = sub.add_parser(mode)
        p.add_argument('--image-id', required=True)
        p.add_argument('--revision', required=True)
        p.add_argument('--report', type=Path)
        if mode != 'smoke':
            p.add_argument('--volume', required=True)
            p.add_argument('--project', default='redisdb-private')
            p.add_argument('--port', type=int, choices=(0, 6380), default=6380)
    args = parser.parse_args()
    if args.report and args.report.exists():
        parser.error('report exists; use a new path')
    report = {'complete': False, 'classification': 'NOT_RUN', 'mode': args.mode,
              'remote_deployment_validated': False, 'cases': []}
    try:
        # Validate arguments before any runtime contact or resource mutation.
        env = settings(args.image_id, args.revision, getattr(args, 'volume', 'argument-check'),
                       getattr(args, 'port', 6380))
        if args.mode == 'config':
            config = json.loads(compose(env, args.project, 'config', '--format', 'json').stdout)
            validate_config(config, env)
            report['cases'].append('offline Compose security/configuration assertions')
            print('PASS offline private Compose configuration; runtime/remote checks NOT RUN')
        else:
            report.update(preflight(args.image_id, args.revision))
            if args.mode == 'inspect':
                validate_config(json.loads(compose(env, args.project, 'config', '--format', 'json').stdout), env)
                _, report['host_port'], report['network'] = inspect_service(env, args.project)
                report['cases'].append('read-only deployment inspection and PING')
            else:
                smoke(args.image_id, args.revision, report)
        report['complete'] = True
        report['classification'] = args.mode.upper() + '_PASS'
        print(f'PASS {len(report["cases"])} checks; SSH tunnel and remote acceptance remain separate')
    except Exception as error:
        report['error'] = str(error)
        report['classification'] = 'FAIL'
        print('FAIL ' + str(error), file=sys.stderr)
        return 1
    finally:
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            with args.report.open('x') as stream:
                json.dump(report, stream, indent=2)
                stream.write('\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
