#!/usr/bin/env bash
# Install build + runtime dependencies for Polaris on non-NixOS hosts.
# Usage: ./01-install-deps.sh [--cuda]
set -euo pipefail
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

WITH_CUDA=0
for arg in "$@"; do
  case "$arg" in
    --cuda) WITH_CUDA=1 ;;
    -h|--help)
      cat <<EOF
Install Polaris build/runtime dependencies (Fedora, Arch, Debian/Ubuntu, openSUSE).

Usage: $0 [--cuda]

  --cuda   Also install CUDA toolkit packages when the distro provides them
EOF
      exit 0
      ;;
    *) die "unknown option: $arg" ;;
  esac
done

DISTRO="$(detect_distro)"
log "detected distro family: $DISTRO"

case "$DISTRO" in
  fedora)
    maybe_sudo dnf install -y dnf-plugins-core git
    if [ -f "$REPO_ROOT/packaging/linux/fedora/Polaris.spec" ]; then
      maybe_sudo dnf builddep -y "$REPO_ROOT/packaging/linux/fedora/Polaris.spec" || true
    fi
    maybe_sudo dnf install -y \
      gcc-c++ cmake ninja-build git nodejs npm \
      boost-devel openssl-devel libevdev-devel \
      pulseaudio-libs-devel opus-devel libcurl-devel \
      libdrm-devel mesa-libgbm-devel libcap-devel \
      wayland-devel wayland-protocols-devel \
      pipewire-devel libX11-devel libXrandr-devel libXfixes-devel libXi-devel \
      libva-devel miniupnpc-devel libnotify-devel \
      json-devel libappindicator-gtk3-devel gtk3-devel \
      ffmpeg-free-devel \
      grim labwc wlr-randr xorg-x11-server-Xwayland xdpyinfo \
      gamescope steam bubblewrap util-linux wireplumber \
      avahi-devel numactl-devel \
      systemd-devel pkgconf-pkg-config
    if [ "$WITH_CUDA" = 1 ]; then
      maybe_sudo dnf install -y cuda-nvcc cuda-cudart-devel 2>/dev/null \
        || warn "CUDA packages not found via dnf; install NVIDIA CUDA toolkit manually"
    fi
    ;;
  arch)
    maybe_sudo pacman -S --needed --noconfirm \
      base-devel git cmake ninja nodejs npm \
      boost openssl libevdev libpulse opus curl \
      libdrm mesa libcap wayland wayland-protocols \
      pipewire libx11 libxrandr libxfixes libxi \
      libva miniupnpc libnotify nlohmann-json \
      libappindicator-gtk3 gtk3 \
      grim labwc wlr-randr xorg-xwayland xorg-xdpyinfo \
      gamescope steam bubblewrap util-linux wireplumber \
      avahi numactl systemd pkgconf
    if [ "$WITH_CUDA" = 1 ]; then
      maybe_sudo pacman -S --needed --noconfirm cuda 2>/dev/null \
        || warn "install 'cuda' from official/extra or use the NVIDIA runfile"
    fi
    ;;
  debian)
    maybe_sudo apt-get update
    maybe_sudo apt-get install -y \
      build-essential ccache cmake git ninja-build \
      nodejs npm \
      libboost-all-dev libssl-dev libevdev-dev \
      libpulse-dev libopus-dev libcurl4-openssl-dev \
      libdrm-dev libgbm-dev libcap-dev libwayland-dev wayland-protocols \
      libpipewire-0.3-dev libx11-dev libxrandr-dev \
      libxfixes-dev libxi-dev libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev \
      libva-dev libminiupnpc-dev libnotify-dev nlohmann-json3-dev \
      libappindicator3-dev libgtk-3-dev \
      libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
      grim labwc wlr-randr xwayland x11-utils \
      gamescope steam-installer bubblewrap util-linux wireplumber \
      libavahi-client-dev libnuma-dev \
      pkg-config
    if [ "$WITH_CUDA" = 1 ]; then
      warn "On Ubuntu/Debian install CUDA from NVIDIA; apt package names vary by release"
    fi
    ;;
  suse)
    maybe_sudo zypper install -y \
      gcc-c++ make cmake ninja git nodejs npm \
      libboost_headers-devel libboost_system-devel libboost_filesystem-devel \
      libboost_thread-devel libboost_log-devel libboost_program_options-devel \
      libopenssl-devel libevdev-devel libpulse-devel libopus-devel \
      libcurl-devel libdrm-devel libgbm-devel libcap-devel \
      wayland-devel wayland-protocols-devel \
      pipewire-devel libX11-devel libXrandr-devel libXfixes-devel libXi-devel \
      libva-devel libminiupnpc-devel libnotify-devel nlohmann_json-devel \
      libappindicator3-devel gtk3-devel \
      grim labwc wlr-randr xwayland xdpyinfo \
      gamescope bubblewrap util-linux wireplumber \
      pkgconf-pkg-config
    if [ "$WITH_CUDA" = 1 ]; then
      warn "Install CUDA toolkit from NVIDIA for openSUSE"
    fi
    ;;
  *)
    die "unsupported distro family '$DISTRO' — install deps manually (see docs/building.md)"
    ;;
esac

log "dependencies installed"
log "next: ./02-build-polaris.sh  (or ./install.sh --from-source)"
