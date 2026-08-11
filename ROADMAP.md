# Polaris Roadmap

Polaris is public and usable today, but it is still early. This roadmap explains
where the Linux host is heading and where testing helps most. **Direction, not a
release calendar:** priorities can move when real hardware, regressions, or
measurements teach us something better.

For the shared host-and-client view, see the
[Polaris + Nova public roadmap](https://papi-ux.com/docs/roadmap/).

## How to read this roadmap

- **What stays true** describes the compatibility and product boundaries users
  can rely on now.
- **Now** is active reliability and support work.
- **Next** is work we expect to evaluate after the current foundations are sound.
- **Explore later** records real goals, not promised releases or dates.

## What stays true

- Polaris remains a Linux-first, self-hosted streaming host.
- Standard Moonlight-compatible clients remain first-class; Nova adds a richer
  Polaris-aware experience but is not required.
- Headless streaming should protect the desktop, restore host state, and clean up
  every process and display it creates.
- Fedora 44 and Arch are the recommended package paths. SteamOS has a versioned
  package; Bazzite and Ubuntu are tester paths; openSUSE Tumbleweed remains a
  source-build path.
- Windows and macOS host ports are not planned. The work is deliberately focused
  on making the Linux host excellent.

## Now — make Linux streaming dependable

- Harden start, stop, recovery, and teardown so failed sessions do not leave
  workloads, virtual displays, input devices, or stale state behind.
- Keep Mission Control diagnostics useful without requiring users to spelunk
  through giant logs.
- Continue validating headless capture, compositor behavior, input isolation, and
  encoder selection across NVIDIA and AMD/Mesa hosts.
- Keep Fedora, Arch, SteamOS, Bazzite, Ubuntu, and openSUSE guidance honest about
  what is recommended, tested, experimental, or source-build only.
- Make Polaris and Nova version pairing easier to understand while preserving
  standard Moonlight-client compatibility.

## Next — make the current path smoother

- Tighten frame pacing, queue behavior, send scheduling, bitrate pacing, and
  120 Hz behavior one measured change at a time.
- Improve capture and encoder diagnostics so the web console reports the path that
  actually ran, including safe fallbacks.
- Broaden AMD/VAAPI and software-encode validation without weakening the heavily
  tested NVIDIA/NVENC path.
- Expand reproducible smoke coverage for launch, pairing, input, stream, resume,
  stop, cleanup, and Browser Stream behavior.
- Prefer improvements that users can compare and roll back over large bundles of
  unrelated performance changes.

## Build alongside — clean seams, no hidden rewrite

When current work already touches session lifecycle, capture, encode, media,
telemetry, or client-facing contracts, Polaris may extract a small
protocol-neutral boundary first. That should make today's Moonlight-compatible
path easier to test and maintain. It is not permission to build a replacement
transport in disguise.

## Explore later — only if evidence says yes

### A native streaming path

The current Moonlight-compatible path remains the production and fallback path.
A native Polaris/Nova path is research, not a foregone rewrite. It should proceed
only if measured work shows that the remaining limitation belongs to the
transport itself and the new path can preserve pairing, input, audio, codecs,
resume, diagnostics, rollback, and standard-client compatibility.

### Proper HDR10+

The goal is end-to-end dynamic metadata that remains attached to the right frames
from source through capture, encode, transport, decode, and display, with honest
HDR10 and SDR fallback. A device advertising HDR10+ is not enough, and static HDR
or host tone mapping must not be relabeled as HDR10+ support.

### True 240 fps

The goal is 240 unique frames per second through render, capture, encode,
transport, decode, and physical presentation on a validated 240 Hz display. A
240 Hz mode, duplicated frames, frame generation, or a 240 fps camera by itself is
not proof of a 240 fps stream.

Passing each goal separately does not prove a combined HDR10+ at 240 fps profile.
That combination would need its own exact hardware, codec, color, bitrate,
quality, latency, thermal, fallback, and rollback evidence.

### Other later work

- Wider distro packaging when user demand justifies the maintenance cost.
- Continued Browser Stream validation after the Chromium/WebTransport path proves
  useful outside development.
- Vulkan Video, Linux 4:4:4, honest HDR headless operation, session continuity,
  and other hardware-specific work behind separate compatibility gates.

## What this roadmap does not promise

- No feature here has an implied date until it appears in a published release.
- Research does not require users to migrate away from a working client or host.
- A benchmark win is not a release if cleanup, compatibility, visual quality, or
  rollback gets worse.
- The roadmap may change when public testing or measured evidence disproves an
  assumption.

## Useful feedback

- Distro, GPU, driver, compositor, encoder, launch mode, and client details for
  successful and failed streams.
- Bounded logs and screenshots for pairing, capture, encoder, input, resume, or
  cleanup failures.
- Bazzite Desktop/Game Mode, Ubuntu, openSUSE, SteamOS, AMD/VAAPI, and unusual
  display-topology reports.
- Comparisons between Nova and another Moonlight-compatible client on the same
  Polaris host and game.
