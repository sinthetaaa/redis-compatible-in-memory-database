"""Separate immutable-OCI Linux adapter; native_smoke's macOS binary pin is unchanged."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from docker_support import DockerResources, connect, docker, inspect
from curated_parity import curated_trace
from resp2_wire import RespReader, comparable, encode_command


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(image, report):
    contract = json.loads((ROOT / 'docker/reference.json').read_text())
    report.update({'cases': [], 'passed': False, 'classification': 'HARNESS_INFRA_FAILURE'})
    with DockerResources() as owned:
        if owned.architecture not in contract['platforms']:
            raise ValueError('unsupported Linux reference architecture')
        pin = contract['platforms'][owned.architecture]
        digest = pin['manifest_digest']
        if not re.fullmatch('sha256:[0-9a-f]{64}', digest):
            raise ValueError('invalid frozen platform manifest')
        reference = 'redis:8.10.2-alpine@' + digest
        report['reference_image'] = reference
        docker('pull', reference, timeout=180)
        metadata = json.loads(docker('image', 'inspect', reference).stdout)[0]
        if (metadata['Os'] != 'linux' or metadata['Architecture'] != owned.architecture or
            metadata['Id'] != pin['config_digest'] or
            not any(item.endswith('@' + digest) for item in metadata['RepoDigests'])):
            raise ValueError('reference image manifest/platform/config identity mismatch')
        ref, ref_port = owned.start(reference, owned.volume(),
            ['redis-server', '--bind', '0.0.0.0', '--port', '6379', '--protected-mode', 'no',
             '--save', '', '--appendonly', 'no', '--daemonize', 'no', '--dir', '/tmp'],
             port=6379, user='999:999')
        require(inspect(ref)['Image'] == metadata['Id'], 'running reference image ID mismatch')
        report.update({'reference_image_id': metadata['Id'],
                       'reference_architecture': metadata['Architecture']})
        report['classification'] = 'REFERENCE_OR_PROTOCOL_FAILURE'
        version = docker('exec', ref, 'redis-server', '--version').stdout.strip()
        require(re.search(r'\bv=8\.10\.2\b', version), 'reference --version mismatch: ' + version)
        executable = docker('exec', ref, 'sha256sum', '/usr/local/bin/redis-server').stdout.split()[0]
        require(executable == pin['executable_sha256'], 'Linux reference executable hash mismatch')
        with connect(ref_port) as sock:
            sock.sendall(encode_command((b'INFO', b'SERVER')))
            info = RespReader(sock).read_reply()[0]
            require(info.kind == 'bulk', 'reference INFO must be a bulk reply')
            fields = dict(line.split(b':', 1) for line in info.value.splitlines() if b':' in line)
            require(fields.get(b'redis_version') == contract['version'].encode(), 'reference INFO version mismatch')
            require(fields.get(b'process_id') == b'1', 'reference endpoint is not owned Redis PID 1')
        report.update({'reference_executable_sha256': executable, 'reference_version_output': version})
        report['classification'] = 'HARNESS_INFRA_FAILURE'
        candidate, port = owned.start(image, owned.volume(),
            ['--resp2-server', '--bind', '0.0.0.0', '--port', '6380',
             '--appendonly', 'yes', '--aof-file', '/data/appendonly.aof', '--appendfsync', 'always'])
        sockets = {}
        try:
            for client in ('a', 'b'):
                for name, endpoint in (('custom', port), ('reference', ref_port)):
                    sock = connect(endpoint)
                    sockets[client, name] = (sock, RespReader(sock))
            trace = curated_trace()
            require(len({command[0].upper() for _, command in trace}) == 40, 'curated route inventory mismatch')
            report['trace_sha256'] = hashlib.sha256(b''.join(encode_command(c) for _, c in trace)).hexdigest()
            for client, command in trace:
                report['classification'] = 'REFERENCE_OR_PROTOCOL_FAILURE'
                replies = []
                for name in ('custom', 'reference'):
                    sock, reader = sockets[client, name]
                    sock.sendall(encode_command(command))
                    replies.append(reader.read_reply())
                if command[0].upper() in (b'SMEMBERS', b'HGETALL'):
                    match = comparable(replies[0][0], command[0]) == comparable(replies[1][0], command[0])
                else:
                    match = replies[0][1] == replies[1][1]
                report['cases'].append({'client': client, 'request_hex': encode_command(command).hex(),
                    'custom_reply_hex': replies[0][1].hex(), 'reference_reply_hex': replies[1][1].hex(),
                    'match': match})
                if not match:
                    report['classification'] = 'PARITY_MISMATCH'
                    raise AssertionError('Linux binary-exact parity mismatch at case ' + str(len(report['cases'])))
        finally:
            for sock, _ in sockets.values():
                sock.close()
        docker('stop', '--time', '20', candidate, ref)
    report.update({'routes': 40, 'comparison': 'binary exact; duplicate-checked canonicalization only for SMEMBERS/HGETALL',
                   'persistence': 'candidate always; reference off', 'passed': True, 'classification': 'PARITY_PASS'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    report = {}
    try:
        run(args.image, report)
    except BaseException as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS Linux container parity: {len(report["cases"])} cases, 40 routes, Redis 8.10.2 immutable OCI')
