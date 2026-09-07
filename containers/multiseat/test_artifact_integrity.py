"""Exercise corrupted and substituted inputs at the offline artifact boundary."""
import hashlib
import importlib.util
import io
import json
import pathlib
import tarfile
import tempfile
import unittest

from oci_archive import verify_archive

spec = importlib.util.spec_from_file_location('verify_inputs', pathlib.Path(__file__).with_name('verify-inputs.py'))
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class ArtifactIntegrity(unittest.TestCase):
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
        for mutation in ['valid', 'config', 'layer', 'size', 'duplicate', 'symlink']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                blobs = {}

                def blob(data, media_type):
                    digest = hashlib.sha256(data).hexdigest()
                    blobs['blobs/sha256/' + digest] = data
                    return {'digest': 'sha256:' + digest, 'size': len(data), 'mediaType': media_type}

                config = blob(b'{"architecture":"amd64","os":"linux"}', 'application/vnd.oci.image.config.v1+json')
                layer = blob(b'original layer', 'application/vnd.oci.image.layer.v1.tar')
                if mutation == 'size':
                    layer['size'] += 1
                manifest = blob(json.dumps({'schemaVersion': 2, 'config': config, 'layers': [layer]}).encode(),
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
                if mutation == 'valid':
                    self.assertEqual(verify_archive(path, expected), manifest['digest'])
                else:
                    with self.assertRaises(ValueError):
                        verify_archive(path, expected)


if __name__ == '__main__':
    unittest.main()
