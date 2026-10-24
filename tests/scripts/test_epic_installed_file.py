"""Bounded local fixture I/O only, never an installed launcher or account store."""

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[2] / "scripts/epic"
SPEC = importlib.util.spec_from_file_location(
    "polaris_epic_file", PACKAGE / "__init__.py", submodule_search_locations=[str(PACKAGE)])
epic = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = epic
SPEC.loader.exec_module(epic)
from polaris_epic_file import installed_file
from polaris_epic_file.installations import ManifestError


class EpicInstalledFileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.path = self.root / "installations.jsonl"
        self.raw = (json.dumps({"version": 1, "entries": 1}) + "\n" + json.dumps({
            "installationId": "install", "dir": "/games/title", "namespace": "sandbox",
            "itemId": "item", "artifactId": "app", "platform": "Windows", "complete": True,
            "version": "1", "launchExecutable": "game.exe"}) + "\n").encode()
        self.path.write_bytes(self.raw)

    def read(self, **kwargs):
        return installed_file.read_installations(str(self.root), **kwargs)

    def test_reads_only_named_installed_state_without_modifying_it(self):
        before = self.path.stat()
        current, = self.read()
        self.assertEqual(current.status, "candidate")
        self.assertEqual(self.path.read_bytes(), self.raw)
        self.assertEqual(before.st_mtime_ns, self.path.stat().st_mtime_ns)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["installations.jsonl"])

    def test_missing_file_does_not_fall_back_to_previous_or_temporary(self):
        self.path.rename(self.root / "installations.jsonl.prev")
        (self.root / "installations.jsonl.tmp").write_bytes(self.raw)
        with self.assertRaises(ManifestError):
            self.read()

    def test_symlink_file_is_not_followed(self):
        target = self.root / "other.jsonl"
        self.path.rename(target)
        self.path.symlink_to(target)
        with self.assertRaises(ManifestError):
            self.read()

    def test_symlink_data_root_is_not_followed(self):
        alias = self.root / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ManifestError):
            installed_file.read_installations(str(alias))

    def test_wrong_owner_or_invalid_uid_cannot_supply_state(self):
        for uid in (os.getuid() + 1, -1, True, "1000"):
            with self.subTest(uid=uid), self.assertRaises(ManifestError):
                self.read(expected_uid=uid)

    def test_fifo_is_refused_without_waiting_for_a_writer(self):
        self.path.unlink()
        os.mkfifo(self.path)
        with self.assertRaises(ManifestError):
            self.read()

    def test_directory_cannot_supply_installed_state(self):
        self.path.unlink()
        self.path.mkdir()
        with self.assertRaises(ManifestError):
            self.read()

    def test_size_limit_is_applied_before_reading(self):
        with patch.object(installed_file, "MAX_BYTES", len(self.raw) - 1), \
             patch.object(installed_file.os, "read") as read, self.assertRaises(ManifestError):
            self.read()
        read.assert_not_called()

    def test_append_during_read_does_not_return_an_old_complete_record(self):
        real_read = os.read
        changed = False

        def read_and_append(fd, count):
            nonlocal changed
            result = real_read(fd, count)
            if not changed:
                changed = True
                with self.path.open("ab") as handle:
                    handle.write(b'{"installationId":"install","complete":false}\n')
            return result

        with patch.object(installed_file.os, "read", side_effect=read_and_append), self.assertRaisesRegex(ManifestError, "changed"):
            self.read()

    def test_atomic_replacement_during_read_cannot_reuse_old_open_file(self):
        real_read = os.read
        changed = False

        def read_and_replace(fd, count):
            nonlocal changed
            result = real_read(fd, count)
            if not changed:
                changed = True
                replacement = self.root / "replacement"
                replacement.write_bytes(self.raw)
                replacement.replace(self.path)
            return result

        with patch.object(installed_file.os, "read", side_effect=read_and_replace), self.assertRaisesRegex(ManifestError, "changed"):
            self.read()

    def test_permission_failure_is_bounded_and_does_not_expose_paths(self):
        with patch.object(installed_file.os, "open", side_effect=PermissionError("private details")), \
             self.assertRaisesRegex(ManifestError, "unavailable") as error:
            self.read()
        self.assertNotIn("private details", str(error.exception))


if __name__ == "__main__":
    unittest.main()
