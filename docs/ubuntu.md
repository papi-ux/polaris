# Ubuntu Install Guide

Ubuntu 24.04 is the first Debian-family target for a direct Polaris DEB package. The CI path
builds and smoke-tests `Polaris-ubuntu24.04-x86_64.deb`, and the asset is published with `v1.0.3`
and later Polaris releases.

## Release Package

```bash
wget https://github.com/papi-ux/polaris/releases/latest/download/Polaris-ubuntu24.04-x86_64.deb
sudo apt install ./Polaris-ubuntu24.04-x86_64.deb
sudo polaris --setup-host
polaris
```

Open `https://localhost:47990`, create the web UI password, and pair Moonlight, Nova, or another
GameStream-compatible client.

## Source Build Fallback

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential ccache cmake git ninja-build \
  libboost-all-dev libssl-dev libevdev-dev \
  libpulse-dev libopus-dev libcurl4-openssl-dev \
  libdrm-dev libgbm-dev libcap-dev libwayland-dev \
  libpipewire-0.3-dev libx11-dev libxrandr-dev \
  libxfixes-dev libxi-dev libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libva-dev libminiupnpc-dev libnotify-dev nlohmann-json3-dev \
  libappindicator3-dev libgtk-3-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  wayland-protocols nodejs npm

git clone --recursive https://github.com/papi-ux/polaris.git
cd polaris
npm ci --no-audit --fund=false
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOLARIS_ENABLE_CUDA=OFF \
  -DCUDA_FAIL_ON_MISSING=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo polaris --setup-host
polaris
```

Use `-DPOLARIS_ENABLE_CUDA=ON` only when the CUDA toolkit is installed and you specifically want a
CUDA-enabled local build.

## Optional Setup

Enable the user service if you want Polaris to start in the background:

```bash
systemctl --user enable --now polaris
```

Only enable DRM/KMS capture if you specifically need it:

```bash
sudo polaris --setup-host --enable-kms
```

The default compositor and portal paths do not require granting KMS capability.

## Validation Checklist

Please include these details when reporting Ubuntu issues:

- Ubuntu version from `lsb_release -a`
- package install or source build
- GPU model and driver stack
- desktop environment and display server, such as GNOME Wayland or KDE Wayland
- whether `sudo polaris --setup-host` completed successfully
- whether the web UI opens at `https://localhost:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- whether headless mode and virtual display behavior worked after a reboot

## Current Status

Ubuntu 24.04 builds and fast C++ tests run in CI. The DEB package is built, installed, checked for
unresolved shared-library dependencies, and published on tagged builds.
