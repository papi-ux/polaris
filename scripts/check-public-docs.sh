#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

python3 <<'PY'
from pathlib import Path
import re
import sys

root = Path.cwd()
readme = root / "README.md"
text = readme.read_text(encoding="utf-8")

targets = set()

for match in re.finditer(r'\]\(([^)]+)\)', text):
    target = match.group(1).strip()
    if "://" in target or target.startswith("#") or target.startswith("mailto:"):
        continue
    target = target.split("#", 1)[0].split("?", 1)[0]
    if target:
        targets.add(target)

for match in re.finditer(r'(?:src|srcset)=["\']([^"\']+)["\']', text):
    target = match.group(1).strip().split(",", 1)[0].strip().split(" ", 1)[0]
    if "://" in target or target.startswith("data:") or target.startswith("#"):
        continue
    target = target.split("#", 1)[0].split("?", 1)[0]
    if target:
        targets.add(target)

missing = sorted(str(path) for path in targets if not (root / path).exists())
if missing:
    print("README references missing local files:", file=sys.stderr)
    for path in missing:
        print(f"  - {path}", file=sys.stderr)
    sys.exit(1)
PY

expected_assets=(
  "Polaris-fedora44-x86_64.rpm"
  "Polaris-ubuntu24.04-x86_64.deb"
  "Polaris-arch-x86_64.pkg.tar.zst"
)

legacy_assets=(
  "Polaris-fedora42-x86_64.rpm"
  "Polaris-fedora43-x86_64.rpm"
)

variable_fedora_patterns=(
  'Polaris-fedora${'
  'fedora_version="$(rpm -E %fedora)"'
)

expected_nova_links=(
  "https://github.com/papi-ux/nova/releases/latest"
  "https://github-store.org/app?repo=papi-ux/nova"
  "versionExtractionRegEx%5C%22%3A%5C%22v%28.%2B%29"
  "app-nonRoot_game-arm64-v8a-release.apk"
)

files_to_check=(
  "README.md"
  "docs/building.md"
  "docs/changelog.md"
  ".github/workflows/build.yml"
)

current_docs=(
  "README.md"
  "docs/building.md"
  "docs/bazzite.md"
)

for expected_asset in "${expected_assets[@]}"; do
  for file in "${files_to_check[@]}"; do
    grep -Fq "$expected_asset" "$file"
  done
done

for legacy_asset in "${legacy_assets[@]}"; do
  for file in "${current_docs[@]}"; do
    if grep -Fq "$legacy_asset" "$file"; then
      echo "Legacy Fedora release asset remains in $file: $legacy_asset" >&2
      exit 1
    fi
  done
done

for variable_pattern in "${variable_fedora_patterns[@]}"; do
  for file in "${current_docs[@]}"; do
    if grep -Fq "$variable_pattern" "$file"; then
      echo "Variable-derived Fedora asset remains in $file: $variable_pattern" >&2
      exit 1
    fi
  done
done

for expected_link in "${expected_nova_links[@]}"; do
  grep -Fq "$expected_link" README.md
done

python3 <<'PY'
from pathlib import Path
import re
import shlex
import sys

building = Path("docs/building.md").read_text(encoding="utf-8")
readme = Path("README.md").read_text(encoding="utf-8")
changelog = Path("docs/changelog.md").read_text(encoding="utf-8")


def bounded_release_section(text: str, current: str, following: str) -> str:
    current_heading = rf"^## {re.escape(current)}(?:\s+-[^\n]+)?\s*$"
    following_heading = rf"^## {re.escape(following)}(?:\s+-[^\n]+)?\s*$"
    match = re.search(
        rf"(?ms){current_heading}\n(?P<body>.*?)(?={following_heading})",
        text,
    )
    if not match:
        print(
            f"Could not find exact, bounded {current} -> {following} changelog headings",
            file=sys.stderr,
        )
        sys.exit(1)
    return match.group("body")


required_compiler_row = re.compile(
    r"^\|\s*C\+\+23 compiler\s*\|\s*GCC or Clang\s*\|\s*$",
    re.MULTILINE,
)
if not required_compiler_row.search(building):
    print("docs/building.md must declare C++23 in the requirements table", file=sys.stderr)
    sys.exit(1)

arch_block = re.search(
    r"(?ms)^#### Arch / CachyOS\s*$.*?^```bash\s*$\n(?P<commands>.*?)^```\s*$",
    building,
)
if not arch_block:
    print("docs/building.md is missing the Arch/CachyOS install command", file=sys.stderr)
    sys.exit(1)
arch_tokens = set(shlex.split(arch_block.group("commands").replace("\\\n", " ")))
for dependency in ("vulkan-headers", "vulkan-icd-loader"):
    if dependency not in arch_tokens:
        print(
            f"Arch/CachyOS install command is missing dependency: {dependency}",
            file=sys.stderr,
        )
        sys.exit(1)

current_release = bounded_release_section(changelog, "v1.3.2", "v1.3.1")
required_release_facts = (
    "webtransport-go v0.11.1",
    "quic-go v0.60.0",
    "CVE-2026-57497",
    "npm audit --audit-level=high",
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-ubuntu24.04-x86_64.deb",
    "vulkan-headers",
    "vulkan-loader-devel",
    "GCC 15",
)
for fact in required_release_facts:
    if fact not in current_release:
        print(f"v1.3.2 changelog is missing final release fact: {fact}", file=sys.stderr)
        sys.exit(1)

readme_release = re.search(
    r"(?ms)^## What is New in v1\.3\.2\s*$\n(?P<body>.*?)(?=^## Install\s*$)",
    readme,
)
if not readme_release:
    print("README is missing the bounded v1.3.2 summary", file=sys.stderr)
    sys.exit(1)
required_readme_facts = (
    "webtransport-go v0.11.1",
    "quic-go v0.60.0",
    "CVE-2026-57497",
    "npm audit",
    "Polaris-arch-x86_64.pkg.tar.zst",
    "Polaris-fedora44-x86_64.rpm",
    "Polaris-ubuntu24.04-x86_64.deb",
    "Vulkan",
    "GCC 15",
)
for fact in required_readme_facts:
    if fact not in readme_release.group("body"):
        print(f"README v1.3.2 summary is missing: {fact}", file=sys.stderr)
        sys.exit(1)
PY

echo "Public docs and release references look clean."
