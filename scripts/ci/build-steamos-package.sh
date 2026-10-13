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
# The release tag's prerelease label, beta.N or rc.N, or nothing for a stable build. Unset means
# stable, so a local candidate build needs nothing new.
POLARIS_PRERELEASE_LABEL="${POLARIS_PRERELEASE_LABEL-}"
if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [[ ! "$POLARIS_PRERELEASE_LABEL" =~ ^(beta|rc)\.[0-9]+$ ]]; then
  printf '%s\n' 'POLARIS_PRERELEASE_LABEL must be empty, beta.N or rc.N' >&2
  exit 1
fi
export POLARIS_PRERELEASE_LABEL
# The SteamOS release run-steamos-build.sh bootstrapped this root from. Unset means 3.8.
POLARIS_STEAMOS_TARGET="${POLARIS_STEAMOS_TARGET-3.8}"
if [ "$POLARIS_STEAMOS_TARGET" != 3.8 ] && [ "$POLARIS_STEAMOS_TARGET" != 3.9 ]; then
  printf '%s\n' 'POLARIS_STEAMOS_TARGET must be 3.8 or 3.9' >&2
  exit 1
fi
STEAMOS_PACKAGE="Polaris-steamos$POLARIS_STEAMOS_TARGET-x86_64.pkg.tar.zst"
STEAMOS_KMS_PACKAGE="Polaris-kms-steamos$POLARIS_STEAMOS_TARGET-x86_64.pkg.tar.zst"
STEAMOS_DEBUG_PACKAGE="Polaris-debug-steamos$POLARIS_STEAMOS_TARGET-x86_64.pkg.tar.zst"
RECEIPT_PREFIX="$OUTPUT_ROOT/steamos$POLARIS_STEAMOS_TARGET"
# Each release's toolchain reports its own namcap findings, so each has its own reviewed list.
NAMCAP_REVIEWED_LIST="$SOURCE_ROOT/packaging/linux/SteamOS/namcap-reviewed-warnings.txt"
if [ "$POLARIS_STEAMOS_TARGET" != 3.8 ]; then
  NAMCAP_REVIEWED_LIST="$SOURCE_ROOT/packaging/linux/SteamOS/namcap-reviewed-warnings-steamos$POLARIS_STEAMOS_TARGET.txt"
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
cmake -S "$SOURCE_ROOT" -B "$BUILD_ROOT" -DPOLARIS_CONFIGURE_STEAMOS_PKGBUILD=ON -DPOLARIS_CONFIGURE_ONLY=ON \
  "-DPOLARIS_STEAMOS_TARGET=$POLARIS_STEAMOS_TARGET"
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
# The release this tree builds, with a prerelease's label joined straight onto the number, which
# pacman sorts below that release (cmake/prep/prerelease_versions.cmake).
EXPECTED_PKGVER="1.5.0${POLARIS_PRERELEASE_LABEL}-1"
if [ "$PACKAGE_IDENTITY" != "polaris|$EXPECTED_PKGVER|x86_64" ]; then
  printf 'unexpected SteamOS package identity: %s\n' "$PACKAGE_IDENTITY" >&2
  exit 1
fi
if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [ "$(vercmp "$PACKAGE_VERSION" "$BUILD_VERSION-1")" != -1 ]; then
  printf 'SteamOS prerelease %s does not sort below %s-1, so an upgrade to that release would not replace it\n' \
    "$PACKAGE_VERSION" "$BUILD_VERSION" >&2
  exit 1
fi
cp "$PACKAGE_PATH" "$OUTPUT_ROOT/$STEAMOS_PACKAGE"

# The DRM/KMS capture helper is its own package. The glob above ends in a digit, so it never picks
# this one up, and this one is checked on its own terms: what it contains is the whole point of
# splitting it, and a leak here would put 31 MiB and a capability on every Deck.
KMS_PACKAGE_PATHS=(polaris-kms-[0-9]*-x86_64.pkg.tar.zst)
if [ "${#KMS_PACKAGE_PATHS[@]}" -ne 1 ]; then
  printf 'expected exactly one SteamOS polaris-kms package, found %s\n' "${#KMS_PACKAGE_PATHS[@]}" >&2
  exit 1
