# Non-NixOS install scripts

Install Polaris on Fedora, Arch, Ubuntu/Debian, or openSUSE **without Nix**.

For packaged releases, prefer the distro assets in the main [README](../../README.md) when they match your host. Use this directory when:

- you are on this branch’s **gamescope_stream** stack (idle gamescope + session helpers), or  
- you need a **source + CUDA** build, or  
- your distro has no release package yet

## Quick path

```bash
cd /path/to/polaris
git submodule update --init --recursive

# Full source install + gamescope_stream user units
./scripts/install/install.sh --from-source

# NVIDIA encode (requires CUDA toolkit / nvcc)
./scripts/install/install.sh --from-source --cuda

# User-local prefix (no root for install; still need sudo -H for --setup-host)
PREFIX=$HOME/.local ./scripts/install/install.sh --from-source
```

Polaris already installed from RPM/DEB/AUR, only wire the gamescope stack:

```bash
./scripts/install/install.sh --package-only
# or step by step:
./scripts/install/03-install-gamescope-stack.sh
./scripts/install/04-enable-services.sh
```

Labwc-only (stock headless path, no gamescope idle):

```bash
./scripts/install/install.sh --package-only --labwc
```

## Scripts

| Script | Role |
|--------|------|
| `install.sh` | Driver: deps → build → setup-host → gamescope stack → enable |
| `01-install-deps.sh` | Distro packages (Fedora / Arch / Debian / openSUSE) |
| `02-build-polaris.sh` | CMake Ninja build + install to `PREFIX` |
| `03-install-gamescope-stack.sh` | Idle/session/wait helpers + systemd user units |
| `04-enable-services.sh` | `systemctl --user enable --now …` |
| `lib/*` | Helper sources installed under `$PREFIX/bin` |

## What gets installed (gamescope_stream)

| Path | Purpose |
|------|---------|
| `$PREFIX/bin/polaris` | Host (from `02` or package) |
| `$PREFIX/bin/polaris-start` | Seeds `~/.config/polaris/polaris.conf` once, then execs polaris |
| `$PREFIX/bin/polaris-gamescope-idle` | Headless gamescope holding `gamescope-0` |
| `$PREFIX/bin/polaris-gamescope-session` | Nested WSI / attach Steam launch helper |
| `$PREFIX/bin/polaris-wait-gamescope` | `ExecStartPre`: wait for `gamescope-0` |
| `~/.config/systemd/user/polaris-gamescope-idle.service` | Idle compositor |
| `~/.config/systemd/user/polaris.service` | Polaris (gamescope-oriented env) |

Default conf seed: `linux_stream_mode = gamescope_stream`, `capture = portal`.

## After install

1. Open **https://127.0.0.1:47990** and create the web UI account.  
2. Pair Moonlight / Nova.  
3. Logs: `journalctl --user -u polaris -f`  
4. Stop/start: `systemctl --user restart polaris polaris-gamescope-idle`

### Optional environment

| Variable | Default | Meaning |
|----------|---------|---------|
| `PREFIX` | `/usr/local` | Install root |
| `POLARIS_HDR_WIDTH` / `HEIGHT` / `REFRESH` | 3840 / 2160 / 120 | Gamescope geometry |
| `POLARIS_GAMESCOPE_BIN` | `gamescope` | Override gamescope binary (e.g. patched build) |
| `POLARIS_GAMESCOPE_PREFER_VK` | unset | `--prefer-vk-device` for multi-GPU |
| `POLARIS_PORTAL_DBUS_ADDRESS` | unset | Private portal bus (advanced) |

## Private portal (optional)

NixOS/hjem can run a **private** gamescope ScreenCast bus at  
`$XDG_RUNTIME_DIR/polaris-portal/bus`. These scripts do **not** install that stack.

On typical desktops, Polaris uses the **host** XDG Desktop Portal + PipeWire, or **gamescopegrab** when the idle compositor exposes a PW node. That is enough for many non-NixOS hosts.

If you later deploy a private portal, set:

```ini
# ~/.config/systemd/user/polaris.service.d/10-private-portal.conf
[Service]
Environment=POLARIS_PORTAL_DBUS_ADDRESS=unix:path=%t/polaris-portal/bus
```

## Color / HDR tips (gamescope_stream)

- SDR capture is SPA **BGRx** → keep host convert as classic BGR (do not force RGB8 on XRGB).  
- Avoid **HDR gamescope + SDR encode** (blue wash): use Moonlight SDR or client profile `hdr: false` until full HDR encode is proven.  

## Uninstall (user units)

```bash
systemctl --user disable --now polaris polaris-gamescope-idle 2>/dev/null || true
rm -f ~/.config/systemd/user/polaris.service \
      ~/.config/systemd/user/polaris-gamescope-idle.service
systemctl --user daemon-reload
# binaries under PREFIX:
#   sudo rm -f /usr/local/bin/polaris-gamescope-{idle,session} \
#              /usr/local/bin/polaris-wait-gamescope /usr/local/bin/polaris-start
```

Package-installed `polaris` is removed with your distro package manager.

## Related docs

- [Building](../../docs/building.md)  
- [Ubuntu](../../docs/ubuntu.md) · [openSUSE](../../docs/openSUSE.md) · [Bazzite](../../docs/bazzite.md)  
- [Stream paths](../../docs/stream-paths.md) · [Runtime](../../docs/runtime.md)  
