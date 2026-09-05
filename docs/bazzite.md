# Install on Bazzite

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
| `bazzite-nvidia-open:stable` `44.20260430` | KDE Plasma Wayland Desktop Mode | Polaris service, ports, Headless Stream launch, client profile application, and host-input isolation validated |
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

Install Polaris from the Fedora 44 release RPM:

```bash
rpm_name="Polaris-fedora44-x86_64.rpm"
wget --output-document="./${rpm_name}" "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}" &&
sudo rpm-ostree install -r "./${rpm_name}"
```

After the reboot:

```bash
sudo -H polaris --setup-host
systemctl --user stop polaris 2>/dev/null || true
sudo install -D -m 0755 "$(readlink -f "$(command -v polaris)")" /usr/local/bin/polaris-kms
sudo setcap cap_sys_admin+ep /usr/local/bin/polaris-kms
getcap /usr/local/bin/polaris-kms
printf '[Service]\nExecStart=\nExecStart=/usr/local/bin/polaris-kms\n' \
  | systemctl --user edit --stdin --drop-in=10-bazzite-kms.conf polaris
systemctl --user daemon-reload
systemctl --user enable --now polaris
```

**Fresh install:** open `https://127.0.0.1:47990/#/welcome`, create the web UI
account, and pair Moonlight, Nova, or another GameStream-compatible client.

