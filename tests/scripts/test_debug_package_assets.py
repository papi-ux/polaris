import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "scripts/ci/validate-pacman-debug.py"
WORKFLOW = Path(os.environ.get("POLARIS_TEST_RELEASE_WORKFLOW", ROOT / ".github/workflows/build.yml"))


def archive(path, name, payload, version="1.4.13-1", architecture="x86_64"):
    metadata = f"pkgname = {name}\npkgver = {version}\narch = {architecture}\n".encode()
    raw = path.with_suffix(".tar")
    with tarfile.open(raw, "w") as output:
        for member, data in {".PKGINFO": metadata, **payload}.items():
            info = tarfile.TarInfo(member)
            info.size = len(data)
            output.addfile(info, io.BytesIO(data))
    subprocess.run(["zstd", "-q", "-f", str(raw), "-o", str(path)], check=True)
    raw.unlink()


class DebugPackageAssets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [tool for tool in ("cc", "objcopy", "strip", "readelf", "tar", "zstd") if not shutil.which(tool)]
        if missing:
            raise RuntimeError("Linux package fixture tools are required: " + ", ".join(missing))
        cls.compiled = tempfile.TemporaryDirectory(prefix="polaris-symbol-fixture-")
        root = Path(cls.compiled.name)
        for value in (0, 1):
            source = root / f"host{value}.c"
            binary = root / f"host{value}"
            symbols = root / f"host{value}.debug"
            source.write_text(f"int main(void) {{ return {value}; }}\n")
            subprocess.run(["cc", "-g", "-Wl,--build-id=sha1", str(source), "-o", str(binary)], check=True)
            subprocess.run(["objcopy", "--only-keep-debug", str(binary), str(symbols)], check=True)
            subprocess.run(["strip", "--strip-debug", str(binary)], check=True)
        cls.host = (root / "host0").read_bytes()
        cls.symbols = (root / "host0.debug").read_bytes()
        cls.other_host = (root / "host1").read_bytes()
        cls.other_symbols = (root / "host1.debug").read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="polaris-debug-assets-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.raw = self.root / "release-assets/raw"
        self.arch = self.raw / "arch"
        self.steam = self.raw / "steamos3.8"
        self.arch.mkdir(parents=True)
        self.steam.mkdir()
        self.archives = {}
        for directory, platform in ((self.arch, "arch"), (self.steam, "steamos3.8")):
            for name, payload in (
                ("polaris", {"usr/bin/polaris-1.4.13": self.host}),
                ("polaris-kms", {"usr/libexec/polaris/polaris-kms": self.host}),
                ("polaris-debug", {"usr/lib/debug/usr/bin/polaris-1.4.13.debug": self.symbols}),
            ):
                suffix = name.removeprefix("polaris")
                filename = f"{name}-1.4.13-1-x86_64.pkg.tar.zst" if platform == "arch" else f"Polaris{suffix}-steamos3.8-x86_64.pkg.tar.zst"
                path = directory / filename
                archive(path, name, payload)
                self.archives[platform, name] = path
        script = self.root / "scripts/ci/validate-pacman-debug.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(VALIDATOR, script)
        for directory, extension in (("fedora44", "rpm"), ("ubuntu24.04", "deb")):
            folder = self.raw / directory
            folder.mkdir()
            for component in ("Polaris", "Polaris-kms"):
                (folder / f"{component}.{extension}").write_bytes(component.encode())

    def validate(self, platform="arch"):
        return subprocess.run([
            "python3", str(VALIDATOR),
            "--main", str(self.archives[platform, "polaris"]),
            "--kms", str(self.archives[platform, "polaris-kms"]),
            "--debug", str(self.archives[platform, "polaris-debug"]),
        ], text=True, capture_output=True, timeout=30)

    def stage(self):
        workflow = WORKFLOW.read_text()
        step = workflow.split("      - name: Prepare release asset names\n", 1)[1].split("\n      - name:", 1)[0]
        body = textwrap.dedent(step.split("        run: |\n", 1)[1])
        return subprocess.run(["bash", "-e", "-c", body], cwd=self.root, text=True, capture_output=True, timeout=60)

    def replace_debug(self, *, name="polaris-debug", version="1.4.13-1", architecture="x86_64", symbols=None, platform="arch"):
        archive(self.archives[platform, "polaris-debug"], name,
                {"usr/lib/debug/usr/bin/polaris-1.4.13.debug": self.symbols if symbols is None else symbols},
                version, architecture)

    def test_matching_real_elf_symbols_validate(self):
        result = self.validate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("host/KMS ELF build ID", result.stdout)

    def test_wrong_package_name_version_or_architecture_is_rejected(self):
        for changes, message in (({"name": "unrelated-debug"}, "expected polaris-debug"),
                                 ({"version": "1.4.12-1"}, "versions differ"),
                                 ({"architecture": "aarch64"}, "for x86_64")):
            with self.subTest(changes=changes):
                self.replace_debug(**changes)
                result = self.validate()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_same_version_with_different_debug_build_id_is_rejected(self):
        self.replace_debug(symbols=self.other_symbols)
        result = self.validate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("different ELF build IDs", result.stderr)

    def test_kms_binary_must_match_the_host_build_id(self):
        archive(self.archives["arch", "polaris-kms"], "polaris-kms", {"usr/libexec/polaris/polaris-kms": self.other_host})
        result = self.validate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("different ELF build IDs", result.stderr)

    def test_missing_host_symbols_are_rejected(self):
        archive(self.archives["arch", "polaris-debug"], "polaris-debug", {"usr/src/debug/source.c": b"source only"})
        result = self.validate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing the host's detached symbols", result.stderr)

    def test_matching_build_id_without_debug_information_is_rejected(self):
        self.replace_debug(symbols=self.host)
        result = self.validate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no DWARF debug information", result.stderr)

    def test_release_stages_matching_debug_packages(self):
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        final = self.root / "release-assets/final"
        for platform in ("arch", "steamos3.8"):
            for name in ("polaris", "polaris-kms", "polaris-debug"):
                output = final / f"Polaris{name.removeprefix('polaris')}-{platform}-x86_64.pkg.tar.zst"
                self.assertEqual(output.read_bytes(), self.archives[platform, name].read_bytes())
        self.assertEqual(len(list(final.iterdir())), 10)

    def test_missing_or_duplicate_debug_package_stops_release_assembly(self):
        for platform in ("arch", "steamos3.8"):
            with self.subTest(platform=platform):
                path = self.archives[platform, "polaris-debug"]
                data = path.read_bytes()
                path.unlink()
                self.assertNotEqual(self.stage().returncode, 0)
                path.write_bytes(data)
                duplicate = path.with_name(path.name.replace(".pkg.tar.zst", "-duplicate.pkg.tar.zst"))
                duplicate.write_bytes(data)
                self.assertNotEqual(self.stage().returncode, 0)
                duplicate.unlink()

    def test_wrong_steamos_symbols_fail_before_writing_staged_packages(self):
        self.replace_debug(symbols=self.other_symbols, platform="steamos3.8")
        result = self.stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("different ELF build IDs", result.stderr)
        self.assertEqual(list((self.root / "release-assets/final").iterdir()), [])

    def test_signatures_follow_their_own_archives(self):
        # A debug/KMS signature must never be renamed as the absent host signature.
        for name in ("polaris-debug", "polaris-kms"):
            Path(str(self.archives["arch", name]) + ".sig").write_bytes(name.encode())
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        final = self.root / "release-assets/final"
        self.assertFalse((final / "Polaris-arch-x86_64.pkg.tar.zst.sig").exists())
        for name in ("polaris-debug", "polaris-kms"):
            self.assertEqual((final / f"Polaris{name.removeprefix('polaris')}-arch-x86_64.pkg.tar.zst.sig").read_bytes(), name.encode())

    def test_debug_assets_are_required_in_upload_and_readback(self):
        workflow = WORKFLOW.read_text()
        upload = workflow.split("      - name: Upload SteamOS 3.8 package\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("polaris-steamos-output/Polaris-debug-steamos3.8-x86_64.pkg.tar.zst", upload)
        required = workflow.split("          required_binary_assets=(\n", 1)[1].split("\n          )", 1)[0]
        self.assertIn("Polaris-debug-arch-x86_64.pkg.tar.zst", required)
        self.assertIn("Polaris-debug-steamos3.8-x86_64.pkg.tar.zst", required)

    def test_steamos_producer_validates_and_copies_debug_output(self):
        script = (ROOT / "scripts/ci/build-steamos-package.sh").read_text()
        block = script.split("# makepkg splits", 1)[1].split("FINAL_COMMIT=", 1)[0]
        block = "# makepkg splits" + block
        output = self.root / "producer-output"
        output.mkdir()
        tools = self.root / "tools"
        tools.mkdir()
        pacman = tools / "pacman"
        pacman.write_text("#!/bin/sh\nprintf 'fixture package receipt\\n'\n")
        pacman.chmod(0o755)
        environment = dict(os.environ, SOURCE_ROOT=str(ROOT), OUTPUT_ROOT=str(output),
                           PACKAGE_PATH=str(self.archives["arch", "polaris"]),
                           KMS_PACKAGE_PATH=str(self.archives["arch", "polaris-kms"]),
                           PATH=str(tools) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["bash", "-e", "-c", "shopt -s nullglob\n" + block], cwd=self.arch,
                                env=environment, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        copied = output / "Polaris-debug-steamos3.8-x86_64.pkg.tar.zst"
        self.assertEqual(copied.read_bytes(), self.archives["arch", "polaris-debug"].read_bytes())
        self.assertIn("ELF build ID", (output / "steamos3.8-debug-build-id.txt").read_text())
        self.assertTrue((output / "steamos3.8-debug-package-sha256.txt").is_file())


if __name__ == "__main__":
    unittest.main()
