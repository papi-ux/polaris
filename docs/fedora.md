# Install on Fedora 44 and 45

Fedora is one of the two recommended Polaris package paths, with Arch Linux. Each release ships a
package for Fedora 44, `Polaris-fedora44-x86_64.rpm`, and from 1.5.0 one for Fedora 45,
`Polaris-fedora45-x86_64.rpm`, each built against its own Fedora release. Install the one for the
release your host runs, which `VERSION_ID` in `/etc/os-release` names. Earlier Fedora versions
should build from source.

Fedora 44 and 45 are supported equally, and Fedora 44 has the longer release history. The 1.5.0
streaming, benchmark and device testing ran on a Fedora 45 host. Before release, on a clean Fedora 45
Workstation, the [one-command install](repositories.md#one-command-install), pointed at a private
copy of the repository, installed the Fedora 45 package, started Polaris and served the web console,
and it refused unsigned and wrongly signed packages.

For an atomic Fedora derivative such as Bazzite, do not follow this page directly — layer the same
RPM with `rpm-ostree` using the [Bazzite guide](bazzite.md) instead.

## Install

The guided install and Mission Control updater consume the matching Fedora release
assets; the separately signed package repository is the `dnf upgrade` path described
in [Repositories](repositories.md). Release RPMs use the same Polaris signing key,
fingerprint `58017EDFFA9F803E07ED26F835F13F14FAAD15CC`. Before updating, the helper
verifies the candidate against the public key shipped by the installed package,
imports that key only if RPM does not already trust it, and leaves RPM's signature
enforcement enabled. An unsigned package or a different signing key stops the update.

On Fedora 44:

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
polaris
```

On Fedora 45:

```bash
wget --output-document=./Polaris-fedora45-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora45-x86_64.rpm &&
sudo dnf install ./Polaris-fedora45-x86_64.rpm &&
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

Neither the package nor `--setup-host` opens firewall ports. If the host runs firewalld or ufw and
a client on your network cannot find, pair with, or stream from it, allow the ports from your
local network only as shown in
[Allow the ports through a Linux firewall](moonlight.md#allow-the-ports-through-a-linux-firewall).

## What `--setup-host` does

The Fedora RPM ships the udev rules and modules-load configuration that make virtual input
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
[upgrade](#upgrade) Polaris first if yours is older. This is the Fedora 44 helper; on Fedora 45,
use `Polaris-kms-fedora45-x86_64.rpm` in both places:

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
pairing keys, and library stay in `~/.config/polaris`. These commands take the Fedora 44 files; on
Fedora 45, use `fedora45` in each file name instead.

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

**Before upgrading the host from Fedora 44 to 45** with the repository added, check that its
repository file follows the Fedora release. A file fetched or written before 1.5.0 names a single
Fedora 44 tree and does not follow the upgrade;
[Package repositories](repositories.md#fedora) explains how to replace it.

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

NVIDIA with NVENC is the most validated path. AMD with Mesa VA-API is supported and uses the same
Headless Stream flow, with the real capture path reported in Mission Control rather than assumed.
Intel encodes through VA-API as well, but no Intel GPU has been through release validation. See
[Compatibility](compatibility.md#gpu-and-encoding) for the current status of each combination.
