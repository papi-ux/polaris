# Frequently Asked Questions

Common questions about hardware requirements, client compatibility, coexisting with other GameStream
hosts, HDR, and the optional AI features. If your question is about a specific failure, start with
[Troubleshooting](troubleshooting.md) instead.

## Hardware and encoding

### Do I need an NVIDIA GPU?

No. NVIDIA and NVENC are the most heavily tested path today, but AMD and Intel Mesa VAAPI and
software encode are supported. GPU-native DMA-BUF capture is an optimization request on both NVIDIA
and AMD-capable stacks, and Polaris reports the actual path when a host falls back to SHM or system
memory.

### Can Polaris stream 10-bit to an SDR handheld screen?

Yes, if the client explicitly requests a 10-bit path and the active encoder and runtime support
Main10. See [Runtime and Streaming Model](runtime.md) for the difference between 10-bit SDR and true
HDR.

### Can Polaris stream true HDR on Linux?

Yes, but Polaris only advertises true HDR when the active capture path reports HDR display metadata.
Headless labwc and wlroots sessions stay honestly SDR until the runtime can provide real metadata.
[Runtime and Streaming Model](runtime.md) has the details.

## Clients

### Does Polaris work with Moonlight on iOS, macOS, and PC?

Yes. Polaris speaks the Moonlight protocol, so any Moonlight client can connect. Polaris-specific
features — launch-mode selection, watch mode, optimization guidance, and richer session state —
require Nova on Android.

### Does Moonlight lock streams to 60 FPS?

No. Moonlight can request higher frame rates on clients that expose them, and Polaris treats the
client's requested display mode as the ceiling. If a client requests `1280x800x60`, Polaris will not
force a 90 FPS optimization above that request even when the device profile supports it.

### Can multiple people watch the same stream?

Yes. Set `max_sessions` above `1`. Polaris tracks owner and viewer roles explicitly, and passive
watch mode is designed so a second client can observe without taking over. Viewers match the active
owner profile rather than silently creating a different, downgraded stream.

## Coexisting with other hosts

### Do I need to uninstall Sunshine before trying Polaris?

No. Polaris keeps its host configuration separate at `~/.config/polaris`, so installing it should not
remove or overwrite an existing Sunshine setup. For testing, stop Sunshine before starting Polaris,
because both are GameStream hosts and can collide on the same default ports and discovery records.

```bash
systemctl --user stop sunshine
systemctl --user enable --now polaris
```

If your Sunshine install runs as a system service instead of a user service, use the matching service
command for your distro. Switch back by stopping Polaris and starting Sunshine again.

## Desktop environments and sessions

### Does headless mode work on Hyprland, Sway, or GNOME?

Yes. The headless `labwc` runtime creates its own Wayland instance, so it is not tied to one desktop
environment. Polaris is tested most heavily on KDE Plasma Wayland, but the model is not KDE-specific.

### My KDE layout gets corrupted after streaming

That failure mode is the reason Polaris exists. Set `headless_mode = enabled` and
`linux_use_cage_compositor = enabled`, and Polaris stops treating your physical displays as the
stream path.

### Steam Big Picture shows a black screen or tiny window

First clear Steam's HTML cache:

```bash
rm -rf ~/.local/share/Steam/config/htmlcache/
```

Then avoid MangoHud on Steam Big Picture and Steam/Proton launches. Polaris and Nova warn about this
because MangoHud can crash helper processes before the session gets a usable frame.

## Pairing

### How does Trusted Pair work?

Trusted Pair is Polaris' TOFU flow. If the client is on a configured trusted subnet, Polaris can
auto-approve first pairing. QR and manual PIN pairing remain available if you want a stricter or more
traditional flow.

## AI features

### Is AI required?

No. Core streaming, pairing, library management, and diagnostics work without AI and without a cloud
account.

### How does the AI optimizer work?

The AI optimizer is optional and disabled by default. When enabled, it sends device specs, app
metadata, and recent session history to the provider you configure: Anthropic, OpenAI, Gemini, or a
local OpenAI-compatible endpoint such as Ollama or LM Studio. Results are cached locally.
