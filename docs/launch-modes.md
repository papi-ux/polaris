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
| My live Hyprland desktop, with the host monitors dark | Desktop Takeover |
| Several family members at once | Not available yet, see [Family Mode](#family-mode-isolated) |

> [!TIP]
> When in doubt, pick **Private Stream** when its card is available. A grey card names the missing host tool instead of accepting a launch that cannot work. Select the card and save it for the next launch; an active stream may keep the previous mode until relaunch. You can change the saved choice back later, and Mission Control [shows you what Polaris actually started](#check-what-your-stream-is-using).

## The modes

### Private Stream

Your game runs in its own invisible session. Your desktop never flickers, resizes, or shows the game, and nothing you do on the desktop leaks into the stream. It even works on a host with no monitor attached and nobody logged in: pair it with `sudo -H polaris --setup-host --enable-headless-boot` for a console-style box that streams straight from power-on ([Bazzite guide](bazzite.md#headless-boot-and-deck-images) has the walkthrough).

- **Best for:** most setups, and the preferred path when you stream to a handheld.
- **One caveat:** it requires both `labwc` and `wlr-randr` on the host `PATH`; the card is greyed out and names the missing tool until both are ready. The built-in Desktop entry still shows your real desktop, because it has **Mirror the host desktop** turned on in the [app editor](apps.md#runtime-behavior). An entry with no command and that setting off opens an empty private session instead. That empty screen is normal, not broken: right-click it to open the session menu.

### Private Stream (GPU-native)

The same private session, tuned so frames can stay on the GPU the whole way from game to encoder, which is the fastest capture Polaris has. To keep that path, the private session may run as a window under your host desktop instead of fully hidden.

- **Best for:** NVIDIA cards. This is the recommended NVIDIA mode.
- **One caveat:** in its windowed form it needs a desktop session running on the host, so it is not a fit for a machine that sits at the login screen unattended.

### Gamescope Stream

Gamescope is the small compositor Valve built for the Steam Deck's Game Mode: it runs one game at a time, fullscreen, in a session it fully owns, and handles scaling and frame pacing itself. This mode runs your games under Gamescope and streams that session, either by joining a Gamescope that is already idle on the host or by starting its own.

- **Best for:** Steam-first hosts and games that behave best inside Game Mode.
- **One caveat:** it needs `gamescope` installed on the host. The card is greyed out until it is. Some packages install a helper for this; see [Build from source](building.md).

### Family Mode (isolated)

Not selectable yet. It appears under the collapsed **Planned modes** section so the idea has a home without looking launch-ready. The per-person isolated sessions it describes are reserved for community work that has not landed ([PR #226](https://github.com/papi-ux/polaris/pull/226)). No dates promised. The closest thing today is Private Stream, which isolates the stream from the desktop but not one family member from another.

### Host Virtual Display

Adds an extra screen to your real desktop, sized to match the client, and streams that screen. Your desktop stays usable on the physical monitors while the stream gets its own space.

- **Best for:** using the stream like a second monitor for your normal desktop session.
- **One caveat:** adding and removing a display can make your desktop icons and windows rearrange, exactly as plugging in a real monitor can.
- **On KDE Plasma 6:** KWin creates a new screen for each stream, so no EVDI, dummy plug or spare connector is needed. The screen is named `Virtual-polaris-0` (`-1` for a second one held at the same time), placed to the right of your monitors at the client's resolution, and removed when the stream ends. It is ranked after all of your screens, never made the primary one, so each desktop, its icons and its panel stay on the monitor they were on; instead, windows that open while the stream runs, the game included, are moved onto the stream screen. A window you open at the desk during a stream moves there too, except the desktop's own prompts such as polkit's; drag it back or use Meta+Shift+Left. Polaris asks KWin for the client's refresh rate with kscreen-doctor, so 120 and 144 Hz work when KWin accepts the mode; if it does not, the stream runs at the rate the screen got and the log says so. KWin virtual screens carry no HDR, so an HDR request streams SDR and the Doctor says why. kscreen-doctor must be installed; it comes with Plasma.
- **Choosing the backend:** Automatic uses the KWin screen on KDE Plasma 6, then EVDI when it can create a display, then Hyprland, then kscreen-doctor. On Plasma the KWin screen comes first even with EVDI loaded, because games open on it; an EVDI screen there is a monitor like any other, so the game opens on your primary one. Set `linux_virtual_display_backend` (Backend, on the Audio/Video tab's Virtual Display panel) to use one of them only; a chosen backend that cannot run refuses the launch with its reason instead of falling back.
- **Borrowing a connector with kscreen-doctor:** the last resort, used when nothing above can add a screen. kscreen-doctor cannot add one. It borrows the connector saved as `linux_streaming_output`, makes it the primary screen, switches it to the client's resolution when the connector offers that mode, and restores the layout afterward, so point it at a spare connector with a dummy plug. Pointed at a monitor you use, it takes over that monitor, which looks the same as Mirror Desktop; a connector with nothing plugged in has no screen to turn on, so the launch fails. The settings page lists the connectors it finds and says when a new choice still needs a restart.

### Desktop Takeover

Creates a client-sized virtual output in the current Hyprland session, moves the live workspaces and windows onto it, and turns the original active displays off with DPMS. On the final disconnect, Polaris immediately turns those displays back on, returns every recorded workspace to its original output, verifies the restoration, and only then removes the virtual output.

- **Best for:** playing from the same Hyprland desktop while keeping the host monitors dark and matching the client's display size.
- **One caveat:** this is an explicitly disruptive mode and is only selectable when a live Hyprland session, `hyprctl`, and an EVDI or native wlroots virtual-output backend are available. Polaris fails closed if it cannot prove or later recover the layout; it does not hold this mode open for resumable disconnects.

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

Use the official packages when you can: the Fedora, Arch and Ubuntu packages are built with CUDA. If you build from source, build with CUDA enabled; without it, every capture path copies each frame through system memory before NVENC, whatever mode you pick, and both the log and the host Doctor say so before you stream ([Troubleshooting](troubleshooting.md#capture-is-on-the-cpu) lists every cause).

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

### Hybrid laptops (an Intel or AMD iGPU next to an NVIDIA card)

On a laptop the desktop usually renders on the integrated GPU while Polaris, left on auto, picks the NVIDIA card for encoding because it is the discrete one. That split works, but know what it costs: Mirror Desktop captures into system memory (frames from the iGPU cannot be handed to the NVIDIA card as DMA-BUF), and the NVIDIA card is woken from its power-saving state for every stream. The Doctor names the split as `linux_gpu_adapter_mismatch` with both ways out:

- Keep everything on the iGPU: `adapter_name = /dev/dri/renderD128` (or whichever node the Doctor names as the compositor's) and `encoder = vaapi`. Slower encoder, no cross-GPU traffic, the NVIDIA card stays asleep.
- Keep NVENC: leave the split and turn off NVIDIA runtime power management (`options nvidia NVreg_DynamicPowerManagement=0x00` in `/etc/modprobe.d/nvidia-pm.conf`, rebuild the initramfs, reboot). Costs battery, removes the sleep/wake cycle that has frozen laptops.

If the whole machine freezes during a stream, [Troubleshooting](troubleshooting.md#whole-machine-freeze-on-a-hybrid-laptop) has the order to test things in.

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

### Desktop Takeover checklist

Desktop Takeover requires an active Hyprland session and `hyprctl`, plus a backend that can create a new output. Polaris records the exact source monitor and workspace layout before moving anything, uses DPMS rather than disabling connectors, restores that layout before destroying the temporary output, and retains its recovery record if restoration cannot be proven.

### Encoder and quality

Set the host encoder (`nvenc` on NVIDIA, `vaapi` on AMD and Intel Mesa hosts), the bitrate, and optionally Auto Quality; the same settings apply to the labwc and gamescope paths alike. A capable Nova client can also keep that host policy or request an advertised encoder for one game. Auto may live-probe and fall back; a named backend is strict and fails the launch if it cannot initialize. The session choice is restored when the session ends and never rewrites the host configuration. With Auto Quality on, Polaris balances bitrate and profile recovery for the selected path instead of asking you to tune them by hand.

### GPU-native capture preference

Leave the GPU-native preference off unless the host Doctor's capture forecast or session health shows SHM or system-memory fallback on a host that should support GPU-resident capture; the flag does not apply to Gamescope Stream. When the preference is on, Polaris may run labwc windowed instead of fully hidden so DMA-BUF capture can stay GPU-resident. NVIDIA hosts running true headless (NVENC, headless labwc) with the preference off can refuse the first launch with a cold-cache 503; switch to Private Stream (GPU-native) or enable the preference, restart Polaris, and retry, as covered in [the NVIDIA warning above](#if-you-have-an-nvidia-card).

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

Headless Dongle itself cannot be requested as a per-launch override because it rearranges physical outputs. Desktop Takeover can be selected for one launch because its recovery record and virtual output are session-owned; an explicit Mirror Desktop request still takes precedence. If Headless Dongle is the host default, a client can still choose Mirror Desktop or another supported mode for one session; the saved host setting returns on the next normal launch.

> [!TIP]
> The reverse situation has a switch too: a private launch is refused when desktop Steam is already running on the host, because starting Steam in the private session would fight the one on your screen. If you would rather have Polaris quit desktop Steam and continue, turn on **Close desktop Steam for private launches** on that app in the Apps editor. Polaris waits for Steam to fully exit before starting the stream. Clients can also request it per launch with `closeDesktopSteamForPrivate=1`.

## Under the hood (optional reading)

- [Runtime and streaming model](runtime.md): how capture paths and fallback decisions actually work.
- [Configuration](configuration.md): every key and value, including the legacy booleans.
- [Stream paths (plugin contract)](stream-paths.md): the developer contract behind the mode cards.
