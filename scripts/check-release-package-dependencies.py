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
    lexer = shlex.shlex(script.replace("\\\n", " "), posix=True, punctuation_chars=";&|")
    lexer.whitespace_split = True
    lexer.commenters = "#"
    return list(lexer)


def contains_subsequence(tokens: list[str], expected: list[str]) -> bool:
    width = len(expected)
    return any(tokens[index:index + width] == expected for index in range(len(tokens) - width + 1))


arch = read("packaging/linux/Arch/PKGBUILD")
require_package(shell_array(arch, "depends"), "vulkan-icd-loader", "Arch runtime dependencies")
require_package(shell_array(arch, "makedepends"), "vulkan-headers", "Arch build dependencies")

fedora = read("packaging/linux/fedora/Polaris.spec")
if not re.search(r"(?m)^BuildRequires:\s+vulkan-loader-devel\s*$", fedora):
    raise AssertionError("Fedora build dependencies must explicitly include vulkan-loader-devel")

workflow = read(".github/workflows/build.yml")
fedora_job = workflow_job(workflow, "fedora-rpm-build")
fedora_versions = re.findall(r'''(?m)^          - fedora:\s*['"]?([0-9]+)['"]?\s*$''', fedora_job)
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
release_job = workflow_job(workflow, "release-assets")
release_upload = re.search(
    r"(?ms)^      - name: Upload release assets to GitHub release\n(?P<body>.*?)(?=^      - name:|\Z)",
    release_job,
)
if not release_upload:
    raise AssertionError("missing release asset upload workflow step")
release_upload_tokens = workflow_run_tokens(release_upload.group("body"))
for legacy_version in ("42", "43"):
    for cleanup_asset in (
        f"Polaris-fedora{legacy_version}-x86_64.rpm",
        f"Polaris-fedora{legacy_version}-src.rpm",
    ):
        if cleanup_asset not in release_upload_tokens:
            raise AssertionError(f"release workflow must delete stale asset: {cleanup_asset}")
cleanup_command = [
    "gh", "release", "delete-asset", "${POLARIS_PACKAGE_REF_NAME}", "${legacy_asset}", "--yes",
]
if not contains_subsequence(release_upload_tokens, cleanup_command):
    raise AssertionError("release workflow must invoke gh release delete-asset for each stale Fedora asset")
release_verify = re.search(
    r"(?ms)^      - name: Verify release assets on GitHub release\n(?P<body>.*?)(?=^      - name:|\Z)",
    release_job,
)
if not release_verify:
    raise AssertionError("missing release asset verification workflow step")
release_verify_body = release_verify.group("body")
legacy_guard = 'if [ "${supported_count}" -ne 3 ] || [ "${legacy_count}" -ne 0 ]; then'
if legacy_guard not in release_verify_body:
    raise AssertionError("release verification must fail when legacy Fedora assets remain")
for legacy_version in ("42", "43"):
    legacy_prefix = f'startswith("Polaris-fedora{legacy_version}-")'
    if legacy_prefix not in release_verify_body:
        raise AssertionError(f"release verification must count Fedora {legacy_version} assets")
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
