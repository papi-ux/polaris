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

## Fullscreen Proton or Wine game renders on the physical monitor

The stream connects, audio and input reach the private session correctly, and the client shows an
empty compositor while the game appears on your real desktop instead.

If the app is launched through Flatpak, this is the Flatpak portal replacing `DISPLAY`. Polaris
exports the private session's display to the command it launches, but when that command spawns back
out through the portal (`org.freedesktop.portal.Flatpak`), the portal builds the new sandbox from
the *portal service's own* environment. The portal service is D-Bus activated and holds the display
your desktop session had at login, so it overwrites `DISPLAY` and binds only that one X socket into
the container. Wine and Proton use the X11 driver by default, follow the substituted `DISPLAY`, and
land on the host desktop.

Nothing set at any layer above the portal survives this. Passing the variable explicitly does not
help either, because the portal applies its own X11 arguments after the caller's environment.

Confirm it in one command while the game is running, using the game's own mount namespace:

```bash
GAME=<game-pid>
sudo ls -la /proc/$GAME/root/tmp/.X11-unix/
sudo tr '\0' '\n' < /proc/$GAME/environ | grep -E '^(DISPLAY|WAYLAND_DISPLAY)='
```

If the only socket present is the one for your desktop session rather than the private session's,
the container never had a path to the private display.

**Workaround:** launch through a native, non-Flatpak build of your launcher (umu-launcher, Lutris,
Heroic, Steam). The Steam Runtime container itself honors `DISPLAY` correctly, so removing the
portal hop is enough. A direct `flatpak run` also passes the display through correctly; it is
specifically the portal spawn underneath a Flatpak launcher that does not.

Two things commonly trip up the switch from a launcher's Flatpak build to its native one:

- **Re-add the game rather than reusing its old ID.** Heroic gives sideload entries a new
  `app_name` per install, so an identifier copied from the Flatpak install will not resolve in the
  native one. Read the current value out of `~/.config/heroic/sideload_apps/library.json`, or your
  launcher's equivalent.
- **Let the launcher finish starting before handing it a launch URL.** Native Heroic given a
  `heroic://launch?...` URL on a cold start throws
  `Cannot read properties of undefined (reading 'getGame')` rather than queuing the request. Start
  the launcher on its own first and send the launch as a separate command.

A working app entry on the reporting host ended up as:

```
WAYLAND_DISPLAY=wayland-1 DISPLAY=:2 heroic --no-gui "heroic://launch?appName=<id>&runner=sideload"
```

Polaris exports both of those variables into the private session already, so setting them by hand
is redundant rather than required. They are shown here because that is the entry that was verified.

Polaris logs a warning at launch when an app command can reach the portal, and reports
`never opened a window in the private session` when a launch produces no window at all, rather than
streaming an empty compositor silently.

## Steam Big Picture black screen or tiny window

Clear Steam's HTML cache:

```bash
rm -rf ~/.local/share/Steam/config/htmlcache/
```

If you are using MangoHud, disable it for Steam Big Picture and Steam/Proton launches first.
Those paths are the most sensitive to early helper-process crashes.

## Input does not work

The udev rules and modules-load configuration ship as package files, so virtual input works after
the next reboot with nothing else to run. To use it without rebooting first, load the modules once:

```bash
sudo modprobe uinput uhid
```

If the problem persists, ensure your user has access to the input stack expected by your distro
setup:

```bash
sudo usermod -aG input "$USER"
```

Then sign out and back in.

On an ostree host — Bazzite, Bluefin, Silverblue and relatives — that command does nothing
useful: the `input` group lives in `/usr/lib/group` rather than `/etc/group`, so `usermod`
finds no group to add anyone to. Universal Blue images ship a recipe that copies the
definition across first:

```bash
ujust add-user-to-input-group
```

Polaris detects this and prints whichever command applies to your host.

If Polaris was installed before the rules became package files, an older copy may still sit in
`/etc/udev/rules.d/60-polaris.rules`. `/etc` overrides the packaged file, so that copy keeps
shadowing later fixes — including the seat isolation rules, which then never apply no matter what
the configuration says.

```bash
sudo -H polaris --setup-host
```

Host setup removes that copy only when its contents still match the file this Polaris ships. An
older version's copy does not match — that is what upgrading changed — so it is **kept**, with a
warning naming the file, because nothing can tell it apart from a copy you edited yourself. Upgrading
is therefore the case most likely to leave a shadowing file behind.

If you did not edit it, remove it and reload:

```bash
sudo rm /etc/udev/rules.d/60-polaris.rules
sudo udevadm control --reload-rules
```

Then confirm the packaged rules are the ones in effect:

```bash
grep -c seat-isolated /usr/lib/udev/rules.d/60-polaris.rules
```

## Client input also types into the host desktop

A private stream session and the desktop session logged in at the machine both see the virtual
keyboard and mouse Polaris creates, so a client's typing reaches both. Enable
`client_keyboard_mouse_seat_isolation` to assign those devices to a dedicated seat, or ignore them
by name in your desktop compositor. See
[host and private session input isolation](configuration.md#linux-host-and-private-session-input-isolation).

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

The Troubleshooting screen in the Polaris web UI inspects **this host**. It is the place to go
when something is wrong and you want to know what Polaris thinks happened. The
[Troubleshooting guide](https://papi-ux.com/docs/troubleshooting/) on papi-ux.com explains the
problems themselves and is the place to go when you know what is wrong and want to fix it.

When reporting a bug, open Troubleshooting and use **Report a problem**. It downloads a redacted
support bundle and opens a pre-filled GitHub issue with your host OS, GPU and driver, client, and
runtime already answered. Attach the bundle to that issue.

Nothing is sent anywhere on its own. The bundle lands on your machine and the issue opens as a
draft you complete, so you see exactly what you are sharing before anyone else does.

If you would rather assemble it yourself, the same screen offers the bundle and the issue draft as
separate downloads, and describing the active route, capture backend, encoder, and client device by
hand is still useful.

### What is redacted

Values whose name reads as a credential are replaced before anything leaves the browser: whole
words such as password, token, secret, cookie, auth and credential, run-together forms such as
apikey, and key when something qualifies it, as in api_key. Names that merely contain one of those,
such as keyboard, stay readable so the bundle remains worth reading. The exact rule is stated in
the bundle itself under `redaction_notice`.

Redaction is not a promise that a bundle is safe to publish unread. Look at it first.

### Reporting a crash

Polaris records how each run ended, so the first question is already answered for you. Open
Troubleshooting after restarting: if the previous run did not shut down normally, the page says so
at the top, and reports one of

- **crashed**, meaning it died on a named fatal signal, with the captured backtrace,
- **unclean**, meaning it never recorded an exit and left no crash evidence, which usually means
  the OOM killer, a `SIGKILL`, or power loss rather than a fault in Polaris.

That distinction matters and is hard to make by hand, which is why Polaris makes it.

The log of the run that crashed is preserved too. The active log describes the run you are looking
at now, so the interesting one is the retained copy from the previous run, and the support bundle
carries it.

#### Getting a symbolised backtrace

The backtrace Polaris captures comes from a stripped binary, so it names addresses more than
functions. For a symbolised one, the matching debug package and `coredumpctl` still work exactly as
before, because the crash handler re-raises rather than swallowing the signal:

```bash
sudo pacman -S polaris-debug
coredumpctl info polaris
```

Include that verbatim alongside the bundle when a maintainer asks for it.

### When the web UI is unavailable

The host config lives in `~/.config/polaris`, the service logs can be captured from your systemd
user journal, and the run-state and crash evidence sit next to the config as `last_run.json` and
`last_crash.txt`.
