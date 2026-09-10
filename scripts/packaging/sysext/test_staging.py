#!/usr/bin/env python3
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from assemble import assemble, finish_receipt
from bounded import BuildError, copy_verified
from staging import Inputs, snapshot_metadata, verify_snapshot


class StagingTests(unittest.TestCase):
    def test_exclusive_staging_preserves_reviewed_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'source').mkdir()
            source = root / 'source' / 'candidate.rpm'
            source.write_bytes(b'candidate')
            frozen = Inputs(source.parent, root / 'frozen')
            record = {'path': source.name, 'sha256': hashlib.sha256(b'candidate').hexdigest()}
            try:
                copied = frozen.copy(record)
                source.write_bytes(b'changed')
                self.assertEqual(copied.read_bytes(), b'candidate')
                with self.assertRaises(BuildError):
                    frozen.copy(record)
            finally:
                frozen.close()

    def test_completion_marker_exclusive_and_durable_failures(self):
        for failure_at in (1, 2, 3):
            with self.subTest(failure_at=failure_at), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                original = os.fsync
                calls = 0
                def fail(fd):
                    nonlocal calls
                    calls += 1
                    if calls == failure_at:
                        raise OSError('injected sync failure')
                    original(fd)
                with patch('assemble.os.fsync', side_effect=fail), self.assertRaises(OSError):
                    finish_receipt(root, {'verdict': 'PRIVATE_PACKAGE_BUILT'})
                self.assertFalse((root / 'build-receipt.json').exists())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            finish_receipt(root, {'verdict': 'PRIVATE_PACKAGE_BUILT'})
            original = (root / 'build-receipt.json').read_bytes()
            with self.assertRaises(FileExistsError):
                finish_receipt(root, {'verdict': 'FAIL'})
            self.assertEqual((root / 'build-receipt.json').read_bytes(), original)


@unittest.skipUnless(sys.platform == 'linux' and os.geteuid() == 0, 'requires a disposable Linux root builder')
class RootStagingTests(unittest.TestCase):
    def test_writable_tool_ancestor_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'usr/bin/tool'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'tool')
            binary.chmod(0o755)
            fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            digest = hashlib.sha256(b'tool').hexdigest()
            try:
                copy_verified(fd, 'usr/bin/tool', digest, io.BytesIO(), trusted=True)
                binary.parent.chmod(0o777)
                with self.assertRaises(BuildError):
                    copy_verified(fd, 'usr/bin/tool', digest, io.BytesIO(), trusted=True)
                binary.parent.chmod(0o755)
                os.chown(binary.parent, 1, 1)
                with self.assertRaises(BuildError):
                    copy_verified(fd, 'usr/bin/tool', digest, io.BytesIO(), trusted=True)
                os.chown(binary.parent, 0, 0)
            finally:
                os.close(fd)

    def test_target_copy_is_complete_and_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / 'target'
            policy = 'etc/selinux/targeted/contexts/files/file_contexts'
            rows = {}
            for name, content in [('usr/share/rpm/rpmdb.sqlite', b'database'), (policy, b'policy')]:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                path.chmod(0o644)
                rows[name] = {'sha256': hashlib.sha256(content).hexdigest(), 'mode': 0o644}
            inventory = {'schema_version': 1, 'kind': 'polaris-sysext-target-metadata', 'files': rows}
            snapshot_metadata(source, root / 'copy', inventory, policy)
            verify_snapshot(source, inventory)
            (root / 'copy/usr/share/rpm/rpmdb.sqlite').write_bytes(b'private database change')
            verify_snapshot(source, inventory)
            for name, row in rows.items():
                self.assertEqual(hashlib.sha256((source / name).read_bytes()).hexdigest(), row['sha256'])
            extra = source / (policy + '.local')
            extra.write_bytes(b'changed policy')
            with self.assertRaises(BuildError):
                snapshot_metadata(source, root / 'unexpected-policy', inventory, policy)
            extra.unlink()
            (source / 'usr/share/rpm/rpmdb.sqlite-wal').write_bytes(b'pending database change')
            with self.assertRaises(BuildError):
                snapshot_metadata(source, root / 'unexpected-wal', inventory, policy)

    def test_rejects_changed_metadata_and_symlink_before_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            policy = 'etc/selinux/targeted/contexts/files/file_contexts'
            source = root / 'source'
            rows = {}
            for name in ('usr/share/rpm/rpmdb.sqlite', policy):
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'original')
                path.chmod(0o644)
                rows[name] = {'sha256': hashlib.sha256(b'original').hexdigest(), 'mode': 0o644}
            inventory = {'schema_version': 1, 'kind': 'polaris-sysext-target-metadata', 'files': rows}
            path = source / policy
            path.write_bytes(b'changed')
            with self.assertRaises(BuildError):
                snapshot_metadata(source, root / 'changed', inventory, policy)
            path.unlink()
            sentinel = root / 'outside'
            sentinel.write_bytes(b'original')
            path.symlink_to(sentinel)
            with self.assertRaises(BuildError):
                snapshot_metadata(source, root / 'symlink', inventory, policy)
            self.assertEqual(sentinel.read_bytes(), b'original')

    def test_wrong_manifest_fails_before_any_command_or_raw_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / 'inputs'
            source.mkdir()
            (source / 'manifest.json').write_text('{}')
            args = SimpleNamespace(inputs=source, output=root / 'output', manifest='manifest.json',
                                   manifest_sha256='0' * 64)
            with patch('assemble.Commands', side_effect=AssertionError('must not invoke commands')) as commands:
                self.assertEqual(assemble(args), 1)
                commands.assert_not_called()
            receipt = json.loads((args.output / 'build-receipt.json').read_text())
            self.assertEqual(receipt['verdict'], 'FAIL')
            self.assertFalse((args.output / 'polaris.raw').exists())


if __name__ == '__main__':
    unittest.main()
