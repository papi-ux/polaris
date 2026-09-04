#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -un)" != builder ]; then
  printf '%s\n' 'build-steamos-package.sh must run as the non-root builder user' >&2
  exit 1
fi

SOURCE_ROOT=/mnt
OUTPUT_ROOT=/opt
REQUESTED_COMMIT="${POLARIS_BUILD_COMMIT:?POLARIS_BUILD_COMMIT is required}"
POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD-0}"
if [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 0 ] && [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 1 ]; then
  printf '%s\n' 'POLARIS_LOCAL_CANDIDATE_BUILD must be 0 or 1' >&2
  exit 1
fi
git config --global --add safe.directory "$SOURCE_ROOT"
SOURCE_COMMIT="$(git -C "$SOURCE_ROOT" rev-parse HEAD)"
SOURCE_TREE="$(git -C "$SOURCE_ROOT" rev-parse 'HEAD^{tree}')"
SOURCE_STATUS="$(git -C "$SOURCE_ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"

if [ "$SOURCE_COMMIT" != "$REQUESTED_COMMIT" ] || [ -n "$SOURCE_STATUS" ]; then
  printf '%s\n' 'source checkout does not match the requested clean commit' >&2
  exit 1
fi

BRANCH="$(git -C "$SOURCE_ROOT" rev-parse --abbrev-ref HEAD)"
BUILD_VERSION="$(grep -Pom1 '^project\(Polaris VERSION \K[^ ]+' "$SOURCE_ROOT/CMakeLists.txt")"
CLONE_URL=https://github.com/papi-ux/polaris.git
if [ "$POLARIS_LOCAL_CANDIDATE_BUILD" = 1 ]; then
  CLONE_URL=file:///mnt
fi
export BRANCH BUILD_VERSION CLONE_URL
export COMMIT="$REQUESTED_COMMIT"
CMAKE_BUILD_PARALLEL_LEVEL="$(nproc)"
export CMAKE_BUILD_PARALLEL_LEVEL

STEAMOS_PKGBUILD_DIR="$BUILD_ROOT/packaging/linux/SteamOS"
cmake -S "$SOURCE_ROOT" -B "$BUILD_ROOT" -DPOLARIS_CONFIGURE_STEAMOS_PKGBUILD=ON -DPOLARIS_CONFIGURE_ONLY=ON
test -f "$STEAMOS_PKGBUILD_DIR/PKGBUILD"
cd "$STEAMOS_PKGBUILD_DIR"
makepkg --noconfirm --cleanbuild

shopt -s nullglob
PACKAGE_PATHS=(polaris-[0-9]*-x86_64.pkg.tar.zst)
if [ "${#PACKAGE_PATHS[@]}" -ne 1 ]; then
  printf 'expected exactly one SteamOS package, found %s\n' "${#PACKAGE_PATHS[@]}" >&2
  exit 1
fi
PACKAGE_PATH="${PACKAGE_PATHS[0]}"
RECEIPT_ROOT="$BUILD_ROOT/package-receipt"
rm -rf -- "$RECEIPT_ROOT"
install -d -m 0755 -- "$RECEIPT_ROOT"
bsdtar -xf "$PACKAGE_PATH" -C "$RECEIPT_ROOT"
test -f "$RECEIPT_ROOT/.PKGINFO"
PACKAGE_NAME="$(sed -n 's/^pkgname = //p' "$RECEIPT_ROOT/.PKGINFO")"
PACKAGE_VERSION="$(sed -n 's/^pkgver = //p' "$RECEIPT_ROOT/.PKGINFO")"
PACKAGE_ARCH="$(sed -n 's/^arch = //p' "$RECEIPT_ROOT/.PKGINFO")"
PACKAGE_IDENTITY="$PACKAGE_NAME|$PACKAGE_VERSION|$PACKAGE_ARCH"
if [ "$PACKAGE_IDENTITY" != 'polaris|1.4.2-1|x86_64' ]; then
  printf 'unexpected SteamOS package identity: %s\n' "$PACKAGE_IDENTITY" >&2
  exit 1
fi
cp "$PACKAGE_PATH" "$OUTPUT_ROOT/Polaris-steamos3.8-x86_64.pkg.tar.zst"

FINAL_COMMIT="$(git -C "$SOURCE_ROOT" rev-parse HEAD)"
FINAL_TREE="$(git -C "$SOURCE_ROOT" rev-parse 'HEAD^{tree}')"
FINAL_STATUS="$(git -C "$SOURCE_ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"
if [ "$FINAL_COMMIT" != "$SOURCE_COMMIT" ] || [ "$FINAL_TREE" != "$SOURCE_TREE" ] || [ -n "$FINAL_STATUS" ]; then
  printf '%s\n' 'source identity changed during SteamOS package creation' >&2
  exit 1
fi

printf 'commit=%s\ntree=%s\npackage=%s\n' \
  "$FINAL_COMMIT" "$FINAL_TREE" Polaris-steamos3.8-x86_64.pkg.tar.zst \
  > "$OUTPUT_ROOT/steamos3.8-build-metadata.txt"
