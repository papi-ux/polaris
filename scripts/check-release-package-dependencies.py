#!/usr/bin/env python3
"""Validate explicit Vulkan dependencies for release-only CUDA package builds."""

from pathlib import Path
import re
import shlex

ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def shell_array(text: str, name: str) -> str:
    match = re.search(rf"(?ms)^{re.escape(name)}=\(\n(?P<body>.*?)^\)", text)
    if not match:
        raise AssertionError(f"missing {name} array")
    return match.group("body")


def require_package(section: str, package: str, context: str) -> None:
    if not re.search(rf"(?m)^\s*'{re.escape(package)}'\s*$", section):
        raise AssertionError(f"{context} must explicitly include {package}")


def workflow_job(text: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        text,
    )
    if not match:
        raise AssertionError(f"missing {name} workflow job")
    return match.group("body")


def workflow_run_tokens(step: str) -> list[str]:
    run = re.search(
        r"(?ms)^        run: \|\n(?P<script>(?:^          .*(?:\n|\Z))*)",
        step,
    )
    if not run:
        raise AssertionError("missing shell run block in workflow step")
    script = "\n".join(line[10:] for line in run.group("script").splitlines())
    return shlex.split(script.replace("\\\n", " "), comments=True, posix=True)


arch = read("packaging/linux/Arch/PKGBUILD")
require_package(shell_array(arch, "depends"), "vulkan-icd-loader", "Arch runtime dependencies")
require_package(shell_array(arch, "makedepends"), "vulkan-headers", "Arch build dependencies")

fedora = read("packaging/linux/fedora/Polaris.spec")
if not re.search(r"(?m)^BuildRequires:\s+vulkan-loader-devel\s*$", fedora):
    raise AssertionError("Fedora build dependencies must explicitly include vulkan-loader-devel")

workflow = read(".github/workflows/build.yml")
fedora_job = workflow_job(workflow, "fedora-rpm-build")
fedora_versions = re.findall(r"(?m)^          - fedora: '([0-9]+)'$", fedora_job)
if fedora_versions != ["44"]:
    raise AssertionError(f"Fedora CI matrix must contain only Fedora 44, found {fedora_versions}")
for legacy_version in ("42", "43"):
    for legacy_marker in (
        f"fedora-{legacy_version}-rpm-artifacts",
        f"release-assets/raw/fedora{legacy_version}",
        f"copy_fedora_rpms {legacy_version}",
    ):
        if legacy_marker in workflow:
            raise AssertionError(f"release workflow retains Fedora {legacy_version}: {legacy_marker}")
    for cleanup_asset in (
        f"Polaris-fedora{legacy_version}-x86_64.rpm",
        f"Polaris-fedora{legacy_version}-src.rpm",
    ):
        if cleanup_asset not in workflow:
            raise AssertionError(f"release workflow must delete stale asset: {cleanup_asset}")
arch_job = workflow_job(workflow, "arch-build")
arch_install = re.search(
    r"(?ms)^      - name: Install dependencies\n(?P<body>.*?)(?=^      - name:|\Z)",
    arch_job,
)
if not arch_install:
    raise AssertionError("missing Arch Install dependencies workflow step")
arch_install_tokens = workflow_run_tokens(arch_install.group("body"))
for package in ("vulkan-headers", "vulkan-icd-loader"):
    if package not in arch_install_tokens:
        raise AssertionError(f"Arch CI dependencies must explicitly install {package}")

print("Release package Vulkan dependency contracts look correct.")
