# FAQ

Common questions about hardware requirements, client compatibility, coexisting with other GameStream
hosts, HDR, and the optional AI features. If your question is about a specific failure, start with
[Troubleshooting](troubleshooting.md) instead.

## Hardware and encoding

### Do I need an NVIDIA GPU?

No. NVIDIA and NVENC are the most heavily tested path. AMD encodes through Mesa VA-API and is
supported. Intel uses the same VA-API path but no Intel GPU has been through release validation yet,
and software encode works anywhere as a fallback. On AMD and Intel, capture copies each frame through
system memory on purpose, and Polaris reports the path it actually used.
[Support and compatibility](compatibility.md#gpu-and-encoding) has the status of each vendor, and
[Launch modes and capture paths](launch-modes.md) has per-vendor recommendations.

### Can Polaris stream 10-bit to an SDR handheld screen?

Yes, if the client explicitly requests a 10-bit path and the active encoder and runtime support
Main10. See [Runtime and streaming model](runtime.md) for the difference between 10-bit SDR and true
HDR.

### Can Polaris stream true HDR on Linux?

Yes, on two routes, both proven on NVIDIA: Mirror Desktop with `capture = kms` streaming an HDR
monitor, and Gamescope Stream with a gamescope that carries Polaris's 10-bit capture patch. Polaris
only advertises true HDR when the active capture path reports HDR display metadata, so Private
Stream, whose labwc session has none, stays SDR. [HDR by stream mode](compatibility.md#hdr-by-stream-mode)
has the conditions, and [Runtime and streaming model](runtime.md#hdr-and-main10) the details.

## Clients

### Does Polaris work with Moonlight on iOS, macOS, and PC?

Yes. Polaris speaks the Moonlight protocol, so any Moonlight client can pair and stream. A launch
mode per launch, Play Setup, Spaces, PyroWave, watching another player's stream and host sleep need
Nova, for Android or Linux. [What each client gets](compatibility.md#clients) compares Nova,
Moonlight, Artemis and Browser Stream, and says what the host can set for a Moonlight player
instead.

### Does Moonlight lock streams to 60 FPS?

No. Moonlight can request higher frame rates on clients that expose them, and Polaris treats the
client's requested display mode as the ceiling. If a client requests `1280x800x60`, Polaris will not
force a 90 FPS optimization above that request even when the device profile supports it.

### Which Moonlight settings matter with Polaris?

Resolution and frame rate, because Polaris treats the request as a ceiling and falls back to the
host's Display Planner mode when it cannot be met; bitrate, because Auto Quality starts from it;
codec and HDR, because the host has to offer the matching encoder profile. The full table is in
[Play with Moonlight](moonlight.md#5-match-the-stream-to-the-device).

### Can Moonlight choose the launch mode?

No, but each app can. The host's saved launch mode applies unless the app's **Launch as** names
another mode; only Nova can pick a mode per launch. A protocol client can ask for a one-off desktop
mirror with `mirrorDesktop=1`, without changing the host setting, but an app set to another fixed
mode refuses it with `app_launch_mode_pinned`. Artemis's virtual display option asks for Host Virtual
Display on an app set to Host default. Standard Moonlight sends neither; give each way of playing
its own app entry under [Launch as](apps.md#launch-as).

### Moonlight shows the library but cannot start anything

The device has **Browse & Watch** access, which lists the library and joins an existing stream but
cannot launch or send input. Open **Devices**, choose **Edit Access** on that device, and pick
**Game Control**. The presets are explained in [Pair and manage devices](devices.md#access-presets).

### How do we play couch co-op on one device?

Pair a second controller to the device that streams, such as a Bluetooth pad next to a
Retroid's own controls, and press a button on each. Each pad becomes a player on the host in the
order its first button press arrives: the first is player 1, the next player 2. In Nova,
**Players** in Command Center lists who is which player, names a pad that has not pressed
anything yet, and **Reassign** lets everyone press again in the order you want without ending
the stream. On a handheld whose own controls Android reports as built in, those controls stay
player 1. Nova's **Automatic Gamepad Presence Detection** must be on; with it off, every pad is
player 1. On the host, the controller test in **Troubleshooting** lists each player and the pad
it emulates. A device that only watches a stream never adds a pad of its own.

### Does the Steam Controller (2026) work?

On the host, yes: a client that says it has a Steam Controller (Moonlight sends this type) gets
an emulated DualSense, the one pad that carries the controller's gyro and its touchpads. Its two
touchpads land on the left and right halves of the DualSense's touchpad, so a game sees both
thumbs. The back grip buttons have no place on a DualSense and are not passed on. Android shows
the controller to apps only as a keyboard and mouse, so Nova cannot read its sticks, gyro or
touchpads yet.

### Can multiple people watch the same stream?

Yes, from Nova. A Nova device can watch the stream another device is playing, and a Nova device
with **Browse & Watch** access can only watch. `max_sessions` is `2` by default, which leaves room for one
watcher; raise it for more. Polaris tracks owner and viewer roles explicitly, and passive watch mode
is designed so a second client can observe without taking over. Viewers match the active owner
profile rather than silently creating a different, downgraded stream. Moonlight cannot watch: it
never asks to, so the host refuses its launch while another device owns the stream.

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

Trusted Pair is Polaris' TOFU flow for Nova. When Nova asks for it from a subnet listed under
**Settings, Network, Trusted Subnet Auto-Pairing**, Polaris approves the first pairing without a PIN.
Moonlight and Artemis never ask for it, so they pair with the PIN. QR and manual PIN pairing remain
available if you want a stricter or more traditional flow.

## AI features

### Is AI required?

No. Core streaming, pairing, library management, and diagnostics work without AI and without a cloud
account.

### What does AI do in Doctor and launch optimization?

Launch presets and Doctor actions are deterministic and work without AI. When AI explanations are
enabled, Polaris sends a bounded structured evidence bundle to the provider you configure:
Anthropic, OpenAI, Gemini, or a local OpenAI-compatible endpoint such as Ollama or LM Studio. The
response can explain the evidence in plain language, but it cannot define an action, target setting,
Doctor confidence override, or launch policy. See [Fix a bad stream with Doctor](doctor.md).
