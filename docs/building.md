# Building Polaris

Polaris is a Linux-first host today. The most validated install paths are the Fedora RPM and Arch
package from the [latest release](https://github.com/papi-ux/polaris/releases/latest). CachyOS and
most pacman-compatible Arch derivatives should start with the Arch package path. Bazzite can use the
matching Fedora RPM through `rpm-ostree`, and Ubuntu 24.04 has a DEB package, but both package paths
need broader real-hardware validation. openSUSE Tumbleweed is source-build supported with a dedicated
[openSUSE guide](openSUSE.md); other distros remain source-build/community-validation oriented.

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

See the [Bazzite install guide](bazzite.md), [Ubuntu install guide](ubuntu.md), and
[openSUSE build guide](openSUSE.md) for caveats, fallback paths, and validation notes before using
those less-turnkey paths.

### Distro compatibility buckets

| Host distro | Current path | Notes |
| --- | --- | --- |
| Fedora 42/43/44 | Published RPM assets | Most validated package path. |
| Arch Linux | Published `pkg.tar.zst` asset | Recommended rolling-release path. |
| CachyOS / Arch derivatives | Start with the Arch package | Pacman-compatible derivatives should work from the Arch asset first; use source/local package fallback if dependency names or runtime helpers drift. |
| Bazzite 44 | Fedora 44 RPM layered with `rpm-ostree` | Experimental; Desktop Mode has NVIDIA Headless Stream coverage, Steam/Game Mode needs more reports. |
| Ubuntu 24.04 | Published DEB asset | Experimental tester package; broader desktop/GPU coverage needed. |
| openSUSE Tumbleweed | Source build | Dedicated guide and CI build coverage; no published release asset yet. |
| Debian-family other than Ubuntu 24.04 | Source build | No general Debian package asset yet. |
| openSUSE Leap, NixOS, Gentoo, custom hosts | Source build/community validation | Please report distro, GPU, driver, compositor, package list, and stream runtime details. |

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
| grim | Dashboard preview capture for labwc/Wayland |
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
sudo dnf install grim labwc wlr-randr xorg-x11-server-Xwayland xdpyinfo
```

#### Arch / CachyOS

```bash
sudo pacman -S --needed base-devel git cmake ninja appstream appstream-glib \
  desktop-file-utils boost boost-libs curl openssl libevdev pipewire wayland \
  wayland-protocols libdrm libcap libnotify libayatana-appindicator \
  libpulse libva libx11 libxcb libxfixes libxi libxrandr libxtst \
  miniupnpc nlohmann-json numactl avahi opus libmfx mesa which nodejs npm \
  grim labwc wlr-randr xorg-xwayland xorg-xdpyinfo cuda
```

CachyOS should use the same package/dependency family first. If a CachyOS kernel, NVIDIA/CUDA stack, or pacman package split behaves differently, include those details when opening an issue.

#### openSUSE Tumbleweed

Use the dedicated [openSUSE guide](openSUSE.md). The short version is: shared Boost is required, so configure with `-DBOOST_USE_STATIC=OFF`; AMD/VAAPI can build with CUDA disabled, while NVIDIA hosts should install CUDA and enable `-DPOLARIS_ENABLE_CUDA=ON`.

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

Experimental Browser Stream builds require Go and can include the WebTransport helper with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPOLARIS_ENABLE_CUDA=ON -DPOLARIS_ENABLE_BROWSER_STREAM=ON
cmake --build build -j"$(nproc)"
```

`POLARIS_ENABLE_WEBRTC` remains available as a deprecated alias for older local build scripts.

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
- Use `scripts/dev-clean.sh status` before or after build sessions to inspect local branches, stashes, worktrees, generated artifacts, and repository size.
- Use `scripts/dev-clean.sh prune --apply` after throwaway local builds to remove allowlisted build/test outputs such as `build/`, `build-*`, `cmake-*`, `arch-pkgbuild/`, `test-results/`, and `playwright-report/`.
- Use `scripts/dev-clean.sh git-prune --apply` after deleting branches, stashes, or worktrees to prune stale remote refs, worktree metadata, and unreachable Git objects.
- Use `scripts/dev-clean.sh nuke-local-builds --apply` when you intentionally want to reset generated build outputs plus `node_modules/`.

### C++ sanitizer check

The `C++ sanitizer tests` CI job runs the fast C++ unit-test target under AddressSanitizer and
UndefinedBehaviorSanitizer. To mirror it locally, use a throwaway Debug build tree:

```bash
cmake -B build-sanitize -G Ninja \
  -DBUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DPOLARIS_ENABLE_CUDA=OFF \
  -DPOLARIS_ENABLE_BROWSER_STREAM=OFF \
  -DCUDA_FAIL_ON_MISSING=OFF
cmake --build build-sanitize --target test_polaris -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitize/tests --output-on-failure -R '^test_polaris$'
```

## Packaging

The public release assets are currently:

- `Polaris-fedora42-x86_64.rpm`
- `Polaris-fedora43-x86_64.rpm`
- `Polaris-fedora44-x86_64.rpm`
- `Polaris-ubuntu24.04-x86_64.deb`
- `Polaris-arch-x86_64.pkg.tar.zst`

Bazzite installs use the matching Fedora RPM through `rpm-ostree` for now. Ubuntu 24.04 has a
direct DEB package. openSUSE Tumbleweed has source-build guidance and CI build coverage but no
published release package asset yet. CachyOS should start with the Arch package path. Other distro
paths are still source-build/community-validation oriented.