fi
KMS_PACKAGE_PATH="${KMS_PACKAGE_PATHS[0]}"
KMS_RECEIPT_ROOT="$BUILD_ROOT/kms-package-receipt"
rm -rf -- "$KMS_RECEIPT_ROOT"
install -d -m 0755 -- "$KMS_RECEIPT_ROOT"
bsdtar -xf "$KMS_PACKAGE_PATH" -C "$KMS_RECEIPT_ROOT"
test -f "$KMS_RECEIPT_ROOT/.PKGINFO"
KMS_PACKAGE_NAME="$(sed -n 's/^pkgname = //p' "$KMS_RECEIPT_ROOT/.PKGINFO")"
KMS_PACKAGE_VERSION="$(sed -n 's/^pkgver = //p' "$KMS_RECEIPT_ROOT/.PKGINFO")"
KMS_PACKAGE_ARCH="$(sed -n 's/^arch = //p' "$KMS_RECEIPT_ROOT/.PKGINFO")"
KMS_IDENTITY="$KMS_PACKAGE_NAME|$KMS_PACKAGE_VERSION|$KMS_PACKAGE_ARCH"
if [ "$KMS_IDENTITY" != "polaris-kms|$EXPECTED_PKGVER|x86_64" ]; then
  printf 'unexpected SteamOS polaris-kms package identity: %s\n' "$KMS_IDENTITY" >&2
  exit 1
fi
# Exactly this version of Polaris, so the helper and the binary can never disagree.
if ! grep -Fqx "depend = polaris=$EXPECTED_PKGVER" "$KMS_RECEIPT_ROOT/.PKGINFO"; then
  printf '%s\n' 'polaris-kms must depend on the exact Polaris it was built with' >&2
  sed -n 's/^depend = /  depends: /p' "$KMS_RECEIPT_ROOT/.PKGINFO" >&2
  exit 1
fi
test -f "$KMS_RECEIPT_ROOT/usr/libexec/polaris/polaris-kms"
test -f "$KMS_RECEIPT_ROOT/usr/lib/sysusers.d/polaris-kms.conf"
test ! -e "$KMS_RECEIPT_ROOT/usr/bin/polaris"
pacman -Qlp "$KMS_PACKAGE_PATH" > "$RECEIPT_PREFIX-kms-package-files.txt"
# Directory entries end in a slash. Matched with awk rather than a grep end-of-line pattern,
# because the packaging analyzer forbids a dollar immediately followed by a quote anywhere in
# the script, including inside a comment like this one.
KMS_PAYLOAD_COUNT="$(awk '!/[/]$/' "$RECEIPT_PREFIX-kms-package-files.txt" | wc -l)"
if [ "$KMS_PAYLOAD_COUNT" -ne 2 ]; then
  printf 'polaris-kms should carry exactly two files, found %s\n' "$KMS_PAYLOAD_COUNT" >&2
  awk '!/[/]$/' "$RECEIPT_PREFIX-kms-package-files.txt" >&2
  exit 1
fi
cp "$KMS_PACKAGE_PATH" "$OUTPUT_ROOT/$STEAMOS_KMS_PACKAGE"
sha256sum "$OUTPUT_ROOT/$STEAMOS_KMS_PACKAGE" \
  > "$RECEIPT_PREFIX-kms-package-sha256.txt"

# makepkg splits the host's detached symbols into polaris-debug. Keep that package
# attached to the same version and ELF build ID as both copies of the host binary.
DEBUG_PACKAGE_PATHS=(polaris-debug-[0-9]*-x86_64.pkg.tar.zst)
if [ "${#DEBUG_PACKAGE_PATHS[@]}" -ne 1 ]; then
  printf 'expected exactly one SteamOS debug package, found %s\n' "${#DEBUG_PACKAGE_PATHS[@]}" >&2
  exit 1
