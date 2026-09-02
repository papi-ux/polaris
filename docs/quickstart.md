# Quick start

Take a Linux host from nothing to a first stream. Fedora 44 and Arch Linux are the
recommended package paths; if you run something else, start from [Compatibility](compatibility.md)
to find your path before following the steps here.

## 1. Install the package

### Fedora 44

```bash
wget --output-document=./Polaris-fedora44-x86_64.rpm https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm &&
sudo dnf install ./Polaris-fedora44-x86_64.rpm &&
sudo -H polaris --setup-host &&
polaris
```

The longer walkthrough, including upgrades and uninstall, is in the [Fedora guide](fedora.md).

### Arch Linux / CachyOS

```bash
wget --output-document=./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
polaris
```

CachyOS and most pacman-compatible Arch derivatives should start with the Arch package path. See the
[Arch guide](arch.md) for details, and fall back to the source flow in
[Build from source](building.md) if a derivative has dependency naming or runtime helper differences.

### Other hosts

| Host | Path |
|---|---|
| SteamOS 3.8 | [SteamOS guide](steamos.md) — Desktop Mode validation only |
| Bazzite 44 | [Bazzite guide](bazzite.md) — layer the Fedora 44 RPM with `rpm-ostree` |
| Ubuntu 24.04 | [Ubuntu guide](ubuntu.md) — experimental tester DEB |
| openSUSE Tumbleweed | [openSUSE guide](openSUSE.md) — source build |
| Anything else | [Build from source](building.md) |

## 2. Create your web console account

Open **https://localhost:47990/#/welcome**, create your web UI account, and pair a client. After
credentials exist, **https://localhost:47990** opens the normal console.

> [!TIP]
> If you changed `port` in `~/.config/polaris/polaris.conf`, the web UI is at
> `https://localhost:<port + 1>`. For background autostart, enable the user service with
> `systemctl --user enable --now polaris`.

## 3. Confirm the recommended Linux path

In the first-run setup, put games in a private runtime instead of on your desktop: under
**Settings → Audio/Video → Where games run**, pick **Private Stream**. On an
NVIDIA card, pick **Private Stream (GPU-native)** instead; it is the best-tested path and keeps
capture on the GPU. In the config file, those two cards correspond to:

```ini
# Private Stream (the default recommendation)
linux_stream_mode = headless_stream
```

```ini
# Private Stream (GPU-native), the NVIDIA pick
linux_stream_mode = windowed_stream
linux_prefer_gpu_native_capture = enabled
```

> **What you'll see:** the built-in **Desktop** entry now streams Polaris' *private* compositor — an
> intentionally empty screen (right-click opens the session menu) until you launch a game from your
> client. Wanting your actual desktop on the stream is a different mode: `desktop_display` mirrors
> the host desktop at host resolution, and `host_virtual_display` adds an extra display sized to the
> client. Both are one click in the web UI under Settings → Audio/Video.

To pick a different mode later, such as Gamescope, a virtual display, or a dummy plug, see
[Launch modes and capture paths](launch-modes.md). [Configuration](configuration.md) explains every
setting, and [Runtime and streaming model](runtime.md) explains what these keys actually change.

## 4. Pair a client

Pick whichever fits your network:

- **Trusted Pair** on a trusted LAN, for a TOFU flow that auto-approves first pairing on a
  configured trusted subnet.
- **QR pairing** for Nova.
- **Manual PIN** for standard Moonlight clients.

## 5. Start a game and verify the path

Launch from the Polaris library, Nova, or a Moonlight client, then watch the live session dashboard
in Mission Control to confirm the active runtime and encoder path. Polaris reports the capture path
it actually used, so if it fell back to system memory you will see that rather than having to infer
it from logs.

If the video is connected but does not feel right, keep the stream running and open Doctor. The
[Doctor guide](doctor.md) explains its Network / Host / Client verdicts and the exact difference
between Auto Fix, Recheck, Manual guidance, and Undo.

## If something does not work

[Troubleshooting](troubleshooting.md) covers the common failure modes, the log markers worth
grepping, and what to include in a bug report. The
[headless fallback matrix](runtime.md#linux-lts-headless-fallback-matrix) explains what capture path
to expect on older LTS hosts.
