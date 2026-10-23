"""Installed-state decisions without launcher, filesystem or account access."""

import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[2] / "scripts/epic"
SPEC = importlib.util.spec_from_file_location(
    "polaris_epic", PACKAGE / "__init__.py", submodule_search_locations=[str(PACKAGE)])
epic = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = epic
SPEC.loader.exec_module(epic)
from polaris_epic import installations as installed


def row(**changes):
    result = dict(installationId="installation-1", dir="/games/AlanWake2",
                  platform="Windows", namespace="sandbox", itemId="catalog",
                  artifactId="artifact", launchExecutable="AlanWake2.exe",
                  complete=True, version="1.2.11", launchCommand="")
    result.update(changes)
    return result


def log(*rows, header=None):
    if header is None:
        header = {"version": 1, "entries": len(rows)}
    return b"\n".join(json.dumps(r).encode() for r in (header, *rows)) + b"\n"


class EpicInstallationsTest(unittest.TestCase):
    def parse(self, raw):
        return installed.parse_installations(raw, "/home/player/.local/share/com.epicgames.eglv2")

    def test_incomplete_then_complete_resolves_current_record(self):
        current, = self.parse(log(row(complete=False), row()))
        self.assertEqual(current.status, "candidate")
        self.assertEqual(str(current.title.image), "/games/AlanWake2/AlanWake2.exe")
        self.assertEqual(current.version, "1.2.11")

    def test_new_incomplete_record_never_resurrects_old_complete(self):
        current, = self.parse(log(row(), row(complete=False, version=None)))
        self.assertEqual(current.status, "updating")
        self.assertIsNone(current.title)

    def test_unknown_current_metadata_supersedes_old_complete(self):
        current, = self.parse(log(row(), {"installationId": "installation-1", "complete": True}))
        self.assertEqual(current.status, "invalid")
        self.assertIsNone(current.title)

    def test_uninstall_tombstone_removes_completed_install(self):
        self.assertEqual(self.parse(log(row(), {"uid": "installation-1", "deleted": True,
                                               "writtenAt": "2026-10-23T00:00:00Z"})), ())

    def test_tombstone_never_requires_older_complete_fallback(self):
        self.assertEqual(self.parse(log(row(), {"uid": "installation-1", "deleted": True})), ())

    def test_reinstall_after_tombstone_is_new_current_record(self):
        current, = self.parse(log(row(), {"uid": "installation-1", "deleted": True}, row(version="2")))
        self.assertEqual(current.version, "2")

    def test_header_entries_is_not_assumed_equal_to_append_count(self):
        current, = self.parse(log(row(), row(version="2"), header={"version": 1, "entries": 0}))
        self.assertEqual(current.version, "2")

    def test_version_and_header_types_are_strict(self):
        for header in ({"version": True, "entries": 0}, {"version": 2, "entries": 0},
                       {"version": "1", "entries": 0}, {"version": 1, "entries": -1},
                       {"version": 1, "entries": True}, {"version": 1}):
            with self.subTest(header=header), self.assertRaises(installed.ManifestError):
                self.parse(log(row(), header=header))

    def test_conflicting_tombstone_identity_refuses_snapshot(self):
        for bad in ({"deleted": True}, {"uid": "installation-1", "deleted": True,
                                       "installationId": "other"},
                    {"uid": "installation-1", "deleted": "true"}):
            with self.subTest(bad=bad), self.assertRaises(installed.ManifestError):
                self.parse(log(row(), bad))

    def test_unkeyable_tail_does_not_recover_an_old_completed_row(self):
        for tail in (b'{"complete":true}', b'{"installationId":', b'[]', b'null', b'\xff'):
            with self.subTest(tail=tail), self.assertRaises(installed.ManifestError):
                self.parse(log(row()) + tail)

    def test_duplicate_json_keys_and_nonfinite_numbers_are_rejected(self):
        for raw in (b'{"version":1,"entries":1,"entries":0}\n',
                    log(row()) + b'{"installationId":"installation-1","complete":false,"complete":true}',
                    log(row()) + b'{"installationId":"installation-1","version":NaN}'):
            with self.subTest(raw=raw), self.assertRaises(installed.ManifestError):
                self.parse(raw)

    def test_file_and_line_bounds_refuse_before_resolving(self):
        with patch.object(installed, "MAX_BYTES", 20), self.assertRaises(installed.ManifestError):
            self.parse(log(row()))
        with patch.object(installed, "MAX_LINE_BYTES", 30), self.assertRaises(installed.ManifestError):
            self.parse(log(row()))
        with patch.object(installed, "MAX_LINES", 1), self.assertRaises(installed.ManifestError):
            self.parse(log(row(), row()))

    def test_empty_snapshot_or_blank_tail_refuses_snapshot(self):
        for raw in (b"", log(row()) + b"\n", "not bytes"):
            with self.subTest(raw=raw), self.assertRaises(installed.ManifestError):
                self.parse(raw)

    def test_pending_update_or_missing_version_is_not_a_candidate(self):
        for record, status in ((row(pendingVersion="next"), "updating"),
                               (row(version=""), "invalid"), (row(complete=1), "invalid")):
            with self.subTest(record=record):
                current, = self.parse(log(record))
                self.assertEqual(current.status, status)
                self.assertIsNone(current.title)

    def test_native_or_custom_launch_command_requires_separate_support(self):
        for record in (row(platform="Linux"), row(launchCommand="different.exe --launch")):
            with self.subTest(record=record):
                current, = self.parse(log(record))
                self.assertEqual(current.status, "unsupported")
                self.assertIsNone(current.title)

    def test_traversal_and_control_characters_cannot_produce_candidates(self):
        for change in ({"dir": "/games/../other"}, {"dir": "relative"}, {"dir": "//other/game"},
                       {"dir": "/games\x00/title"}, {"dir": "/games\n/title"},
                       {"launchExecutable": "../game.exe"}, {"launchExecutable": "/game.exe"},
                       {"launchExecutable": "sub\\game.exe"}, {"launchExecutable": "game\n.exe"},
                       {"artifactId": "artifact?action=uninstall"}):
            with self.subTest(change=change):
                current, = self.parse(log(row(**change)))
                self.assertEqual(current.status, "invalid")
                self.assertIsNone(current.title)

    def test_unicode_controls_separators_and_surrogates_cannot_be_paths(self):
        for control in ("\x85", "\x9b", "\u2028", "\u2029", "\u202e", "\ud800"):
            for change in ({"dir": "/games/a" + control + "b"},
                           {"launchExecutable": "game" + control + ".exe"}):
                with self.subTest(change=change):
                    current, = self.parse(log(row(**change)))
                    self.assertEqual(current.status, "invalid")
                    self.assertIsNone(current.title)

    def test_printable_unicode_title_paths_are_preserved(self):
        current, = self.parse(log(row(dir="/games/勇者の冒険", launchExecutable="勇者.exe")))
        self.assertEqual(current.status, "candidate")
        self.assertEqual(str(current.title.image), "/games/勇者の冒険/勇者.exe")

    def test_same_app_id_in_two_installations_is_not_merged(self):
        current = self.parse(log(row(), row(installationId="installation-2", dir="/games/another")))
        self.assertEqual([r.status for r in current], ["conflict", "conflict"])
        self.assertTrue(all(r.title is None for r in current))

    def test_shared_directory_with_newer_invalid_install_still_conflicts(self):
        current = self.parse(log(row(), {"installationId": "installation-2", "dir": "/games/AlanWake2"}))
        self.assertEqual([r.status for r in current], ["conflict", "conflict"])

    def test_deleting_conflicting_installation_resolves_live_identity(self):
        current, = self.parse(log(row(), row(installationId="installation-2"),
                                 {"uid": "installation-2", "deleted": True}))
        self.assertEqual(current.status, "candidate")

    def test_two_different_titles_and_directories_remain_separate(self):
        current = self.parse(log(row(), row(installationId="installation-2", itemId="other",
                                           artifactId="other", dir="/games/other")))
        self.assertEqual([r.status for r in current], ["candidate", "candidate"])
        self.assertNotEqual(current[0].title.uri, current[1].title.uri)

    def test_unrelated_extension_fields_do_not_change_known_state(self):
        current, = self.parse(log(row(futureField={"ignored": True})))
        self.assertEqual(current.status, "candidate")

    def test_valid_final_line_needs_no_trailing_newline(self):
        current, = self.parse(log(row()).rstrip(b"\n"))
        self.assertEqual(current.status, "candidate")

    def test_invalid_data_root_never_constructs_a_title(self):
        for root in ("relative", "/", "/home/../tmp", "/home/player\x00/data"):
            with self.subTest(root=root), self.assertRaises(installed.ManifestError):
                installed.parse_installations(log(row()), root)


if __name__ == "__main__":
    unittest.main()
