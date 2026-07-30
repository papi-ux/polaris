#!/usr/bin/env python3
"""Validate explicit Vulkan dependencies for release-only CUDA package builds."""

from pathlib import Path
import re

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


arch = read("packaging/linux/Arch/PKGBUILD")
require_package(shell_array(arch, "depends"), "vulkan-icd-loader", "Arch runtime dependencies")
require_package(shell_array(arch, "makedepends"), "vulkan-headers", "Arch build dependencies")

fedora = read("packaging/linux/fedora/Polaris.spec")
if not re.search(r"(?m)^BuildRequires:\s+vulkan-loader-devel\s*$", fedora):
    raise AssertionError("Fedora build dependencies must explicitly include vulkan-loader-devel")

workflow = read(".github/workflows/build.yml")
arch_install = re.search(
    r"(?ms)^\s+- name: Install dependencies\n.*?^\s+- name: Configure\n",
    workflow,
)
if not arch_install:
    raise AssertionError("missing Arch Install dependencies workflow step")
for package in ("vulkan-headers", "vulkan-icd-loader"):
    if not re.search(rf"\b{re.escape(package)}\b", arch_install.group(0)):
        raise AssertionError(f"Arch CI dependencies must explicitly install {package}")

print("Release package Vulkan dependency contracts look correct.")
