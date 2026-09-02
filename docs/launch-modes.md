# Choose where games run

Choose what appears on the stream, whether the host monitors are used, and which option best fits your gaming PC.

Every card under **Settings → Audio/Video → Where games run** starts with what the player will experience. Pick the card that matches your setup and Polaris chooses the capture method automatically. Backend names such as labwc, PipeWire, DMA-BUF, and SHM remain available under **Technical details**, but you do not need to understand them to make a safe choice.

## Which mode should I use?

| Your setup | Start with |
|---|---|
| NVIDIA gaming PC | Private Stream (GPU-native) |
| AMD gaming PC | Private Stream |
| Intel GPU | Private Stream |
| Streaming mostly to a handheld | Private Stream |
| Steam-first host, Deck-style sessions | Gamescope Stream |
| Two GPUs in the host | Private Stream, then see [the multi-GPU note](#if-you-have-an-amd-card) |
| Dedicated streaming PC with a dummy plug | Headless Dongle |
| "I want my actual desktop on the stream" | Mirror Desktop |
| An extra screen sized to the client | Host Virtual Display |
| Several family members at once | Not available yet, see [Family Mode](#family-mode-isolated) |

> [!TIP]
> When in doubt, pick **Private Stream** when its card is available. A grey card names the missing host tool instead of accepting a launch that cannot work. Select the card and save it for the next launch; an active stream may keep the previous mode until relaunch. You can change the saved choice back later, and Mission Control [shows you what Polaris actually started](#check-what-your-stream-is-using).

## The modes

### Private Stream

Your game runs in its own invisible session. Your desktop never flickers, resizes, or shows the game, and nothing you do on the desktop leaks into the stream. It even works on a host with no monitor attached and nobody logged in: pair it with `sudo -H polaris --setup-host --enable-headless-boot` for a console-style box that streams straight from power-on ([Bazzite guide](bazzite.md#headless-boot-and-deck-images) has the walkthrough).

- **Best for:** most setups, and the preferred path when you stream to a handheld.
- **One caveat:** it requires both `labwc` and `wlr-randr` on the host `PATH`; the card is greyed out and names the missing tool until both are ready. Once running, the built-in Desktop entry looks like an empty screen until you launch something into it. That is normal, not broken. Right-click the empty screen to open the session menu, or use Mirror Desktop if you actually wanted your desktop.

### Private Stream (GPU-native)

The same private session, tuned so frames can stay on the GPU the whole way from game to encoder, which is the fastest capture Polaris has. To keep that path, the private session may run as a window under your host desktop instead of fully hidden.

- **Best for:** NVIDIA cards. This is the recommended NVIDIA mode.
- **One caveat:** in its windowed form it needs a desktop session running on the host, so it is not a fit for a machine that sits at the login screen unattended.

### Gamescope Stream

Gamescope is the small compositor Valve built for the Steam Deck's Game Mode: it runs one game at a time, fullscreen, in a session it fully owns, and handles scaling and frame pacing itself. This mode runs your games under Gamescope and streams that session, either by joining a Gamescope that is already idle on the host or by starting its own.

- **Best for:** Steam-first hosts and games that behave best inside Game Mode.
- **One caveat:** it needs `gamescope` installed on the host. The card is greyed out until it is. Some packages install a helper for this; see [Building Polaris](building.md).

### Family Mode (isolated)

Not selectable yet. It appears under the collapsed **Planned modes** section so the idea has a home without looking launch-ready. The per-person isolated sessions it describes are reserved for community work that has not landed ([PR #226](https://github.com/papi-ux/polaris/pull/226)). No dates promised. The closest thing today is Private Stream, which isolates the stream from the desktop but not one family member from another.

### Host Virtual Display

Adds an extra screen to your real desktop, sized to match the client, and streams that screen. Your desktop stays usable on the physical monitors while the stream gets its own space.

- **Best for:** using the stream like a second monitor for your normal desktop session.
- **One caveat:** adding and removing a display can make your desktop icons and windows rearrange, exactly as plugging in a real monitor can.

### Headless EVDI

Not selectable yet. It appears under **Planned modes** and is reserved for the same community work as Family Mode ([PR #226](https://github.com/papi-ux/polaris/pull/226)).

### Headless Dongle

For hosts with a dummy plug (an HDMI or DisplayPort dongle): the desktop moves onto the dongle. In **Privacy** mode the real panel goes dark after one-time portal approval is saved; Polaris keeps it on during that approval so the picker remains visible. In **Off** mode the panel stays primary and the desktop extends onto the dongle.

- **Best for:** a dedicated streaming PC, with optional panel blanking when privacy matters.
- **One caveat:** it needs the dongle plugged in and both a streaming output and a primary output configured. Headless Dongle itself is a host setting, so a client cannot turn that physical swap on for one game. A client may choose another supported mode for one launch; Polaris leaves the dongle swap inactive for that session and restores the host default afterward.

### Mirror Desktop

Streams the desktop you see, like a remote desktop tool. Whatever is on screen is on the stream.

- **Best for:** non-gaming use, quick checks, and "I just want my computer from the couch".
- **One caveat:** zero isolation. Notifications, chats, and everything else on your desktop are visible to the client. This is also the mode Polaris falls back to when it does not recognize the configured mode.

Setting modes from the config file instead of the web UI? The key and value names live in [Configuration](configuration.md#linux-display-modes).

## If you have an NVIDIA card

Pick **Private Stream (GPU-native)**. NVIDIA with NVENC is the most heavily tested Polaris path, and it supports the fast capture route where frames never leave the GPU.

Use the official packages when you can. If you build from source, build with CUDA enabled; without it, capture takes a slower copy through system memory, and the log says so ([Troubleshooting](troubleshooting.md#nvidia-kms-capture-issues) shows the exact line).

> [!WARNING]
> Plain **Private Stream** with GPU-native capture off can refuse the very first launch on a fresh NVIDIA setup with a 503 error, even though the GPU is healthy. If that happens, switch to Private Stream (GPU-native), restart Polaris, and retry. [Troubleshooting](troubleshooting.md#headless-session-does-not-start-cleanly) explains why.

## If you have an AMD card

Pick **Private Stream** and expect it to just work. AMD hosts encode with VA-API through the Mesa drivers your distro already ships, so there is usually nothing to install.

One thing to know, stated plainly: Polaris deliberately uses a slower-but-stable capture path on AMD, because the faster GPU-native path has crashed real machines and stays off until it is proven safe per driver generation. Seeing **SHM** as the capture path on an AMD host is the intended, healthy state, not a misconfiguration. At 4K and high refresh rates this safe path can become the frame rate limit; at 1080p and 1440p it usually is not.

Two practical settings:

- Keep HDR off for now (`hdr_mode = 0` in the config file) and prefer HEVC. AMD HDR handling is still being validated.
- Two GPUs in the machine (for example a Ryzen iGPU plus a Radeon card)? Tell Polaris which one to use with `adapter_name`. [Stream paths](stream-paths.md#render-device-labwc-runtime) explains how the device is chosen.

### Intel

Same advice as AMD: Private Stream, Mesa VA-API, expect SHM capture. On an Arc discrete card, set `adapter_name` so Polaris picks the Arc GPU rather than the integrated one.

## Linux setup checklist

The **Advanced & diagnostics** disclosure on the Video/Audio settings page shows a short checklist for the selected launch mode. Each checklist step is one line in the UI; this section carries the full detail behind each step.

### Private Stream checklist

Private Stream (labwc) is the solid default: apps stay off the host desktop, and capture uses wlroots. Nothing extra is needed beyond `labwc` and `wlr-randr` on the host `PATH` (see [Private Stream](#private-stream)). The encoder, bitrate, and HDR settings below the checklist all apply to this path.

### Gamescope Stream checklist

Gamescope Stream attaches to an idle `gamescope-0` unit or spawns an owned headless Gamescope session, and the portal captures it. It needs `gamescope` on the host `PATH`, and some host setups also need private portal units; that part is host and packaging specific, see [the gamescope notes in Stream paths](stream-paths.md#relation-to-gamescope). The web UI's labwc-only flags (cage compositor, GPU-native capture preference) are ignored on this path. Encoder, bitrate, and HDR settings below the checklist still apply.

### Headless Dongle checklist

Set the streaming output (the dummy plug) and the primary output (the real panel), pick a swap mode, and save. Capture goes through the portal after Polaris prepares the display topology. The **Detect connectors** button fills empty output fields from host discovery; the swap behavior itself is described under [Headless Dongle](#headless-dongle), and the `headless_swap_mode` values live in [Configuration](configuration.md#common-options).

### Mirror Desktop checklist

Mirror Desktop captures the visible host session through the portal and needs no extra setup. Prefer Private Stream or Gamescope Stream when apps should stay isolated from the desktop.

### Encoder and quality

Set the encoder (`nvenc` on NVIDIA, `vaapi` on AMD and Intel Mesa hosts), the bitrate, and optionally Auto Quality; the same settings apply to the labwc and gamescope paths alike. With Auto Quality on, Polaris balances bitrate and profile recovery for the selected path instead of asking you to tune them by hand.

### GPU-native capture preference

Leave the GPU-native preference off unless session health shows SHM or system-memory fallback on a host that should support GPU-resident capture; the flag does not apply to Gamescope Stream. When the preference is on, Polaris may run labwc windowed instead of fully hidden so DMA-BUF capture can stay GPU-resident. NVIDIA hosts running true headless (NVENC, headless labwc) with the preference off can refuse the first launch with a cold-cache 503; switch to Private Stream (GPU-native) or enable the preference, restart Polaris, and retry, as covered in [the NVIDIA warning above](#if-you-have-an-nvidia-card).

## How capture works

Polaris selects the capture path after the launch mode is chosen:

- **GPU-native** keeps frames on the GPU from the game to the encoder. It is the fastest path when the host supports it.
- **System-memory** copies frames through RAM. That can be the intended safe path on AMD and Intel, not a failure.

You do not need to select either path separately. Mission Control reports the path that actually ran, and Doctor uses the measured result when explaining a bottleneck.

## Check what your stream is using

Mission Control shows the mode Polaris actually started and the capture path it is using, and never pretends: if you asked for one thing and the host could only do another, both are shown.

Reading the capture path:

- **GPU-native** means frames stay on the GPU from game to encoder.
- **SHM** means frames take a copy through system memory. Slower, not broken, and often the expected state on AMD and Intel hosts.

If a stream misbehaves, the exact reason codes and what to do about them are in [Troubleshooting](troubleshooting.md#vaapi-or-software-encode-fallback).

## Streaming your desktop just once

You do not need to change the host mode to briefly share your desktop. Any Moonlight-protocol client can add `mirrorDesktop=1` to a launch request to mirror the desktop for that single session, and Nova exposes this as a launch option. The host configuration is untouched.

Headless Dongle itself cannot be requested as a per-launch override because it rearranges physical outputs. If Headless Dongle is the host default, a client can still choose Mirror Desktop or another supported mode for one session; the saved host setting returns on the next normal launch.

> [!TIP]
> The reverse situation has a switch too: a private launch is refused when desktop Steam is already running on the host, because starting Steam in the private session would fight the one on your screen. If you would rather have Polaris quit desktop Steam and continue, turn on **Close desktop Steam for private launches** on that app in the Apps editor. Polaris waits for Steam to fully exit before starting the stream. Clients can also request it per launch with `closeDesktopSteamForPrivate=1`.

## Under the hood (optional reading)

- [Runtime and streaming model](runtime.md): how capture paths and fallback decisions actually work.
- [Configuration](configuration.md): every key and value, including the legacy booleans.
- [Stream paths (plugin contract)](stream-paths.md): the developer contract behind the mode cards.
