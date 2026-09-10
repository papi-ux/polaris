import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import inspect_host as host


class AssessmentTests(unittest.TestCase):
    def setUp(self):
        self.service = {
            "available": True, "MainPID": "42", "InvocationID": "a" * 32,
            "ActiveState": "active", "SubState": "running",
        }
        self.process = {
            "available": True, "pid": 42, "credentials": {"Uid": [1000] * 4},
            "executable": {"available": True, "sha256": "b" * 64},
        }

    def assess(self, after=None, expected="b" * 64):
        return host.assess(self.service, after or self.service, self.process, 1000,
                           {"available": True, "sha256": "c" * 64}, expected)

    def test_preview_is_distinct_from_usr_bin_without_claiming_readiness(self):
        result = self.assess()
        self.assertEqual(result["service_snapshot"], "consistent")
        self.assertEqual(result["runtime_matches_expected"], "match")
        self.assertEqual(result["usr_bin_matches_running_executable"], "mismatch")
        for key in ("input_access", "installed_lifecycle", "streaming"):
            self.assertEqual(result[key], "not_tested")

    def test_different_candidate_does_not_match_preview(self):
        self.assertEqual(self.assess(expected="d" * 64)["runtime_matches_expected"], "mismatch")

    def test_restart_or_pid_change_invalidates_runtime_comparison(self):
        for field, value in [("MainPID", "43"), ("InvocationID", "d" * 32), ("ActiveState", "inactive"), ("SubState", "stop")]:
            with self.subTest(field=field):
                after = dict(self.service, **{field: value})
                result = self.assess(after=after)
                self.assertEqual(result["service_snapshot"], "unavailable_or_changed")
                self.assertEqual(result["runtime_matches_expected"], "unavailable")

    def test_missing_invocation_or_wrong_process_user_is_not_consistent(self):
        for change in ("invocation", "uid", "saved_uid", "pid"):
            with self.subTest(change=change):
                self.setUp()
                if change == "invocation": self.service["InvocationID"] = ""
                if change == "uid": self.process["credentials"]["Uid"] = [0] * 4
                if change == "saved_uid": self.process["credentials"]["Uid"][2] = 0
                if change == "pid": self.process["pid"] = 43
                self.assertEqual(self.assess()["service_snapshot"], "unavailable_or_changed")

    def test_unreadable_executable_is_unknown_not_matching(self):
        self.process["executable"] = {"available": False, "error": "PermissionError"}
        self.assertEqual(self.assess()["runtime_matches_expected"], "unavailable")
        self.assertEqual(self.assess()["usr_bin_matches_running_executable"], "unavailable")

    def test_no_expected_digest_has_no_implicit_candidate_match(self):
        self.assertEqual(self.assess(expected=None)["runtime_matches_expected"], "not_requested")


