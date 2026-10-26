"""Offline deployment guards. Compose rendering needs the CLI, never a daemon."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import private_deployment as deployment

IMAGE = 'sha256:' + '0' * 64
REVISION = 'd69b3ede7405b4fe843ffb1c3bbfe4d957810615'


class PrivateDeploymentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = deployment.settings(IMAGE, REVISION, 'offline-data')
        cls.config = json.loads(deployment.compose(cls.env, 'offline-private',
                                                 'config', '--format', 'json').stdout)

    def test_actual_compose_render_and_health_shell_syntax(self):
        deployment.validate_config(self.config, self.env)
        health = self.config['services']['redisdb']['healthcheck']['test']
        subprocess.run(['bash', '-n', '-c', health[3]], check=True)

    def test_healthcheck_wire_bytes_and_bad_reply(self):
        script = self.config['services']['redisdb']['healthcheck']['test'][3]
        # A regular FD fixture avoids network/daemon dependencies. Reserve the
        # request bytes, then put the response at the subsequent read position.
        request = b'*1\r\n$4\r\nPING\r\n'
        with tempfile.TemporaryDirectory(prefix='redisdb-health-wire-') as temporary:
            fixture = Path(temporary)/'wire'
            for reply, expected_code in [(b'+PONG\r\n', 0), (b'+WRONG\n', 1)]:
                fixture.write_bytes(b' ' * len(request) + reply)
                local = script.replace('exec 3<>/dev/tcp/127.0.0.1/6380', f'exec 3<>"{fixture}"')
                result = subprocess.run(['bash', '-ec', local], capture_output=True, timeout=3)
                self.assertEqual(result.returncode, expected_code)
                self.assertEqual(fixture.read_bytes()[:len(request)], request)

    def test_unsafe_configurations_fail(self):
        mutations = [lambda s: s['ports'][0].update(host_ip='0.0.0.0'),
                     lambda s: s['ports'].append(dict(s['ports'][0])),
                     lambda s: s.update(network_mode='host'),
                     lambda s: s.update(user='0:0'), lambda s: s.update(privileged=True),
                     lambda s: s.update(read_only=False), lambda s: s.update(cap_add=['SYS_ADMIN']),
                     lambda s: s.update(environment={'TOKEN': 'not-a-real-secret'}),
                     lambda s: s.update(cpus=0), lambda s: s.update(pids_limit=0),
                     lambda s: s.update(mem_limit='0'), lambda s: s.update(restart='no'),
                     lambda s: s.update(command=['--resp2-server']),
                     lambda s: s.update(image='redisdb:latest'), lambda s: s.update(build={}),
                     lambda s: s['volumes'][0].update(type='bind', source='/home')]
        for change in mutations:
            with self.subTest(change=change):
                config = copy.deepcopy(self.config)
                change(config['services']['redisdb'])
                with self.assertRaises(RuntimeError):
                    deployment.validate_config(config, self.env)
        config = copy.deepcopy(self.config)
        config['volumes']['data']['external'] = False
        with self.assertRaisesRegex(RuntimeError, 'external volume'):
            deployment.validate_config(config, self.env)

    def test_identity_arguments_reject_floating_or_partial_references(self):
        for image, revision, volume, port in [('redisdb:latest', REVISION, 'data', 6380),
                                               (IMAGE, 'd69b3ed', 'data', 6380),
                                               (IMAGE, REVISION, '../data', 6380),
                                               (IMAGE, REVISION, 'data', 6379)]:
            with self.assertRaises(RuntimeError):
                deployment.settings(image, revision, volume, port)

    def test_disposable_port_stays_loopback(self):
        env = deployment.settings(IMAGE, REVISION, 'offline-data', 0, 'owned-token')
        config = json.loads(deployment.compose(env, 'offline-private', 'config', '--format', 'json').stdout)
        deployment.validate_config(config, env)
        self.assertEqual(config['services']['redisdb']['ports'][0]['host_ip'], '127.0.0.1')

    def test_implicit_dotenv_files_cannot_redirect_compose(self):
        with patch.object(deployment.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stderr='')) as run:
            deployment.compose(self.env, 'offline-private', 'config', '--quiet')
            command = run.call_args.args[0]
            self.assertEqual(command[:4], ['docker', 'compose', '--env-file', deployment.os.devnull])
            self.assertIn(str(deployment.COMPOSE), command)

    def test_runtime_inspection_rejects_public_mapping_and_wrong_identity(self):
        state = {'Image': IMAGE, 'Config': {'User': '10001:10001',
                 'Entrypoint': ['/usr/local/bin/redisdb-server'], 'Cmd': deployment.COMMAND,
                 'Labels': {'io.redisdb.deployment.revision': REVISION}, 'StopTimeout': 60},
                 'HostConfig': {'ReadonlyRootfs': True, 'Privileged': False, 'CapAdd': [],
                 'CapDrop': ['ALL'], 'SecurityOpt': ['no-new-privileges:true'],
                 'Memory': 512*1024*1024, 'NanoCpus': 1_000_000_000, 'PidsLimit': 64,
                 'RestartPolicy': {'Name': 'unless-stopped'},
                 'PortBindings': {'6380/tcp': [{'HostIp': '127.0.0.1', 'HostPort': '6380'}]}},
                 'Mounts': [{'Type': 'volume', 'Destination': '/data', 'Name': 'offline-data', 'RW': True}],
                 'NetworkSettings': {'Ports': {'6380/tcp': [{'HostIp': '127.0.0.1', 'HostPort': '6380'}]}},
                 'State': {'Running': True, 'Health': {'Status': 'healthy'}}}
        self.assertEqual(deployment.validate_runtime(state, self.env), 6380)
        wrong = copy.deepcopy(state)
        wrong['NetworkSettings']['Ports']['6380/tcp'][0]['HostIp'] = '::'
        with self.assertRaisesRegex(RuntimeError, 'actual publication'):
            deployment.validate_runtime(wrong, self.env)
        wrong = copy.deepcopy(state)
        wrong['Image'] = 'sha256:' + '1'*64
        with self.assertRaisesRegex(RuntimeError, 'image/user'):
            deployment.validate_runtime(wrong, self.env)

    def test_remote_context_refused_before_daemon_contact(self):
        with patch.object(deployment.platform, 'system', return_value='Linux'), \
             patch.dict(deployment.os.environ, {'DOCKER_HOST': 'ssh://USER@HOST', 'DOCKER_CONTEXT': ''}), \
             patch.object(deployment, 'docker') as docker:
            with self.assertRaisesRegex(RuntimeError, 'remote Docker endpoint'):
                deployment.preflight(IMAGE, REVISION)
            docker.assert_not_called()

    def test_nonlinux_runtime_is_not_reported_as_linux(self):
        with patch.object(deployment.platform, 'system', return_value='Darwin'), \
             patch.object(deployment, 'docker') as docker:
            with self.assertRaisesRegex(RuntimeError, 'actual Linux Docker host'):
                deployment.preflight(IMAGE, REVISION)
            docker.assert_not_called()

    def test_old_engine_refused_before_image_or_container_access(self):
        def docker(*args):
            self.assertEqual(args[0], 'version')
            return SimpleNamespace(stdout=json.dumps({'Os': 'linux', 'Version': '27.5.1'}))
        with (patch.object(deployment.platform, 'system', return_value='Linux'),
              patch.dict(deployment.os.environ, {'DOCKER_HOST': 'unix:///var/run/docker.sock',
                                                'DOCKER_CONTEXT': ''}),
              patch.object(deployment, 'docker', side_effect=docker)):
            with self.assertRaisesRegex(RuntimeError, 'Engine >=28'):
                deployment.preflight(IMAGE, REVISION)

    def test_wrong_revision_refused_before_resource_mutation(self):
        labels = {'org.opencontainers.image.revision': '1'*40,
                  'org.opencontainers.image.source': deployment.SOURCE,
                  'io.redisdb.build.configuration': 'Release', 'io.redisdb.build.cxx_standard': '20'}
        def docker(*args):
            if args[0] == 'version':
                return SimpleNamespace(stdout=json.dumps({'Os': 'linux', 'Version': '28.0.4', 'Arch': 'amd64'}))
            if args[:2] == ('compose', 'version'):
                return SimpleNamespace(stdout='2.40.3')
            if args[:2] == ('image', 'inspect'):
                return SimpleNamespace(stdout=json.dumps([{'Id': IMAGE, 'Os': 'linux',
                       'Architecture': 'amd64', 'Config': {'Labels': labels, 'User': '10001:10001'}}]))
            self.fail('unexpected resource access before image verification')
        with (patch.object(deployment.platform, 'system', return_value='Linux'),
              patch.dict(deployment.os.environ, {'DOCKER_HOST': 'unix:///var/run/docker.sock',
                                                'DOCKER_CONTEXT': ''}),
              patch.object(deployment, 'docker', side_effect=docker)):
            with self.assertRaisesRegex(RuntimeError, 'Release identity mismatch'):
                deployment.preflight(IMAGE, REVISION)

    def test_public_listener_or_connection_fails(self):
        interfaces = [{'addr_info': [{'family': 'inet', 'local': '10.1.2.3'}]}]
        def commands(args, **kwargs):
            return SimpleNamespace(stdout='' if args[0] == 'ss' else json.dumps(interfaces))
        with patch.object(deployment.subprocess, 'run', side_effect=commands), \
             patch.object(deployment.socket, 'create_connection', side_effect=ConnectionRefusedError):
            self.assertEqual(deployment.network_check(6380)['non_loopback_connections'], 'blocked_or_refused')
        connection = Mock()
        with patch.object(deployment.subprocess, 'run', side_effect=commands), \
             patch.object(deployment.socket, 'create_connection', return_value=connection):
            with self.assertRaisesRegex(RuntimeError, 'reachable on host interface'):
                deployment.network_check(6380)
            connection.close.assert_called_once()
        with patch.object(deployment.subprocess, 'run',
                          return_value=SimpleNamespace(stdout='LISTEN 0 128 0.0.0.0:6380 0.0.0.0:*\n')):
            with self.assertRaisesRegex(RuntimeError, 'non-loopback host listener'):
                deployment.network_check(6380)

    def test_probe_infrastructure_error_is_not_a_security_pass(self):
        interfaces = [{'addr_info': [{'family': 'inet', 'local': '10.1.2.3'}]}]
        with patch.object(deployment.subprocess, 'run', side_effect=[SimpleNamespace(stdout=''),
                         SimpleNamespace(stdout=json.dumps(interfaces))]), \
             patch.object(deployment.socket, 'create_connection', side_effect=OSError('resource failure')):
            with self.assertRaisesRegex(OSError, 'resource failure'):
                deployment.network_check(6380)

    def test_partial_compose_cleanup_uses_both_ownership_labels(self):
        owned = SimpleNamespace(owner='owned-token', containers=['helper'], network=None)
        def docker(*args):
            self.assertIn('label='+deployment.LABEL+'=owned-token', args)
            self.assertIn('label=com.docker.compose.project=owned-project', args)
            return SimpleNamespace(stdout='owned-container\n' if args[0] == 'ps' else 'owned-network\n')
        with patch.object(deployment, 'docker', side_effect=docker):
            deployment.collect_owned(owned, 'owned-project')
        self.assertEqual(owned.containers, ['helper', 'owned-container'])
        self.assertEqual(owned.network, 'owned-network')

    def test_foreign_volume_collision_refused_before_mounting(self):
        owned = SimpleNamespace(owner='owned-token', volume=Mock(return_value='colliding-name'))
        with patch.object(deployment, 'docker', return_value=SimpleNamespace(
                stdout=json.dumps([{'Labels': {deployment.LABEL: 'foreign'}}]))):
            with self.assertRaisesRegex(RuntimeError, 'foreign volume'):
                deployment.owned_volume(owned)

    def test_config_report_does_not_claim_runtime_or_remote_acceptance(self):
        with tempfile.TemporaryDirectory(prefix='redisdb-offline-config-') as temporary:
            report = Path(temporary)/'result.json'
            result = subprocess.run([sys.executable, '-B', str(ROOT/'tools/private_deployment.py'),
                'config', '--image-id', IMAGE, '--revision', REVISION, '--volume', 'offline-data',
                '--report', str(report)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('runtime/remote checks NOT RUN', result.stdout)
            data = json.loads(report.read_text())
            self.assertTrue(data['complete'])
            self.assertFalse(data['remote_deployment_validated'])

    def test_smoke_cannot_adopt_an_existing_user_volume(self):
        result = subprocess.run([sys.executable, '-B', str(ROOT/'tools/private_deployment.py'),
            'smoke', '--image-id', IMAGE, '--revision', REVISION, '--volume', 'user-data'],
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('unrecognized arguments', result.stderr)


if __name__ == '__main__':
    unittest.main()
