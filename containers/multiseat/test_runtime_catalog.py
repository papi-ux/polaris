import copy
import hashlib
import io
import json
import pathlib
import tarfile
import tempfile
import unittest

from oci_archive import docker_to_oci
from runtime_catalog import REQUIRED_PROVIDER_TESTS, prepare_candidate, validate_catalog


class RuntimeCatalog(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.revision = 'a' * 40
        layer = b'verified filesystem'
        self.config = {'architecture': 'amd64', 'os': 'linux', 'rootfs': {'type': 'layers', 'diff_ids': [
            'sha256:' + hashlib.sha256(layer).hexdigest()]}, 'config': {
                'Entrypoint': ['/usr/bin/polaris-seat-worker'], 'Cmd': ['run'],
                'Labels': {'org.opencontainers.image.source': 'https://github.com/papi-ux/polaris',
                           'org.opencontainers.image.revision': self.revision,
                           'io.polaris.multiseat.profile': 'steam',
                           'io.polaris.multiseat.architecture': 'linux/amd64',
                           'io.polaris.multiseat.media-contract': '1'}}}
        self.layer = layer
        self.build_fixture()

    def write_json(self, name, value):
        (self.root / name).write_text(json.dumps(value))

    def build_fixture(self):
        encoded = json.dumps(self.config).encode()
        config_digest = 'sha256:' + hashlib.sha256(encoded).hexdigest()
        with tarfile.open(self.root / 'worker.docker.tar', 'w') as archive:
            for name, payload in {'config.json': encoded, 'layer.tar': self.layer,
                                  'manifest.json': b'[{"Config":"config.json","Layers":["layer.tar"]}]'}.items():
                member = tarfile.TarInfo(name)
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
        worker = docker_to_oci(self.root / 'worker.docker.tar', self.root / 'worker.oci.tar', config_digest)
        self.write_json('providers.json', {'schema': 1, 'result': 'passed', 'variant': 'default',
            'worker_config_digest': config_digest, 'tests': sorted(REQUIRED_PROVIDER_TESTS)})
        (self.root / 'packages.tsv').write_text('package\t1.0\tamd64\n')
        self.write_json('sbom.cdx.json', {'bomFormat': 'CycloneDX'})
        self.artifact = {'schema': 1, 'development': False, 'profile': 'steam', 'variant': 'default',
            'build_engine': 'docker', 'platform': 'linux/amd64', 'media_contract': 1,
            'owner_uid': 1000, 'owner_gid': 1000, 'source_revision': self.revision,
            'worker_config_digest': config_digest, 'worker_digest': worker,
            'validation': {'dependencies': 'passed', 'session_bus_audio_display': 'passed'}, 'files': {}}
        for name in ['worker.docker.tar', 'worker.oci.tar', 'providers.json', 'packages.tsv', 'sbom.cdx.json']:
            self.receipt(name)
        self.registry = json.dumps({'schemaVersion': 2, 'mediaType': 'application/vnd.oci.image.manifest.v1+json',
            'config': {'digest': config_digest, 'size': len(encoded),
                       'mediaType': 'application/vnd.oci.image.config.v1+json'},
            'layers': [{'digest': 'sha256:' + hashlib.sha256(self.layer).hexdigest(),
                        'size': len(self.layer), 'mediaType': 'application/vnd.oci.image.layer.v1.tar'}]}).encode()

    def receipt(self, name):
        value = (self.root / name).read_bytes()
        self.artifact['files'][name] = {'sha256': hashlib.sha256(value).hexdigest(), 'bytes': len(value)}
        self.write_json('artifact.json', self.artifact)

    def candidate(self, raw=None, digest=None):
        raw = self.registry if raw is None else raw
        digest = 'sha256:' + hashlib.sha256(raw).hexdigest() if digest is None else digest
        return prepare_candidate(self.root, digest, raw)

    def test_real_archive_and_registry_binding_produce_a_candidate_without_mutation(self):
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        result = self.candidate()
        self.assertEqual(result['runtimes'][0]['source_revision'], self.revision)
        self.assertEqual(result['runtimes'][0]['config_digest'], self.artifact['worker_config_digest'])
        self.assertEqual(result, validate_catalog(result))
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.root.iterdir()})

    def test_corrupted_or_substituted_artifact_files_are_rejected(self):
        for name in self.artifact['files']:
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(b'X' * len(original))
                with self.assertRaises(ValueError):
                    self.candidate()
                path.write_bytes(original)
        path = self.root / 'packages.tsv'
        original = path.read_bytes()
        path.unlink()
        path.symlink_to(self.root / 'providers.json')
        with self.assertRaises(ValueError):
            self.candidate()
        path.unlink()
        path.write_bytes(original)

    def test_provider_reports_must_cover_every_required_test_and_same_image(self):
        original = json.loads((self.root / 'providers.json').read_text())
        for key, value in [('result', 'skipped'), ('worker_config_digest', 'sha256:' + 'f' * 64),
                           ('tests', original['tests'][:-1]), ('tests', original['tests'] + original['tests'][:1]),
                           ('variant', 'nvidia')]:
            with self.subTest(key=key):
                report = copy.deepcopy(original)
                report[key] = value
                self.write_json('providers.json', report)
                self.receipt('providers.json')
                with self.assertRaises(ValueError):
                    self.candidate()

    def test_registry_cannot_replace_the_validated_configuration(self):
        with self.assertRaises(ValueError):
            self.candidate(digest='sha256:' + 'd' * 64)
        for mutation in ['config', 'index', 'extra-layer', 'duplicate-key', 'missing-layer']:
            with self.subTest(mutation=mutation):
                manifest = json.loads(self.registry)
                if mutation == 'config':
                    manifest['config']['digest'] = 'sha256:' + 'e' * 64
                elif mutation == 'index':
                    manifest['mediaType'] = 'application/vnd.oci.image.index.v1+json'
                elif mutation == 'extra-layer':
                    manifest['layers'] *= 2
                elif mutation == 'missing-layer':
                    manifest['layers'] = []
                raw = json.dumps(manifest).encode()
                if mutation == 'duplicate-key':
                    raw = b'{"schemaVersion":2,' + raw[1:]
                with self.assertRaises(ValueError):
                    self.candidate(raw=raw)

    def test_self_consistent_but_incompatible_worker_configuration_is_rejected(self):
        original = copy.deepcopy(self.config)
        for key, value in [('Entrypoint', ['/bin/sh']), ('Cmd', ['other']), ('Volumes', {'/shared': {}}),
                           ('ExposedPorts', {'80/tcp': {}}), ('OnBuild', ['RUN touch /oops'])]:
            with self.subTest(key=key):
                self.config = copy.deepcopy(original)
                self.config['config'][key] = value
                self.build_fixture()
                with self.assertRaises(ValueError):
                    self.candidate()

    def test_catalog_rejects_unknown_sources_versions_identities_and_driver_variants(self):
        original = self.candidate()
        # gamescope is a real runtime profile and never a Space's launcher.
        for key, value in [('profile', 'gamescope'), ('profile', 'epic'), ('platform', 'linux/arm64'), ('media_contract', 2),
                           ('media_contract', True), ('uid', 1001), ('gid', '1000'), ('id', '../steam'),
                           ('source_revision', 'master'), ('registry_digest', 'latest'),
                           ('config_digest', 'sha256:no'), ('variant', 'other'), ('nvidia_driver', '610.57.04'),
                           ('url', 'https://untrusted.invalid/image')]:
            with self.subTest(key=key):
                catalog = copy.deepcopy(original)
                catalog['runtimes'][0][key] = value
                with self.assertRaises(ValueError):
                    validate_catalog(catalog)
        duplicate = copy.deepcopy(original)
        duplicate['runtimes'] *= 2
        with self.assertRaises(ValueError):
            validate_catalog(duplicate)

    def test_catalog_admits_every_launcher_family(self):
        original = self.candidate()
        for family in ['steam', 'heroic', 'lutris']:
            with self.subTest(family=family):
                catalog = copy.deepcopy(original)
                catalog['runtimes'][0]['profile'] = family
                validate_catalog(catalog)

    def test_nvidia_candidate_requires_matching_driver_receipts(self):
        self.config['config']['Labels']['io.polaris.multiseat.nvidia.driver'] = '610.57.04'
        self.build_fixture()
        self.artifact['variant'] = 'nvidia'
        providers = json.loads((self.root / 'providers.json').read_text())
        providers['variant'] = 'nvidia'
        self.write_json('providers.json', providers)
        self.receipt('providers.json')
        self.write_json('nvidia-files.json', {'architectures': ['amd64', 'i386']})
        self.receipt('nvidia-files.json')
        report = {'result': 'passed', 'driver_version': '610.57.04',
                  'manifest_sha256': self.artifact['files']['nvidia-files.json']['sha256']}
        self.write_json('nvidia-runtime.json', report)
        self.receipt('nvidia-runtime.json')
        self.assertEqual(self.candidate()['runtimes'][0]['nvidia_driver'], '610.57.04')
        report['driver_version'] = '610.57.05'
        self.write_json('nvidia-runtime.json', report)
        self.receipt('nvidia-runtime.json')
        with self.assertRaises(ValueError):
            self.candidate()


if __name__ == '__main__':
    unittest.main()
