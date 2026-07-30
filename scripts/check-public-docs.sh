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
import sys

building = Path("docs/building.md").read_text(encoding="utf-8")
readme = Path("README.md").read_text(encoding="utf-8")
changelog = Path("docs/changelog.md").read_text(encoding="utf-8")

if "C++23" not in building or "C++20" in building:
    print("docs/building.md must advertise the required C++23 toolchain", file=sys.stderr)
    sys.exit(1)

for dependency in ("vulkan-headers", "vulkan-icd-loader"):
    if dependency not in building:
        print(f"docs/building.md is missing the Arch dependency: {dependency}", file=sys.stderr)
        sys.exit(1)

try:
    current_release = changelog.split("## v1.3.2", 1)[1].split("## v1.3.1", 1)[0]
except IndexError:
    print("docs/changelog.md is missing a bounded v1.3.2 section", file=sys.stderr)
    sys.exit(1)

required_release_facts = (
    "webtransport-go v0.11.1",
    "quic-go v0.60.0",
    "CVE-2026-57497",
    "npm audit --audit-level=high",
    "Polaris-fedora44-x86_64.rpm",
    "vulkan-headers",
    "vulkan-loader-devel",
    "GCC 15",
)
for fact in required_release_facts:
    if fact not in current_release:
        print(f"v1.3.2 changelog is missing final release fact: {fact}", file=sys.stderr)
        sys.exit(1)

for fact in ("webtransport-go v0.11.1", "CVE-2026-57497"):
    if fact not in readme:
        print(f"README v1.3.2 summary is missing: {fact}", file=sys.stderr)
        sys.exit(1)
PY

echo "Public docs and release references look clean."
