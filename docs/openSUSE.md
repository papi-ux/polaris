# Building Polaris on openSUSE Tumbleweed

Polaris has no prebuilt openSUSE package yet, but it builds cleanly from source on
**openSUSE Tumbleweed** (x86_64). This guide covers the dependencies, the build, and
a few openSUSE-specific gotchas. A CI workflow that builds an installable RPM in an
`opensuse/tumbleweed` container lives at
[`.github/workflows/opensuse-build.yml`](../.github/workflows/opensuse-build.yml).

> Tested on Tumbleweed with an AMD GPU (VAAPI encode). NVIDIA hosts should add the
> CUDA toolkit and build with `-DPOLARIS_ENABLE_CUDA=ON`.

## 1. Install build dependencies

```bash
sudo zypper install \
  gcc-c++ make cmake ninja git python3 which wget \
  nodejs24 npm24 go \
  libboost_headers-devel libboost_filesystem-devel libboost_log-devel \
  libboost_locale-devel libboost_program_options-devel libboost_thread-devel \
  libcap-devel libcurl-devel libdrm-devel libevdev-devel libgudev-1_0-devel \
  libnotify-devel libva-devel libopenssl-devel systemd-devel \
  libX11-devel libxcb-devel libXcursor-devel libXfixes-devel libXi-devel \
  libXinerama-devel libXrandr-devel libXtst-devel \
  Mesa-libGL-devel libgbm-devel \
  libminiupnpc-devel nlohmann_json-devel libnuma-devel libpulse-devel \
  libopus-devel libopusenc-devel \
  wayland-devel wayland-protocols-devel pipewire-devel \
  libayatana-appindicator3-devel desktop-file-utils appstream-glib \
  labwc grim wlr-randr xdpyinfo xwayland
```

(The last line — `labwc grim wlr-randr xdpyinfo xwayland` — are runtime helpers for the
isolated compositor / headless capture path.)

## 2. Build

```bash
git clone --recursive https://github.com/papi-ux/polaris.git
cd polaris
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DPOLARIS_ASSETS_DIR=/usr/share/polaris \
  -DBOOST_USE_STATIC=OFF \
  -DPOLARIS_ENABLE_CUDA=OFF
cmake --build build --parallel "$(nproc)"
sudo cmake --install build
sudo polaris --setup-host
```

### openSUSE-specific notes

- **`-DBOOST_USE_STATIC=OFF` is required.** openSUSE ships only *shared* Boost. With the
  default static lookup, `find_package(Boost ...)` reports missing component targets and
  the FetchContent fallback collides with the partially-detected system targets
  (`add_library cannot create ALIAS target "Boost::headers" ... already exists`). Turning
  static off makes it use the shared system Boost directly.
- **AMD / VAAPI** is the default-friendly path (`-DPOLARIS_ENABLE_CUDA=OFF`); choose the
  VAAPI encoder in the web UI. NVIDIA hosts: install the CUDA toolkit and use
  `-DPOLARIS_ENABLE_CUDA=ON`.

## 3. Optional: build an installable RPM

`cpack -G RPM` works, but the project's explicit `CPACK_RPM_PACKAGE_REQUIRES` uses
Fedora package names (`boost-filesystem`, `libcurl`, `xorg-x11-server-Xwayland`, …) that
don't exist on openSUSE. Override them with openSUSE names — the shared-library
dependencies are still auto-detected:

```bash
( cd build && cpack -G RPM \
    -D CPACK_RPM_PACKAGE_REQUIRES="grim, labwc, wlr-randr, xdpyinfo, xwayland, which" )
sudo zypper install --allow-unsigned-rpm ./build/cpack_artifacts/*.rpm
```

## 4. Container/CI note

When building inside a minimal `opensuse/tumbleweed` container, the base image ships
`busybox-gawk`, which blocks the real `gawk` that `desktop-file-utils`/`rpm-build`
require. Install gawk first to swap it in:

```bash
zypper --non-interactive install --force-resolution gawk
```

This is unnecessary on a normal Tumbleweed desktop (gawk is already present).
