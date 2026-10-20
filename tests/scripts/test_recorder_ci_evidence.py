"""Run the real workflow shell with fixture-only commands, never users or native tests.

The runuser stand-in keeps the actual env -i boundary and redirects only ctest
to a fake callee. UID changes are modeled; Linux CI must prove the real UID.
"""
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = Path(os.environ.get("POLARIS_TEST_RECORDER_WORKFLOW", ROOT / ".github/workflows/build.yml"))
KINDS = ("negative", "window", "expiry", "timing")
ARCH_STEP = "Run multiseat runtime, stream and Go interop tests"
SANITIZER_STEP = "Run sanitizer and gamescope ownership tests"


def step(name):
    source = WORKFLOW.read_text().split("      - name: " + name + "\n", 1)[1]
    return re.split(r"\n(?:      - |  [a-z][a-z0-9-]*:)", source, maxsplit=1)[0]


def shell(name):
    return textwrap.dedent(step(name).split("        run: |\n", 1)[1])


class WorkflowFixture:
    def __init__(self, root, kind, *, test_failure=False, copy_failure=False, test_uid=1000):
        self.root, self.kind = root, kind
        self.tools = root / "bin"
        for directory in (self.tools, root / "runner", root / "temp", root / "home",
                          root / "build/tests", root / "build-sanitize"):
            directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        # Every effect is confined to this private fixture. No real runuser,
        # useradd, chown, install, native callee, or caller's HOME can be reached.
        fake = f"#!{sys.executable}\n" + textwrap.dedent(r'''
            import json, os
            from pathlib import Path
            import stat, sys, tempfile
            root = Path(ROOT_LITERAL)
            tool, args = Path(sys.argv[0]).name, sys.argv[1:]
            def record(value):
                with (root / "commands.jsonl").open("a") as f:
                    f.write(json.dumps(value) + "\n")
            record({"tool": tool, "args": args})
            if tool == "mktemp":
                assert args[0] == "-d" and len(args) == 2
                pattern = args[1]
                if pattern == "/tmp/polaris-ci.XXXXXX":
                    pattern = str(root / "temp/polaris-ci.XXXXXX")
                parent = Path(pattern).parent
                assert parent.is_relative_to(root)
                print(tempfile.mkdtemp(prefix=Path(pattern).name.replace("XXXXXX", ""), dir=parent))
            elif tool == "useradd":
                assert args[:3] == ["--user-group", "--no-create-home", "--home-dir"]
                assert len(args) == 5 and args[-1] == "polaris-tests"
                assert Path(args[3]).is_relative_to(root)
            elif tool == "chown":
                assert args == ["-hR", "polaris-tests:polaris-tests", "build/tests"]
            elif tool == "install":
                assert args[:7] == ["-d", "-m", "700", "-o", "polaris-tests", "-g", "polaris-tests"]
                for name in args[7:]:
                    p = Path(name)
                    assert p.is_relative_to(root)
                    p.mkdir(mode=0o700, parents=True, exist_ok=True)
                    p.chmod(0o700)
            elif tool == "runuser":
                assert args[:3] == ["-u", "polaris-tests", "--"]
                command = args[3:]
                if command == ["id", "-u"]:
                    print(TEST_UID_LITERAL)
                else:
                    assert command[:2] == ["env", "-i"]
                    # Redirect the callee, without changing env -i or any of
                    # its assignments, selectors, or lifecycle shell code.
                    command[command.index("ctest")] = str(root / "bin/ctest")
                    os.execv("/usr/bin/env", command)
            elif tool == "ctest":
                selector = args[args.index("-R") + 1]
                keys = ["PATH", "HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "GOCACHE"]
                keys += ["POLARIS_RECORDER_" + kind.upper() + "_EVIDENCE_DIR" for kind in KINDS_LITERAL]
                record({"selector": selector, "environment": {key: os.environ.get(key) for key in keys}})
                if KIND_LITERAL == "arch":
                    assert "POLARIS_TEST_POISON" not in os.environ
                    assert os.environ["PATH"] == "/usr/local/bin:/usr/bin"
                    assert Path(os.environ["HOME"]).is_relative_to(root)
                    (Path(os.environ["HOME"]) / "auth-secret").write_text("must not be exported")
                if "test_polaris_stream" in selector:
                    keys = ["POLARIS_RECORDER_" + kind.upper() + "_EVIDENCE_DIR" for kind in KINDS_LITERAL]
                    missing = [key for key in keys if not os.environ.get(key)]
                    if missing:
                        print("missing recorder evidence roots: " + ", ".join(missing), file=sys.stderr)
                        sys.exit(83)
                    for key in keys:
                        p = Path(os.environ[key])
                        assert p.is_absolute() and p.is_relative_to(root) and not p.is_symlink()
                        assert p.is_dir() and stat.S_IMODE(p.stat().st_mode) == 0o700
                        assert p.stat().st_uid == os.getuid()
                        case = p / "case"
                        case.mkdir(mode=0o700)
                        for name, data in [("packet.bin", b"synthetic fixture packet"), ("manifest.json", b'{}')]:
                            fd = os.open(case / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                            with os.fdopen(fd, "wb") as f:
                                f.write(data)
                    if TEST_FAILURE_LITERAL:
                        sys.exit(47)
            elif tool == "cp":
                assert COPY_FAILURE_LITERAL
                sys.exit(61)
            else:
                raise AssertionError(tool)
        ''')
        for old, value in (("ROOT_LITERAL", repr(str(root))), ("KIND_LITERAL", repr(kind)),
                           ("KINDS_LITERAL", repr(KINDS)), ("TEST_FAILURE_LITERAL", repr(test_failure)),
                           ("COPY_FAILURE_LITERAL", repr(copy_failure)), ("TEST_UID_LITERAL", repr(test_uid))):
            fake = fake.replace(old, value)
        commands = ["mktemp", "useradd", "chown", "install", "runuser", "ctest"]
        if copy_failure:
            commands.append("cp")
        for name in commands:
            p = self.tools / name
            p.write_text(fake)
            p.chmod(0o700)

    def run(self):
        environment = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.defpath,
                           HOME=str(self.root / "home"), RUNNER_TEMP=str(self.root / "runner"),
                           GITHUB_WORKSPACE=str(self.root), POLARIS_TEST_POISON="must be cleared")
        # Supplying poisoned inherited bindings also proves the Arch fix must
        # pass fresh roots through env -i, rather than depend on its parent.
        for kind in KINDS:
            environment["POLARIS_RECORDER_" + kind.upper() + "_EVIDENCE_DIR"] = "/unusable-inherited-evidence"
        return subprocess.run(["bash", "-euo", "pipefail", "-c", shell(ARCH_STEP if self.kind == "arch" else SANITIZER_STEP)],
                              cwd=self.root, env=environment, text=True, capture_output=True, timeout=15)

    def records(self):
        return [json.loads(line) for line in (self.root / "commands.jsonl").read_text().splitlines()]

    def retained(self):
        build = "build" if self.kind == "arch" else "build-sanitize"
        return list((self.root / build).glob("recorder-fixture-evidence.*"))


