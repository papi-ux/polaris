"""Notice supplements must match the exact vendored source and reviewed bytes."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('plugin_notices', Path(__file__).with_name('collect-plugin-notices.py'))
notices = importlib.util.module_from_spec(spec); spec.loader.exec_module(notices)


class PluginNotices(unittest.TestCase):
    def test_authors_file_with_license_grant_is_retained(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); package = root / 'source/vendor/pkg-1.0'; package.mkdir(parents=True)
            (package / 'Cargo.toml').write_text('[package]\nname="pkg"\nversion="1.0"\nlicense="MIT"\n')
            (package / 'AUTHORS').write_text('A project may place its license grant here.\n')
            notices.collect(root / 'source', root / 'out')
            self.assertEqual((root / 'out/pkg-1.0/AUTHORS').read_bytes(), (package / 'AUTHORS').read_bytes())

    def test_supplement_substitution_and_stale_source_are_rejected(self):
        for case in ('valid', 'hash', 'revision', 'missing', 'duplicate', 'unused', 'traversal', 'symlink'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); package = root / 'source/vendor/pkg-1.0'; package.mkdir(parents=True)
                (package / 'Cargo.toml').write_text('[package]\nname="pkg"\nversion="1.0"\nlicense="MIT"\n')
                (package / '.cargo_vcs_info.json').write_text(json.dumps({'git': {'sha1': 'a' * 40}}))
                supplement = root / 'extra'; (supplement / 'pkg-1.0').mkdir(parents=True)
                text = b'reviewed upstream notice'
                path = supplement / 'pkg-1.0/LICENSE'; path.write_bytes(text)
                entry = {'package': 'pkg-1.0', 'vcs_revision': 'a' * 40,
                    'files': [{'path': 'LICENSE', 'url': 'https://example.invalid/source/LICENSE',
                               'sha256': hashlib.sha256(text).hexdigest()}]}
                if case == 'hash': path.write_text('substitution')
                elif case == 'revision': entry['vcs_revision'] = 'b' * 40
                elif case == 'missing': path.unlink()
                elif case == 'unused': entry['package'] = 'pkg-2.0'
                elif case == 'traversal': entry['files'][0]['path'] = '../LICENSE'
                elif case == 'symlink':
                    path.unlink(); path.symlink_to(package / 'Cargo.toml')
                entries = [entry, entry] if case == 'duplicate' else [entry]
                (supplement / 'index.json').write_text(json.dumps({'schema': 1, 'packages': entries}))
                if case == 'valid':
                    notices.collect(root / 'source', root / 'out', supplement)
                    self.assertEqual((root / 'out/pkg-1.0/LICENSE').read_bytes(), text)
                    record = json.loads((root / 'out/index.json').read_text())['packages'][0]['notices'][0]
                    self.assertEqual(record['source_revision'], 'a' * 40)
                else:
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        notices.collect(root / 'source', root / 'out', supplement)


    def test_notice_origin_does_not_replace_package_identity(self):
        cases = ('later', 'canonical', 'wrong_package', 'mutable_notice',
                 'unoffered_license', 'empty_license', 'compound_license', 'ambiguous_origin')
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); package = root / 'source/vendor/pkg-1.0'; package.mkdir(parents=True)
                (package / 'Cargo.toml').write_text('[package]\nname="pkg"\nversion="1.0"\nlicense="MIT OR Apache-2.0"\n')
                (package / '.cargo_vcs_info.json').write_text(json.dumps({'git': {'sha1': 'a' * 40}}))
                supplement = root / 'extra'; (supplement / 'pkg-1.0').mkdir(parents=True)
                data = b'reviewed notice bytes'
                (supplement / 'pkg-1.0/LICENSE').write_bytes(data)
                notice = {'path': 'LICENSE', 'sha256': hashlib.sha256(data).hexdigest(),
                          'url': 'https://example.invalid/LICENSE', 'notice_revision': 'b' * 40}
                entry = {'package': 'pkg-1.0', 'vcs_revision': 'a' * 40, 'files': [notice]}
                if case == 'wrong_package': entry['vcs_revision'] = 'c' * 40
                elif case == 'mutable_notice': notice['notice_revision'] = 'master'
                elif case in ('canonical', 'unoffered_license', 'empty_license', 'compound_license'):
                    notice.pop('notice_revision')
                    notice['license_id'] = {'canonical': 'Apache-2.0', 'unoffered_license': 'BSD-3-Clause',
                                            'empty_license': '', 'compound_license': 'MIT OR Apache-2.0'}[case]
                elif case == 'ambiguous_origin': notice['license_id'] = 'Apache-2.0'
                (supplement / 'index.json').write_text(json.dumps({'schema': 1, 'packages': [entry]}))
                if case in ('later', 'canonical'):
                    notices.collect(root / 'source', root / 'out', supplement)
                    record = json.loads((root / 'out/index.json').read_text())['packages'][0]
                    self.assertEqual(record['declared_license'], 'MIT OR Apache-2.0')
                    copied = record['notices'][0]
                    self.assertEqual(copied['package_source_revision'], 'a' * 40)
                    self.assertEqual(copied['source_revision'], 'b' * 40 if case == 'later' else None)
                    if case == 'canonical': self.assertEqual(copied['license_id'], 'Apache-2.0')
                    self.assertEqual((root / 'out/pkg-1.0/LICENSE').read_bytes(), data)
                else:
                    with self.assertRaises(ValueError):
                        notices.collect(root / 'source', root / 'out', supplement)


if __name__ == '__main__':
    unittest.main()
