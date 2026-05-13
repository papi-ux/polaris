<div align="center">

<img src="docs/screenshots/logo-polaris.svg" width="72" alt="Polaris" />

# Polaris

**Self-hosted game streaming for Linux.**

Stream PC games to Nova and Moonlight clients without letting the stream take
over your desktop. Polaris combines an isolated Linux compositor runtime,
GameStream-compatible pairing, GPU-aware capture, and a web console that shows
what the host is actually doing.

[![Stars](https://img.shields.io/github/stars/papi-ux/polaris?style=for-the-badge&color=7c73ff&labelColor=1a1a2e)](https://github.com/papi-ux/polaris/stargazers)
[![License](https://img.shields.io/github/license/papi-ux/polaris?style=for-the-badge&color=4c5265&labelColor=1a1a2e)](LICENSE)
[![Release](https://img.shields.io/github/v/release/papi-ux/polaris?style=for-the-badge&color=4ade80&labelColor=1a1a2e&label=latest)](https://github.com/papi-ux/polaris/releases/latest)

[Quick Start](#quick-start) · [Install](#install) · [Compatibility](#compatibility) · [Tour](#tour) · [Nova](#use-with-nova) · [Docs](#docs) · [FAQ](#faq) · [Security](SECURITY.md) · [Changelog](docs/changelog.md) · [Roadmap](ROADMAP.md)

**Support**: [Issues](https://github.com/papi-ux/polaris/issues) · [Discussions](https://github.com/papi-ux/polaris/discussions)

<br/>

<picture>
  <source media="(prefers-color-scheme: light)" srcset="docs/screenshots/polaris-showcase.gif" width="820" />
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/polaris-showcase-oled.gif" width="820" />
  <img src="docs/screenshots/polaris-showcase-oled.gif" width="820" alt="Polaris dashboard, live session view, game library, and pairing flow" />
</picture>

</div>

> [!IMPORTANT]
> Polaris is a Linux host today. Fedora 42, Fedora 43, Fedora 44, and Arch Linux are the recommended package paths. Bazzite and Ubuntu 24.04 packages are available for testers, but they need more real-hardware coverage.

> [!NOTE]
> Start with **Headless Stream** if you want games to launch into a stream-only runtime without changing your KDE, GNOME, or wlroots desktop layout.

## Quick Start

### Fedora 42/43/44

```bash
fedora_version="$(rpm -E %fedora)"
wget "https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora${fedora_version}-x86_64.rpm"
sudo dnf install "./Polaris-fedora${fedora_version}-x86_64.rpm"
sudo polaris --setup-host
polaris
```

### Arch Linux

```bash
wget https://github.com/papi-ux/polaris/releases/latest/download/Polaris-arch-x86_64.pkg.tar.zst
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst
sudo polaris --setup-host
polaris
```

Open **https://localhost:47990/#/welcome**, create your web UI account, and pair a client. After credentials are created, **https://localhost:47990** opens the normal console.

### First stream checklist

1. Open the first-run setup at **https://localhost:47990/#/welcome**.
2. Confirm the recommended Linux path: `headless_mode = enabled`, `linux_use_cage_compositor = enabled`, and `linux_prefer_gpu_native_capture = disabled`.
3. Pair with Trusted Pair on a trusted LAN, QR pairing for Nova, or manual PIN for standard Moonlight clients.
4. Start a game from the Polaris library, Nova, or a Moonlight client.
5. Watch the live session dashboard to confirm the active runtime and encoder path.

> [!TIP]
> If you changed `port` in `~/.config/polaris/polaris.conf`, the web UI is at `https://localhost:<port + 1>`. If you want background autostart, enable the user service with `systemctl --user enable --now polaris`.

## What's New in v1.0.13

Polaris `v1.0.13` is the Auto Quality release: the host and Nova now work together to choose a stream profile, watch how the session behaves, and recover toward the best playable balance of smoothness and image quality.

- **Auto Quality replaces guesswork**: AI Optimizer and Adaptive Bitrate now feed the same decision path, so Polaris can reason about bitrate, FPS, resolution, codec, and stream health together.
- **Smarter game recovery**: poor frame pacing can trigger safer follow-up profiles, while strong sessions can climb back toward higher FPS and quality instead of staying stuck in a conservative mode.
- **Nova sync awareness**: Polaris exposes client capabilities, applied stream settings, presentation state, sync status, and launch recommendations so Nova can show what is actually happening.
- **Cleaner handheld launches**: Polaris can normalize AI recommendations against the explicit client request, keeping launch FPS, resolution, and bitrate aligned with the device and the selected quality preference.
- **Better Linux runtime truth**: logs and session state now call out capture transport, frame residency, encoder target, and GPU-native fallback behavior more clearly.
- **More coverage**: new tests cover optimizer decisions, adaptive bitrate behavior, stream stats, launch validation, and runtime/process handling.

See the [changelog](docs/changelog.md) for the full release history.

## Install

Use the release package for your distro before considering source builds. Packages install the host binary, web console assets, desktop metadata, and user service file; host integration remains explicit through `sudo polaris --setup-host`.

| Host | Best path |
|---|---|
| Fedora 42 | `Polaris-fedora42-x86_64.rpm` from the latest release |
| Fedora 43 | `Polaris-fedora43-x86_64.rpm` from the latest release |
| Fedora 44 | `Polaris-fedora44-x86_64.rpm` from the latest release |
| Arch Linux | `Polaris-arch-x86_64.pkg.tar.zst` from the latest release |
| Bazzite 44 | Layer the matching Fedora 44 RPM with `rpm-ostree`; see [Bazzite guide](docs/bazzite.md) |
| Ubuntu 24.04 | `Polaris-ubuntu24.04-x86_64.deb` experimental tester path; see [Ubuntu guide](docs/ubuntu.md) |
| Debian-family or custom host | Source build; see [Building Polaris](docs/building.md) |

Detailed source builds, local Arch package builds, distro dependency lists, and Browser Stream build flags live in [docs/building.md](docs/building.md).

> [!WARNING]
> Only grant `cap_sys_admin` with `sudo polaris --setup-host --enable-kms` when you actually need DRM/KMS capture. Polaris works fine without it on the default compositor and Headless Stream paths.

## Compatibility

| Area | Status | Notes |
|---|---|---|
| Linux host OS | Supported | Polaris is Linux-first today |
| Fedora 42/43/44 | Recommended | Official RPM assets |
| Arch Linux | Recommended | Official package asset |
| Bazzite | Experimental | Desktop Mode validated on NVIDIA with Headless Stream; real Steam/Game Mode needs more coverage |
| Ubuntu 24.04 | Extremely experimental | DEB asset is available, but this path needs broader real-hardware validation |
| Debian-family distros | Supported from source | Less turnkey than Fedora or Arch |
| NVIDIA / NVENC | Best-tested | Main fast path and most validated encoder/runtime combination |
| VAAPI / software encode | Supported | Works, but is less battle-tested than NVENC |
| Nova for Android | Best experience | Full launch contract, watch mode, tuning, and richer live state |
| Standard Moonlight clients | Compatible | Core streaming works without Nova-specific UX |
| Browser Stream | Experimental | Browser-based streaming path using WebTransport and WebCodecs; best tested on Chromium-family browsers |

## Known Limitations

- Polaris is not a Windows host today. Linux is the supported platform.
- Bazzite support is experimental. Desktop Mode has NVIDIA validation with the recommended Headless Stream settings, while real Steam/Game Mode, AMD, and Steam Deck client flows need more hardware coverage.
- Ubuntu 24.04 DEB packaging is extremely experimental; other Debian-family distros are still source-build oriented.
- NVIDIA/NVENC is the most heavily validated hardware path. Other encode backends work, but they are not equally battle-tested.
- Some UX surfaced in Nova, such as explicit launch recommendations, watch mode polish, and live tuning, depends on the Nova Android client.
- MangoHud can still be risky on Steam Big Picture and some Steam/Proton launches.

## Why Polaris

Traditional Linux streaming hosts often treat your real desktop as disposable: mode switches, broken layouts, portal prompts, and post-session cleanup are all your problem.

Polaris takes a different route:

- **Desktop-safe streaming**: games run in a dedicated compositor instead of hijacking your normal desktop layout
- **Runtime transparency**: the dashboard shows the real backend, transport, frame residency, and format
- **Headless-first Linux path**: designed to avoid HDMI dummy plugs, display scripts, and manual compositor surgery
- **Practical control surface**: live preview, telemetry, quality controls, library management, and pairing in one UI
- **Shared viewing**: additional clients can watch an active stream without stealing ownership

## Use with Nova

[Nova](https://github.com/papi-ux/nova) is the Polaris-aware Android client and the best way to experience the newer host features.

| Polaris + Nova capability | What it means |
|---|---|
| Launch contract | Polaris tells Nova which launch modes are preferred, recommended, and currently allowed |
| Headless vs Virtual Display | Nova can present both choices directly in the library instead of silently guessing |
| 10-bit SDR | Nova can explicitly request a Main10 stream even on SDR handheld panels when the host supports it |
| Watch Stream | A second device can join as a viewer without taking over the owner session |
| Auto Quality | Nova can show AI recommendations, adaptive bitrate state, cached/recovery tuning, host-render limits, and the current target profile in one place |
| Live tuning | Auto Quality and MangoHud controls can be surfaced directly in Nova’s quick menu |
| Session truth | HUD and quick menu can show live server-backed mode, role, shutdown state, and tuning state |

<div align="center">

[![Get it on Obtainium](https://img.shields.io/badge/Obtainium-Get_Nova-7c73ff?style=for-the-badge&logo=data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0iI2ZmZiIgZD0iTTEyIDJMMi41IDcuNVYxNi41TDEyIDIybDkuNS01LjVWNy41TDEyIDJ6bTAgMi4xN2w2LjkgNHYuMDFsLTYuOSA0LTYuOS00di0uMDFMNiA4LjE3bDYtMy44M3oiLz48L3N2Zz4=&labelColor=1a1a2e)](https://apps.obtainium.imranr.dev/redirect?r=obtainium://app/%7B%22id%22%3A%22com.papi.nova%22%2C%22url%22%3A%22https%3A%2F%2Fgithub.com%2Fpapi-ux%2Fnova%22%2C%22author%22%3A%22papi-ux%22%2C%22name%22%3A%22Nova%22%2C%22additionalSettings%22%3A%22%7B%5C%22apkFilterRegEx%5C%22%3A%5C%22app-nonRoot_game-arm64-v8a-release%5C%5C%5C%5C.apk%24%5C%22%2C%5C%22versionExtractionRegEx%5C%22%3A%5C%22v%28.%2B%29%5C%22%2C%5C%22matchGroupToUse%5C%22%3A%5C%221%5C%22%7D%22%7D)
&nbsp;
[![Get it on GitHub Store](https://img.shields.io/badge/GitHub_Store-Get_Nova-24292f?style=for-the-badge&logo=github&labelColor=1a1a2e)](https://github-store.org/app?repo=papi-ux/nova)
&nbsp;
[![Get it on GitHub](https://img.shields.io/badge/GitHub-Releases-4c5265?style=for-the-badge&logo=github&labelColor=1a1a2e)](https://github.com/papi-ux/nova/releases/latest)

</div>

The Obtainium shortcut is prefiltered to Nova's public `app-nonRoot_game-arm64-v8a-release.apk` asset so updates resolve cleanly. Polaris is also compatible with standard [Moonlight](https://moonlight-stream.org) clients on any platform.

## Tour

### Mission Control

Polaris is built around a dashboard that answers the questions stream hosts usually have to reverse-engineer from logs: what runtime is active, what capture path is in use, whether the GPU-native path survived, and how much headroom remains.

<p align="center">
  <picture>
    <img src="docs/screenshots/mission-control-tour.gif" width="900" alt="Polaris Mission Control dashboard with quick controls, GPU gauges, recent games, and system status" />
  </picture>
</p>

### Live Session View

When a stream is active, Polaris shifts from setup to operations: preview, charts, runtime-path telemetry, recording controls, and recommendations are visible in one place.

<p align="center">
  <picture>
    <img src="docs/screenshots/live-session-tour.gif" width="900" alt="Polaris live streaming view with preview, charts, runtime path, and recording controls" />
  </picture>
</p>

### Library and Pairing

<table>
  <tr>
    <td width="50%" valign="top">
      <picture>
        <img src="docs/screenshots/game-library-tour.gif" width="100%" alt="Polaris game library with imported games, cover art, and categories" />
      </picture>
      <p><strong>Game library</strong><br/>Import from Steam, Lutris, and Heroic, attach art, organize categories, and tune launch behavior.</p>
    </td>
    <td width="50%" valign="top">
      <picture>
        <img src="docs/screenshots/pairing-tour.gif" width="100%" alt="Polaris pairing interface with QR code, trusted pairing, and device management" />
      </picture>
      <p><strong>Pairing</strong><br/>Use Trusted Pair (TOFU), QR for Nova, or manual PIN pairing for standard Moonlight clients.</p>
    </td>
  </tr>
</table>

<details>
<summary><b>More screens</b></summary>

<table>
  <tr>
    <td width="50%" valign="top">
      <picture>
        <source media="(prefers-color-scheme: light)" srcset="docs/screenshots/configuration.png" width="100%" />
        <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/configuration-oled.png" width="100%" />
        <img src="docs/screenshots/configuration-oled.png" alt="Polaris configuration screen with general, input, audio-video, and AI settings" />
      </picture>
      <p><strong>Configuration</strong><br/>General, input, audio/video, network, AI, and encoder settings in one place.</p>
    </td>
    <td width="50%" valign="top">
      <picture>
        <source media="(prefers-color-scheme: light)" srcset="docs/screenshots/troubleshooting.png" width="100%" />
        <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/troubleshooting-oled.png" width="100%" />
        <img src="docs/screenshots/troubleshooting-oled.png" alt="Polaris troubleshooting screen with diagnostics and system state" />
      </picture>
      <p><strong>Troubleshooting</strong><br/>Inspect diagnostics without jumping between CLI tools and guesswork.</p>
    </td>
  </tr>
</table>

</details>

## How It Works

Polaris launches games into a dedicated stream runtime, captures that runtime instead of your desktop session, and routes frames into the best available encoder path for the host.

The practical result: your real desktop can keep its layout, refresh rate, and windows while the stream gets its own controlled environment. The dashboard shows the active runtime, capture transport, frame residency, encoder, and session role so you can verify what actually happened.

For the deeper runtime model, see [Runtime and Streaming Model](docs/runtime.md).

## Configuration

Config file: `~/.config/polaris/polaris.conf`

### Recommended first config

```ini
# Headless Stream path
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = disabled

# Pairing on your trusted LAN
trusted_subnets = ["10.0.0.0/24"]

# Encoding
encoder = nvenc

# Optional
adaptive_bitrate_enabled = enabled
max_sessions = 2
```

> [!TIP]
> With Headless Stream you generally do not need KDE window rules, `kscreen-doctor` scripts, HDMI dummy plugs, or manual portal juggling. Turn on the recommended stream runtime and let Polaris manage the compositor, app routing, and input isolation.

Full config tables, AI provider examples, HDR notes, and credential recovery steps live in [docs/configuration.md](docs/configuration.md).

## Docs

| Guide | Use it for |
|---|---|
| [Runtime and Streaming Model](docs/runtime.md) | Headless Stream, capture/encoder paths, Browser Stream, session lifecycle, HDR/Main10 behavior |
| [Configuration](docs/configuration.md) | Config file paths, common options, AI provider settings, credential reset |
| [Building Polaris](docs/building.md) | Source builds, local packages, distro dependencies, Browser Stream build flags |
| [Troubleshooting](docs/troubleshooting.md) | Runtime logs, capture fallbacks, audio/session issues |
| [Bazzite Install Guide](docs/bazzite.md) | Bazzite layering, validation status, rollback, Game Mode notes |
| [Ubuntu Install Guide](docs/ubuntu.md) | Ubuntu DEB status, source fallback, validation notes |

## FAQ

<details>
<summary><b>Do I need an NVIDIA GPU?</b></summary>

No, but NVENC is the most heavily tested path today. VAAPI and software encode paths are supported, but current Linux compositor and DMA-BUF work has been tuned most heavily around NVIDIA.

</details>

<details>
<summary><b>Does Polaris work with Moonlight on iOS, macOS, and PC?</b></summary>

Yes. Polaris speaks the Moonlight protocol. Any Moonlight client can connect. Polaris-specific features such as launch-mode selection, watch mode UX, optimization guidance, and richer session state require Nova on Android.

</details>

<details>
<summary><b>Do I need to uninstall Sunshine before trying Polaris?</b></summary>

No. Polaris keeps its host config separate at `~/.config/polaris`, so installing it should not remove or overwrite an existing Sunshine setup. For testing, stop Sunshine before starting Polaris because both are GameStream/Moonlight hosts and can collide on the same default ports and discovery records.

```bash
systemctl --user stop sunshine
systemctl --user enable --now polaris
```

If your Sunshine install runs as a system service instead of a user service, use the matching service command for your distro. You can switch back by stopping Polaris and starting Sunshine again.

</details>

<details>
<summary><b>Does Moonlight lock streams to 60 FPS?</b></summary>

No. Moonlight can request higher frame rates on clients that expose them, but Polaris treats the client's requested display mode as the ceiling. If a client requests `1280x800x60`, Polaris will not force a `90 FPS` optimization above that request even when the device profile supports it.

</details>

<details>
<summary><b>Does headless mode work on Hyprland, Sway, or GNOME?</b></summary>

The headless `labwc` runtime creates its own Wayland instance, so it is not tied to one desktop environment. Polaris has been tested most heavily on KDE Plasma Wayland, but the model is not KDE-specific.

</details>

<details>
<summary><b>How does Trusted Pair work?</b></summary>

Trusted Pair is Polaris’ TOFU flow. If the client is on a configured trusted subnet, Polaris can auto-approve first pairing. You can still use QR or manual PIN pairing if you want a stricter or more traditional flow.

</details>

<details>
<summary><b>Can Polaris stream 10-bit to an SDR handheld screen?</b></summary>

Yes, if the client explicitly requests a 10-bit path and the active encoder/runtime support Main10. See [Runtime and Streaming Model](docs/runtime.md) for the difference between 10-bit SDR and true HDR.

</details>

<details>
<summary><b>Can Polaris stream true HDR on Linux?</b></summary>

Yes, but Polaris only advertises true HDR when the active capture path reports HDR display metadata. Headless labwc/wlroots sessions remain honest SDR until that runtime can provide real metadata. See [Runtime and Streaming Model](docs/runtime.md) for details.

</details>

<details>
<summary><b>Can multiple people watch the same stream?</b></summary>

Yes. Set `max_sessions` above `1`. Polaris tracks owner vs viewer roles explicitly, and passive watch mode is designed so a second client can observe without taking over the session. Viewers must match the active owner profile rather than silently creating a different downgraded stream.

</details>

<details>
<summary><b>My KDE layout gets corrupted after streaming</b></summary>

That failure mode is the reason Polaris exists. Enable `headless_mode = enabled` and `linux_use_cage_compositor = enabled`, and Polaris will stop treating your physical displays as the stream path.

</details>

<details>
<summary><b>Steam Big Picture shows a black screen or tiny window</b></summary>

First clear Steam’s HTML cache:

```bash
rm -rf ~/.local/share/Steam/config/htmlcache/
```

Then avoid MangoHud on Steam Big Picture and Steam/Proton launches. Polaris and Nova now warn more aggressively there because MangoHud can crash helper processes before the session gets a usable frame.

</details>

<details>
<summary><b>How does the AI optimizer work?</b></summary>

The AI optimizer is optional and disabled by default. When enabled, it sends device specs, app metadata, and recent session history to the provider you configure: Anthropic, OpenAI, Gemini, or a local OpenAI-compatible endpoint such as Ollama or LM Studio. Results are cached locally.

</details>

## AI Transparency

Polaris is a maintainer-led project. I use AI-assisted tools as research,
debugging, comparison, and drafting aids, especially when validating unfamiliar
Linux compositor, packaging, and client behavior.

Those tools do not decide what Polaris is or what ships. I review changes,
test every aspect, and own the final decisions around validation,
trust boundaries, and release quality.

## Contributing

Contributions are welcome, especially focused fixes, docs, translations, packaging improvements, and careful feature work. Polaris is still a small maintainer-led project, so the easiest pull requests to review are the ones that explain the problem clearly and keep the change scoped.

1. Fork the repo and branch from `master`.
2. Make your changes and test them locally.
3. For web UI changes, run `npm run lint`, `npm test`, and `npm run build` in the repo root.
4. For browser-facing changes, run `npm run test:e2e` against a local Polaris instance when possible.
5. Open a pull request that explains what changed, why it helps, and what you were able to test.

> [!NOTE]
> The web UI lives in `src_assets/common/assets/web/` and uses Vue 3 with Tailwind CSS v4. The backend lives in `src/`. CMake builds both together.

## Donate

Polaris is a spare-time project built to make Linux game streaming safer,
clearer, and easier to trust. If it becomes part of your setup, that alone makes my day, donations are appreciated but never expected. They help with my actual coffee budget, which coffee obviously keeps the project moving. Bug reports, testing notes, and thoughtful feedback help too.

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-ff5e5b?style=for-the-badge&logo=ko-fi&labelColor=1a1a2e)](https://ko-fi.com/papiux)
&nbsp;
[![PayPal](https://img.shields.io/badge/PayPal-Donate-7c73ff?style=for-the-badge&logo=paypal&labelColor=1a1a2e)](https://www.paypal.com/donate/?hosted_button_id=KD9R5KLYF6GN4)

## License

Polaris is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text.

Polaris builds on [Apollo](https://github.com/ClassicOldSong/Apollo) and [Sunshine](https://github.com/LizardByte/Sunshine) under GPLv3 lineage, and remains compatible with [Moonlight](https://moonlight-stream.org) clients.
