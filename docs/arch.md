# Install on Arch Linux and CachyOS

Arch Linux is one of the two recommended Polaris package paths, and the official
`Polaris-arch-x86_64.pkg.tar.zst` asset ships with every release. CachyOS and most pacman-compatible
Arch derivatives should start with this same package.

SteamOS is pacman-based but is **not** covered by this page: it has its own versioned package and a
read-only root, so follow the [SteamOS guide](steamos.md) instead. The two packages are not
interchangeable.

## Install

```bash
curl --fail --location --output ./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
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

It installs the udev rules and modules-load configuration that make virtual input work unless
the package already provides them, and reports anything it could not complete.

If you ran `--setup-host` on a version before v1.3.5, a copy of the udev rules may still sit in
`/etc/udev/rules.d/60-polaris.rules` and override the packaged file. Host setup keeps it and warns
rather than deleting it; see [Troubleshooting](troubleshooting.md) for the check and removal.

## Autostart

```bash
systemctl --user enable --now polaris
```

The application menu entry starts this same user service, so a desktop launch and autostart never
run two copies. Quitting from the tray stops it; the menu entry or `systemctl --user start polaris`
brings it back.

## Optional DRM/KMS capture

Only turn this on when you need DRM/KMS capture. Polaris works without it on the default
compositor and Headless Stream paths. It takes a second package, `polaris-kms`, from the same
release as Polaris.

With the [package repository](repositories.md#arch-and-cachyos) added:

```bash
sudo pacman -S polaris-kms &&
sudo -H polaris --setup-host --enable-kms
```

Without it, download the helper next to the Polaris package. The link takes the latest release, so
[upgrade](#upgrade) Polaris first if yours is older:

```bash
curl --fail --location --output ./Polaris-kms-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-kms-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-kms-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host --enable-kms
```

The first time, log out and back in, or reboot where lingering is on (headless boot turns it
on; `loginctl show-user $USER -p Linger` shows it). Only members of the `polaris-kms` group can
run the helper, `--enable-kms` adds you to it, and a session picks up its groups at login. Then run
`sudo -H polaris --setup-host --enable-kms` again and do what it prints.

The capability lives in the helper package rather than on the Polaris binary, so an update does
not take it away. `polaris-kms` depends on the exact version of `polaris` beside it, so the two
are upgraded and reinstalled together, as [Upgrade](#upgrade) shows. `--disable-kms` points the
service back at the ordinary binary.

## Arch derivatives

CachyOS is expected to work through this package path. CachyOS handheld edition boots into a Steam
Game Mode session; read [Handhelds and Game Mode](handhelds.md) before the first mode switch, or
Polaris goes offline with the desktop. If a derivative renames dependencies or ships
different runtime helpers, the package may refuse to install or Polaris may fail to find a helper at
launch. In that case use the local package or source build in
[Build from source](building.md), and please report the derivative-specific gap with your distro, GPU,
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
> `/launch`. An app whose Launch as names a fixed mode refuses that request with
> `app_launch_mode_pinned`. And mind the trap: `headless_mode = enabled` *without* `linux_use_cage_compositor`
> selects `host_virtual_display`, not a headless session.

Then start a game and read the active runtime, capture path, and encoder in Mission Control. See
[Runtime and streaming model](runtime.md) for what those values mean.

## Upgrade

With the [package repository](repositories.md) added once, an upgrade is one command:

```bash
sudo pacman -Syu &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

Without the repository, install the newer package the same way:

```bash
curl --fail --location --output ./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

If `polaris-kms` is installed, upgrade both in one transaction instead, because the helper
depends on the exact version of `polaris` beside it:

```bash
curl --fail --location --output ./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
curl --fail --location --output ./Polaris-kms-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-kms-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst ./Polaris-kms-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

**On a 1.4.13 beta**, reinstall. The 1.4.13 betas carry the release's own version, so
1.4.13-beta.3 and 1.4.13 are both `polaris 1.4.13-1`, and `sudo pacman -Syu` keeps the beta.
With the repository, `sudo pacman -Syu polaris polaris-kms` reinstalls both; with the files
downloaded as above, `sudo pacman -U` both files. Leave out `polaris-kms` if the helper is not
installed, then restart Polaris.

Your configuration, pairing keys, and library stay in `~/.config/polaris` across upgrades.
Sign back in at **https://localhost:47990/#/login** with the existing web credentials.

## Uninstall

```bash
systemctl --user disable --now polaris
sudo pacman -R polaris
```

If `polaris-kms` is installed, run `sudo -H polaris --setup-host --disable-kms` first and remove
both: `sudo pacman -R polaris polaris-kms`. `pacman -R polaris` alone refuses while the helper
depends on it.

Package-owned udev rules and modules-load configuration are removed with the package. Host
configuration in `~/.config/polaris` is left in place.

For a clean slate, or to remove what the package leaves behind, see
[Uninstall Polaris, or start over](uninstall.md).

## Debug package

Arch and SteamOS builds produce a separate `polaris-debug` package with detached
symbols. Release assembly checks that its version, architecture and ELF build ID
match the host and KMS helper before including it in the release.

Download `Polaris-debug-arch-x86_64.pkg.tar.zst` or
`Polaris-debug-steamos3.8-x86_64.pkg.tar.zst` from the same release as the installed
host, then install that file with `sudo pacman -U`. Older releases may not include
a debug asset; matching symbols then need to be built from that release's source.

Use `coredumpctl list` to find the Polaris crash, then `coredumpctl debug PID` with
that crash's PID to open the debugger with the installed symbols. `coredumpctl info`
shows the recorded report; installing symbols does not rewrite that earlier report.
