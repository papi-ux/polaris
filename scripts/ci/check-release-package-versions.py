#!/usr/bin/env python3
"""Check that every package a release publishes carries the version its tag calls for.

A tag vX.Y.Z-beta.N or vX.Y.Z-rc.N publishes packages that sort below the X.Y.Z release: RPM and DEB
Version X.Y.Z~beta.N, and pacman pkgver X.Y.Zbeta.N with pkgrel 1 (cmake/prep/prerelease_versions.cmake).
A tag vX.Y.Z publishes X.Y.Z, X.Y.Z and X.Y.Z-1. Each package build receives the label on its own, and a
build that lost it makes the release's own version for a beta and still passes its own smoke step, since
that step reads the same empty label. This reads the final assets, every one of them, against the tag.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path


# Every package release assembly stages; a missing one is an error, not a smaller check.
REQUIRED = (
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-kms-fedora44-x86_64.rpm",
    "Polaris-ubuntu24.04-x86_64.deb",
    "Polaris-kms-ubuntu24.04-x86_64.deb",
    *(
        f"Polaris{component}-{platform}-x86_64.pkg.tar"
        for platform in ("arch", "steamos3.8")
        for component in ("", "-kms", "-debug")
    ),
)
PACMAN = re.compile(r"\.pkg\.tar(\.[a-z0-9]+)?$")


def expected_versions(tag, version, prerelease, label):
    """The RPM Version, DEB Version and pacman pkgver-pkgrel the tag calls for."""
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError(f"the release version must be MAJOR.MINOR.PATCH, not {version!r}")
    if prerelease not in ("true", "false"):
        raise ValueError(f"prerelease must be true or false, not {prerelease!r}")
    if prerelease == "true" and not re.fullmatch(r"(beta|rc)\.[0-9]+", label):
        raise ValueError(f"a prerelease needs its label, beta.N or rc.N, and has {label!r}")
    if prerelease == "false" and label:
        raise ValueError(f"a stable release carries no prerelease label, and has {label!r}")
    expected_tag = f"v{version}-{label}" if label else f"v{version}"
    if tag != expected_tag:
        raise ValueError(f"tag {tag!r} does not match version {version!r} and label {label!r}")
    package = f"{version}~{label}" if label else version
    return package, package, f"{version}{label}-1"


def package_name(path):
    # Polaris-kms-... is polaris-kms, Polaris-debug-... is polaris-debug, and the rest is polaris.
    for component in ("kms", "debug"):
        if path.name.startswith(f"Polaris-{component}-"):
            return f"polaris-{component}"
    return "polaris"


def run(command):
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout


def read_rpm(path):
    name, version, release = run(["rpm", "-qp", "--qf", "%{NAME}|%{VERSION}|%{RELEASE}", str(path)]).strip().split("|")
    return name, f"{version}-{release}"


def read_deb(path):
    fields = dict(
        line.split(": ", 1) for line in run(["dpkg-deb", "--field", str(path), "Package", "Version"]).splitlines() if ": " in line
    )
    return fields.get("Package", ""), fields.get("Version", "")


def read_pacman(path):
    info = run(["tar", "-xOf", str(path), ".PKGINFO"])
    fields = {}
    for key in ("pkgname", "pkgver"):
        values = [line[len(key) + 3:] for line in info.splitlines() if line.startswith(f"{key} = ")]
        fields[key] = values[0] if len(values) == 1 else ""
    return fields["pkgname"], fields["pkgver"]


def check(directory, tag, version, prerelease, label):
    rpm_version, deb_version, pacman_version = expected_versions(tag, version, prerelease, label)
    errors = []
    checked = []
    packages = sorted(
        path for path in directory.iterdir()
        if path.is_file() and (path.suffix in (".rpm", ".deb") or PACMAN.search(path.name))
    )
    for required in REQUIRED:
        matches = [path for path in packages if path.name == required or path.name.startswith(f"{required}.")]
        if len(matches) != 1:
            errors.append(f"expected one {required}, found {len(matches)}")
    for path in packages:
        if path.suffix == ".rpm":
            # The Release is 1 in every channel, so the Version alone says which one this is.
            reader, expected = read_rpm, f"{rpm_version}-1"
        elif path.suffix == ".deb":
            reader, expected = read_deb, deb_version
        else:
            reader, expected = read_pacman, pacman_version
        try:
            name, actual = reader(path)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            errors.append(f"{path.name}: cannot read its version: {error}")
            continue
        if name != package_name(path) or actual != expected:
            errors.append(f"{path.name} is {name} {actual}; tag {tag} needs {package_name(path)} {expected}")
        else:
            checked.append(f"{path.name}: {name} {actual}")
    return checked, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--prerelease", required=True)
    parser.add_argument("--label", default="")
    parser.add_argument("directory", type=Path)
    arguments = parser.parse_args()
    try:
        checked, errors = check(arguments.directory, arguments.tag, arguments.version, arguments.prerelease, arguments.label)
    except ValueError as error:
        print(f"release package versions: {error}", file=sys.stderr)
        return 1
    for line in checked:
        print(line)
    for error in errors:
        print(f"release package versions: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
