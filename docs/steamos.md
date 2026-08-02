# SteamOS 3.8 Install Guide

Polaris provides a dedicated x86_64 package for SteamOS 3.8:

`Polaris-steamos3.8-x86_64.pkg.tar.zst`

This package is built against Valve's versioned SteamOS 3.8 package repositories. It is not built against rolling Arch Linux, and the rolling Arch package is not a supported substitute on SteamOS.

## Validation Status

Initial support covers package installation and Polaris startup in SteamOS Desktop Mode only. It does not certify physical Steam Deck gameplay, Game Mode, OLED 90 Hz behavior, suspend and resume, or persistence across SteamOS updates. Those claims require separate hardware evidence.

SteamOS uses a read-only root by default. Polaris installation and `sudo -H polaris --setup-host` must run while the root is writable. Read-only mode must be restored before the user service starts.

## Install

Open a terminal in Desktop Mode and run:

```bash
wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-steamos3.8-x86_64.pkg.tar.zst &&
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst || exit $?
sudo -H polaris --setup-host || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
) &&
systemctl --user enable --now polaris
```

The `EXIT` trap attempts to restore read-only mode if disabling the root, package installation, host setup, or explicit restoration fails. The user service starts only after the package and setup steps succeed and read-only mode is restored.

Open `https://localhost:47990/#/welcome`, create the web UI account, and pair Nova, Moonlight, or another GameStream-compatible client. After credentials are created, `https://localhost:47990` opens the normal console.

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

## Reporting SteamOS Results

Include the SteamOS version, device model, Desktop Mode or Game Mode, GPU, client, package filename, and relevant Polaris logs. Clearly separate package and startup success from gameplay, display-rate, suspend, and OS-update persistence results.
