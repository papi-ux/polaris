#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  printf '%s\n' 'run-steamos-build.sh must run as root inside a privileged disposable container' >&2
  exit 1
fi
if [ ! -f /.dockerenv ]; then
  printf '%s\n' 'run-steamos-build.sh must run inside a disposable container' >&2
  exit 1
fi
if [ ! -e /workspace/.git ] || [ ! -d /output ]; then
  printf '%s\n' 'expected /workspace checkout and /output artifact mounts' >&2
  exit 1
fi

STEAMOS_ROOT=/steamos-root
# The SteamOS release this package is built for. Each one gets its own package: the binary links the
# Boost and other libraries of that release's repositories, so a 3.8 package cannot run on 3.9.
# Unset means 3.8, the original target, so existing callers build exactly what they built before.
POLARIS_STEAMOS_TARGET="${POLARIS_STEAMOS_TARGET-3.8}"
case "$POLARIS_STEAMOS_TARGET" in
  3.8)
    STEAMOS_REPOSITORY_SUFFIX=3.8.1x
    HOLO_KEYRING_PACKAGE=holo-keyring-20250801-1-any.pkg.tar.zst
    HOLO_KEYRING_SHA256=a5efa4f9c161ce9607fd9dfcccaf2a587baa9acd35eae04d3c01d967dddc9722
    ;;
  3.9)
    # Same Valve signing keys as 20250801-1: holo.gpg, holo-trusted, holo-revoked and the install
    # script are byte-identical. Only the package metadata was rebuilt for SteamOS 3.9.
    STEAMOS_REPOSITORY_SUFFIX=3.9
    HOLO_KEYRING_PACKAGE=holo-keyring-20250801-2-any.pkg.tar.zst
    HOLO_KEYRING_SHA256=cc21dbcbe1b4130438bbb109bb8382a1513c6e116a9e59ad0b139475ccad4252
    ;;
  *)
    printf '%s\n' 'POLARIS_STEAMOS_TARGET must be 3.8 or 3.9' >&2
    exit 1
    ;;
esac
export POLARIS_STEAMOS_TARGET
STEAMOS_PACKAGE="Polaris-steamos$POLARIS_STEAMOS_TARGET-x86_64.pkg.tar.zst"
STEAMOS_RECEIPT_PREFIX="/output/steamos$POLARIS_STEAMOS_TARGET"
STEAMOS_PACMAN_CONFIG="/tmp/steamos-$STEAMOS_REPOSITORY_SUFFIX.conf"
POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD-0}"
if [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 0 ] && [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 1 ]; then
  printf '%s\n' 'POLARIS_LOCAL_CANDIDATE_BUILD must be 0 or 1' >&2
  exit 1
fi
export POLARIS_LOCAL_CANDIDATE_BUILD
# The release tag's prerelease label, beta.N or rc.N, or nothing for a stable build. Unset means
# stable, so a local candidate build needs nothing new.
POLARIS_PRERELEASE_LABEL="${POLARIS_PRERELEASE_LABEL-}"
if [ -n "$POLARIS_PRERELEASE_LABEL" ] && [[ ! "$POLARIS_PRERELEASE_LABEL" =~ ^(beta|rc)\.[0-9]+$ ]]; then
  printf '%s\n' 'POLARIS_PRERELEASE_LABEL must be empty, beta.N or rc.N' >&2
  exit 1
fi
export POLARIS_PRERELEASE_LABEL

cleanup() {
  umount "$STEAMOS_ROOT/etc/resolv.conf" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/opt" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/mnt" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/run" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/dev" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/sys" 2>/dev/null || true
  umount -R "$STEAMOS_ROOT/proc" 2>/dev/null || true
  umount "$STEAMOS_ROOT" 2>/dev/null || true
}
trap cleanup EXIT

pacman -Sy --noconfirm arch-install-scripts curl git
pacman-key --init

curl --fail --location --proto '=https' --tlsv1.2 \
  --output "/tmp/$HOLO_KEYRING_PACKAGE" \
  "https://steamdeck-packages.steamos.cloud/archlinux-mirror/holo-$STEAMOS_REPOSITORY_SUFFIX/os/x86_64/$HOLO_KEYRING_PACKAGE"
printf '%s  %s\n' \
  "$HOLO_KEYRING_SHA256" \
  "/tmp/$HOLO_KEYRING_PACKAGE" | sha256sum -c -

printf '%s\n' '[options]
Architecture = auto
SigLevel = Never
LocalFileSigLevel = Never' > /tmp/steamos-keyring-bootstrap.conf
pacman --config /tmp/steamos-keyring-bootstrap.conf --noconfirm -U \
  "/tmp/$HOLO_KEYRING_PACKAGE"

printf '%s\n' '[options]' 'Architecture = auto' 'CheckSpace' \
  'SigLevel = Required DatabaseOptional' 'LocalFileSigLevel = Required' > "$STEAMOS_PACMAN_CONFIG"
