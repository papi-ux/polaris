# Building Polaris

Polaris is a Linux-first host today. The most validated install paths are the Fedora RPM and Arch
package from the [latest release](https://github.com/papi-ux/polaris/releases/latest). Source builds
remain the right path for other Debian-family distros and local development, and Arch also supports
a local package-build flow in addition to the published package asset. Bazzite can use the matching
Fedora RPM through `rpm-ostree`, and Ubuntu 24.04 has a DEB package, but both of those package paths
need broader real-hardware validation.

## Release packages

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo dnf install "./Polaris-fedora${fedora_version}-x86_64.rpm"
sudo polaris --setup-host
polaris
```

```bash
wget https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst
sudo polaris --setup-host
polaris
```

```bash
wget https://github.com/papi-ux/polaris/releases/latest/download/Polaris-ubuntu24.04-x86_64.deb
sudo apt install ./Polaris-ubuntu24.04-x86_64.deb
sudo polaris --setup-host
polaris
```

```bash
fedora_version="$(rpm -E %fedora)"
rpm_name="Polaris-fedora${fedora_version}-x86_64.rpm"
wget "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}"
sudo rpm-ostree install -r "./${rpm_name}"

# After reboot:
sudo polaris --setup-host
systemctl --user enable --now polaris
```

The web UI will be available at `https://localhost:47990`.

See the [Bazzite install guide](bazzite.md) and [Ubuntu install guide](ubuntu.md) for caveats,
fallback paths, and validation notes before using either experimental path.

## Source Build

### Requirements

| Tool | Notes |
| --- | --- |
| CMake 3.20+ | Build system |
| C++20 compiler | GCC or Clang |
| Boost | Core libraries |
| OpenSSL | TLS and pairing |
| libevdev | Virtual input |
| PipeWire | Audio capture |
| Wayland client libs | Linux compositor integration |
| Node.js 18+ | Web UI build |
| labwc | Isolated stream compositor |
| wlr-randr | Configure the isolated stream output mode |
| Xwayland and xdpyinfo | Launch and detect X11 clients inside labwc |
| CUDA toolkit | Needed for NVENC builds |

### Example packages

#### Fedora

Run this from the cloned Polaris checkout so `dnf builddep` can read the packaged build requirements.

```bash
sudo dnf install dnf-plugins-core git
sudo dnf builddep -y packaging/linux/fedora/Polaris.spec
sudo dnf install labwc wlr-randr xorg-x11-server-Xwayland xdpyinfo
```

#### Arch

```bash
sudo pacman -S --needed base-devel git cmake ninja appstream appstream-glib \
  desktop-file-utils boost boost-libs curl openssl libevdev pipewire wayland \
  wayland-protocols libdrm libcap libnotify libayatana-appindicator \
  libpulse libva libx11 libxcb libxfixes libxi libxrandr libxtst \
  miniupnpc nlohmann-json numactl avahi opus libmfx mesa which nodejs npm \
  labwc wlr-randr xorg-xwayland xorg-xdpyinfo cuda
```

### Build and install

```bash
git clone --recursive https://github.com/papi-ux/polaris.git
cd polaris
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPOLARIS_ENABLE_CUDA=ON
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo polaris --setup-host
polaris
```

Optional DRM/KMS setup:

```bash
sudo polaris --setup-host --enable-kms
```

Optional user-service autostart:

```bash
systemctl --user enable --now polaris
```

## Local Arch package build

If you prefer an installable Arch package over a direct `cmake --install`, Polaris can generate
a local `PKGBUILD` and build a `pkg.tar.zst` from the current commit:

```bash
BUILD_VERSION="$(grep -Pom1 '^project\(Polaris VERSION \K[^ ]+' CMakeLists.txt)"
BRANCH="$(git branch --show-current)"
COMMIT="$(git rev-parse HEAD)"
env BRANCH="$BRANCH" BUILD_VERSION="$BUILD_VERSION" CLONE_URL="file://$PWD" COMMIT="$COMMIT" \
  cmake -S . -B arch-pkgbuild -DPOLARIS_CONFIGURE_PKGBUILD=ON -DPOLARIS_CONFIGURE_ONLY=ON
env GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always \
  bash -lc 'cd arch-pkgbuild && makepkg -si'
```

This local package path builds from the current committed state. The GitHub release asset is the
shortest install path; the local package path is useful when you want to package the current
checkout yourself.

## Development Notes

- The main local web build commands are `npm run lint` and `npm run build`.
- `build-tests/` is useful when you want to rebuild the served web bundle without touching your main local build.
- Polaris stores runtime config in `~/.config/polaris`.

## Packaging

The public release assets are currently:

- `Polaris-fedora42-x86_64.rpm`
- `Polaris-fedora43-x86_64.rpm`
- `Polaris-fedora44-x86_64.rpm`
- `Polaris-ubuntu24.04-x86_64.deb`
- `Polaris-arch-x86_64.pkg.tar.zst`

Bazzite installs use the matching Fedora RPM through `rpm-ostree` for now. Ubuntu 24.04 has a
direct DEB package. Both still need broader validation; other Debian-family distro paths are still
source-build oriented.
