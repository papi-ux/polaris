"""Exercise corrupted and substituted inputs at the offline artifact boundary."""
import hashlib
import gzip
import importlib.util
import io
import json
import pathlib
import tarfile
import tempfile
import unittest
from unittest import mock

from oci_archive import verify_archive

spec = importlib.util.spec_from_file_location('verify_inputs', pathlib.Path(__file__).with_name('verify-inputs.py'))
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)
spec = importlib.util.spec_from_file_location('build_image', pathlib.Path(__file__).with_name('build-image.py'))
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ArtifactIntegrity(unittest.TestCase):
    def test_committed_context_excludes_ignored_injection_and_concurrent_edits(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(build, 'REPO', pathlib.Path(temporary)):
            root = pathlib.Path(temporary)
            here = root / 'containers/multiseat'
            (here / 'locks').mkdir(parents=True)
            (root / 'multiseat_worker').mkdir()
            (root / '.gitignore').write_text('*sync-conflict*\nbuild/\n')
            original = 'package main\n'
            source = root / 'multiseat_worker/main.go'
            source.write_text(original)
            checksum = hashlib.sha256(b'locked').hexdigest()
            packages = {'platform': 'linux/amd64', 'source_root': 'locked-root',
                        'runtime': [{'filename': 'pkg.deb', 'sha256': checksum}],
                        'build': [{'filename': 'pkg.deb', 'sha256': checksum}]}
            (here / 'locks/gamescope.packages.json').write_text(json.dumps(packages))
            locks = {name: 'locks/' + name + '.json' for name in ['rust', 'plugin', 'gamescope']}
            for name, path in locks.items():
                (here / path).write_text(json.dumps({'sha256': checksum, 'url': 'https://example.invalid/rust.tar.xz'}))
            (here / 'images.lock.json').write_text(json.dumps({
                'runtime_profiles': [{'id': 'gamescope', 'dependency_lock': 'locks/gamescope.packages.json'}],
                'dependency_locks': locks}))
            for filename in ['gamescope/runtime/pkg.deb', 'gamescope/build/pkg.deb',
                             'toolchains/rust.tar.xz', 'plugin.tar', 'gamescope.tar']:
                path = root / 'build/runtime-inputs' / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'locked')
            build.run(['git', 'init', '-q'])
            build.run(['git', 'add', '.'])
            build.run(['git', '-c', 'user.name=Artifact Test', '-c', 'user.email=artifact@example.invalid',
                       '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture'])
            revision = build.output(['git', 'rev-parse', 'HEAD']).strip()
            (root / 'multiseat_worker/extra.sync-conflict-local.go').write_text('package main\nfunc init() {}\n')
            source.write_text('edited during build')
            (here / 'locks/plugin.json').write_text('{}')
            with build.materialized_context(revision, 'gamescope', False) as context:
                self.assertFalse((context / 'multiseat_worker/extra.sync-conflict-local.go').exists())
                self.assertEqual((context / 'multiseat_worker/main.go').read_text(), original)
                self.assertEqual(json.loads((context / 'containers/multiseat/locks/plugin.json').read_text())['sha256'], checksum)
                (root / 'build/runtime-inputs/plugin.tar').write_bytes(b'changed cache')
                self.assertEqual((context / 'build/runtime-inputs/plugin.tar').read_bytes(), b'locked')

    def test_offline_packages_reject_substitution(self):
        for mutation in ['valid', 'modified', 'missing', 'extra', 'symlink', 'traversal', 'duplicate']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                (root / 'packages').mkdir()
                package = root / 'packages/pkg.deb'
                package.write_bytes(b'locked package')
                entry = {'filename': 'pkg.deb', 'sha256': hashlib.sha256(package.read_bytes()).hexdigest()}
                lock = {'platform': 'linux/amd64', 'runtime': [entry]}
                if mutation == 'modified':
                    package.write_bytes(b'substituted package')
                elif mutation == 'missing':
                    package.unlink()
                elif mutation == 'extra':
                    (root / 'packages/extra.deb').write_bytes(b'extra')
                elif mutation == 'symlink':
                    package.rename(root / 'other')
                    package.symlink_to(root / 'other')
                elif mutation == 'traversal':
                    entry['filename'] = '../pkg.deb'
                elif mutation == 'duplicate':
                    lock['runtime'].append(dict(entry))
                (root / 'packages.json').write_text(json.dumps(lock))
                if mutation == 'valid':
                    inputs.verify_inputs(root, root, 'runtime')
                else:
                    with self.assertRaises(ValueError):
                        inputs.verify_inputs(root, root, 'runtime')

    def test_export_is_bound_to_config_and_all_layers(self):
        for mutation in ['valid', 'gzip', 'config', 'layer', 'size', 'duplicate', 'symlink',
                         'substituted', 'additional', 'reordered', 'encoding']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                blobs = {}

                def blob(data, media_type):
                    digest = hashlib.sha256(data).hexdigest()
                    blobs['blobs/sha256/' + digest] = data
                    return {'digest': 'sha256:' + digest, 'size': len(data), 'mediaType': media_type}

                layer = blob(b'original layer', 'application/vnd.oci.image.layer.v1.tar')
                second = blob(b'second original layer', 'application/vnd.oci.image.layer.v1.tar')
                config = blob(json.dumps({'architecture': 'amd64', 'os': 'linux',
                                         'rootfs': {'type': 'layers', 'diff_ids': [layer['digest'], second['digest']]}}).encode(),
                              'application/vnd.oci.image.config.v1+json')
                layers = [layer, second]
                if mutation == 'size':
                    layer['size'] += 1
                elif mutation == 'substituted':
                    layers[0] = blob(b'self-consistent substitution', 'application/vnd.oci.image.layer.v1.tar')
                elif mutation == 'additional':
                    layers.append(blob(b'added layer', 'application/vnd.oci.image.layer.v1.tar'))
                elif mutation == 'reordered':
                    layers.reverse()
                elif mutation == 'encoding':
                    layer['mediaType'] += '+unsupported'
                elif mutation == 'gzip':
                    layers[0] = blob(gzip.compress(b'original layer'), 'application/vnd.oci.image.layer.v1.tar+gzip')
                manifest = blob(json.dumps({'schemaVersion': 2, 'config': config, 'layers': layers}).encode(),
                                'application/vnd.oci.image.manifest.v1+json')
                if mutation == 'layer':
                    blobs['blobs/sha256/' + layer['digest'][7:]] = b'tampered layer'
                blobs['oci-layout'] = b'{"imageLayoutVersion":"1.0.0"}'
                blobs['index.json'] = json.dumps({'schemaVersion': 2, 'manifests': [manifest]}).encode()
                path = pathlib.Path(temporary) / 'worker.tar'
                with tarfile.open(path, 'w') as archive:
                    for name, data in blobs.items():
                        member = tarfile.TarInfo(name)
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
                    if mutation in ['duplicate', 'symlink']:
                        member = tarfile.TarInfo('index.json' if mutation == 'duplicate' else 'link')
                        if mutation == 'symlink':
                            member.type = tarfile.SYMTYPE
                            member.linkname = '/etc/passwd'
                        archive.addfile(member)
                expected = 'sha256:' + '0' * 64 if mutation == 'config' else config['digest']
                if mutation in ['valid', 'gzip']:
                    self.assertEqual(verify_archive(path, expected), manifest['digest'])
                else:
                    with self.assertRaises(ValueError):
                        verify_archive(path, expected)


if __name__ == '__main__':
    unittest.main()
