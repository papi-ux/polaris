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
linux_prefer_gpu_native_capture = disabled
```

That is the intended Headless Stream path. It avoids touching your normal desktop layout and reduces
display mode churn after a session ends. On Bazzite, this is also the first NVIDIA Desktop Mode
validation path; SHM/RAM capture warnings are performance notes if the client is receiving a stable
`HEADLESS-1` stream. Enable GPU-native capture later only after the headless path is working on your
driver and compositor stack.

The built-in Desktop entry does not launch your existing KDE, GNOME, or wlroots desktop inside this
private compositor. If the client connects but shows an empty or black desktop while app entries work,
that usually means the headless runtime is alive but nothing visible has been launched in it. Use
Desktop Display mode when you want to stream the already-running host desktop session.

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

## NVIDIA KMS capture issues

If KMS capture gives a black screen on NVIDIA, confirm the kernel is using:

```text
nvidia_drm.modeset=1
```

If you do not need DRM/KMS capture, keep using the default compositor and portal paths instead.

## VAAPI or software encode fallback

If Polaris cannot hold the preferred hardware path, open Mission Control or Troubleshooting and
check the active runtime path. Polaris surfaces when capture or encode falls back so you do not
need to guess from a black-box client session.

## Linux HDR or Main10 has wrong colors

If the log says `Display is HDR: false`, treat that stream as SDR. A client HDR request or
`hdr_mode = 2` can still move the encoder into a 10-bit/P010 path, but it does not make a non-HDR
Linux capture path into a true HDR source. On AMD VAAPI systems, keep `hdr_mode = 0` and disable
client HDR requests until SDR colors are correct, then test HEVC Main 8-bit before testing Main10.

## Support bundle and logs

When reporting a bug:

1. Export the support bundle from the Troubleshooting screen.
2. Include the active route, capture backend, encoder, and client device.
3. Mention whether the session was headless, host-display, or virtual-display.

If the UI is unavailable, the main host config lives in `~/.config/polaris` and the service logs
can be captured with your systemd user journal.
