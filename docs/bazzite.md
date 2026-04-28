# Bazzite Install Guide

Bazzite is Fedora-based, but it is an immutable rpm-ostree system rather than a normal
DNF-managed Fedora install. Polaris can use the matching Fedora RPM as a layered system package.

This path is experimental until it has been validated on real Bazzite Desktop Mode and Game Mode
hardware. Use it when you want to test Polaris as a Bazzite host and are comfortable with
rpm-ostree package layering.

## Install

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo rpm-ostree install "./Polaris-fedora${fedora_version}-x86_64.rpm" labwc wlr-randr
systemctl reboot
```

After reboot:

```bash
sudo polaris --setup-host
polaris
```

Open `https://localhost:47990`, create the web UI password, and pair Moonlight, Nova, or another
GameStream-compatible client.

## First validation path

Start in Desktop Mode first. Game Mode and Deck-style gamescope sessions are still experimental as
host environments, and they can hide display, portal, and environment details that are easier to
debug from Desktop Mode.

The recommended Bazzite test path is:

```ini
headless_mode = enabled
linux_use_cage_compositor = true
linux_prefer_gpu_native_capture = enabled
```

This path creates an isolated `labwc` runtime for the stream. It does not target a physical HDMI
dummy plug. If you want to test a physical dummy plug instead, leave headless/labwc disabled and
test it as a normal host display.

Do not manually export `WAYLAND_DISPLAY` when testing headless mode. Polaris starts `labwc` with
its own Wayland socket and routes launched apps into that socket.

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

## Known Bazzite log messages

`labwc: No new Wayland socket appeared within 5s` means the isolated `labwc` runtime failed to
start or exited before creating its Wayland socket. Confirm `labwc` and `wlr-randr` are layered,
rebooted into the new deployment, and retry from Desktop Mode first.

`Environment variable WAYLAND_DISPLAY has not been defined` usually points to a windowed Wayland
runtime being launched without a parent Wayland session. It is expected to appear after a failed
manual environment experiment, but it is not the intended headless flow.

`Couldn't scale frame ... src_fmt=bgr0 ... src_stride=0` means Polaris received a CPU BGR0 frame
without a valid row pitch. This is a Polaris capture/conversion bug, not a user configuration
mistake.

## Update

Download the newer Fedora RPM from the latest release and layer it again:

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo rpm-ostree uninstall polaris
sudo rpm-ostree install "./Polaris-fedora${fedora_version}-x86_64.rpm" labwc wlr-randr
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
- Desktop Mode or Game Mode
- GPU model and driver stack
- output of `command -v labwc wlr-randr`
- whether `sudo polaris --setup-host` completed successfully
- whether the web UI opens at `https://localhost:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- whether headless mode and virtual display behavior worked after a reboot

## Current Status

Fedora 42 and Fedora 43 RPMs are release-tested in CI. Bazzite uses those same RPM assets, but
the immutable install flow and Steam Deck-oriented runtime behavior need separate validation before
this path is marked recommended.
