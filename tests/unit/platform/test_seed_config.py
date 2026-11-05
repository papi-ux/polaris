"""Filesystem checks for the shared seeder and the actual installer output.

No service or installed Polaris is run. The installer writes a temporary prefix
under the unchanged HOME (common.sh's user-prefix rule), with isolated XDG paths.
"""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time
import unittest


SOURCE = Path(os.environ.get("POLARIS_SOURCE_DIR", Path(__file__).resolve().parents[3]))
HELPER = Path(os.environ.get("POLARIS_SEED_CONFIG_LIB", SOURCE / "scripts/install/lib/polaris-seed-config.sh"))
PAYLOAD = b"public = defaults\nsecond = value\n"


def metadata(path, follow_symlinks=True):
    # Looking up symlinks or listing a directory may itself update atime on
    # Linux. Compare metadata the seeder must preserve, excluding access time.
    info = os.stat(path, follow_symlinks=follow_symlinks)
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_nlink,
            info.st_uid, info.st_gid, info.st_mtime_ns, info.st_ctime_ns)


class SeedTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="polaris-seed-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.target = self.root / "polaris.conf"

    def command(self, *args):
        return ["bash", "-c", 'set -euo pipefail; umask 000; . "$1"; shift; polaris_seed_config "$@"',
                "test-seeder", str(HELPER), str(self.target), *map(str, args)]

    def seed(self, *args, data=PAYLOAD, env=None):
        return subprocess.run(self.command(*args), input=data, capture_output=True, timeout=10, env=env)

    def assert_private(self, path):
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def assert_clean(self):
        self.assertEqual(list(self.root.glob("*.tmp.*")), [])

    def start_stream(self, first_chunk=b"first = chunk\n"):
        existing = set(self.root.glob("*.tmp.*"))
        process = subprocess.Popen(self.command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.reap, process)
        process.stdin.write(first_chunk)
        process.stdin.flush()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            copies = list(set(self.root.glob("*.tmp.*")) - existing)
            if copies and copies[0].stat().st_size:
                self.assert_private(copies[0])
                self.assertFalse(os.path.lexists(self.target))
                return process
            if process.poll() is not None:
                self.fail("seeder exited before reading the stream: " + process.stderr.read().decode())
            time.sleep(0.01)
        self.fail("seeder did not begin its private copy within 5 seconds")

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.kill()
        process.communicate()

    def finish(self, process, clean=True):
        process.stdin.write(b"second = chunk\n")
        process.stdin.close()
        process.stdin = None
        _, err = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, err.decode())
        if clean:
            self.assert_clean()

    def test_stream_is_private_and_absent_until_complete(self):
        process = self.start_stream()
        self.finish(process)
        self.assertEqual(self.target.read_bytes(), b"first = chunk\nsecond = chunk\n")
        self.assert_private(self.target)

    def test_file_source_is_private_under_permissive_umask(self):
        source = self.root / "seed with ' quotes & spaces"
        source.write_bytes(PAYLOAD)
        source.chmod(0o644)
        self.assertEqual(self.seed(source).returncode, 0)
        self.assertEqual(self.target.read_bytes(), PAYLOAD)
        self.assert_private(self.target)
        self.assert_clean()

    def test_empty_stream_creates_private_empty_file(self):
        self.assertEqual(self.seed(data=b"").returncode, 0)
        self.assertEqual(self.target.read_bytes(), b"")
        self.assert_private(self.target)
        self.assert_clean()

    def test_existing_files_preserve_bytes_inode_and_every_mode(self):
        for mode in (0o000, 0o400, 0o600, 0o644):
            with self.subTest(mode=oct(mode)):
                self.target.write_bytes(b"retained = secret\n")
                self.target.chmod(mode)
                before = self.target.stat()
                # A nonexistent source must never be opened for an existing target.
                self.assertEqual(self.seed(self.root / "missing").returncode, 0)
                after = self.target.stat()
                self.assertEqual((after.st_ino, after.st_mode, after.st_mtime_ns),
                                 (before.st_ino, before.st_mode, before.st_mtime_ns))
                self.target.chmod(0o600)
                self.assertEqual(self.target.read_bytes(), b"retained = secret\n")
                self.target.unlink()
                self.assert_clean()

    def test_existing_empty_file_is_unchanged(self):
        self.target.touch(mode=0o400)
        self.assertEqual(self.seed().returncode, 0)
        self.assertEqual(self.target.read_bytes(), b"")
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o400)
        self.assert_clean()

    def test_existing_directory_is_unchanged(self):
        self.target.mkdir(mode=0o700)
        before = metadata(self.target)
        self.assertEqual(self.seed().returncode, 0)
        self.assertEqual(metadata(self.target), before)
        self.assertEqual(list(self.target.iterdir()), [])
        self.assert_clean()

    def test_existing_symlinks_are_unchanged(self):
        for kind in ("file", "directory", "missing"):
            with self.subTest(kind=kind):
                other = self.root / kind
                if kind == "file":
                    other.write_bytes(b"retained\n")
                    other.chmod(0o400)
                elif kind == "directory":
                    other.mkdir(mode=0o700)
                self.target.symlink_to(other)
                before = metadata(self.target, follow_symlinks=False)
                self.assertEqual(self.seed().returncode, 0)
                self.assertEqual(metadata(self.target, follow_symlinks=False), before)
                self.assertEqual(os.readlink(self.target), str(other))
                if kind == "file":
                    self.assertEqual(other.read_bytes(), b"retained\n")
                    self.assertEqual(stat.S_IMODE(other.stat().st_mode), 0o400)
                elif kind == "directory":
                    self.assertEqual(list(other.iterdir()), [])
                    self.assertEqual(stat.S_IMODE(other.stat().st_mode), 0o700)
                else:
                    self.assertFalse(other.exists())
                self.target.unlink()
                self.assert_clean()

    def test_existing_fifo_is_unchanged_without_opening_it(self):
        os.mkfifo(self.target, 0o600)
        before = metadata(self.target)
        self.assertEqual(self.seed().returncode, 0)
        self.assertEqual(metadata(self.target), before)
        self.assert_clean()

    def test_racing_file_creator_keeps_bytes_inode_and_mode(self):
        process = self.start_stream()
        self.target.write_bytes(b"concurrent = retained\n")
        self.target.chmod(0o400)
        before = metadata(self.target)
        self.finish(process)
        self.assertEqual(metadata(self.target), before)
        self.assertEqual(self.target.read_bytes(), b"concurrent = retained\n")

    def test_racing_directory_does_not_receive_a_link(self):
        process = self.start_stream()
        self.target.mkdir(mode=0o700)
        before = metadata(self.target)
        self.finish(process)
        self.assertEqual(list(self.target.iterdir()), [])
        self.assertEqual(metadata(self.target), before)

    def test_racing_directory_symlink_does_not_receive_a_link(self):
        process = self.start_stream()
        other = self.root / "other"
        other.mkdir(mode=0o700)
        self.target.symlink_to(other)
        before = (metadata(self.target, follow_symlinks=False), metadata(other))
        self.finish(process)
        self.assertEqual(list(other.iterdir()), [])
        self.assertEqual((metadata(self.target, follow_symlinks=False), metadata(other)), before)

    def test_racing_dangling_symlink_is_preserved(self):
        process = self.start_stream()
        other = self.root / "missing"
        self.target.symlink_to(other)
        self.finish(process)
        self.assertEqual(os.readlink(self.target), str(other))
        self.assertFalse(other.exists())

    def test_two_real_seeders_publish_one_complete_private_file(self):
        first = self.start_stream()
        second = self.start_stream(b"other = chunk\n")
        self.finish(first, clean=False)
        self.finish(second)
        self.assertEqual(self.target.read_bytes(), b"first = chunk\nsecond = chunk\n")
        self.assert_private(self.target)

    def test_missing_seed_read_fails_without_publication(self):
        self.assertNotEqual(self.seed(self.root / "missing").returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assert_clean()

    def test_directory_seed_read_fails_without_publication(self):
        self.assertNotEqual(self.seed(self.root).returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assert_clean()

    def test_partial_stream_read_failure_cleans_up(self):
        shim = self.root / "commands"
        shim.mkdir()
        cat = shim / "cat"
        cat.write_text("#!/usr/bin/env bash\nprintf 'partial = data\\n'\nexit 71\n")
        cat.chmod(0o755)
        env = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ["PATH"])
        self.assertNotEqual(self.seed(env=env).returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assert_clean()

    def test_publication_failure_without_competing_path_is_propagated(self):
        shim = self.root / "commands"
        shim.mkdir()
        for name in ("link", "ln"):
            command = shim / name
            command.write_text("#!/usr/bin/env bash\nexit 72\n")
            command.chmod(0o755)
        env = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ["PATH"])
        result = self.seed(env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assert_clean()

    def test_missing_parent_fails_without_creating_it(self):
        self.target = self.root / "missing" / "polaris.conf"
        self.assertNotEqual(self.seed().returncode, 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_temporary_creation_failure_is_propagated(self):
        shim = self.root / "commands"
        shim.mkdir()
        command = shim / "mktemp"
        command.write_text("#!/usr/bin/env bash\nexit 73\n")
        command.chmod(0o755)
        env = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ["PATH"])
        self.assertNotEqual(self.seed(env=env).returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assert_clean()

    def test_seed_does_not_change_callers_umask(self):
        result = subprocess.run(["bash", "-c",
                                 'set -euo pipefail; umask 000; . "$1"; polaris_seed_config "$2"; test "$(umask)" = 0000',
                                 "test-seeder", str(HELPER), str(self.target)],
                                input=PAYLOAD, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assert_private(self.target)
        self.assert_clean()

    def test_quoted_and_option_like_relative_paths(self):
        source = self.root / "-seed ' & | $() ` literal"
        source.write_bytes(PAYLOAD)
        result = subprocess.run(["bash", "-c",
                                 'set -euo pipefail; umask 000; . "$1"; polaris_seed_config "$2" "$3"',
                                 "test-seeder", str(HELPER), "-config ' & | $() ` literal", str(source)],
                                cwd=self.root, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        target = self.root / "-config ' & | $() ` literal"
        self.assertEqual(target.read_bytes(), PAYLOAD)
        self.assert_private(target)
        self.assert_clean()


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix=".polaris-seed-installer-", dir=Path.home())
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)

    def install(self, quoted=False):
        parent = self.root / ("quoted ' & | $() ` \\ paths" if quoted else "plain")
        self.prefix = parent / "prefix"
        self.config = parent / "xdg" / "polaris"
        commands = self.root / "commands"
        commands.mkdir()
        for name in ("sudo", "systemctl"):
            command = commands / name
            command.write_text("#!/usr/bin/env bash\necho 'unexpected host command' >&2\nexit 97\n")
            command.chmod(0o755)
        self.marker = self.root / "exec-called"
        # Exercise the generated launcher using only Bash and the coreutils
        # commands it needs, independent of the developer's interactive PATH.
        runtime = self.root / "runtime-commands"
        runtime.mkdir()
        for name in ("bash", "cat", "dirname", "link", "mkdir", "mktemp", "rm"):
            executable = shutil.which(name)
            self.assertIsNotNone(executable, "missing runtime command: " + name)
            (runtime / name).symlink_to(executable)
        self.runtime_path = str(runtime)
        binary = parent / "polaris binary"
        binary.parent.mkdir(parents=True)
        binary.write_text('#!/usr/bin/env bash\nprintf called >"$POLARIS_TEST_EXEC_MARKER"\n')
        binary.chmod(0o755)
        self.env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                        XDG_CONFIG_HOME=str(parent / "xdg"), XDG_RUNTIME_DIR=str(parent / "runtime"),
                        PREFIX=str(self.prefix), BIN_DIR=str(self.prefix / "bin"),
                        LIBEXEC_DIR=str(self.prefix / "libexec/polaris"), CONFIG_DIR=str(self.config),
                        SYSTEMD_USER_DIR=str(parent / "xdg/systemd/user"),
                        TMPDIR=str(self.root), POLARIS_INSTALL_SKIP_SYSTEMD="1",
                        POLARIS_TEST_EXEC_MARKER=str(self.marker))
        result = subprocess.run(["bash", str(SOURCE / "scripts/install/03-install-gamescope-stack.sh"),
                                 "--polaris-bin", str(binary)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode())
        self.launcher = self.prefix / "bin/polaris-start"
        self.seed_file = self.config / "polaris.conf.gamescope-stream.example"
        self.target = self.config / "polaris.conf"

    def launch(self):
        return subprocess.run(["bash", "-c", 'umask 000; exec bash "$1"', "test-launcher", str(self.launcher)],
                              env=dict(self.env, PATH=self.runtime_path), capture_output=True, timeout=10)

    def test_installer_ships_shared_helper_and_launcher_uses_file_seed(self):
        self.install()
        self.assertEqual((self.prefix / "bin/polaris-seed-config.sh").read_bytes(), HELPER.read_bytes())
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self.target.read_bytes(), self.seed_file.read_bytes())
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o600)
        self.assertTrue(self.marker.exists())

    def test_installed_launcher_fallback_is_private(self):
        self.install()
        self.seed_file.unlink()
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"linux_stream_mode = gamescope_stream\n", self.target.read_bytes())
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o600)
        self.assertTrue(self.marker.exists())

    def test_installed_launcher_preserves_existing_secret_and_mode(self):
        self.install()
        self.target.write_bytes(b"retained = fake-secret\n")
        self.target.chmod(0o400)
        before = metadata(self.target)
        self.assertEqual(self.launch().returncode, 0)
        self.assertEqual(metadata(self.target), before)
        self.assertEqual(self.target.read_bytes(), b"retained = fake-secret\n")

    def test_installed_launcher_preserves_directory(self):
        self.install()
        self.target.mkdir(mode=0o700)
        before = metadata(self.target)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(list(self.target.iterdir()), [])
        self.assertEqual(metadata(self.target), before)

    def test_installer_generated_launcher_handles_quoted_paths(self):
        self.install(quoted=True)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self.target.read_bytes(), self.seed_file.read_bytes())
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o600)
        self.assertTrue(self.marker.exists())

    def test_installed_launcher_seed_read_failure_prevents_exec(self):
        self.install()
        self.seed_file.unlink()
        # A directory is an existing seed path, but cannot be read as a file.
        self.seed_file.mkdir()
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(os.path.lexists(self.target))
        self.assertFalse(self.marker.exists())
        self.assertEqual(list(self.config.glob("*.tmp.*")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