pacman -Q > "$OUTPUT_ROOT/steamos3.8-installed-packages.txt"
sha256sum /var/lib/pacman/sync/*.db > "$OUTPUT_ROOT/steamos3.8-repository-sha256.txt"
pacman -Qip "$PACKAGE_PATH" > "$OUTPUT_ROOT/steamos3.8-package-info.txt"
pacman -Qlp "$PACKAGE_PATH" > "$OUTPUT_ROOT/steamos3.8-package-files.txt"
sha256sum "$OUTPUT_ROOT/Polaris-steamos3.8-x86_64.pkg.tar.zst" \
  > "$OUTPUT_ROOT/steamos3.8-package-sha256.txt"

sed -n 's/^depend = //p' "$RECEIPT_ROOT/.PKGINFO" \
  > "$OUTPUT_ROOT/steamos3.8-package-dependencies.txt"
test -s "$OUTPUT_ROOT/steamos3.8-package-dependencies.txt"

HOST_BINARIES=("$RECEIPT_ROOT"/usr/bin/polaris-[0-9]*)
if [ "${#HOST_BINARIES[@]}" -ne 1 ]; then
  printf 'expected exactly one versioned Polaris host binary, found %s\n' "${#HOST_BINARIES[@]}" >&2
  exit 1
fi
BINARY_PATH="${HOST_BINARIES[0]}"
test -x "$BINARY_PATH"
if [ ! -L "$RECEIPT_ROOT/usr/bin/polaris" ] && [ ! -x "$RECEIPT_ROOT/usr/bin/polaris" ]; then
  printf '%s\n' 'package is missing the Polaris binary selector' >&2
  exit 1
fi
test -x "$RECEIPT_ROOT/usr/bin/polaris-browser-stream-helper"
test -x "$RECEIPT_ROOT/usr/bin/polaris-gamescope-session"
test -x "$RECEIPT_ROOT/usr/bin/polaris-gamescope-runtime-lib.sh"
test -d "$RECEIPT_ROOT/usr/share/polaris"
test -f "$RECEIPT_ROOT/usr/share/applications/dev.polaris-stream.app.Polaris.desktop"
test -f "$RECEIPT_ROOT/usr/share/applications/dev.polaris-stream.app.Polaris.terminal.desktop"
test -f "$RECEIPT_ROOT/usr/lib/systemd/user/polaris.service"
ldd "$BINARY_PATH" > "$OUTPUT_ROOT/steamos3.8-binary-needed.txt"
if grep -q 'not found' "$OUTPUT_ROOT/steamos3.8-binary-needed.txt"; then
  printf '%s\n' 'packaged Polaris binary has unresolved shared-library dependencies' >&2
  exit 1
fi
readelf --version-info "$BINARY_PATH" > "$OUTPUT_ROOT/steamos3.8-binary-version-info.txt"
objdump -p "$BINARY_PATH" >> "$OUTPUT_ROOT/steamos3.8-binary-needed.txt"
"$SOURCE_ROOT/scripts/check-packaged-binary-paths.sh" \
  "$BINARY_PATH" "$OUTPUT_ROOT/steamos3.8-package-strings.txt"
# The Valve toolchain does not emit CET SHSTK notes for every C++/Go object,
# cgo links libresolv through runtime-selected resolver paths, and namcap cannot
# see command/runtime-discovered dependencies. Every exception remains exact;
# additions, removals, or wording changes fail the candidate gate.
NAMCAP_ACTUAL="$BUILD_ROOT/namcap-actual.sorted"
NAMCAP_ALLOWED="$BUILD_ROOT/namcap-allowed.sorted"
NAMCAP_MISSING="$BUILD_ROOT/namcap-reviewed-missing.txt"
# Namcap 3.6 resolves shebang basenames through PATH, then compares the
# unresolved lexical result with pacman's owned paths. SteamOS inherits a root
# PATH that prefers /usr/sbin (a symlink to /usr/bin), so Bash is found as
# /usr/sbin/bash even though the package database owns /usr/bin/bash. That
# produces the contradictory "dependency not needed" and "uninstalled
# dependency" warnings for valid Bash scripts. Use the canonical user binary
# paths so interpreter ownership remains deterministic and reviewable.
PATH=/usr/bin:/bin namcap "$PACKAGE_PATH" > "$OUTPUT_ROOT/steamos3.8-namcap-all.txt"
LC_ALL=C sort -u "$OUTPUT_ROOT/steamos3.8-namcap-all.txt" > "$NAMCAP_ACTUAL"
LC_ALL=C sort -u "$SOURCE_ROOT/packaging/linux/SteamOS/namcap-reviewed-warnings.txt" > "$NAMCAP_ALLOWED"
comm -23 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$OUTPUT_ROOT/steamos3.8-namcap.txt"
comm -12 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$OUTPUT_ROOT/steamos3.8-namcap-reviewed.txt"
comm -13 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$NAMCAP_MISSING"
if [ -s "$OUTPUT_ROOT/steamos3.8-namcap.txt" ]; then
  printf '%s\n' 'namcap emitted unreviewed warnings or a reviewed warning disappeared' >&2
  sed 's/^/namcap: /' "$OUTPUT_ROOT/steamos3.8-namcap.txt" >&2
  exit 1
fi
if [ -s "$NAMCAP_MISSING" ]; then
  printf '%s\n' 'namcap emitted unreviewed warnings or a reviewed warning disappeared' >&2
  sed 's/^/missing reviewed namcap warning: /' "$NAMCAP_MISSING" >&2
  exit 1
fi
