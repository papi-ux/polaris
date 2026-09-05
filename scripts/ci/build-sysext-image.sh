#!/usr/bin/env bash
# Repack the Fedora RPM as a systemd system extension image.
#
# A system extension overlays /usr (and /opt) at runtime through systemd-sysext,
# so an rpm-ostree host such as Bazzite can run Polaris without layering a
# package into a new deployment or rebooting. The RPM already installs
# everything under /usr, which is exactly what this checks before packing.
#
# Usage: build-sysext-image.sh <polaris.rpm> <output.raw> [<version>]
set -euo pipefail

rpm_path="${1:?usage: build-sysext-image.sh <polaris.rpm> <output.raw> [<version>]}"
output="${2:?usage: build-sysext-image.sh <polaris.rpm> <output.raw> [<version>]}"
version="${3:-}"

for tool in rpm2cpio cpio mksquashfs; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'build-sysext-image.sh: missing tool: %s\n' "$tool" >&2
    exit 1
  fi
done
[ -f "$rpm_path" ] || { printf 'build-sysext-image.sh: no such RPM: %s\n' "$rpm_path" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-sysext.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tree"
(cd "$work/tree" && rpm2cpio "$rpm_path" | cpio -idm --quiet)

# systemd refuses an extension that carries anything outside /usr and /opt.
# Anything else here means the RPM layout drifted and the sysext must not ship.
stray="$(find "$work/tree" -mindepth 1 -maxdepth 1 ! -name usr ! -name opt)"
if [ -n "$stray" ]; then
  printf 'build-sysext-image.sh: RPM installs outside /usr and /opt, which a system extension cannot overlay:\n%s\n' "$stray" >&2
  exit 1
fi
if [ ! -e "$work/tree/usr/bin/polaris" ]; then
  printf 'build-sysext-image.sh: RPM does not install /usr/bin/polaris\n' >&2
  exit 1
fi

if [ -z "$version" ]; then
  # The RPM installs the versioned binary beside the polaris symlink.
  version="$(find "$work/tree/usr/bin" -maxdepth 1 -name 'polaris-[0-9]*' -printf '%f\n' | sed -n 's/^polaris-\([0-9][0-9.]*\)$/\1/p' | head -1)"
fi
if [ -z "$version" ]; then
  printf 'build-sysext-image.sh: could not determine the Polaris version from the RPM\n' >&2
  exit 1
fi

# The file name is the extension name: it must be installed as polaris.raw.
mkdir -p "$work/tree/usr/lib/extension-release.d"
cat >"$work/tree/usr/lib/extension-release.d/extension-release.polaris" <<RELEASE
ID=_any
ARCHITECTURE=x86-64
SYSEXT_LEVEL=1.0
EXTENSION_RELOAD_MANAGER=1
POLARIS_VERSION=${version}
RELEASE

mkdir -p "$(dirname "$output")"
rm -f "$output"
mksquashfs "$work/tree" "$output" -comp zstd -noappend -all-root -quiet -no-progress
printf 'built %s (%s bytes) for Polaris %s\n' "$(basename "$output")" "$(stat -c %s "$output")" "$version"
