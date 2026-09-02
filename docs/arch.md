# Install on Arch Linux and CachyOS

Arch Linux is one of the two recommended Polaris package paths, and the official
`Polaris-arch-x86_64.pkg.tar.zst` asset ships with every release. CachyOS and most pacman-compatible
Arch derivatives should start with this same package.

SteamOS is pacman-based but is **not** covered by this page: it has its own versioned package and a
read-only root, so follow the [SteamOS guide](steamos.md) instead. The two packages are not
interchangeable.

## Install

```bash
wget --output-document=./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
polaris
```

The package installs the host binary, the web console assets, desktop metadata, and the user service
file. Host integration stays explicit: `--setup-host` is a separate step you run yourself.

**Fresh install:** open **https://localhost:47990/#/welcome**, create your web UI account, and pair
a client.

**Upgrade or reinstall:** open **https://localhost:47990/#/login** and use the existing account.
Arch and CachyOS package operations preserve credentials, pairing keys, settings, and the library
under `~/.config/polaris`; removing the package does not reset the web account. If needed, follow
the [credential reset](troubleshooting.md#web-ui-credentials) instead of returning to Welcome.

## What `--setup-host` does

It installs the udev rules and modules-load configuration that make virtual input work, and reports
anything it could not complete.

> [!WARNING]
> Only add `--enable-kms` when you actually need DRM/KMS capture:
> `sudo -H polaris --setup-host --enable-kms` grants `cap_sys_admin`. Polaris works without it on the
> default compositor and Headless Stream paths.

If you ran `--setup-host` on a version before v1.3.5, a copy of the udev rules may still sit in
`/etc/udev/rules.d/60-polaris.rules` and override the packaged file. Host setup keeps it and warns
rather than deleting it; see [Troubleshooting](troubleshooting.md) for the check and removal.

## Autostart

```bash
systemctl --user enable --now polaris
```

## Arch derivatives

CachyOS is expected to work through this package path. If a derivative renames dependencies or ships
different runtime helpers, the package may refuse to install or Polaris may fail to find a helper at
launch. In that case use the local package or source build in
[Building Polaris](building.md), and please report the derivative-specific gap with your distro, GPU,
driver, compositor, and package details.

## Verify the stream path

Confirm the recommended Linux configuration:

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
```

> **What you'll see:** with this configuration the built-in **Desktop** entry streams Polaris'
> *private* compositor — an intentionally empty screen (right-click opens the session menu) until a
> game is launched from your client. If you wanted a desktop stream instead, pick the mode for it:
>
> | I want | Set `linux_stream_mode` to |
> | --- | --- |
> | My real desktop, at host resolution | `desktop_display` (Mirror Desktop) |
> | An extra display, sized to the client | `host_virtual_display` |
> | An isolated game-only session, desktop untouched | `headless_stream` (this recommended setup) |
>
> Moonlight-protocol clients can also request the mirror per launch with `mirrorDesktop=1` on
> `/launch`. And mind the trap: `headless_mode = enabled` *without* `linux_use_cage_compositor`
> selects `host_virtual_display`, not a headless session.

Then start a game and read the active runtime, capture path, and encoder in Mission Control. See
[Runtime and streaming model](runtime.md) for what those values mean.

## Upgrade

```bash
wget --output-document=./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

Your configuration, pairing keys, and library stay in `~/.config/polaris` across upgrades.
Sign back in at **https://localhost:47990/#/login** with the existing web credentials.

## Uninstall

```bash
systemctl --user disable --now polaris
sudo pacman -R polaris
```

Package-owned udev rules and modules-load configuration are removed with the package. Host
configuration in `~/.config/polaris` is left in place.

## Debug package

Arch and SteamOS also publish a `polaris-debug` package. Install it when you need
`coredumpctl info polaris` to produce a real backtrace for a crash report.
