# Bazzite Install Guide

Bazzite is Fedora-based, but it is an immutable `rpm-ostree` system rather than a
normal DNF-managed Fedora install. The clean Polaris path for everyday Bazzite
users is to layer the matching Fedora RPM, reboot into the new deployment, run
the host setup once with DRM/KMS capture enabled, then start the Polaris user
service.

This is still a validation path until Bazzite Desktop Mode, Game Mode, NVIDIA,
AMD, and common Moonlight client flows have more real-hardware coverage. The
install should be simple, but keep the rollback notes handy.

> [!IMPORTANT]
> Use a Polaris release that includes an RPM matching your Bazzite Fedora base.
> Bazzite 44 should use `Polaris-fedora44-x86_64.rpm`. If the latest release
> does not include your Fedora version yet, wait for the next release or use a
> tester build intentionally.

## Install

If you already enabled Sunshine on Bazzite, stop it first. Sunshine and Polaris
both use the default GameStream ports, so only one host should be running.

```bash
systemctl --user disable --now homebrew.sunshine.service 2>/dev/null || true
systemctl --user disable --now app-dev.lizardbyte.app.Sunshine.service 2>/dev/null || true
```

Install Polaris from the matching Fedora release RPM:

```bash
fedora_version="$(rpm -E %fedora)"
rpm_name="Polaris-fedora${fedora_version}-x86_64.rpm"
wget "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}"
sudo rpm-ostree install -r "./${rpm_name}"
```

After the reboot:

```bash
sudo polaris --setup-host --enable-kms
systemctl --user enable --now polaris
```

Open `https://127.0.0.1:47990/#/welcome`, create the web UI account, and pair
Moonlight, Nova, or another GameStream-compatible client. After credentials are
created, `https://127.0.0.1:47990` opens the normal console.

The `--enable-kms` flag applies `cap_sys_admin` to the installed Polaris binary.
That is required for the current Bazzite DRM/KMS capture path. If you already
ran `sudo polaris --setup-host` without that flag, run the command above and
restart the user service:

```bash
systemctl --user restart polaris
```

## Why rpm-ostree Layering

Polaris needs host-level integration: the binary, web assets, desktop metadata,
the user service, udev rules for virtual input, and compositor helpers such as
`labwc`, `wlr-randr`, Xwayland, and `xdpyinfo`. On Bazzite, layering the RPM is
cleaner than running Polaris from a toolbox, distrobox, or unpacked archive
because the package manager can install those host dependencies into the booted
deployment.

The Polaris RPM declares the headless runtime dependencies, so the install
command should not need separate `labwc` or `wlr-randr` arguments.

## First Validation Path

Start in Desktop Mode first. Game Mode and Deck-style gamescope sessions can hide
display, portal, and environment details that are easier to debug from Desktop
Mode.

Use the headless labwc path for the first stream:

```ini
headless_mode = enabled
linux_use_cage_compositor = true
linux_prefer_gpu_native_capture = enabled
```

This creates an isolated `labwc` runtime for the stream and does not target your
physical monitor. Do not manually export `WAYLAND_DISPLAY`; Polaris starts
`labwc` with its own Wayland socket and routes launched apps into that socket.

If you want to test a physical dummy plug instead, leave headless/labwc disabled
and test it as a normal host display.

## Update

Layer the newer matching Fedora RPM and reboot. `rpm-ostree` will stage the
newer local RPM over the existing layered Polaris package:

```bash
fedora_version="$(rpm -E %fedora)"
rpm_name="Polaris-fedora${fedora_version}-x86_64.rpm"
wget -O "${rpm_name}" "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}"
sudo rpm-ostree install -r "./${rpm_name}"
```

## Roll Back

Bazzite keeps previous deployments. If the new deployment does not work, choose
the previous deployment from the boot menu or run:

```bash
sudo rpm-ostree rollback -r
```

## Uninstall

Disable the user service before removing the layer:

```bash
systemctl --user disable --now polaris
sudo rpm-ostree uninstall -r polaris
```

## Known Bazzite Log Messages

`labwc: No new Wayland socket appeared within 5s` means the isolated `labwc`
runtime failed to start or exited before creating its Wayland socket. Confirm the
matching Fedora RPM was installed, rebooted into the new deployment, and retry
from Desktop Mode first.

`Environment variable WAYLAND_DISPLAY has not been defined` usually points to a
windowed Wayland runtime being launched without a parent Wayland session. It is
expected after a failed manual environment experiment, but it is not the intended
headless flow.

`Couldn't scale frame ... src_fmt=bgr0 ... src_stride=0` means Polaris received a
CPU BGR0 frame without a valid row pitch. Use a release newer than `v1.0.4`, where
the headless CPU fallback path was fixed.

`Failed to gain CAP_SYS_ADMIN` or `You must run [sudo setcap ...] for KMS display
capture to work` means the KMS capability was not applied. Run
`sudo polaris --setup-host --enable-kms`, then restart the user service.

If local Plasma receives remote mouse or keyboard input while using headless
labwc, treat that as an input-isolation bug and include the validation details
below.

## Validation Checklist

Please include these details when reporting Bazzite issues:

- Bazzite image name and version from `rpm-ostree status`
- Desktop Mode or Game Mode
- GPU model and driver stack
- Polaris RPM asset used, such as `Polaris-fedora44-x86_64.rpm`
- output of `command -v polaris labwc wlr-randr`
- output of `getcap "$(readlink -f "$(command -v polaris)")"`
- whether `sudo polaris --setup-host --enable-kms` completed successfully
- whether `systemctl --user status polaris` is running
- whether the web UI opens at `https://127.0.0.1:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- whether headless mode and virtual display behavior worked after a reboot

## Current Status

Fedora 42, Fedora 43, and Fedora 44 RPMs are release-tested in CI. Bazzite uses
the matching Fedora RPM through `rpm-ostree`; Bazzite-specific Desktop Mode and
Game Mode runtime behavior still needs broader real-hardware validation before
this path is marked recommended.
