# Bazzite Install Guide

Bazzite is Fedora-based, but it is an immutable `rpm-ostree` system rather than a
normal DNF-managed Fedora install. The clean Polaris path for everyday Bazzite
users is to layer the matching Fedora RPM, reboot into the new deployment, run
the host setup once, then start Polaris from a writable `/usr/local` copy when
DRM/KMS capture is needed.

This is still a validation path until Bazzite Desktop Mode, Game Mode, NVIDIA,
AMD, and common Moonlight client flows have more real-hardware coverage. The
install should be simple, but keep the rollback notes handy.

> [!IMPORTANT]
> Use a Polaris release that includes an RPM matching your Bazzite Fedora base.
> Bazzite 44 should use `Polaris-fedora44-x86_64.rpm`. If the latest release
> does not include your Fedora version yet, wait for the next release or use a
> tester build intentionally.

## Validation Status

| Image | Session | Result |
|:------|:--------|:-------|
| `bazzite-nvidia-open:stable` `44.20260430` | KDE Plasma Wayland Desktop Mode | Polaris service and ports validated |
| `bazzite-nvidia-open:stable` `44.20260430` | Steam/Game Mode | Pending on a Game Mode-capable image |

The tested `bazzite-nvidia-open:stable` host is a Desktop image based on
Kinoite. It exposes only `/usr/share/wayland-sessions/plasma.desktop` to the
display manager. The host has `gamescope`, `gamescopectl`, `gamescopestream`,
`bazzite-steam`, and Steam installed, but it does not include a
`gamescope-session` package or a selectable Steam/Game Mode session.

That means this validation currently covers Desktop Mode only. Do not treat this
image as real Bazzite Game Mode coverage until Polaris is retested on an image
that can enter a gamescope Steam session from the host UI.

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
sudo polaris --setup-host
systemctl --user stop polaris 2>/dev/null || true
sudo install -D -m 0755 "$(readlink -f "$(command -v polaris)")" /usr/local/bin/polaris-kms
sudo setcap cap_sys_admin+ep /usr/local/bin/polaris-kms
getcap /usr/local/bin/polaris-kms
printf '[Service]\nExecStart=\nExecStart=/usr/local/bin/polaris-kms\n' \
  | systemctl --user edit --stdin --drop-in=10-bazzite-kms.conf polaris
systemctl --user daemon-reload
systemctl --user enable --now polaris
```

Open `https://127.0.0.1:47990/#/welcome`, create the web UI account, and pair
Moonlight, Nova, or another GameStream-compatible client. After credentials are
created, `https://127.0.0.1:47990` opens the normal console.

This Bazzite-specific copy is intentional. Bazzite's `/usr` deployment is backed
by composefs, so `setcap` can fail on the layered `/usr/bin/polaris-*` binary
even when run with `sudo`. `/usr/local` points into writable `/var/usrlocal`,
which can hold the capability-marked runtime copy used by the user service.

Re-run the `/usr/local/bin/polaris-kms` copy and `setcap` commands after each
Polaris package update so the service uses the newly installed binary.

If you want to test the EVDI virtual display path instead of the headless labwc
path, pre-create one EVDI device before starting Polaris:

```bash
systemctl --user stop polaris
sudo modprobe -r evdi
sudo modprobe evdi initial_device_count=1
cat /sys/devices/evdi/count
ls -l /dev/dri/card*
systemctl --user start polaris
```

The expected result is `cat /sys/devices/evdi/count` returning `1` and an extra
`/dev/dri/cardN` whose driver is `evdi`. To make that survive reboots:

```bash
echo evdi | sudo tee /etc/modules-load.d/evdi.conf
echo 'options evdi initial_device_count=1' | sudo tee /etc/modprobe.d/evdi-polaris.conf
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
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = disabled
```

This creates an isolated `labwc` runtime for the stream and does not target your
physical monitor. Do not manually export `WAYLAND_DISPLAY`; Polaris starts
`labwc` with its own Wayland socket and routes launched apps into that socket.
Do not add EVDI or dummy-plug display routing for this validation path.

If you want to test a physical dummy plug instead, leave headless/labwc disabled
and test it as a normal host display.

## Desktop Mode Baseline

On the tested NVIDIA Desktop image:

- `polaris.service` was active under the user manager.
- The service was enabled through `xdg-desktop-autostart.target`.
- A local drop-in launched `/usr/local/bin/polaris-kms`.
- `/usr/local/bin/polaris-kms` had `cap_sys_admin=ep`.
- Polaris listened on `47984`, `47989`, `47990`, and `48010`.
- The active graphical session was KDE Plasma Wayland through
  `plasmalogin-autologin`.

Baseline checks:

