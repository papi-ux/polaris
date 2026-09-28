# Install on Fedora 44

Fedora 44 is one of the two recommended Polaris package paths, and the official
`Polaris-fedora44-x86_64.rpm` asset ships with every release. Fedora 44 is the only Fedora release
with a published package; earlier Fedora versions should build from source.

On Fedora 45 the 1.4.13 RPM has been installed but not streamed from. In a clean Fedora 45
prerelease container, dnf installed it with every library it links, and `polaris --version` and
`polaris --setup-host` both exited 0. Nothing more was checked there, and the
[one-command install](repositories.md#one-command-install) refuses Fedora 45.

For an atomic Fedora derivative such as Bazzite, do not follow this page directly — layer the same
RPM with `rpm-ostree` using the [Bazzite guide](bazzite.md) instead.

## Install

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
polaris
```

The package installs the host binary, the web console assets, desktop metadata, and the user service
file. Host integration stays explicit: `--setup-host` is a separate step you run yourself.

**Fresh install:** open **https://localhost:47990/#/welcome**, create your web UI account, and pair
a client.

**Upgrade or reinstall:** open **https://localhost:47990/#/login** and use the existing account.
Fedora package operations preserve credentials, pairing keys, settings, and the library under
`~/.config/polaris`; removing the RPM does not reset the web account. If needed, follow the
[credential reset](troubleshooting.md#web-ui-credentials) instead of returning to Welcome.

## What `--setup-host` does

The Fedora 44 RPM ships the udev rules and modules-load configuration that make virtual input
work. `--setup-host` installs them itself only when a package did not, and reports anything it
could not complete. It does not silently take privileges you did not ask for.

If you ran `--setup-host` on a version before v1.3.5, a copy of the udev rules may still sit in
`/etc/udev/rules.d/60-polaris.rules` and override the packaged file. Host setup keeps it and warns
rather than deleting something it cannot prove is disposable; see
[Troubleshooting](troubleshooting.md) for the check and the removal.

## Autostart

```bash
systemctl --user enable --now polaris
```

Polaris runs as a user service, so it starts with your graphical session and has access to it. Check
status with `systemctl --user status polaris`.

The application menu entry starts this same service, so opening Polaris from the desktop and
autostart never run two copies. Quitting from the tray stops it; the menu entry, the next login, or
`systemctl --user start polaris` brings it back. For a host that boots with no desktop at all, see
[Headless Boot](bazzite.md#headless-boot-and-deck-images).

## Optional DRM/KMS capture

Only turn this on when you need DRM/KMS capture. Polaris works without it on the default
compositor and Headless Stream paths. It takes a second package, `polaris-kms`, from the same
release as Polaris.

With the [package repository](repositories.md) added:

```bash
sudo dnf install polaris-kms &&
sudo -H polaris --setup-host --enable-kms
```

Without it, download the helper next to the Polaris RPM. The link takes the latest release, so
[upgrade](#upgrade) Polaris first if yours is older:

```bash
wget --output-document=./Polaris-kms-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-kms-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-kms-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host --enable-kms
```

The first time, log out and back in, or reboot where lingering is on (headless boot turns it
on; `loginctl show-user $USER -p Linger` shows it). Only members of the `polaris-kms` group can
run the helper, `--enable-kms` adds you to it, and a session picks up its groups at login. Then run
`sudo -H polaris --setup-host --enable-kms` again and do what it prints.

The capability lives in the helper package rather than on the Polaris binary, so an update does
not take it away. `polaris-kms` requires the exact version of `polaris` beside it, so the two are
upgraded and reinstalled together, as [Upgrade](#upgrade) shows. `--disable-kms` points the
service back at the ordinary binary.

## Verify the stream path

Confirm the recommended Linux configuration in the first-run wizard or
`~/.config/polaris/polaris.conf`:

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
```

Then start a game and read the active runtime, capture path, and encoder in Mission Control. See
[Runtime and streaming model](runtime.md) for what each value means, and
[Configuration](configuration.md) for the full setting reference.

## Upgrade

With the [package repository](repositories.md) added once, an upgrade is one command:

```bash
sudo dnf upgrade polaris &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

If `polaris-kms` is installed, name it too: `sudo dnf upgrade polaris polaris-kms`.

Without the repository, install the newer RPM the same way. `dnf` replaces the package in place, and your configuration,
pairing keys, and library stay in `~/.config/polaris`.

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

If `polaris-kms` is installed, upgrade both in one transaction instead, because the helper
requires the exact version of `polaris` beside it:

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
wget --output-document=./Polaris-kms-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-kms-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm ./Polaris-kms-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

**On a 1.4.13 beta**, reinstall rather than upgrade. The 1.4.13 betas carry the release's own
version, so 1.4.13-beta.3 and 1.4.13 are both `polaris-1.4.13-1`, and the commands above answer
`Nothing to do` and keep the beta. With the repository:

```bash
sudo dnf reinstall polaris polaris-kms
```

With the files downloaded as above, it is
`sudo dnf reinstall ./Polaris-fedora44-x86_64.rpm ./Polaris-kms-fedora44-x86_64.rpm`. Leave out
`polaris-kms` or its file if the helper is not installed, then restart Polaris.

Re-running `--setup-host` after an upgrade is how packaged udev rules and module configuration get
refreshed.

After the restart, return to **https://localhost:47990/#/login** with the existing web credentials.

## Uninstall

```bash
systemctl --user disable --now polaris
sudo dnf remove polaris
```

If `polaris-kms` is installed, run `sudo -H polaris --setup-host --disable-kms` first, while
Polaris is still installed. `dnf remove polaris` takes `polaris-kms` with it.

Package-owned udev rules and modules-load configuration are removed with the package. Your host
configuration in `~/.config/polaris` is left alone; delete it yourself if you want a clean slate.

For a clean slate, or to remove what the package leaves behind, see
[Uninstall Polaris, or start over](uninstall.md).

## GPU notes

NVIDIA with NVENC is the most validated path. AMD and Intel Mesa VAAPI are supported and use the same
Headless Stream flow, with the real capture path reported in Mission Control rather than assumed. See
[Compatibility](compatibility.md) for the current status of each combination.
