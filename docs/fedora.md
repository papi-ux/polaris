# Install on Fedora 44

Fedora 44 is one of the two recommended Polaris package paths, and the official
`Polaris-fedora44-x86_64.rpm` asset ships with every release. Fedora 44 is the only Fedora release
with a published package; earlier Fedora versions should build from source.

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

Open **https://localhost:47990/#/welcome**, create your web UI account, and pair a client.

## What `--setup-host` does

It installs the udev rules and modules-load configuration that make virtual input work, and reports
anything it could not complete. It does not silently take privileges you did not ask for.

> [!WARNING]
> Only add `--enable-kms` when you actually need DRM/KMS capture:
> `sudo -H polaris --setup-host --enable-kms` grants `cap_sys_admin`. Polaris works without it on the
> default compositor and Headless Stream paths.

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

Install the newer RPM the same way. `dnf` replaces the package in place, and your configuration,
pairing keys, and library stay in `~/.config/polaris`.

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
systemctl --user restart polaris
```

Re-running `--setup-host` after an upgrade is how packaged udev rules and module configuration get
refreshed.

## Uninstall

```bash
systemctl --user disable --now polaris
sudo dnf remove polaris
```

Package-owned udev rules and modules-load configuration are removed with the package. Your host
configuration in `~/.config/polaris` is left alone; delete it yourself if you want a clean slate.

## GPU notes

NVIDIA with NVENC is the most validated path. AMD and Intel Mesa VAAPI are supported and use the same
Headless Stream flow, with the real capture path reported in Mission Control rather than assumed. See
[Compatibility](compatibility.md) for the current status of each combination.
