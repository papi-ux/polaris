"""Offline provenance and permission-delta checks for the Steam sandbox policy."""
import hashlib
import json
from pathlib import Path
import unittest

DIRECTORY = Path(__file__).resolve().parent / "seccomp"


class SteamSeccompProfile(unittest.TestCase):
    def test_pinned_upstream_bytes(self):
        source = json.loads((DIRECTORY / "upstream.json").read_text())
        self.assertEqual(source["repository"], "https://github.com/moby/profiles")
        self.assertEqual(source["revision"], "61eaf32614c7c71b60bd8927d3e6a4ffc8ff1f31")
        self.assertEqual(hashlib.sha256((DIRECTORY / "docker-default.json").read_bytes()).hexdigest(),
                         source["sha256"])
        self.assertIn("Apache License", (DIRECTORY / "LICENSE.moby").read_text())

    def test_only_the_reviewed_sandbox_operations_are_added(self):
        base = json.loads((DIRECTORY / "docker-default.json").read_text())
        expected = json.loads(json.dumps(base))
        expected["syscalls"] += [
            {"names": ["unshare"], "action": "SCMP_ACT_ALLOW", "args": [
                {"index": 0, "value": 0x10000000, "valueTwo": 0x10000000,
                 "op": "SCMP_CMP_MASKED_EQ"}]},
            {"names": ["clone"], "action": "SCMP_ACT_ALLOW", "args": [
                {"index": 0, "value": 0x10000000, "valueTwo": 0x10000000,
                 "op": "SCMP_CMP_MASKED_EQ"}], "includes": {"arches": ["amd64", "x86"]}},
            {"names": ["mount", "umount2", "pivot_root", "chroot"], "action": "SCMP_ACT_ALLOW"},
            {"names": ["clone"], "action": "SCMP_ACT_ALLOW", "args": [
                {"index": 0, "value": 0x20000011, "op": "SCMP_CMP_EQ"}],
             "includes": {"arches": ["amd64", "x86"]}},
        ]
        actual = json.loads((DIRECTORY / "steam.json").read_text())
        self.assertEqual(actual, expected)
        self.assertEqual(actual["defaultAction"], "SCMP_ACT_ERRNO")

    def test_docker_compaction_matches_the_compiled_policy(self):
        raw = (DIRECTORY / "steam.json").read_text()
        self.assertEqual(raw, json.dumps(json.loads(raw), sort_keys=True, indent=2) + "\n")
        self.assertLess(len(raw), 65536)
        self.assertNotIn(')POLARIS_SECCOMP"', raw)


if __name__ == "__main__":
    unittest.main()