```bash
systemctl --user status polaris --no-pager -l
systemctl --user cat polaris
getcap /usr/local/bin/polaris-kms
grep -E 'headless_mode|linux_use_cage_compositor|linux_prefer_gpu_native_capture' \
  ~/.config/polaris/polaris.conf
loginctl list-sessions
loginctl show-session "$XDG_SESSION_ID" -p Type -p Desktop -p Class -p State
ss -ltnup | grep -E '47984|47989|47990|48010'
```

The Desktop Mode logs still reported the physical display:

```text
Name: DP-3
Found monitor: Samsung Electric Company Odyssey G95NC
Resolution: 7680x2160
```

This is expected for the Desktop image before a client launches a headless labwc
stream.

## Game Mode Validation

Game Mode remains pending for `bazzite-nvidia-open:stable` Desktop images. A
valid Game Mode test host must expose a real Steam/Game Mode session, usually
through a gamescope session package and display-manager entry.

After entering Game Mode, verify Polaris before connecting a client:

```bash
systemctl --user is-active polaris
systemctl --user status polaris --no-pager -l
ss -ltnup | grep -E '47984|47989|47990|48010' || true
journalctl --user -u polaris --since "5 minutes ago" --no-pager
```

Then connect with Nova at `1920x1080x60`, followed by Moonlight or a Retroid
profile such as `1280x720x60`. For each connection, collect:

```bash
journalctl --user -u polaris --since "3 minutes ago" --no-pager \
  | grep -Ei "New streaming|stream_active|CLIENT|RTSP|session_event|labwc|HEADLESS|Steam|failed|Warning|Error"
```

Success markers include:

```text
labwc: Starting in headless mode
labwc: Ready
Selected monitor [Headless output 1] for streaming
Encoder cache saved: nvenc
New streaming session started
session_event: stream_active
CLIENT CONNECTED
```

Steam should report the client stream resolution, not the physical `7680x2160`
`DP-3` desktop.

If Polaris is inactive after entering Game Mode, treat it as a service or
autostart packaging issue first:

```bash
systemctl --user restart polaris
systemctl --user status polaris --no-pager -l
```

If Polaris is active but clients cannot discover or connect, verify listener
ports and mDNS/Avahi from the Game Mode session before changing encoder code.

If clients connect but the stream is black, check whether logs mention
`HEADLESS-1` or the physical display. `DP-3` means app routing escaped the
headless labwc runtime. `HEADLESS-1` means routing worked and capture or encoder
warnings should be inspected next.

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

`labwc: No new Wayland socket appeared within 10s` means the isolated `labwc`
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
capture to work` means the KMS capability was not applied to the binary that the
user service is running. On Bazzite, copy the current packaged binary to
`/usr/local/bin/polaris-kms`, apply `setcap` there, and make sure the
`~/.config/systemd/user/polaris.service.d/10-bazzite-kms.conf` override points
`ExecStart` at that file.

`Virtual display: failed to open EVDI device` usually means the EVDI kernel
module is loaded without a pre-created DRM card. Load EVDI with
`initial_device_count=1` and confirm that `/sys/devices/evdi/count` returns `1`
before starting Polaris.

`Virtual display: could not determine EVDI output name, using fallback
[VIRTUAL-1]` means Polaris could not map the opened EVDI card to its DRM
connector. On Bazzite with a pre-created device, the connector should look like
`card1-DVI-I-1` under `/sys/class/drm`.

`wlr: Using RAM capture path because this build does not include a GPU-native
uploader for the selected encoder` and `capture will incur an extra CPU-side
copy/conversion path` are expected with the current headless labwc runtime. They
are performance notes, not startup failures. Confirm the stream is healthy by
looking for `session_event: stream_active`, `CLIENT CONNECTED`, `Selected monitor
[Headless output 1]`, and `Found H.264 encoder: h264_nvenc [nvenc]`.
The `capture_transport=shm frame_residency=cpu frame_format=bgra8` warning is
acceptable when the stream works.

`display_preview: Failed to capture cage screenshot` affects the web dashboard
preview path. It does not mean the Moonlight/Nova stream failed if the client is
already connected and receiving frames.

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
- output of `getcap /usr/local/bin/polaris-kms`
- output of `systemctl --user cat polaris`
- whether `sudo polaris --setup-host` completed successfully
- whether `systemctl --user status polaris` is running
- whether the web UI opens at `https://127.0.0.1:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- whether headless mode and virtual display behavior worked after a reboot

## Current Status

Fedora 42, Fedora 43, and Fedora 44 RPMs are release-tested in CI. Bazzite uses
the matching Fedora RPM through `rpm-ostree`; `bazzite-nvidia-open:stable`
`44.20260430` has Desktop Mode service and port validation only. Game Mode still
needs validation on a Bazzite image that exposes a real gamescope Steam session.