for repository in jupiter holo core extra; do
  # Pacman expands the literal $arch placeholder when it reads this file.
  # shellcheck disable=SC2016
  printf '\n[%s-%s]\nServer = https://steamdeck-packages.steamos.cloud/archlinux-mirror/%s-%s/os/$arch\n' \
    "$repository" "$STEAMOS_REPOSITORY_SUFFIX" "$repository" "$STEAMOS_REPOSITORY_SUFFIX" >> "$STEAMOS_PACMAN_CONFIG"
done
pacman --config "$STEAMOS_PACMAN_CONFIG" --noconfirm -Syy

export SOURCE_ROOT=/mnt
export OUTPUT_ROOT=/opt
export BUILD_ROOT=/home/builder/polaris-steamos-build
git config --global --add safe.directory /workspace
: "${POLARIS_BUILD_COMMIT:?POLARIS_BUILD_COMMIT is required}"
CHECKOUT_COMMIT="$(git -C /workspace rev-parse HEAD)"
if [ "$CHECKOUT_COMMIT" != "$POLARIS_BUILD_COMMIT" ]; then
  printf '%s\n' 'checkout HEAD does not match the independently requested commit' >&2
  exit 1
fi

rm -rf -- "$STEAMOS_ROOT"
install -d -m 0755 -- "$STEAMOS_ROOT"
mount --bind "$STEAMOS_ROOT" "$STEAMOS_ROOT"
pacstrap -G -M -C "$STEAMOS_PACMAN_CONFIG" "$STEAMOS_ROOT" \
  base-devel appstream appstream-glib avahi binutils boost boost-libs ccache cmake curl \
  desktop-file-utils gcc git go grim labwc libayatana-appindicator libcap libdrm libei libevdev \
  libmfx libnotify libpulse libva libx11 libxcb libxfixes libxi libxrandr libxtst make mesa \
  miniupnpc namcap ninja nlohmann-json nodejs npm numactl openssl opus pipewire python shellcheck \
  shaderc sudo systemd vulkan-headers vulkan-icd-loader wayland which wlr-randr xorg-xdpyinfo xorg-xwayland

mount --bind /workspace "$STEAMOS_ROOT/mnt"
mount --bind /output "$STEAMOS_ROOT/opt"
mount --bind /etc/resolv.conf "$STEAMOS_ROOT/etc/resolv.conf"
# arch-chroot enters a PID namespace after mounting procfs, which hides the
# build process from /proc. Own the API mounts here and stay in this namespace.
mount --rbind /proc "$STEAMOS_ROOT/proc"
mount --make-rslave "$STEAMOS_ROOT/proc"
mount --rbind /sys "$STEAMOS_ROOT/sys"
mount --make-rslave "$STEAMOS_ROOT/sys"
mount --rbind /dev "$STEAMOS_ROOT/dev"
mount --make-rslave "$STEAMOS_ROOT/dev"
mount --rbind /run "$STEAMOS_ROOT/run"
mount --make-rslave "$STEAMOS_ROOT/run"
chroot "$STEAMOS_ROOT" useradd --create-home --uid 1000 builder
chroot "$STEAMOS_ROOT" chown -R builder:builder /home/builder /opt
chroot "$STEAMOS_ROOT" runuser --user builder -- \
  /mnt/scripts/ci/build-steamos-package.sh
chroot "$STEAMOS_ROOT" pacman --noconfirm -U \
  "/opt/$STEAMOS_PACKAGE"
PACKAGE_DEPENDENCIES=()
while IFS= read -r dependency; do
  if [ -n "$dependency" ]; then
    PACKAGE_DEPENDENCIES+=("$dependency")
  fi
done < "$STEAMOS_RECEIPT_PREFIX-package-dependencies.txt"
chroot "$STEAMOS_ROOT" pacman -T "${PACKAGE_DEPENDENCIES[@]}" \
  > "$STEAMOS_RECEIPT_PREFIX-missing-dependencies.txt"
test ! -s "$STEAMOS_RECEIPT_PREFIX-missing-dependencies.txt"
chroot "$STEAMOS_ROOT" /usr/bin/polaris --version \
  > "$STEAMOS_RECEIPT_PREFIX-installed-version.txt"
# The binary reports the release number, and a prerelease's label the way the host spells it,
# X.Y.Z-beta.N. The package version alone cannot show that the build itself had the label.
EXPECTED_RUNTIME_VERSION="$(grep -Pom1 '^project\(Polaris VERSION \K[^ ]+' /workspace/CMakeLists.txt)"
if [ -n "$POLARIS_PRERELEASE_LABEL" ]; then
  EXPECTED_RUNTIME_VERSION="$EXPECTED_RUNTIME_VERSION-$POLARIS_PRERELEASE_LABEL"
fi
if ! grep -Fq "Polaris version: $EXPECTED_RUNTIME_VERSION commit:" "$STEAMOS_RECEIPT_PREFIX-installed-version.txt"; then
  printf 'installed SteamOS binary reports a version other than %s\n' "$EXPECTED_RUNTIME_VERSION" >&2
  exit 1
fi
