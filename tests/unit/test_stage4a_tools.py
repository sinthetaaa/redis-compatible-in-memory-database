"""Offline safety checks: cleanup ownership and localhost endpoint validation."""
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import docker_support


class DockerSafetyTests(unittest.TestCase):
    def test_cleanup_refuses_foreign_containers_and_volumes(self):
        resources = object.__new__(docker_support.DockerResources)
        resources.owner = 'owned'
        resources.containers = ['foreign-container']
        resources.volumes = ['foreign-volume']
        resources.network = 'foreign-network'
        def fake(*args, **kwargs):
            if args[0] == 'inspect':
                return SimpleNamespace(stdout=json.dumps([{'Config': {'Labels': {docker_support.LABEL: 'other'}}}]))
            if args[:2] == ('volume', 'inspect'):
                return SimpleNamespace(stdout=json.dumps([{'Labels': {docker_support.LABEL: 'other'}}]))
            if args[:2] == ('network', 'inspect'):
                return SimpleNamespace(stdout=json.dumps([{'Labels': {docker_support.LABEL: 'other'}}]))
            self.fail('foreign resource mutation attempted: ' + repr(args))
        with patch.object(docker_support, 'docker', side_effect=fake):
            with self.assertRaisesRegex(RuntimeError, 'ownership label mismatch'):
                resources.close()

    def test_public_binding_is_rejected_before_connect(self):
        resources = object.__new__(docker_support.DockerResources)
        state = {'State': {'Running': True}, 'NetworkSettings': {'Ports': {
            '6380/tcp': [{'HostIp': '0.0.0.0', 'HostPort': '12345'}]}}}
        with patch.object(resources, 'create', return_value='owned'), \
             patch.object(docker_support, 'docker'), \
             patch.object(docker_support, 'inspect', return_value=state), \
             patch.object(docker_support, 'connect') as connection:
            with self.assertRaisesRegex(RuntimeError, 'unexpected host publication'):
                resources.start('image', 'volume')
            connection.assert_not_called()

    def test_non_linux_or_old_daemon_rejected(self):
        for server in ({'Os': 'linux', 'Version': '27.5.0'}, {'Os': 'windows', 'Version': '29.0.0'}):
            with patch.object(docker_support, 'docker', return_value=SimpleNamespace(stdout=json.dumps(server))):
                with self.assertRaisesRegex(RuntimeError, 'Linux Docker Engine >=28'):
                    docker_support.DockerResources()

    def test_linux_reference_rejects_wrong_image_before_start(self):
        sys.path.insert(0, str(ROOT / 'tests/differential'))
        spec = importlib.util.spec_from_file_location('linux_container', ROOT / 'tests/differential/linux_container.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        contract = json.loads((ROOT / 'docker/reference.json').read_text())
        digest = contract['platforms']['amd64']['manifest_digest']
        class Owned:
            architecture = 'amd64'
            def __enter__(self): return self
            def __exit__(self, *args): pass
        metadata = {'Os': 'linux', 'Architecture': 'amd64', 'Id': 'sha256:' + '0' * 64,
                    'RepoDigests': ['redis@' + digest]}
        def fake(*args, **kwargs):
            if args[0] == 'pull': return SimpleNamespace(stdout='')
            if args[:2] == ('image', 'inspect'):
                return SimpleNamespace(stdout=json.dumps([metadata]))
            self.fail('reference started before immutable identity check')
        with patch.object(module, 'DockerResources', return_value=Owned()), \
             patch.object(module, 'docker', side_effect=fake):
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                module.run('candidate', {})

    def test_benchmark_report_math_and_coverage(self):
        spec = importlib.util.spec_from_file_location('tcp_baseline', ROOT / 'benchmarks/tcp_baseline.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.percentile([1_000_000, 3_000_000, 2_000_000], .5), 2.0)
        self.assertEqual(module.percentile([1_000_000, 3_000_000, 2_000_000], .99), 3.0)
        profiles = module.scenarios(False)
        self.assertEqual({p[0] for p in profiles}, {'off', 'always', 'everysec', 'no'})
        self.assertEqual({p[1] for p in profiles}, {100, 1000})
        self.assertEqual({p[2] for p in profiles}, {'get', 'set', 'read80', 'write80'})
        self.assertEqual({p[3] for p in profiles}, {1, 4})
        self.assertEqual({p[4] for p in profiles}, {1, 16})


if __name__ == '__main__':
    unittest.main()
