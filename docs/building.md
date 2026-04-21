# Building Polaris

Polaris is a Linux-first host today. The fastest install path is the Fedora RPM from the
[latest release](https://github.com/papi-ux/polaris/releases/latest). Source builds are the
right path for Arch, Debian-family distros, and local development.

## Release RPM

```bash
wget https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora42-x86_64.rpm
sudo dnf install ./Polaris-fedora42-x86_64.rpm
sudo polaris --setup-host
polaris
```

The web UI will be available at `https://localhost:47990`.

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
| CUDA toolkit | Needed for NVENC builds |

### Example packages

#### Fedora

```bash
sudo dnf install cmake gcc-c++ boost-devel openssl-devel libevdev-devel \
  pipewire-devel wayland-devel libdrm-devel libcap-devel \
  mesa-libEGL-devel mesa-libGL-devel cuda-toolkit nodejs npm labwc
```

#### Arch

```bash
sudo pacman -S cmake boost openssl libevdev pipewire wayland \
  libdrm libcap mesa cuda nodejs npm labwc
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

## Development Notes

- The main local web build commands are `npm run lint` and `npm run build`.
- `build-tests/` is useful when you want to rebuild the served web bundle without touching your main local build.
- Polaris stores runtime config in `~/.config/polaris`.

## Packaging

The public release asset is currently:

- `Polaris-fedora42-x86_64.rpm`

Other distro paths are source-build oriented for now.
