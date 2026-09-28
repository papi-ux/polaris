"""Release assembly refuses packages whose versions do not match the tag's channel.

scripts/ci/check-release-package-versions.py reads every final asset. These tests give it a full set
of release assets, with rpm and dpkg-deb stood in by scripts that print what each fixture says it is,
and pacman packages as real archives, then lose the label in one lane at a time.
"""

import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts/ci/check-release-package-versions.py"

# Each stand-in prints the fixture's contents, which are written in the tool's own output format.
STUB_RPM = '#!/bin/sh\nfor last; do :; done\ncat "$last"\n'
STUB_DPKG_DEB = '#!/bin/sh\n[ "$1" = --field ] || exit 2\ncat "$2"\n'


def pacman_archive(path, name, version):
    metadata = f"pkgname = {name}\npkgver = {version}\narch = x86_64\n".encode()
    raw = path.with_name(path.name + ".raw")
    with tarfile.open(raw, "w") as output:
        info = tarfile.TarInfo(".PKGINFO")
        info.size = len(metadata)
        output.addfile(info, io.BytesIO(metadata))
    subprocess.run(["zstd", "-q", "-f", str(raw), "-o", str(path)], check=True)
    raw.unlink()


class ReleasePackageVersions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [tool for tool in ("tar", "zstd") if not shutil.which(tool)]
        if missing:
            raise RuntimeError("pacman package fixtures need " + ", ".join(missing))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="polaris-release-versions-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.final = self.root / "final"
        self.final.mkdir()
        tools = self.root / "bin"
        tools.mkdir()
        for name, body in (("rpm", STUB_RPM), ("dpkg-deb", STUB_DPKG_DEB)):
            (tools / name).write_text(body)
            (tools / name).chmod(0o755)
        self.environment = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}")

    def assets(self, *, rpm, deb, pacman, lost=None):
        """A full set of final assets. lost names one asset that carries the release version instead."""
        release = {"rpm": "1.4.13", "deb": "1.4.13", "pacman": "1.4.13-1"}
        for component in ("", "-kms"):
            name = f"polaris{component}"
            filename = f"Polaris{component}-fedora44-x86_64.rpm"
            version = release["rpm"] if filename == lost else rpm
            (self.final / filename).write_text(f"{name}|{version}|1")
            filename = f"Polaris{component}-ubuntu24.04-x86_64.deb"
            version = release["deb"] if filename == lost else deb
            (self.final / filename).write_text(f"Package: {name}\nVersion: {version}\n")
        for platform in ("arch", "steamos3.8"):
            for component in ("", "-kms", "-debug"):
                filename = f"Polaris{component}-{platform}-x86_64.pkg.tar.zst"
                version = release["pacman"] if filename == lost else pacman
                pacman_archive(self.final / filename, f"polaris{component}", version)
        # A detached signature and the Arch source archive are not packages, and are not read.
        (self.final / "Polaris-arch-x86_64.pkg.tar.zst.sig").write_bytes(b"signature")
        (self.final / "Polaris-arch-src.tar.gz").write_bytes(b"source")

    def check(self, tag, version="1.4.13", prerelease="true", label="beta.3"):
        return subprocess.run(
            [
                "python3", str(CHECKER),
                "--tag", tag, "--version", version, "--prerelease", prerelease, "--label", label,
                str(self.final),
            ],
            env=self.environment, text=True, capture_output=True, timeout=60,
        )

    def test_a_beta_publishes_every_package_below_its_release(self):
        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        result = self.check("v1.4.13-beta.3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 10, result.stdout)
        self.assertIn("Polaris-kms-fedora44-x86_64.rpm: polaris-kms 1.4.13~beta.3-1", result.stdout)
        self.assertIn("Polaris-debug-steamos3.8-x86_64.pkg.tar.zst: polaris-debug 1.4.13beta.3-1", result.stdout)

    def test_a_release_candidate_and_a_stable_release(self):
        self.assets(rpm="1.4.13~rc.1", deb="1.4.13~rc.1", pacman="1.4.13rc.1-1")
        result = self.check("v1.4.13-rc.1", label="rc.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assets(rpm="1.4.13", deb="1.4.13", pacman="1.4.13-1")
        result = self.check("v1.4.13", prerelease="false", label="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Polaris-ubuntu24.04-x86_64.deb: polaris 1.4.13", result.stdout)

    def test_a_lane_that_lost_the_label_stops_the_release(self):
        for lost in (
            "Polaris-fedora44-x86_64.rpm",
            "Polaris-kms-fedora44-x86_64.rpm",
            "Polaris-ubuntu24.04-x86_64.deb",
            "Polaris-kms-ubuntu24.04-x86_64.deb",
            "Polaris-arch-x86_64.pkg.tar.zst",
            "Polaris-kms-arch-x86_64.pkg.tar.zst",
            "Polaris-debug-arch-x86_64.pkg.tar.zst",
            "Polaris-steamos3.8-x86_64.pkg.tar.zst",
            "Polaris-kms-steamos3.8-x86_64.pkg.tar.zst",
            "Polaris-debug-steamos3.8-x86_64.pkg.tar.zst",
        ):
            with self.subTest(lost=lost):
                self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1", lost=lost)
                result = self.check("v1.4.13-beta.3")
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(f"{lost} is polaris", result.stderr)
                self.assertEqual(result.stderr.count(" needs "), 1, result.stderr)

    def test_the_published_1413_betas_would_have_been_refused(self):
        # v1.4.13-beta.3 published 1.4.13 in every format, the release's own version.
        self.assets(rpm="1.4.13", deb="1.4.13", pacman="1.4.13-1")
        result = self.check("v1.4.13-beta.3")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr.count(" needs "), 10, result.stderr)

    def test_a_stable_release_refuses_prerelease_packages(self):
        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        result = self.check("v1.4.13", prerelease="false", label="")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr.count(" needs "), 10, result.stderr)

    def test_other_spellings_of_a_beta_are_refused(self):
        # Each sorts above the release in the format that would carry it.
        for rpm, deb, pacman in (
            ("1.4.13_beta.3", "1.4.13~beta.3", "1.4.13beta.3-1"),
            ("1.4.13~beta.3", "1.4.13-beta.3", "1.4.13beta.3-1"),
            ("1.4.13~beta.3", "1.4.13~beta.3", "1.4.13~beta.3-1"),
            ("1.4.13~beta.3", "1.4.13~beta.3", "1.4.13beta.3-2"),
        ):
            with self.subTest(rpm=rpm, deb=deb, pacman=pacman):
                self.assets(rpm=rpm, deb=deb, pacman=pacman)
                result = self.check("v1.4.13-beta.3")
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_the_channel_has_to_agree_with_the_tag(self):
        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        for arguments, message in (
            ({"tag": "v1.4.13-beta.3", "label": ""}, "a prerelease needs its label"),
            ({"tag": "v1.4.13-beta.3", "prerelease": "false"}, "a stable release carries no prerelease label"),
            ({"tag": "v1.4.13-beta.4"}, "does not match version"),
            ({"tag": "v1.4.13"}, "does not match version"),
            ({"tag": "v1.4.13-beta.3", "label": "beta"}, "a prerelease needs its label"),
            ({"tag": "v1.4.13-beta.3", "prerelease": ""}, "prerelease must be true or false"),
            ({"tag": "v1.4.13-beta.3", "version": "1.4.13-beta.3"}, "must be MAJOR.MINOR.PATCH"),
        ):
            with self.subTest(arguments=arguments):
                result = self.check(**arguments)
                self.assertEqual(result.returncode, 1)
                self.assertIn(message, result.stderr)

    def test_a_missing_or_duplicate_package_stops_the_release(self):
        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        (self.final / "Polaris-kms-ubuntu24.04-x86_64.deb").unlink()
        result = self.check("v1.4.13-beta.3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected one Polaris-kms-ubuntu24.04-x86_64.deb, found 0", result.stderr)

        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        pacman_archive(self.final / "Polaris-arch-x86_64.pkg.tar.xz", "polaris", "1.4.13beta.3-1")
        result = self.check("v1.4.13-beta.3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected one Polaris-arch-x86_64.pkg.tar, found 2", result.stderr)

    def test_a_package_under_the_wrong_name_is_refused(self):
        self.assets(rpm="1.4.13~beta.3", deb="1.4.13~beta.3", pacman="1.4.13beta.3-1")
        (self.final / "Polaris-kms-fedora44-x86_64.rpm").write_text("polaris|1.4.13~beta.3|1")
        result = self.check("v1.4.13-beta.3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Polaris-kms-fedora44-x86_64.rpm is polaris 1.4.13~beta.3-1; tag v1.4.13-beta.3 needs polaris-kms", result.stderr)


if __name__ == "__main__":
    unittest.main()
