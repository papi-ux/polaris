# SteamOS 3.8 Install Guide

Polaris provides a dedicated x86_64 package for SteamOS 3.8:

`Polaris-steamos3.8-x86_64.pkg.tar.zst`

This package is built against Valve's versioned SteamOS 3.8 package repositories. It is not built against rolling Arch Linux, and the rolling Arch package is not a supported substitute on SteamOS.

## Validation Status

Initial support covers package installation and Polaris startup in SteamOS Desktop Mode only. It does not certify physical Steam Deck gameplay, Game Mode, OLED 90 Hz behavior, suspend and resume, or persistence across SteamOS updates. Those claims require separate hardware evidence.

Continuous integration builds and validates this package inside a clean SteamOS 3.8 root bootstrapped from Valve's repositories. A clean root has none of the packages a shipped SteamOS image already carries, so that gate proves the package builds and its libraries resolve. It does not prove the install transaction is conflict-free on a device, and it did not catch the `libdisplay-info` conflict described under Stream Paths on SteamOS.

SteamOS uses a read-only root by default. Polaris installation and `sudo -H polaris --setup-host` must run while the root is writable. Read-only mode must be restored before the user service starts.

## Install

Open a terminal in Desktop Mode and run:

```bash
wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-steamos3.8-x86_64.pkg.tar.zst &&
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman-key --init || exit $?
sudo pacman-key --populate || exit $?
sudo pacman -Sy || exit $?
sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst || exit $?
sudo -H polaris --setup-host || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
) &&
systemctl --user enable --now polaris
```

The `EXIT` trap attempts to restore read-only mode if disabling the root, package installation, host setup, or explicit restoration fails. The user service starts only after the package and setup steps succeed and read-only mode is restored.

SteamOS ships without an initialized pacman keyring, so the first `pacman` transaction that has to download anything fails signature verification. `pacman-key --init` creates `/etc/pacman.d/gnupg`, and `pacman-key --populate` imports every keyring already on the image, which on SteamOS means the Arch and Holo keyrings. Both steps are one-time and idempotent, and both write to the root filesystem, so they run after `steamos-readonly disable`.

`pacman -Sy` refreshes the sync databases so `pacman -U` can resolve the handful of Polaris dependencies a stock SteamOS image does not carry. Do not substitute `-Syu`. A full upgrade replaces Valve's pinned packages with rolling Arch versions and is not supported on SteamOS.

Installing v1.3.9 or earlier additionally needs `--assume-installed labwc=0.9.0` on the `pacman -U` step. Those packages declared a `labwc` dependency that pulls in a library downgrade which breaks Desktop Mode. See Troubleshooting below.

Open `https://localhost:47990/#/welcome`, create the web UI account, and pair Nova, Moonlight, or another GameStream-compatible client. After credentials are created, `https://localhost:47990` opens the normal console.

## Stream Paths on SteamOS

Polaris defaults to Mirror Desktop, which streams the visible Desktop Mode session through the portal and needs nothing beyond this package. Gamescope Stream is also available, because SteamOS ships gamescope.

The Private Stream paths need `labwc` on `PATH`, and labwc cannot be installed on SteamOS 3.8 without breaking the system. labwc 0.9.0 in `extra-3.8.1x` depends on wlroots0.19, which requires `libdisplay-info.so.2` from libdisplay-info 0.2.0. SteamOS installs libdisplay-info 0.3.0 from `holo-3.8.1x`, which provides `libdisplay-info.so.3`, and KWin, Mesa, and Vulkan link against that. Installing labwc therefore downgrades a library the desktop depends on and breaks Desktop Mode.

Polaris does not depend on labwc for this reason, and links nothing from it. Private Stream cards are greyed out on SteamOS and name the missing `labwc` tool; a pre-existing Private Stream configuration also fails closed at launch. Use Mirror Desktop or Gamescope Stream instead.

## Update

Download the new `Polaris-steamos3.8-x86_64.pkg.tar.zst` artifact and repeat the failure-safe install command. The package manager replaces the prior Polaris files, host setup refreshes required integration, and read-only mode is restored before service startup.

A SteamOS operating-system update may remove packages layered into the mutable root. If Polaris disappears after an OS update, return to Desktop Mode and reinstall the current SteamOS 3.8 artifact. Do not substitute the rolling Arch package.

## Remove and Roll Back

Stop the service, remove the package while the root is writable, and restore read-only mode even if removal fails:

```bash
systemctl --user disable --now polaris
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman -Rns polaris || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
)
```

Package removal does not automatically delete user configuration under `~/.config/polaris`. Keep that directory if you plan to reinstall, or remove it separately only after backing up any settings you need.

## Troubleshooting

### `keyring is not writable` or `required key missing from keyring`

```
warning: Public keyring not found; have you run 'pacman-key --init'?
downloading required keys...
error: keyring is not writable
error: required key missing from keyring
error: failed to commit transaction (unexpected error)
```

The pacman keyring was never initialized, or the root filesystem is still read-only. Run `sudo steamos-readonly disable`, then `sudo pacman-key --init` and `sudo pacman-key --populate`, then retry. Nothing was installed when this error appears, so it is safe to rerun the whole install block.

### pacman offers to downgrade `libdisplay-info`

Answer no and let the transaction abort. A downgrade from 0.3.0 to 0.2.0 breaks KWin, Mesa, and Vulkan, which takes Desktop Mode with it. It means labwc or another wlroots0.19 consumer entered the transaction.

Polaris no longer declares that dependency. If you are installing v1.3.9 or earlier, add `--assume-installed labwc=0.9.0` to the package step so pacman treats it as already satisfied:

```bash
sudo pacman -U --assume-installed labwc=0.9.0 ./Polaris-steamos3.8-x86_64.pkg.tar.zst
```

Polaris does not use labwc on SteamOS either way, so nothing is lost by skipping it.

## Reporting SteamOS Results

Include the SteamOS version, device model, Desktop Mode or Game Mode, GPU, client, package filename, and relevant Polaris logs. Clearly separate package and startup success from gameplay, display-rate, suspend, and OS-update persistence results.
