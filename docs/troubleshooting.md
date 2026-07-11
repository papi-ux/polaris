# Troubleshooting

This page covers the fastest recovery steps for the public Polaris host flow. Use the web UI's
Troubleshooting screen first when it is still reachable.

## Web UI credentials

Reset the web UI username and password:

```bash
polaris --creds new-username new-password
```

Run this as the same user account that runs Polaris. Do not use `sudo` unless Polaris itself runs
as root, because that can update a different config directory.

Restart Polaris after changing credentials. A running Polaris process keeps the previous credentials
in memory until restart.

For packaged user-service installs:

```bash
systemctl --user restart polaris
```

For foreground sessions, stop Polaris and start it again.

## Web UI does not load

1. Confirm Polaris is running.
2. Check that you are opening `https://localhost:47990` or `https://localhost:<port + 1>` if you changed `port`.
3. Accept the local HTTPS certificate warning in the browser.
4. Check your local firewall rules if the UI is unreachable from another device on the LAN.

## Polaris and Sunshine on the same host

Polaris keeps its config under `~/.config/polaris`, so installing it should not remove or overwrite
an existing Sunshine setup. Do not run both hosts on the default GameStream/Moonlight ports at the
same time unless you intentionally change one host's `port` value.

For a quick Polaris test, stop Sunshine first:

```bash
systemctl --user stop sunshine
systemctl --user enable --now polaris
```

If Sunshine runs as a system service on your distro, use the matching system-service command instead.
To switch back, stop Polaris and start Sunshine again.

## Headless session does not start cleanly

Confirm these settings first:

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
```

That is the intended Headless Stream path for NVIDIA/NVENC and AMD/Mesa VAAPI hosts that can keep
frames GPU-resident. It avoids touching your normal desktop layout and reduces display mode churn
after a session ends. If the stream is stable but logs report SHM/RAM capture, treat that as a
performance/capability fallback first, not a startup failure. If enabling GPU-native capture blocks
launch on a specific driver/compositor stack, temporarily set it to `disabled` and include the
capture decision fields in the bug report.

The built-in Desktop entry does not launch your existing KDE, GNOME, or wlroots desktop inside this
private compositor. If the client connects but shows an empty or black desktop while app entries work,
that usually means the headless runtime is alive but nothing visible has been launched in it. Use
Desktop Display mode when you want to stream the already-running host desktop session.

## Portal/PipeWire desktop capture falls back to system memory

For Wayland Desktop Display capture, these messages show that the Portal session and PipeWire stream negotiated successfully:

```text
portal: PipeWire state: ... -> streaming
portal: PipeWire format negotiated: <width>x<height> format=<format>
portal: capture_transport=<shm|dmabuf> frame_residency=<cpu|gpu> frame_format=bgra8
```

The `capture_transport` line is the authoritative result. A healthy same-GPU path also logs `portal: PipeWire DMA-BUF format negotiated` and reports `capture_transport=dmabuf frame_residency=gpu` without a CPU-copy warning. If the log instead says DMA-BUF was disabled because the capture render node is missing, the encoder adapter is not an explicit canonical render node, the render nodes differ, or EGL exposes no importable modifier, Polaris should negotiate `MemPtr`, report `capture_transport=shm frame_residency=cpu`, and continue through system memory.

For deterministic GPU selection, configure the encoder adapter as an explicit `/dev/dri/renderD*` path. Polaris compares the Portal capture node and encoder node before offering DMA-BUF. CUDA then maps that node to a CUDA ordinal through PCI sysfs identity; an unmappable explicit node fails rather than silently using device 0. On mixed-GPU systems, do not force DMA-BUF across different render nodes—use the system-memory fallback or select matching capture and encode devices.

A log saying `PipeWire capture transport or dimensions changed; reinitializing encoder` is expected when the compositor renegotiates the frame contract. Repeated reinitialization without a stable `format negotiated` line is not expected; include the Portal/PipeWire state, negotiated format, render-node messages, and configured adapter path in the bug report. Do not include Portal tokens or credentials.

### Reset persistent Portal selection

The first real Portal capture may open the system picker. Polaris requests persistent selection and stores any granted restore token in `portal_restore_token.txt` beside the active `polaris.conf`. Do not post or copy the token value into a support report.

To force the picker to appear again, stop Polaris, delete only `portal_restore_token.txt`, and start Polaris again. This resets Polaris' saved auto-selection but does not necessarily revoke the desktop Portal's permission. To revoke permission completely, also use the desktop environment's Privacy or Screen Sharing controls when it exposes them. The exact control varies between KDE, GNOME, and other Portal implementations.

## Steam Big Picture black screen or tiny window

Clear Steam's HTML cache:

```bash
rm -rf ~/.local/share/Steam/config/htmlcache/
```

If you are using MangoHud, disable it for Steam Big Picture and Steam/Proton launches first.
Those paths are the most sensitive to early helper-process crashes.

## Input does not work

Reload udev rules or reboot after install. If the problem persists, ensure your user has access to
the input stack expected by your distro setup.

Example:

```bash
sudo usermod -aG input "$USER"
```

Then sign out and back in.

## Local desktop audio is captured during a headless stream

In headless `labwc` sessions, Polaris routes launched apps to the Polaris virtual sink and captures
that sink directly instead of changing the user's global default audio output. The healthy log path
looks like:

```text
Linux audio isolation: routing launched apps to virtual sink [sink-sunshine-stereo] without changing the user's default sink
Linux audio isolation: capturing virtual sink without changing the user's default sink
```

If local Plasma/GNOME audio is still mixed into the stream, include the audio section of the logs
and whether the client requested host audio. Host-audio mode intentionally captures the host sink,
so same-user local apps can still be part of that stream.

## Thread priority warning during a stream

If the log shows this warning during an otherwise working stream:

```text
Thread priority elevation unavailable; continuing with the default scheduler
```

Polaris is running, but the user service cannot raise capture, encode, or audio worker priority.
Packaged installs include `LimitRTPRIO=95` and `LimitNICE=-10` in `polaris.service`; reload the user
manager and restart Polaris after updating the package:

```bash
systemctl --user daemon-reload
systemctl --user restart polaris
```

If the warning remains, the user manager inherited stricter limits from the login session. Confirm
the active unit with:

```bash
systemctl --user cat polaris
journalctl --user -u polaris -b --no-pager | grep -E 'Thread priority|RealtimeKit|SCHED_FIFO'
```

Installing and running RealtimeKit can also allow priority elevation without granting broad
capabilities to the Polaris binary.

Packaged Linux user units are ordered with `graphical-session.target` and pass through common
desktop environment variables such as `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, and
`DBUS_SESSION_BUS_ADDRESS`. In private Headless Stream mode, a missing parent `WAYLAND_DISPLAY`
is logged as a limited desktop-preview/portal warning instead of a stream startup failure because
Polaris starts its own `labwc` Wayland socket for the client session.

