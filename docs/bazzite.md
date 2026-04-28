# Bazzite Install Guide

Bazzite is Fedora-based, but it is an immutable rpm-ostree system rather than a normal
DNF-managed Fedora install. Polaris can use the matching Fedora RPM as a layered system package.

This path is experimental until it has been validated on real Bazzite desktop and gamemode
hardware. Use it when you want to test Polaris as a Bazzite host and are comfortable with
rpm-ostree package layering.

## Install

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo rpm-ostree install "./Polaris-fedora${fedora_version}-x86_64.rpm"
systemctl reboot
```

After reboot:

```bash
sudo polaris --setup-host
polaris
```

Open `https://localhost:47990`, create the web UI password, and pair Moonlight, Nova, or another
GameStream-compatible client.

## Optional setup

Enable the user service if you want Polaris to start in the background:

```bash
systemctl --user enable --now polaris
```

Only enable DRM/KMS capture if you specifically need it:

```bash
sudo polaris --setup-host --enable-kms
```

The default compositor and portal paths do not require granting KMS capability.

## Update

Download the newer Fedora RPM from the latest release and layer it again:

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo rpm-ostree uninstall polaris
sudo rpm-ostree install "./Polaris-fedora${fedora_version}-x86_64.rpm"
systemctl reboot
```

## Uninstall

```bash
sudo rpm-ostree uninstall polaris
systemctl reboot
```

If you enabled the user service, disable it before uninstalling:

```bash
systemctl --user disable --now polaris
```

## Validation Checklist

Please include these details when reporting Bazzite issues:

- Bazzite image name and version from `rpm-ostree status`
- desktop mode or gamemode
- GPU model and driver stack
- whether `sudo polaris --setup-host` completed successfully
- whether the web UI opens at `https://localhost:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- whether headless mode and virtual display behavior worked after a reboot

## Current Status

Fedora 42 and Fedora 43 RPMs are release-tested in CI. Bazzite uses those same RPM assets, but
the immutable install flow and Steam Deck-oriented runtime behavior need separate validation before
this path is marked recommended.
