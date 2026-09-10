#!/bin/sh
# Run only in a disposable instance of an exact locked source root.
# This resolver creates reviewable inputs; final builds never invoke apt update.
set -eu
export DEBIAN_FRONTEND=noninteractive
sed -i 's|http://archive.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/20260120T000000Z/|g; s|http://security.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/20260120T000000Z/|g' /etc/apt/sources.list.d/ubuntu.sources
apt-get -o Acquire::Check-Valid-Until=false update
runtime='dbus pipewire pipewire-pulse pipewire-bin wireplumber libspa-0.2-modules libpipewire-0.3-modules pulseaudio-utils gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-gl gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libegl1 libgbm1 libgl1 libvulkan1 libwayland-client0 xwayland'
build='ca-certificates pkg-config build-essential clang libclang-dev libudev-dev libinput-dev libxkbcommon-dev libwayland-dev libegl-dev libgbm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev meson ninja-build cmake glslang-tools libpipewire-0.3-dev libx11-dev libxdamage-dev libxcomposite-dev libxcursor-dev libxrender-dev libxext-dev libxfixes-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev libxi-dev libdrm-dev libvulkan-dev wayland-protocols libpixman-1-dev libdecor-0-dev libluajit-5.1-dev libseat-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-res0-dev libxcb-ewmh-dev hwdata git'
for role in runtime build; do
  mkdir -p "/out/$role/partial"
  requested="$runtime"
  if [ "$role" = build ]; then requested="$runtime $build"; fi
  # Word splitting is intentional for the fixed package list above.
  apt-get --yes --no-install-recommends --print-uris install $requested > "/out/$role.uris"
  apt-get --yes --no-install-recommends --download-only -o "Dir::Cache::archives=/out/$role" install $requested
  for package in "/out/$role/"*.deb; do
    [ -f "$package" ] || continue
    basename "$package"
    dpkg-deb --show --showformat='${Package}\t${Version}\t${Architecture}\n' "$package"
    sha256sum "$package"
  done > "/out/$role.manifest"
done
dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /out/source-packages.tsv
cp /etc/apt/sources.list.d/ubuntu.sources /out/resolved.sources