**Upgrade or reinstall:** open `https://127.0.0.1:47990/#/login` and use the
existing account. The rpm-ostree transaction intentionally leaves credentials,
pairing keys, settings, and the library under `~/.config/polaris`, even if an old
Polaris layer had to be removed before the new RPM could be installed. If needed,
follow the [credential reset](troubleshooting.md#web-ui-credentials) instead of
returning to Welcome.

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

## Controller and Input Group

Seat isolation (`client_gamepad_seat_isolation`, `client_keyboard_mouse_seat_isolation`)
needs the account Polaris runs as to be in the `input` group. Polaris warns at startup when
it is not.

**`sudo usermod -aG input $USER` does not work on Bazzite.** The `input` group is defined in
`/usr/lib/group` rather than `/etc/group`, so `usermod` cannot find a group to add anyone to.
Use the Universal Blue recipe, which copies the definition across first:

```bash
ujust add-user-to-input-group
```

Then sign out and back in — group membership only applies to new sessions.

```bash
id -nG | tr ' ' '\n' | grep -qx input && echo "in the input group" || echo "not in it"
```

Thanks to [@SVelothi](https://github.com/papi-ux/polaris/issues/274) for finding this.

## Why rpm-ostree Layering

Polaris needs host-level integration: the binary, web assets, desktop metadata,
the user service, udev rules for virtual input, and compositor helpers such as
`grim`, `labwc`, `wlr-randr`, Xwayland, and `xdpyinfo`. On Bazzite, layering the RPM is
cleaner than running Polaris from a toolbox, distrobox, or unpacked archive
because the package manager can install those host dependencies into the booted
deployment.

The Polaris RPM declares the headless runtime dependencies, so the install
command should not need separate `grim`, `labwc`, or `wlr-randr` arguments.

## Recommended Bazzite Optimization

Start in Desktop Mode first. Game Mode and Deck-style gamescope sessions can hide
display, portal, and environment details that are easier to debug from Desktop
Mode.

Use Headless Stream for the first stream:

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
```

This is the recommended Bazzite Desktop Mode optimization for NVIDIA/NVENC and
AMD/Mesa VAAPI hosts. It creates an isolated headless `labwc` runtime for the
stream, routes launched apps and virtual input into that socket, and avoids
targeting the physical KDE desktop.

With `linux_prefer_gpu_native_capture = enabled`, logs may still report SHM/RAM
capture, CPU frame residency, or an extra CPU-side copy/conversion path when the
current compositor, driver, or encoder import path cannot stay GPU-native. Treat
those as performance notes, not startup failures, when the client receives a
stable stream from `HEADLESS-1`. If the setting prevents launch on a specific
AMD/NVIDIA stack, temporarily switch it to `disabled` and report the capture
decision JSON/logs.

Do not manually export `WAYLAND_DISPLAY`; Polaris starts `labwc` with its own
Wayland socket and routes launched apps into that socket. Do not add EVDI or
dummy-plug display routing for this validation path.

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

## Headless Boot and Deck Images

The packaged service enables into `xdg-desktop-autostart.target`, which only a
full desktop session fires. Two common Bazzite setups never fire it:

- Deck images that boot straight into the gamescope Steam session. The gamescope
  session does not run XDG autostart, so Polaris only starts once you visit
  Desktop Mode.
- A monitor-less host (dedicated streaming box, dummy plug removed). With no
  graphical session at all, nothing starts the service after a reboot.

For both, make Polaris boot-independent explicitly:

```bash
sudo -H polaris --setup-host --enable-headless-boot
```

This enables lingering for your account (your user services start at boot,
before any login) and hooks the Polaris user service into `default.target`. Run
it from Desktop Mode's terminal or over SSH, as your normal user via sudo. Undo
it later with `--disable-headless-boot`.

Verify after a reboot, over SSH if there is no display:

```bash
systemctl --user is-active polaris
journalctl --user -u polaris --since "10 minutes ago" --no-pager
```

Two honest notes:

- Private Stream and Gamescope Stream need no desktop session, so streaming
  works on a fully headless boot. Streaming the visible desktop (Mirror
  Desktop, Host Virtual Display) still needs a desktop login, and Polaris must
  be restarted after that login to see it.
- You do not need the host to boot into Big Picture to play Big Picture. With
  Private Stream, launching Steam Big Picture from the client starts it inside
  the private session at the client's resolution; the host can sit at a black
  screen. See [Launch modes and capture paths](launch-modes.md).

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
Applying client profile for "<client name>"
session_optimization: ... layers=client_profile+device_db+runtime_policy
labwc: Starting in headless mode
labwc: Ready
Selected monitor [Headless output 1] for streaming
Wayland virtual input: routing supported devices to labwc socket
Encoder cache saved: nvenc
New streaming session started
session_event: stream_active
CLIENT CONNECTED
```

Steam should report the client stream resolution, not the physical `7680x2160`
`DP-3` desktop.

There should not be a warning that virtual input is falling back to host uinput
during a healthy headless `labwc` stream. If that appears, stop testing and
report it as an input-isolation issue because host Plasma may receive remote
mouse or keyboard input.

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

## System Extension (withdrawn)

> [!WARNING]
> The experimental `Polaris-sysext-x86_64.raw` image was withdrawn on
> September 5, 2026. The image did not include the runtime dependencies required by Bazzite.
> Polaris can therefore fail to start immediately. A reported boot problem after
> installation is also under investigation. Do not use cached copies.

The supported Bazzite install remains the Fedora 44 RPM through `rpm-ostree`, as
described in [Install](#install). The extension is not a substitute for the RPM
until its dependency closure, SELinux behavior, failed-install rollback, and
boot recovery have been validated on real Bazzite hardware.

An installed extension remains at `/var/lib/extensions/polaris.raw`. That path is
outside an individual rpm-ostree `/usr` deployment, so selecting another
deployment does not remove the image. If the host still reaches Desktop Mode, a
TTY, or SSH, remove the extension before rebooting again:

```bash
systemctl --user disable --now polaris 2>/dev/null || true
sudo systemd-sysext unmerge
sudo rm -f /var/lib/extensions/polaris.raw
sudo systemd-sysext refresh
systemctl --user daemon-reload
sudo systemd-sysext status
```

The final status must not list `polaris`. The `refresh` step restores any other
installed system extensions after Polaris is removed. After cleanup, use the
Fedora 44 RPM install path above.

If the host no longer reaches a login, do not keep switching deployments or
reinstall Bazzite. Ask in the
[public Polaris Matrix room](https://matrix.to/#/#polaris:papi-ux.com) before
making more changes so recovery can be matched to the exact boot symptom.

## Update

Layer the newer Fedora 44 RPM and reboot. `rpm-ostree` will stage the
newer local RPM over the existing layered Polaris package:

```bash
rpm_name="Polaris-fedora44-x86_64.rpm"
wget --output-document="./${rpm_name}" "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}" &&
sudo rpm-ostree install -r "./${rpm_name}"
```

If you installed without `-r`, or rolled back a deployment and installed again, the
new package is only staged: the running deployment, and the version the dashboard
reports, do not change until you reboot. `rpm-ostree status` lists the staged
deployment above the booted one. `rpm-ostree install` answering "already installed"
while the dashboard still shows the old version means exactly this.

After the reboot, refresh `/usr/local/bin/polaris-kms` and its capability using
the copy and `setcap` steps from [Install](#install), restart the service, and
return to `https://127.0.0.1:47990/#/login` with the existing credentials. Do not
use the first-run Welcome page merely because the package layer was replaced.

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

After rebooting, re-enable the Sunshine user service that matches the previous
installation. The unit names are alternatives; run only the applicable command:

```bash
# Homebrew Sunshine
systemctl --user enable --now homebrew.sunshine.service

# Flatpak Sunshine
systemctl --user enable --now app-dev.lizardbyte.app.Sunshine.service
```

Polaris and Sunshine use the same default GameStream ports, so do not enable both
hosts at the same time.

## Known Bazzite Log Messages

`labwc: No new Wayland socket appeared within 10s` means the isolated `labwc`
runtime failed to start or exited before creating its Wayland socket. Confirm the
matching Fedora RPM was installed, rebooted into the new deployment, and retry
from Desktop Mode first.

`Environment variable WAYLAND_DISPLAY has not been defined` usually points to a
windowed Wayland runtime being launched without a parent Wayland session. In
private Headless Stream mode, Polaris can still start its own `labwc` socket for
the client; treat the message as a desktop-preview or portal-capture clue unless
the client stream itself fails to connect.

`Couldn't scale frame ... src_fmt=bgr0 ... src_stride=0` means Polaris received a
CPU BGR0 frame without a valid row pitch. Use a release newer than `v1.0.4`, where
the headless CPU fallback path was fixed.

`Failed to gain CAP_SYS_ADMIN` with `KMS probe could not access DRM framebuffer
handles; continuing with non-KMS capture backends when available` is only a
startup probe warning for portal/compositor users. Do not apply `setcap` for the
normal portal path.

`KMS display capture requires CAP_SYS_ADMIN` is actionable only when you
intentionally selected explicit KMS capture. On Bazzite, copy the current
packaged binary to `/usr/local/bin/polaris-kms`, apply `setcap` there, and make
sure the `~/.config/systemd/user/polaris.service.d/10-bazzite-kms.conf` override
points `ExecStart` at that file.

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
copy/conversion path` are not startup failures. Confirm the stream is connected
by looking for `session_event: stream_active`, `CLIENT CONNECTED`, `Selected
monitor [Headless output 1]`, and `Found H.264 encoder: h264_nvenc [nvenc]`.
For performance reports, though, treat `capture_transport=shm
frame_residency=cpu frame_format=bgra8`, `target_residency=cpu`, or `Build
features: cuda=disabled` with NVENC as important clues because they mean Polaris
is taking a CPU copy/upload path.

For NVIDIA true-headless performance testing, the fast path should report
`Build features: cuda=enabled`, `capture_transport=dmabuf frame_residency=gpu`,
and `target_device=cuda target_residency=gpu`. In the web UI or
`/polaris/v1/session/status`, `capture.reason=headless_extcopy_dmabuf` is the
desired true-headless marker; `headless_shm_fallback` means the stream can still
be healthy, but it is using the conservative CPU-side capture path.

`display_preview: Failed to capture cage screenshot` affects the web dashboard
preview path. It does not mean the Moonlight/Nova stream failed if the client is
already connected and receiving frames. Polaris rate-limits repeated preview
capture failures, but if the preview itself matters, include `command -v grim`
with the report.

If local Plasma receives remote mouse or keyboard input while using headless
labwc, treat that as an input-isolation bug and include the validation details
below.

## Validation Checklist

Please include these details when reporting Bazzite issues:

- Bazzite image name and version from `rpm-ostree status`
- Desktop Mode or Game Mode
- GPU model and driver stack
- Polaris RPM asset used, such as `Polaris-fedora44-x86_64.rpm`
- output of `command -v polaris grim labwc wlr-randr`
- the `Build features: cuda=...` line
- output of `getcap /usr/local/bin/polaris-kms`
- output of `systemctl --user cat polaris`
- whether `sudo -H polaris --setup-host` completed successfully
- whether `systemctl --user status polaris` is running
- whether the web UI opens at `https://127.0.0.1:47990`
- client used for pairing, such as Steam Deck Moonlight, Android Moonlight, or Nova
- active capture path shown in the Polaris dashboard
- requested client resolution, FPS, codec, and whether the web UI preview was open
- whether headless mode and virtual display behavior worked after a reboot