class CollectionBoundaryTests(unittest.TestCase):
    def test_output_is_private_and_does_not_overwrite_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "before.json"
            host.write_observation(path, os.geteuid(), {"observation": True})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            original = path.read_bytes()
            with self.assertRaises(FileExistsError):
                host.write_observation(path, os.geteuid(), {"replacement": True})
            self.assertEqual(path.read_bytes(), original)

    def test_shared_output_directory_is_rejected_before_creating_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root.chmod(0o755)
            with self.assertRaises(ValueError):
                host.write_observation(root / "before.json", os.geteuid(), {})
            self.assertFalse((root / "before.json").exists())

    def test_output_parent_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "private"
            target.mkdir(mode=0o700)
            (root / "link").symlink_to(target, target_is_directory=True)
            with self.assertRaises(OSError):
                host.write_observation(root / "link/before.json", os.geteuid(), {})
            self.assertFalse((target / "before.json").exists())

    def test_same_service_pid_with_changed_process_invalidates_snapshot(self):
        service = {"available": True, "MainPID": "42", "InvocationID": "a" * 32,
                   "ActiveState": "active", "SubState": "running"}
        process = {"available": True, "pid": 42, "start_ticks": "1",
                   "credentials": {"Uid": [1000] * 4},
                   "executable": {"available": True, "sha256": "b" * 64}}
        changed = dict(process, start_ticks="2")
        with patch.object(host, "service_state", return_value=service), \
             patch.object(host, "process_identity", side_effect=[process, changed]), \
             patch.object(host, "file_identity", return_value=process["executable"]), \
             patch.object(host, "command", return_value={"returncode": 1, "stdout": ""}), \
             patch.object(host, "device_metadata", return_value={"available": False}), \
             patch.object(host.Path, "read_text", return_value="boot"):
            result = host.collect(1000, "b" * 64)
        self.assertEqual(result["assessment"]["service_snapshot"], "unavailable_or_changed")
        self.assertEqual(result["assessment"]["runtime_matches_expected"], "unavailable")

    def test_device_metadata_does_not_open_device(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "uinput"
            path.write_bytes(b"fixture")
            with patch.object(host.os, "open", side_effect=AssertionError("device opened")):
                result = host.device_metadata(path)
            self.assertFalse(result["character_device"])
            self.assertNotIn("readable", result)

    def test_extension_symlink_is_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "polaris.raw"
            target = Path(directory) / "target"
            target.write_bytes(b"fixture")
            path.symlink_to(target)
            self.assertFalse(host.file_identity(path, no_follow=True)["available"])
            self.assertEqual(host.file_identity(target)["sha256"], hashlib.sha256(b"fixture").hexdigest())

    def test_missing_or_nonregular_file_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            for path in (Path(directory), Path(directory) / "absent"):
                self.assertFalse(host.file_identity(path)["available"])

    def test_deployment_snapshot_omits_embedded_oci_manifest_and_keys(self):
        data = {"deployments": [{"booted": True, "checksum": "d", "base-commit-meta": {"secret": "omit"}}], "transaction": None}
        result = host.deployment_summary({"returncode": 0, "stdout": json.dumps(data)})
        self.assertTrue(result["available"])
        self.assertNotIn("secret", json.dumps(result))
        self.assertFalse(result["transaction_active"])

    def test_bad_deployment_responses_are_unavailable(self):
        for result in [{"returncode": 1}, {"returncode": 0, "stdout": "not json"}, {"returncode": 0, "stdout": '{"deployments": "wrong"}'}]:
            self.assertFalse(host.deployment_summary(result)["available"])

    def test_service_query_rejects_root_or_another_users_manager(self):
        with patch.object(host.os, "geteuid", return_value=1000):
            for uid in (0, -1, 1001):
                with self.assertRaises(ValueError): host.service_command(uid)
            query = host.service_command(1000)
            start = query.index("systemctl")
            self.assertEqual(query[start:start + 4], ["systemctl", "--user", "show", "polaris.service"])

    def test_service_query_selects_desktop_bus_without_ssh_environment(self):
        with patch.object(host.os, "geteuid", return_value=1000):
            query = host.service_command(1000)
        self.assertEqual(query[:2], ["env", "-i"])
        self.assertIn("XDG_RUNTIME_DIR=/run/user/1000", query)
        self.assertIn("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus", query)

    def test_root_observation_queries_service_as_selected_desktop_user(self):
        account = host.pwd.struct_passwd(("desktop", "x", 1000, 1000, "", "", ""))
        with patch.object(host.os, "geteuid", return_value=0), \
             patch.object(host.pwd, "getpwuid", return_value=account) as lookup:
            query = host.service_command(1000)
            with self.assertRaises(ValueError):
                host.service_command(0)
        lookup.assert_called_once_with(1000)
        self.assertEqual(query[:6], ["runuser", "--user", "desktop", "--", "env", "-i"])
        self.assertIn("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus", query)

    def test_service_query_omits_command_arguments_and_environment(self):
        with patch.object(host.os, "geteuid", return_value=1000):
            query = host.service_command(1000)
        self.assertNotIn("--property=ExecStart", query)
        self.assertNotIn("--property=Environment", query)

    def test_proc_stat_handles_spaces_and_parentheses_in_command_name(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "42"
            root.mkdir()
            (root / "stat").write_text("42 (some ) process) " + " ".join(["S"] + ["0"] * 18 + ["12345"]))
            (root / "status").write_text("Uid:\t1000 1000 1000 1000\nGid:\t1000 1000 1000 1000\nGroups:\t10 1000\nCapEff:\t00000000\n")
            (root / "exe").write_bytes(b"elf fixture")
            result = host.process_identity(42, Path(directory))
            self.assertTrue(result["available"])
            self.assertEqual(result["start_ticks"], "12345")
            self.assertEqual(result["credentials"]["Groups"], [10, 1000])


if __name__ == "__main__":
    unittest.main()