fi
DEBUG_PACKAGE_PATH="${DEBUG_PACKAGE_PATHS[0]}"
python3 "$SOURCE_ROOT/scripts/ci/validate-pacman-debug.py" \
  --main "$PACKAGE_PATH" --kms "$KMS_PACKAGE_PATH" --debug "$DEBUG_PACKAGE_PATH" \
  > "$RECEIPT_PREFIX-debug-build-id.txt"
cp "$DEBUG_PACKAGE_PATH" "$OUTPUT_ROOT/$STEAMOS_DEBUG_PACKAGE"
pacman -Qip "$DEBUG_PACKAGE_PATH" > "$RECEIPT_PREFIX-debug-package-info.txt"
pacman -Qlp "$DEBUG_PACKAGE_PATH" > "$RECEIPT_PREFIX-debug-package-files.txt"
sha256sum "$OUTPUT_ROOT/$STEAMOS_DEBUG_PACKAGE" \
  > "$RECEIPT_PREFIX-debug-package-sha256.txt"

FINAL_COMMIT="$(git -C "$SOURCE_ROOT" rev-parse HEAD)"
FINAL_TREE="$(git -C "$SOURCE_ROOT" rev-parse 'HEAD^{tree}')"
FINAL_STATUS="$(git -C "$SOURCE_ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"
if [ "$FINAL_COMMIT" != "$SOURCE_COMMIT" ] || [ "$FINAL_TREE" != "$SOURCE_TREE" ] || [ -n "$FINAL_STATUS" ]; then
  printf '%s\n' 'source identity changed during SteamOS package creation' >&2
  exit 1
fi

printf 'commit=%s\ntree=%s\npackage=%s\nsteamos=%s\n' \
  "$FINAL_COMMIT" "$FINAL_TREE" "$STEAMOS_PACKAGE" "$POLARIS_STEAMOS_TARGET" \
  > "$RECEIPT_PREFIX-build-metadata.txt"
