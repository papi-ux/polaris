# Quick start

Take a Linux host from nothing to a first stream. Fedora 44 and Arch Linux are the
recommended package paths; if you run something else, start from [Compatibility](compatibility.md)
to find your path before following the steps here.

## 1. Install the package

On Fedora 44, Arch Linux or CachyOS, one command adds the signed
[package repository](repositories.md), installs Polaris and runs host setup:

```bash
curl -fsSL https://repo.papi-ux.com/install.sh | sh
```

Then do what its last lines say, usually starting Polaris with
`systemctl --user enable --now polaris`, and go on to step 2. To read every command it would run
first, without changing anything, run
`curl -fsSL https://repo.papi-ux.com/install.sh | sh -s -- --dry-run`. On Arch and CachyOS it
upgrades the rest of the system in the same step. It installs the stable release only, not the
optional `polaris-kms` helper, and it does not replace a 1.4.13 beta;
[One-command install](repositories.md#one-command-install) has the details. The sections below
are the same steps by hand.

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
curl --fail --location --output ./Polaris-arch-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst &&
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&
sudo -H polaris --setup-host &&
polaris
```

There is a pacman repository too, and it is worth adding for the same reason: `sudo pacman -Syu` then
carries Polaris. The one-command install above adds it for you. By hand it takes longer to set up
than the download, because pacman has no equivalent of dnf's `gpgkey=`, so the key has to be added
and locally signed first. See [Package repositories](repositories.md#arch-and-cachyos).

CachyOS and most pacman-compatible Arch derivatives should start with the Arch package path. See the
[Arch guide](arch.md) for details, and fall back to the source flow in
[Build from source](building.md) if a derivative has dependency naming or runtime helper differences.

### Other hosts

| Host | Path |
|---|---|
| SteamOS 3.8 | [SteamOS guide](steamos.md), proven on a Steam Deck OLED in Desktop Mode and Game Mode |
| Bazzite 44 | [Bazzite guide](bazzite.md), supported RPM installation: stage, reboot, then run host setup |
| Steam Deck, ROG Ally, other handhelds | [Handhelds and Game Mode](handhelds.md), keeps Polaris reachable across mode switches |
| Ubuntu 24.04 | [Ubuntu guide](ubuntu.md), experimental tester DEB |
| openSUSE Tumbleweed | [openSUSE guide](openSUSE.md), source build |
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

> **What you'll see:** the built-in **Desktop** entry streams your real desktop, whatever the launch
> mode, because its **Launch as** is **Mirror Desktop** in the [app editor](apps.md#launch-as).
> Games still run in Private Stream's own session, off your desktop. For an empty private session
> to launch things into, add an entry with no command set to Host default; right-click its empty
> screen to open the session menu. Other ways to put a desktop on
> the stream are modes of their own: Host Virtual Display adds an extra display sized to the client,
> and on Hyprland, Desktop Takeover moves the live desktop onto a temporary client-sized output and
> blanks the original displays until the stream ends. Both are one click in the web UI under
> Settings → Audio/Video.

To pick a different mode later, such as Gamescope, a virtual display, or a dummy plug, see
[Launch modes and capture paths](launch-modes.md). [Configuration](configuration.md) explains every
setting, and [Runtime and streaming model](runtime.md) explains what these keys actually change.

## 4. Pair a client

Pick whichever fits your network:

- **Trusted Pair** for Nova on a trusted LAN, a TOFU flow that auto-approves Nova's first pairing
  from a configured trusted subnet.
- **QR pairing** for Nova for Android.
- **Manual PIN** for Moonlight, Artemis and every other client. Moonlight cannot ask for Trusted
  Pair or scan the QR code.

New devices use **Game Control** by default, which is the least-privilege preset that can browse,
launch, and control a game. **Browse & Watch** is intentionally read-only: it can list the library
and join an existing stream, but it cannot start Desktop or a game and cannot send input. Existing
paired devices keep their saved access until you change it under **Devices → Edit Access**.

For Moonlight on Linux, Android, or another non-Nova client: add the Polaris host in Moonlight and
leave its displayed four-digit PIN open. In the Polaris web UI, open **Devices → Manual PIN**, enter
that PIN, keep **Game Control** selected, and choose **Send**. Return to Moonlight and refresh the
host if its library does not appear immediately. Steam is not required on the client.

The Moonlight flow step by step, including which client settings matter, is in
[Play with Moonlight](moonlight.md). What Moonlight, Artemis and Browser Stream get next to Nova is
in [Clients](compatibility.md#clients). The access presets and the device editor are in
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