## NVIDIA KMS capture issues

If KMS capture gives a black screen on NVIDIA, confirm the kernel is using:

```text
nvidia_drm.modeset=1
```

If you do not need DRM/KMS capture, keep using the default compositor and portal paths instead.
A startup warning that says `KMS probe could not access DRM framebuffer handles; continuing with
non-KMS capture backends when available` is informational for portal/compositor users; do not apply
`setcap` unless you intentionally selected KMS capture.

If a manually copied explicit-KMS test binary still logs `Failed to gain CAP_SYS_ADMIN` after
`setcap`, check the mount options for the binary path. File capabilities are ignored on `nosuid`
mounts, so `/tmp` builds can be misleading; copy the test binary to a normal path such as
`/usr/local/bin` before applying `setcap`.

For low-FPS NVIDIA headless reports, check `Build features: cuda=...` first. If the log says
`cuda=disabled` and later shows `Attempting to use NVENC without CUDA support. Reverting back to
GPU -> RAM -> GPU`, the stream is taking an extra CPU copy/upload path. Use a CUDA-enabled package
or rebuild with `-DPOLARIS_ENABLE_CUDA=ON` before comparing headless performance against Sunshine.

The expected fast-path markers for NVIDIA true-headless testing look like this:

```text
Build features: cuda=enabled
labwc: Starting in headless mode
wlr: Using ext-image-copy-capture DMA-BUF for headless labwc
capture_transport=dmabuf frame_residency=gpu
target_device=cuda target_residency=gpu
```

`display_preview: Failed to capture cage screenshot` is the web dashboard preview path, not the
stream capture path. Repeated failures are rate-limited in the log, and the dashboard backs off
preview refreshes after failed captures. If the preview is missing, confirm `grim` is installed with
`command -v grim`.

For capture performance, check `/polaris/v1/session/status`; its `capture` object includes
`path`, `reason`, `reason_message`, `cpu_copy`, `gpu_native`, and nested `decision` fields.
`/polaris/v1/stream-policy` exposes the same data as `capture_path`, `capture_path_reason`,
`capture_path_reason_message`, `capture_cpu_copy`, `capture_gpu_native`, and `capture_decision`.
A reason such as `headless_shm_fallback` means Headless Stream is healthy enough to run but still
using the conservative SHM/system-memory path. `headless_extcopy_dmabuf` is the true-headless
DMA-BUF path, and `gpu_native_requested_shm_fallback` means GPU-native capture was requested but
the Wayland capture path still fell back to SHM. Support bundles include the same normalized
decision data under `capture.decision` and stream stats `capture_decision` so a report captures
the selected path, reason message, transport, residency, runtime backend, effective headless
state, and GPU-native override state.

For LTS distro expectations and package caveats, see the [Linux LTS Headless Fallback Matrix](runtime.md#linux-lts-headless-fallback-matrix). Xvfb or gamescope should be treated as investigation-only unless this supported labwc path cannot cover a confirmed target environment.

## VAAPI or software encode fallback

If Polaris cannot hold the preferred hardware path, open Mission Control or Troubleshooting and
check the active runtime path. Polaris surfaces when capture or encode falls back so you do not
need to guess from a black-box client session.

## Linux HDR or Main10 has wrong colors

If the log says `stream_hdr_enabled=false`, treat that stream as SDR. A client HDR request or
`hdr_mode = 2` can still move the encoder into a 10-bit/P010 path, but it does not make a non-HDR
Linux capture path into a true HDR source. On AMD VAAPI systems, keep `hdr_mode = 0` and disable
client HDR requests until SDR colors are correct, then test HEVC Main 8-bit before testing Main10.

For true HDR, look for all of these lines in the same launch:

```text
HDR metadata: available=true usable=true
Color coding: HDR (Rec. 2020 + SMPTE 2084 PQ)
HDR decision: ... display_hdr=true hdr_metadata_available=true stream_hdr_enabled=true
```

If `stream_hdr_enabled=false`, Polaris is being conservative: the client may have requested HDR or Main10,
but the active Linux display path did not provide enough metadata to advertise a real HDR stream.
If `usable=false`, the display path exposed an HDR metadata blob, but Polaris rejected it because core
static metadata such as display primaries or max display luminance was missing.

## Support bundle and logs

When reporting a bug:

1. Export the support bundle from the Troubleshooting screen.
2. Include the active route, capture backend, encoder, and client device.
3. Mention whether the session was headless, host-display, or virtual-display.

If the UI is unavailable, the main host config lives in `~/.config/polaris` and the service logs
can be captured with your systemd user journal.
