# Quick start

Take a Linux host from nothing to a first stream. Fedora 44 and Arch Linux are the
recommended package paths; if you run something else, start from [Compatibility](compatibility.md)
to find your path before following the steps here.

## 1. Install the package

### Fedora 44

```bash
sudo curl --location --output /etc/yum.repos.d/polaris.repo https://repo.papi-ux.com/fedora/polaris.repo &&
sudo dnf install polaris &&
sudo -H polaris --setup-host &&
polaris
```

Same four commands as downloading the RPM by hand, and `sudo dnf upgrade` carries Polaris from then on
instead of another download at an exact filename. `dnf install` asks you to accept the signing key;
the fingerprint to check it against is on the [repositories page](repositories.md#fedora).

The repository serves the latest stable release, and publishing runs on a schedule, so for a few hours
after a release it still serves the previous one. If you want a brand new release the moment it lands,
or a prerelease, install the RPM directly as the [Fedora guide](fedora.md) describes. That guide is also
the longer walkthrough, including upgrades and uninstall.

### Arch Linux / CachyOS

```bash
wget --output-document=./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
polaris
```

There is a pacman repository too, and it is worth adding for the same reason: `sudo pacman -Syu` then
carries Polaris. It is not the default here only because it is longer to set up rather than shorter.
pacman has no equivalent of dnf's `gpgkey=`, so the key has to be added and locally signed first. See
[Package repositories](repositories.md#arch-and-cachyos).

CachyOS and most pacman-compatible Arch derivatives should start with the Arch package path. See the
[Arch guide](arch.md) for details, and fall back to the source flow in
[Build from source](building.md) if a derivative has dependency naming or runtime helper differences.

### Other hosts

| Host | Path |
|---|---|
| SteamOS 3.8 | [SteamOS guide](steamos.md) — Desktop Mode validation only |
| Bazzite 44 | [Bazzite guide](bazzite.md) — supported RPM installation; stage, reboot, then run host setup |
| Steam Deck, ROG Ally, other handhelds | [Handhelds and Game Mode](handhelds.md), keeps Polaris reachable across mode switches |
| Ubuntu 24.04 | [Ubuntu guide](ubuntu.md) — experimental tester DEB |
| openSUSE Tumbleweed | [openSUSE guide](openSUSE.md) — source build |
| Anything else | [Build from source](building.md) |

## 2. Open the right web console path

**Fresh install:** if this host has never had a Polaris web account, open
**https://localhost:47990/#/welcome** and create the account. The wizard then walks the rest of
the setup. GPU and Encoder shows each GPU, the encoder Polaris will use and why, and anything
hardware encoding still needs, such as the RPM Fusion driver an AMD card needs on Fedora. Launch
Mode picks where games run, with Private Stream recommended. Network lists the ports and can
trust your home network with one click, so Nova pairs without a PIN. Two optional steps add a
SteamGridDB key for covers on non-Steam games and an AI provider for Doctor explanations; both
can be skipped and set later under Settings. Pair Client comes next, and First App finishes on
the Applications page. The SteamGridDB key, the AI provider and a trusted network take effect
right away; an encoder or launch mode change waits for a restart, which the last step offers.

**Upgrade or reinstall:** open **https://localhost:47990/#/login** and sign in with the existing
account. Package upgrades and removals intentionally preserve credentials, pairing keys, settings,
and the library under `~/.config/polaris`; reinstalling the package does not make the host a new
first-run installation. If the credentials are no longer known, use the bounded reset in
[Troubleshooting](troubleshooting.md#web-ui-credentials).

> [!TIP]
> If you changed `port` in `~/.config/polaris/polaris.conf`, the web UI is at
> `https://localhost:<port + 1>`. For background autostart, enable the user service with
> `systemctl --user enable --now polaris`. The application menu entry starts that same service,
> so a desktop launch and autostart never run two copies.

## 3. Confirm the recommended Linux path

Put games in a private runtime instead of on your desktop: in the first-run wizard's Launch Mode
step, or later under **Settings → Audio/Video → Where games run**, pick **Private Stream**. On an
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
> client. On Hyprland, `desktop_takeover` moves the live desktop onto that temporary client-sized
> output and blanks the original displays until the stream ends. All three are one click in the web
> UI under Settings → Audio/Video.

To pick a different mode later, such as Gamescope, a virtual display, or a dummy plug, see
[Launch modes and capture paths](launch-modes.md). [Configuration](configuration.md) explains every
setting, and [Runtime and streaming model](runtime.md) explains what these keys actually change.

## 4. Pair a client

Pick whichever fits your network:

- **Trusted Pair** on a trusted LAN, for a TOFU flow that auto-approves first pairing on a
  configured trusted subnet.
- **QR pairing** for Nova.
- **Manual PIN** for standard Moonlight clients.

New devices use **Game Control** by default, which is the least-privilege preset that can browse,
launch, and control a game. **Browse & Watch** is intentionally read-only: it can list the library
and join an existing stream, but it cannot start Desktop or a game and cannot send input. Existing
paired devices keep their saved access until you change it under **Devices → Edit Access**.

For Moonlight on Linux, Android, or another non-Nova client: add the Polaris host in Moonlight and
leave its displayed four-digit PIN open. In the Polaris web UI, open **Devices → Manual PIN**, enter
that PIN, keep **Game Control** selected, and choose **Send**. Return to Moonlight and refresh the
host if its library does not appear immediately. Steam is not required on the client.

The Moonlight flow step by step, including which client settings matter, is in
[Play with Moonlight](moonlight.md). The access presets and the device editor are in
[Pair and manage devices](devices.md).

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
