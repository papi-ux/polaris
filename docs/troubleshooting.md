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

## Headless session does not start cleanly

Confirm these settings first:

```ini
headless_mode = enabled
linux_use_cage_compositor = true
linux_prefer_gpu_native_capture = enabled
```

That is the intended Linux path. It avoids touching your normal desktop layout and reduces display
mode churn after a session ends.

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

## Support bundle and logs

When reporting a bug:

1. Export the support bundle from the Troubleshooting screen.
2. Include the active route, capture backend, encoder, and client device.
3. Mention whether the session was headless, host-display, or virtual-display.

If the UI is unavailable, the main host config lives in `~/.config/polaris` and the service logs
can be captured with your systemd user journal.
