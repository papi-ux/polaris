"""Config identity must survive Docker's change of image storage backend."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from oci_archive import docker_config_digest


class DockerIdentity(unittest.TestCase):
    def test_config_manifest_and_index_ids_require_exact_saved_bytes(self):
        for case in ('classic', 'manifest', 'index', 'foreign', 'wrong-config',
                     'wrong-size', 'corrupt', 'ambiguous', 'duplicate'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                config = b'{"architecture":"amd64","os":"linux"}'
                expected = 'sha256:' + hashlib.sha256(config).hexdigest()
                files = {'config.json': config,
                         'manifest.json': b'[{"Config":"config.json","Layers":[]}]'}
                descriptor = {'digest': expected, 'size': len(config)}
                if case == 'wrong-config': descriptor['digest'] = 'sha256:' + 'f' * 64
                if case == 'wrong-size': descriptor['size'] += 1
                manifest = json.dumps({'schemaVersion': 2, 'config': descriptor, 'layers': []}).encode()
                manifest_id = 'sha256:' + hashlib.sha256(manifest).hexdigest()
                files['blobs/sha256/' + manifest_id[7:]] = manifest
                children = [{'digest': manifest_id, 'size': len(manifest)}]
                if case == 'ambiguous': children *= 2
                index = json.dumps({'schemaVersion': 2, 'manifests': children}).encode()
                index_id = 'sha256:' + hashlib.sha256(index).hexdigest()
                files['blobs/sha256/' + index_id[7:]] = index
                if case == 'corrupt': files['blobs/sha256/' + manifest_id[7:]] += b' '
                identity = expected if case == 'classic' else manifest_id if case == 'manifest' else index_id
                if case == 'foreign': identity = 'sha256:' + 'e' * 64
                path = Path(temporary) / 'saved.tar'
                with tarfile.open(path, 'w') as archive:
                    for name, data in list(files.items()) + ([('config.json', config)] if case == 'duplicate' else []):
                        member = tarfile.TarInfo(name); member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
                if case in ('classic', 'manifest', 'index'):
                    self.assertEqual(docker_config_digest(path, identity), expected)
                else:
                    with self.assertRaises(ValueError): docker_config_digest(path, identity)


if __name__ == '__main__':
    unittest.main()
