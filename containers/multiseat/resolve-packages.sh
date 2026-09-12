#!/bin/sh
# Run only in a disposable instance of the locked Ubuntu root, never on a host.
# This produces reviewable inputs; final builds cannot contact repositories.
set -eu
profile=${1:?profile required}
case "$profile" in gamescope|steam|heroic|lutris) ;; *) exit 2 ;; esac
. /etc/os-release
test "$ID:$VERSION_ID" = ubuntu:26.04
export DEBIAN_FRONTEND=noninteractive
# Mount a public CA bundle at /resolver-ca.crt for TLS bootstrap. Ubuntu's
# shipped archive keyring independently authenticates the snapshot metadata.
cat > /etc/apt/sources.list.d/ubuntu.sources <<'SOURCES'
Types: deb
URIs: https://snapshot.ubuntu.com/ubuntu/20260911T000000Z/
Suites: resolute resolute-updates resolute-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SOURCES
if [ "$profile" != gamescope ]; then dpkg --add-architecture i386; fi
apt-get -o Acquire::https::CaInfo=/resolver-ca.crt -o APT::Update::Error-Mode=any -o Acquire::Check-Valid-Until=false update
runtime='python3 ca-certificates passwd util-linux procps locales tzdata fontconfig fonts-dejavu-core dbus dbus-x11 pipewire pipewire-pulse pipewire-bin wireplumber libspa-0.2-modules libpipewire-0.3-modules pulseaudio-utils gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-gl gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libegl1 libgbm1 libgl1 libvulkan1 mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 libva2 libvdpau1 libwayland-client0 libwayland-server0 libseat1 libinput10 libluajit-5.1-2 hwdata xwayland xauth x11-utils x11-xserver-utils xdg-utils'
build='pkg-config build-essential clang libclang-dev libudev-dev libinput-dev libxkbcommon-dev libwayland-dev libegl-dev libgbm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev meson ninja-build cmake glslang-tools libpipewire-0.3-dev libx11-dev libxdamage-dev libxcomposite-dev libxcursor-dev libxrender-dev libxext-dev libxfixes-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev libxi-dev libdrm-dev libvulkan-dev wayland-protocols libpixman-1-dev libdecor-0-dev libluajit-5.1-dev libseat-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-res0-dev libxcb-ewmh-dev hwdata git'
case "$profile" in
  steam) runtime="$runtime steam-installer steam-libs:amd64 steam-libs:i386 mesa-vulkan-drivers:i386 libgl1-mesa-dri:i386" ;;
  heroic) runtime="$runtime /launchers/heroic.deb mesa-vulkan-drivers:i386 libgl1-mesa-dri:i386" ;;
  lutris) runtime="$runtime lutris wine winetricks mesa-vulkan-drivers:i386 libgl1-mesa-dri:i386" ;;
esac
if [ "$profile" != gamescope ]; then
  runtime="$runtime libegl1:i386 libgles2:i386 libgl1:i386 libwayland-server0:i386"
fi
for role in runtime build; do
  mkdir -p "/out/$role/partial"
  requested="$runtime"
  if [ "$role" = build ]; then requested="$runtime $build"; fi
  # Word splitting is intentional for the fixed lists above.
  apt-get --yes --no-install-recommends --print-uris install $requested > "/out/$role.uris"
  apt-get -o Acquire::https::CaInfo=/resolver-ca.crt --yes --no-install-recommends --download-only -o "Dir::Cache::archives=/out/$role" install $requested
  if [ "$profile" = heroic ]; then
    launcher_filename=$(awk '$1 ~ /^.file:/ {print $2}' "/out/$role.uris")
    case "$launcher_filename" in */*|*[!a-zA-Z0-9._+%:~=-]*|'') exit 1 ;; esac
    cp /launchers/heroic.deb "/out/$role/$launcher_filename"
  fi
  # Reused caches can contain packages no longer selected by APT. Record only
  # this resolution's HTTPS inputs, plus the explicitly pinned local launcher.
  selected=$(awk '$1 ~ /^.https?:/ {print $2}' "/out/$role.uris")
  if [ "$profile" = heroic ]; then selected="$selected $launcher_filename"; fi
  for filename in $selected; do
    case "$filename" in */*|*[!a-zA-Z0-9._+%:~=-]*|'') exit 1 ;; esac
    package="/out/$role/$filename"
    test -f "$package" && test ! -L "$package"
    printf '%s\n' "$filename"
    dpkg-deb --show --showformat='${Package}\t${Version}\t${Architecture}\n' "$package"
    sha256sum "$package"
  done > "/out/$role.manifest"
done
dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /out/source-packages.tsv
cp /etc/apt/sources.list.d/ubuntu.sources /out/resolved.sources
