"""A prerelease package must sort below the release of its number, in every format Polaris ships.

The v1.4.13 betas carried 1.4.13 itself, so dnf answered the 1.4.13 release with "Nothing to do"
and a beta tester stayed on the beta. cmake/prep/prerelease_versions.cmake now spells the label
per format. These tests run that module, pin what it produces for a release, a beta and an rc,
and check the order of the whole sequence with each real package manager this host has.

A package manager the host lacks is skipped, except where the run names it in
POLARIS_VERSION_ORDER_TOOLS: then its absence fails. CI has to name at least one, and each job
names the one its image carries, dpkg on the Ubuntu runner, vercmp in the Arch container and rpm in
the Fedora container, so none of the three orderings can quietly stop being checked.
"""

import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/prep/prerelease_versions.cmake"
FIELDS = ("runtime", "rpm", "deb", "pacman")
REQUIRED_TOOLS_VARIABLE = "POLARIS_VERSION_ORDER_TOOLS"
ORDER_TOOLS = ("dpkg", "rpm", "vercmp")

# Strictly ascending: two releases, the prereleases between them, and a beta of the next one.
SEQUENCE = (
    ("1.4.12", ""),
    ("1.4.13", "beta.2"),
    ("1.4.13", "beta.3"),
    ("1.4.13", "beta.10"),
    ("1.4.13", "rc.1"),
    ("1.4.13", "rc.2"),
    ("1.4.13", ""),
    ("1.4.14", "beta.1"),
    ("1.4.14", ""),
)


def require_tool(tool, environ=os.environ, which=shutil.which):
    """Return when tool is installed; otherwise fail if this run named it, or skip."""
    named = environ.get(REQUIRED_TOOLS_VARIABLE, "").split()
    unknown = sorted(set(named) - set(ORDER_TOOLS))
    if unknown:
        raise AssertionError(f"{REQUIRED_TOOLS_VARIABLE} names tools this test does not use: {', '.join(unknown)}")
    if environ.get("CI", "").lower() not in ("", "0", "false") and not named:
        raise AssertionError(f"a CI run must name the package managers it checks in {REQUIRED_TOOLS_VARIABLE}")
    if which(tool):
        return
    if tool in named:
        raise AssertionError(f"{tool} is named in {REQUIRED_TOOLS_VARIABLE} but is not installed")
    raise unittest.SkipTest(f"{tool} is not installed on this host")


def run_module(version, label):
    with tempfile.TemporaryDirectory(prefix="polaris-prerelease-versions-") as temporary:
        directory = Path(temporary)
        output = directory / "versions.txt"
        probe = directory / "probe.cmake"
        probe.write_text(
            f'include("{MODULE.as_posix()}")\n'
            # Read from the environment, the way build_version.cmake receives it from CI.
            'polaris_prerelease_versions("${VERSION}" "$ENV{POLARIS_PRERELEASE_LABEL}")\n'
            'file(WRITE "${OUTPUT}" "runtime=${POLARIS_RUNTIME_VERSION}\\n'
            'rpm=${POLARIS_RPM_PACKAGE_VERSION}\\n'
            'deb=${POLARIS_DEB_PACKAGE_VERSION}\\n'
            'pacman=${POLARIS_PACMAN_PKGVER}\\n'
            'sub=${POLARIS_SUB_VERSION}\\n")\n'
        )
        result = subprocess.run(
            ["cmake", f"-DVERSION={version}", f"-DOUTPUT={output}", "-P", str(probe)],
            env=dict(os.environ, POLARIS_PRERELEASE_LABEL=label),
            text=True,
            capture_output=True,
            timeout=60,
        )
        if result.returncode != 0:
            return result, None
        values = dict(line.split("=", 1) for line in output.read_text().splitlines())
        return result, values


def versions(version, label):
    result, values = run_module(version, label)
    if values is None:
        raise AssertionError(f"{version} {label!r} was refused: {result.stderr}")
    return values


