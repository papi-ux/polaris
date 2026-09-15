"""The image cleanup must never delete an unreviewed or aliased executable."""
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('initialize_root', Path(__file__).with_name('initialize-root.py'))
initialize = importlib.util.module_from_spec(spec)
spec.loader.exec_module(initialize)


class ImageInitRemoval(unittest.TestCase):
    def test_only_the_exact_unaliased_image_file_is_removed(self):
        original = b'reviewed init'
        expected = hashlib.sha256(original).hexdigest()
        for mutation in ('valid', 'changed', 'missing', 'symlink', 'hardlink', 'directory'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / 'usr/bin/pebble'
                path.parent.mkdir(parents=True)
                other = path.parent / 'unrelated'
                other.write_bytes(original)
                if mutation == 'directory':
                    path.mkdir()
                elif mutation == 'symlink':
                    path.symlink_to(other)
                elif mutation == 'hardlink':
                    os.link(other, path)
                elif mutation != 'missing':
                    path.write_bytes(b'changed' if mutation == 'changed' else original)
                if mutation == 'valid':
                    initialize.remove_inherited_init(root, expected)
                    self.assertFalse(path.exists())
                else:
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        initialize.remove_inherited_init(root, expected)
                    self.assertEqual(path.exists(), mutation != 'missing')
                self.assertEqual(other.read_bytes(), original)