pacman -Q > "$RECEIPT_PREFIX-installed-packages.txt"
sha256sum /var/lib/pacman/sync/*.db > "$RECEIPT_PREFIX-repository-sha256.txt"
pacman -Qip "$PACKAGE_PATH" > "$RECEIPT_PREFIX-package-info.txt"
pacman -Qlp "$PACKAGE_PATH" > "$RECEIPT_PREFIX-package-files.txt"
sha256sum "$OUTPUT_ROOT/$STEAMOS_PACKAGE" \
  > "$RECEIPT_PREFIX-package-sha256.txt"

sed -n 's/^depend = //p' "$RECEIPT_ROOT/.PKGINFO" \
  > "$RECEIPT_PREFIX-package-dependencies.txt"
test -s "$RECEIPT_PREFIX-package-dependencies.txt"

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
ldd "$BINARY_PATH" > "$RECEIPT_PREFIX-binary-needed.txt"
if grep -q 'not found' "$RECEIPT_PREFIX-binary-needed.txt"; then
  printf '%s\n' 'packaged Polaris binary has unresolved shared-library dependencies' >&2
  exit 1
fi
readelf --version-info "$BINARY_PATH" > "$RECEIPT_PREFIX-binary-version-info.txt"
objdump -p "$BINARY_PATH" >> "$RECEIPT_PREFIX-binary-needed.txt"
# SteamOS is where gamescope_stream lives, and libei is found at configure time
# and quietly left out when it is missing. Ask the binary.
if ! grep -Eq 'NEEDED +libei[.]so[.]1' "$RECEIPT_PREFIX-binary-needed.txt"; then
  printf '%s\n' 'packaged Polaris binary was built without libei; gamescope_stream would have no mouse or keyboard' >&2
  exit 1
fi
# Boost changes its library names with every release, and a SteamOS release ships exactly one. A
# package that names boost-libs alone installs on the next SteamOS and then cannot start: the
# 3.8 package did exactly that on SteamOS 3.9.2. makepkg turns each listed Boost soname into an exact
# dependency on the version this build linked, which only that release's boost-libs provides. Ask
# the binary which Boost libraries it needs, then require that exact pin for every one of them.
awk '$1 == "NEEDED" && $2 ~ /^libboost_/ { print $2 }' "$RECEIPT_PREFIX-binary-needed.txt" \
  | LC_ALL=C sort -u > "$RECEIPT_PREFIX-boost-sonames.txt"
if [ ! -s "$RECEIPT_PREFIX-boost-sonames.txt" ]; then
  printf '%s\n' 'packaged Polaris binary links no Boost library; the Boost ABI check has nothing to bind' >&2
  exit 1
fi
: > "$RECEIPT_PREFIX-boost-abi.txt"
while IFS= read -r soname; do
  dependency="${soname%%.so.*}.so"
  boost_version="${soname#*.so.}"
  if ! grep -Fqx "depend = ${dependency}=${boost_version}-64" "$RECEIPT_ROOT/.PKGINFO"; then
    printf 'packaged Polaris binary needs %s but the package does not depend on exactly %s=%s-64\n' \
      "$soname" "$dependency" "$boost_version" >&2
    sed -n 's/^depend = /  depends: /p' "$RECEIPT_ROOT/.PKGINFO" >&2
    exit 1
  fi
  printf '%s %s=%s-64\n' "$soname" "$dependency" "$boost_version" >> "$RECEIPT_PREFIX-boost-abi.txt"
done < "$RECEIPT_PREFIX-boost-sonames.txt"
"$SOURCE_ROOT/scripts/check-packaged-binary-paths.sh" \
  "$BINARY_PATH" "$RECEIPT_PREFIX-package-strings.txt"
# The Valve toolchain does not emit CET SHSTK notes for every C++/Go object,
# cgo links libresolv through runtime-selected resolver paths, and namcap cannot
# see command/runtime-discovered dependencies. Every exception remains exact;
# additions, removals, or wording changes fail the candidate gate.
# SteamOS 3.9's namcap 3.6 disables the SHSTK rule, so its separate reviewed list
# omits those two messages. The archived host and browser ELF notes still lack
# SHSTK; the missing diagnostics do not establish additional binary hardening.
#
# libvulkan is the same class. Polaris resolves every Vulkan entry point it calls from the loader by
# name at runtime (src/platform/linux/vulkan_loader.cpp), and the compute codec's volk does the same,
# so no object makes a direct call and namcap reports the library as unused. The dependency is real
# and stays declared on purpose: dropping it would move the failure on a host without Vulkan from
# install time to the middle of a stream.
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
PATH=/usr/bin:/bin namcap "$PACKAGE_PATH" > "$RECEIPT_PREFIX-namcap-all.txt"
# Recorded rather than gated: the reviewed list is an exact pin for the main package, and the
# helper's own findings have not been reviewed yet. What actually matters about that package, its
# identity, its dependency and its two files, is asserted above and does not need namcap to say so.
PATH=/usr/bin:/bin namcap "$KMS_PACKAGE_PATH" > "$RECEIPT_PREFIX-kms-namcap-all.txt" || true
LC_ALL=C sort -u "$RECEIPT_PREFIX-namcap-all.txt" > "$NAMCAP_ACTUAL"
LC_ALL=C sort -u "$NAMCAP_REVIEWED_LIST" > "$NAMCAP_ALLOWED"
comm -23 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$RECEIPT_PREFIX-namcap.txt"
comm -12 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$RECEIPT_PREFIX-namcap-reviewed.txt"
comm -13 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED" > "$NAMCAP_MISSING"
if [ -s "$RECEIPT_PREFIX-namcap.txt" ]; then
  printf '%s\n' 'namcap emitted unreviewed warnings or a reviewed warning disappeared' >&2
  sed 's/^/namcap: /' "$RECEIPT_PREFIX-namcap.txt" >&2
  exit 1
fi
if [ -s "$NAMCAP_MISSING" ]; then
  printf '%s\n' 'namcap emitted unreviewed warnings or a reviewed warning disappeared' >&2
  sed 's/^/missing reviewed namcap warning: /' "$NAMCAP_MISSING" >&2
  exit 1
fi