class PrereleaseVersionMapping(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise RuntimeError("cmake is required to run cmake/prep/prerelease_versions.cmake")

    def test_a_release_tag_keeps_the_versions_it_always_had(self):
        # The package repository requires exactly these: RPM 0:X.Y.Z-1 and pacman X.Y.Z-1, with the
        # Release and pkgrel of 1 set by CPack and the PKGBUILDs.
        self.assertEqual(
            versions("1.4.13", ""),
            {"runtime": "1.4.13", "rpm": "1.4.13", "deb": "1.4.13", "pacman": "1.4.13", "sub": ""},
        )

    def test_a_beta_tag_is_spelled_per_format(self):
        self.assertEqual(
            versions("1.4.13", "beta.3"),
            {
                "runtime": "1.4.13-beta.3",
                "rpm": "1.4.13~beta.3",
                "deb": "1.4.13~beta.3",
                "pacman": "1.4.13beta.3",
                "sub": "beta.3",
            },
        )

    def test_an_rc_tag_is_spelled_per_format(self):
        self.assertEqual(
            versions("1.4.13", "rc.1"),
            {
                "runtime": "1.4.13-rc.1",
                "rpm": "1.4.13~rc.1",
                "deb": "1.4.13~rc.1",
                "pacman": "1.4.13rc.1",
                "sub": "rc.1",
            },
        )

    def test_other_versions_without_a_label_pass_through_unchanged(self):
        # Development builds, whose short hash is hex and so never spells a channel, and the Nix
        # package, which sets BUILD_VERSION to 0-unstable-DATE.
        for version in ("1.4.13.b38588e2.dirty", "1.4.13.abcdef1", "0-unstable-2026-07-28"):
            with self.subTest(version=version):
                self.assertEqual(
                    versions(version, ""),
                    {"runtime": version, "rpm": version, "deb": version, "pacman": version, "sub": ""},
                )

    def test_refuses_labels_a_release_tag_cannot_carry(self):
        for label in ("beta", "beta.", "beta.3.1", "alpha.1", "Beta.3", "-beta.3", "~beta.3", "beta.3 ", "beta3"):
            with self.subTest(label=label):
                result, values = run_module("1.4.13", label)
                self.assertIsNone(values)
                self.assertIn("POLARIS_PRERELEASE_LABEL must be beta.N or rc.N", result.stderr)

    def test_refuses_a_version_that_already_carries_a_suffix(self):
        # The obvious fix, BUILD_VERSION=1.4.13-beta.3, is the one that sorts above the release in
        # rpm (CPack makes it 1.4.13_beta.3) and in dpkg (-beta.3 is read as the Debian revision).
        for version in ("1.4.13-beta.3", "1.4.13~beta.3", "1.4.13beta.3", "1.4.13.rc.1", "1.4.13_alpha.1"):
            with self.subTest(version=version):
                result, values = run_module(version, "")
                self.assertIsNone(values)
                self.assertIn("already carries a prerelease suffix", result.stderr)

    def test_a_label_needs_a_plain_release_number(self):
        for version in ("1.4.13.b38588e2", "1.4", "1.4.13.1"):
            with self.subTest(version=version):
                result, values = run_module(version, "beta.3")
                self.assertIsNone(values)
                self.assertIn("needs a plain release version", result.stderr)


class RequiredOrderTools(unittest.TestCase):
    """A package manager a run names has to be there; one it does not name may be skipped."""

    def outcome(self, tool, installed, **environ):
        try:
            require_tool(tool, environ, lambda name: f"/usr/bin/{name}" if name in installed else None)
        except unittest.SkipTest:
            return "skip"
        except AssertionError as error:
            return f"fail: {error}"
        return "run"

    def test_an_installed_tool_runs(self):
        self.assertEqual(self.outcome("vercmp", {"vercmp"}), "run")
        self.assertEqual(self.outcome("rpm", {"rpm"}, POLARIS_VERSION_ORDER_TOOLS="rpm", CI="true"), "run")

    def test_a_missing_tool_is_skipped_only_where_nobody_asked_for_it(self):
        self.assertEqual(self.outcome("vercmp", set()), "skip")
        self.assertEqual(self.outcome("vercmp", {"dpkg"}, POLARIS_VERSION_ORDER_TOOLS="dpkg", CI="true"), "skip")
        self.assertEqual(
            self.outcome("vercmp", {"dpkg"}, POLARIS_VERSION_ORDER_TOOLS="dpkg vercmp"),
            "fail: vercmp is named in POLARIS_VERSION_ORDER_TOOLS but is not installed",
        )

    def test_ci_has_to_name_what_it_checks(self):
        for ci in ("true", "1"):
            with self.subTest(CI=ci):
                self.assertEqual(
                    self.outcome("dpkg", {"dpkg"}, CI=ci),
                    "fail: a CI run must name the package managers it checks in POLARIS_VERSION_ORDER_TOOLS",
                )
        self.assertEqual(self.outcome("dpkg", {"dpkg"}, CI="false"), "run")

    def test_an_unknown_tool_name_fails(self):
        self.assertEqual(
            self.outcome("dpkg", {"dpkg"}, POLARIS_VERSION_ORDER_TOOLS="pacman"),
            "fail: POLARIS_VERSION_ORDER_TOOLS names tools this test does not use: pacman",
        )


class PrereleaseVersionOrder(unittest.TestCase):
    """Every earlier entry in SEQUENCE must sort strictly below every later one."""

    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise RuntimeError("cmake is required to run cmake/prep/prerelease_versions.cmake")
        cls.mapped = [versions(version, label) for version, label in SEQUENCE]

    def pairs(self, field, release_suffix=""):
        spelled = [entry[field] + release_suffix for entry in self.mapped]
        return list(itertools.combinations(spelled, 2))

    def require(self, tool):
        require_tool(tool)

    def test_dpkg_orders_the_deb_versions(self):
        self.require("dpkg")
        for lower, higher in self.pairs("deb"):
            with self.subTest(lower=lower, higher=higher):
                result = subprocess.run(["dpkg", "--compare-versions", lower, "lt", higher], timeout=30)
                self.assertEqual(result.returncode, 0, f"dpkg does not sort {lower} below {higher}")

    def test_rpm_orders_the_rpm_versions(self):
        self.require("rpm")
        # Version-Release, with the Release of 1 every Polaris RPM carries.
        pairs = self.pairs("rpm", "-1")
        script = "".join(
            f"print(rpm.vercmp('{lower}', '{higher}') .. ' ')\n" for lower, higher in pairs
        )
        with tempfile.NamedTemporaryFile("w", suffix=".lua", prefix="polaris-rpm-order-") as lua:
            lua.write(script)
            lua.flush()
            result = subprocess.run(
                ["rpm", "--eval", f'%{{lua: dofile("{lua.name}")}}'],
                text=True,
                capture_output=True,
                timeout=30,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        answers = result.stdout.split()
        self.assertEqual(len(answers), len(pairs), result.stdout)
        for (lower, higher), answer in zip(pairs, answers):
            with self.subTest(lower=lower, higher=higher):
                self.assertEqual(answer, "-1", f"rpm does not sort {lower} below {higher}")

    def test_pacman_orders_the_pkgver_values(self):
        self.require("vercmp")
        # pkgver-pkgrel, with the pkgrel of 1 both PKGBUILDs carry.
        for lower, higher in self.pairs("pacman", "-1"):
            with self.subTest(lower=lower, higher=higher):
                result = subprocess.run(["vercmp", lower, higher], text=True, capture_output=True, timeout=30)
                self.assertEqual(result.stdout.strip(), "-1", f"pacman does not sort {lower} below {higher}")


if __name__ == "__main__":
    unittest.main()