class RecorderEvidenceWiring(unittest.TestCase):
    def fixture(self, kind="arch", **options):
        temporary = tempfile.TemporaryDirectory(prefix="polaris-ci-wiring-")
        self.addCleanup(temporary.cleanup)
        return WorkflowFixture(Path(temporary.name), kind, **options)

    def assert_evidence(self, directory):
        expected = {f"{kind}/case/{name}" for kind in KINDS for name in ("packet.bin", "manifest.json")}
        files = {str(p.relative_to(directory)) for p in directory.rglob("*") if p.is_file()}
        self.assertEqual(files, expected)
        for p in directory.rglob("*"):
            self.assertFalse(p.is_symlink())
            self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o700 if p.is_dir() else 0o600)
            if p.name == "packet.bin":
                self.assertEqual(p.read_bytes(), b"synthetic fixture packet")

    def test_arch_passes_all_roots_through_env_i(self):
        f = self.fixture()
        result = f.run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([r["selector"] for r in f.records() if "selector" in r],
                         ["^test_multiseat_runtime$", "^test_polaris_stream$", "^polaris_multiseat_worker_go$"])
        for call in (r for r in f.records() if "selector" in r and "test_polaris_stream" not in r["selector"]):
            self.assertTrue(all(call["environment"]["POLARIS_RECORDER_" + kind.upper() + "_EVIDENCE_DIR"] is None
                                for kind in KINDS))
        self.assertEqual(len(f.retained()), 1)
        self.assert_evidence(f.retained()[0])
        self.assertEqual(list((f.root / "temp").iterdir()), [])
        self.assertIn({"tool": "chown", "args": ["-hR", "polaris-tests:polaris-tests", "build/tests"]}, f.records())
        self.assertTrue(all(r["args"][:3] == ["-u", "polaris-tests", "--"] for r in f.records() if r.get("tool") == "runuser"))

    def test_arch_test_failure_keeps_evidence_before_cleanup(self):
        f = self.fixture(test_failure=True)
        result = f.run()
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assert_evidence(f.retained()[0])
        self.assertEqual(list((f.root / "temp").iterdir()), [])
        self.assertNotIn("^polaris_multiseat_worker_go$", [r["selector"] for r in f.records() if "selector" in r])

    def test_arch_refuses_a_root_test_uid_before_any_callee(self):
        f = self.fixture(test_uid=0)
        result = f.run()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([r for r in f.records() if "selector" in r], [])
        self.assertEqual(list((f.root / "temp").iterdir()), [])
        self.assertEqual(f.retained(), [])

    def test_failed_retention_keeps_originals_and_original_test_failure(self):
        f = self.fixture(test_failure=True, copy_failure=True)
        result = f.run()
        self.assertEqual(result.returncode, 47, result.stderr)
        roots = list((f.root / "temp").iterdir())
        self.assertEqual(len(roots), 1)
        self.assert_evidence(roots[0] / "recorder-evidence")
        self.assertIn("retaining original", result.stderr)

    def test_failed_retention_cannot_turn_a_pass_into_success(self):
        f = self.fixture(copy_failure=True)
        result = f.run()
        self.assertNotEqual(result.returncode, 0)
        self.assert_evidence(next((f.root / "temp").iterdir()) / "recorder-evidence")

    def test_sanitizer_passes_all_roots_and_keeps_inventory(self):
        f = self.fixture("sanitizer")
        result = f.run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_evidence(f.retained()[0])
        calls = [r for r in f.records() if "selector" in r]
        self.assertEqual(len(calls), 1)
        self.assertEqual(set(calls[0]["selector"][2:-2].split("|")), {
            "test_inputtino_phys", "test_inputtino_device", "test_inputtino_reports", "test_multiseat_runtime",
            "test_polaris", "test_polaris_audio", "test_polaris_audit", "test_polaris_config", "test_polaris_http",
            "test_polaris_input", "test_polaris_network", "test_polaris_pairing", "test_polaris_platform",
            "test_polaris_steam_shutdown", "test_polaris_stream", "test_polaris_video", "polaris_gamescope_runtime_shell",
            "polaris_gamescope_session_stop_shell", "polaris_gamescope_session_geometry_shell",
            "polaris_gamescope_primary_child_shell", "polaris_gamescope_composite_cursor_shell"})

    def test_sanitizer_failure_retains_partial_fixture_outputs(self):
        f = self.fixture("sanitizer", test_failure=True)
        result = f.run()
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assert_evidence(f.retained()[0])

    def test_sanitizer_each_invocation_gets_fresh_roots(self):
        f = self.fixture("sanitizer")
        for _ in range(2):
            result = f.run()
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(f.retained()), 2)
        for directory in f.retained():
            self.assert_evidence(directory)

    def test_uploads_are_always_scoped_to_synthetic_roots(self):
        for kind in ("arch", "sanitizer"):
            upload = step(f"Upload {kind} recorder fixture evidence")
            self.assertIn("if: always()", upload)
            self.assertIn("uses: actions/upload-artifact@v7", upload)
            build = "build" if kind == "arch" else "build-sanitize"
            self.assertIn("path: " + build + "/recorder-fixture-evidence.*", upload)
            self.assertNotIn("runner.temp", upload)
            self.assertNotIn("$test_root", upload)
            self.assertNotIn("/home", upload)
            self.assertNotIn("/runtime", upload)


if __name__ == "__main__":
    unittest.main()
