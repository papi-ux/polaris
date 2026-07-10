# KDE Wayland Host Virtual Display Research

This document captures public research for a future Polaris **Host Virtual Display** backend on KDE Wayland. It is intentionally generalized: do not add private hostnames, LAN addresses, pairing secrets, screenshots, or support-bundle contents.

## Source

- Reddit / r/MoonlightStreaming: <https://www.reddit.com/r/MoonlightStreaming/comments/1tg1dnc/guide_sunshine_on_kde_wayland_virtual_display/>

The guide describes Sunshine on KDE Wayland using KWin capture plus `krfb-virtualmonitor` so Moonlight clients receive a temporary virtual display at the client resolution/aspect ratio without HDMI dummy plugs, EDID kernel injection, or permanent physical-port state.

## What It Proves

- KDE/KWin can create a temporary virtual monitor through `krfb-virtualmonitor`.
- `kscreen-doctor` can add/select a custom resolution and refresh rate for that virtual output.
- KWin/portal-style capture can see the virtual monitor because it exists in the compositor, even though it is not a real DRM connector.
- Fedora 44 KDE, Bazzite, CachyOS, and Arch users reported success in the thread.
- HDR is not solved by this path: KWin virtual outputs do not expose the DRM/KMS HDR metadata Polaris needs to truthfully advertise true HDR.

## Product Position

This should not replace Polaris' default **Private Stream** path. The default player-facing recommendation remains:

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
```

Private Stream remains the safest default for NVIDIA/NVENC and AMD/Mesa VAAPI hosts because it avoids physical desktop layout changes and reports whether capture stayed GPU-native or fell back to SHM/system memory.

The useful Polaris lane is a KDE-only **Host Virtual Display** backend that is advertised only when available, with a clear unavailable reason when KDE/krfb/kscreen prerequisites are missing.

## Backend Sketch

A Polaris-owned backend should do the work directly instead of asking users to paste Sunshine prep scripts:

1. Detect KDE Wayland:
   - `XDG_CURRENT_DESKTOP` contains `KDE`
   - `XDG_SESSION_TYPE=wayland`
   - `WAYLAND_DISPLAY` and `DBUS_SESSION_BUS_ADDRESS` are available from the user manager
2. Detect helpers:
   - `krfb-virtualmonitor`
   - `kscreen-doctor`
3. Import the graphical session environment for helper processes:
   - `WAYLAND_DISPLAY`
   - `DISPLAY`
   - `XDG_CURRENT_DESKTOP`
   - `XDG_SESSION_TYPE`
   - `XDG_RUNTIME_DIR`
   - `DBUS_SESSION_BUS_ADDRESS`
   - `QT_QPA_PLATFORM=wayland`
4. Create a session-scoped output name, for example `polaris-vmon-<session-id>`.
5. Launch `krfb-virtualmonitor --resolution <WIDTH>x<HEIGHT> --name <name> --password <random> --port <ephemeral-or-reserved>`.
6. Wait for KWin to register `Virtual-<name>`.
7. Use `kscreen-doctor` to add/select the requested mode:
   - refresh rate is expressed in mHz for `addCustomMode`, e.g. `120000` for 120 Hz.
8. Route capture to that virtual output when Host Virtual Display is selected.
9. Persist enough state for stale cleanup if Polaris exits mid-session.
10. Destroy the virtual monitor and clear state during normal session stop, process deinit, and explicit stale-cleanup API calls.

## Polaris/Nova Contract

- `/polaris/v1/client-settings` should advertise `host_virtual_display` only when the backend is available and configured enough to launch.
- If unavailable, Nova should show Host Virtual Display disabled with the host-provided reason rather than pretending the client can force it.
- `/polaris/v1/session/status` and Mission Control should report the actual mode, backend, output name, capture transport, frame residency, and encoder.
- Nova labels should stay vendor-inclusive where technically true: NVIDIA/NVENC and AMD/Mesa VAAPI users should both see Host Virtual Display and GPU-native copy as host capability/fallback facts, not NVIDIA-only branding.

## Do Not Copy Blindly

- Do not use `sed -i` to rewrite config files.
- Do not hard-code `DP-1` or any physical connector name.
- Do not disable all physical monitors as the default Polaris behavior.
- Do not depend on Sunshine-specific environment variables such as `SUNSHINE_CLIENT_WIDTH`; Polaris should use its own launch/stream policy.
- Do not recommend broad `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` as normal setup. Treat it as a last-resort diagnostic for KWin permission rejection.
- Do not claim true HDR support until the capture path exposes real HDR metadata.

## Open Questions

- Can `krfb-virtualmonitor` be used only for monitor creation without exposing a VNC listener, or must Polaris randomize/isolate the VNC password and port every session?
- Which KDE/Plasma versions require the portal desktop-file/app-id fix mentioned in community reports?
- Can KWin virtual monitor creation be controlled through a more direct D-Bus or portal API than shelling out to `krfb-virtualmonitor`?
- How should Polaris choose between EVDI, wlroots headless outputs, kscreen existing-output management, and KDE `krfb-virtualmonitor` when more than one backend exists?
- Should Host Virtual Display preserve physical outputs by default, or offer an explicit advanced option to temporarily disable them for game focus/window-placement reliability?

## Validation Checklist

For the first implementation spike, verify on KDE Wayland with both NVIDIA/NVENC and AMD/Mesa VAAPI where possible:

- `krfb-virtualmonitor --version` succeeds when Polaris imports the graphical session environment.
- The virtual output appears as `Virtual-polaris-vmon-*` in `kscreen-doctor`.
- Requested width/height/FPS match the Nova/Moonlight launch policy.
- Host Virtual Display appears in Nova only when available.
- A stream reaches active state and reports actual capture transport/residency.
- Stop/cleanup removes the virtual output even after a failed launch.
- True HDR remains disabled unless real HDR metadata is present.
