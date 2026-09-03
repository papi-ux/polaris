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
POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD-0}"
if [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 0 ] && [ "$POLARIS_LOCAL_CANDIDATE_BUILD" != 1 ]; then
  printf '%s\n' 'POLARIS_LOCAL_CANDIDATE_BUILD must be 0 or 1' >&2
  exit 1
fi
export POLARIS_LOCAL_CANDIDATE_BUILD

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
  --output /tmp/holo-keyring-20250801-1-any.pkg.tar.zst \
  https://steamdeck-packages.steamos.cloud/archlinux-mirror/holo-3.8.1x/os/x86_64/holo-keyring-20250801-1-any.pkg.tar.zst
printf '%s  %s\n' \
  a5efa4f9c161ce9607fd9dfcccaf2a587baa9acd35eae04d3c01d967dddc9722 \
  /tmp/holo-keyring-20250801-1-any.pkg.tar.zst | sha256sum -c -

printf '%s\n' '[options]
Architecture = auto
SigLevel = Never
LocalFileSigLevel = Never' > /tmp/steamos-keyring-bootstrap.conf
pacman --config /tmp/steamos-keyring-bootstrap.conf --noconfirm -U \
  /tmp/holo-keyring-20250801-1-any.pkg.tar.zst

# Pacman expands the literal repository placeholders when it reads this file.
# shellcheck disable=SC2016
printf '%s\n' '[options]
Architecture = auto
CheckSpace
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Required

[jupiter-3.8.1x]
Server = https://steamdeck-packages.steamos.cloud/archlinux-mirror/jupiter-3.8.1x/os/$arch

[holo-3.8.1x]
Server = https://steamdeck-packages.steamos.cloud/archlinux-mirror/holo-3.8.1x/os/$arch

[core-3.8.1x]
Server = https://steamdeck-packages.steamos.cloud/archlinux-mirror/core-3.8.1x/os/$arch

[extra-3.8.1x]
Server = https://steamdeck-packages.steamos.cloud/archlinux-mirror/extra-3.8.1x/os/$arch' > /tmp/steamos-3.8.1x.conf
pacman --config /tmp/steamos-3.8.1x.conf --noconfirm -Syy

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
pacstrap -G -M -C /tmp/steamos-3.8.1x.conf "$STEAMOS_ROOT" \
  base-devel appstream appstream-glib avahi binutils boost boost-libs ccache cmake curl \
  desktop-file-utils gcc git go grim labwc libayatana-appindicator libcap libdrm libevdev \
  libmfx libnotify libpulse libva libx11 libxcb libxfixes libxi libxrandr libxtst make mesa \
  miniupnpc namcap ninja nlohmann-json nodejs npm numactl openssl opus pipewire shellcheck \
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
  /opt/Polaris-steamos3.8-x86_64.pkg.tar.zst
PACKAGE_DEPENDENCIES=()
while IFS= read -r dependency; do
  if [ -n "$dependency" ]; then
    PACKAGE_DEPENDENCIES+=("$dependency")
  fi
done < /output/steamos3.8-package-dependencies.txt
chroot "$STEAMOS_ROOT" pacman -T "${PACKAGE_DEPENDENCIES[@]}" \
  > /output/steamos3.8-missing-dependencies.txt
test ! -s /output/steamos3.8-missing-dependencies.txt
chroot "$STEAMOS_ROOT" /usr/bin/polaris --version \
  > /output/steamos3.8-installed-version.txt
