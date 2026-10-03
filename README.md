<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/Polaris_fulllogo_2.svg">
  <img src="docs/screenshots/Polaris_fulllogo_2_light.svg" width="250" alt="Polaris">
</picture>

**Linux game streaming that answers to you.**

Polaris turns a Linux gaming PC or a Steam Deck into a modern, self-hosted
streaming home for Nova, Moonlight, and other compatible clients. Choose whether
a session uses a private compositor, a virtual display, your desktop, or the
Steam Game Mode screen, then see the active capture, encoder, client, and
runtime path in one web console.

[![Stars](https://img.shields.io/github/stars/papi-ux/polaris?style=for-the-badge&color=7c73ff&labelColor=1f1d31)](https://github.com/papi-ux/polaris/stargazers)
[![Matrix](https://img.shields.io/badge/Matrix-Join_chat-0dbd8b?style=for-the-badge&logo=matrix&logoColor=white&labelColor=1f1d31)](https://matrix.to/#/#papi-ux:papi-ux.com)
[![License](https://img.shields.io/github/license/papi-ux/polaris?style=for-the-badge&color=4c5265&labelColor=1f1d31)](LICENSE)
[![Release](https://img.shields.io/github/v/release/papi-ux/polaris?style=for-the-badge&color=c8d6e5&labelColor=1f1d31&label=latest)](https://github.com/papi-ux/polaris/releases/latest)

[**Explore Polaris**](https://papi-ux.com/polaris/) ·
[**Download**](https://github.com/papi-ux/polaris/releases/latest) ·
[Join the Matrix community](https://matrix.to/#/#papi-ux:papi-ux.com) ·
[Quick start](https://papi-ux.com/docs/quickstart/) ·
[Compatibility](https://papi-ux.com/docs/compatibility/)

</div>

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

> [!IMPORTANT]
> Polaris is a Linux host application by design. Windows and macOS host ports
> are not planned; Nova and standard Moonlight clients can connect from their
> supported platforms.

![Polaris Aurora Mission Control ready for a client, with host vitals, launch checks, and quick controls](docs/screenshots/polaris-mission-control-ready-v1.3.8.webp)

## Built for the whole player loop

Polaris is the matched host for Nova. Together they make the whole player loop
explicit:

- **Where games run is a real choice.** Private Stream, Gamescope Stream, Host
  Virtual Display, Headless Dongle, and Mirror Desktop are described by their
  display and privacy impact, unavailable modes fail closed, each app can fix its own mode
  with **Launch as**, and the displays Polaris creates for a stream go up to 240 FPS.
- **Steam Game Mode is supported.** Stream a gamescope Steam session, launch a
  Steam title from Nova, and end only the title the stream opened. Proven on
  Steam Deck OLED; the [handhelds guide](https://papi-ux.com/docs/handhelds/)
  covers switching between Desktop Mode and Game Mode.
- **Spaces, an early preview.** Give a player their own sign-in, library and
  saves with Steam, Heroic or Lutris. Host Setup installs the runtime; NVIDIA
  Spaces use the host driver. One Space can be active at a time. Read
  [Spaces](https://papi-ux.com/docs/spaces/) and
  [Spaces or regular streaming](https://papi-ux.com/docs/spaces-or-regular/).
- **Anyone can watch.** A second device can watch the stream that is running,
  whatever its own resolution. The host says up front what there is to watch, so
  Nova asks for exactly that stream; Moonlight still asks for the stream's mode.
- **ROM folders become a library.** Import once and rescan for new games.
  Emulator presets, local covers and launch-health checks explain what is
  ready; the [emulators guide](https://papi-ux.com/docs/emulators/) covers setup.
- **Refusals say why.** A refused launch carries what went wrong and the one
  change that fixes it, with a code Nova shows, instead of error 503.
- **Doctor acts only when it can prove the step is safe.** It can make one
  reversible same-stream bitrate change, verify the encoder and fresh evidence,
  and restore the previous target when verification fails. Other findings stay
  read-only guidance, including the forecast, before any stream, of when capture
  on this host would copy frames through system memory and why.
- **Launches are deterministic.** Auto, Quality, High FPS, and Stability resolve
  into one app- and topology-bound envelope that Nova sends back unchanged.
- **The Library keeps identity intact.** Native and Flatpak Heroic GOG/Epic
  imports retain their runner and installation identity, while built-in utility
  entries keep their shipped artwork unless the player chooses a manual match.

Read the [changelog](docs/changelog.md) for the release-by-release change and
validation record.

## Why Polaris

<table>
<tr>
<td width="50%" valign="top"><img src="docs/screenshots/glyph-isolation.svg" width="22" height="22" alt=""><br>
<b>A private streaming desktop.</b> Private Stream runs a game in its own compositor instead of changing your physical monitor layout.</td>
<td width="50%" valign="top"><img src="docs/screenshots/glyph-truth.svg" width="22" height="22" alt=""><br>
<b>Operational truth, not a mystery box.</b> Mission Control shows the chosen runtime, capture path, encoder, viewers, latency, loss, and Doctor guidance.</td>
</tr>
<tr>
<td valign="top"><img src="docs/screenshots/glyph-client.svg" width="22" height="22" alt=""><br>
<b>A client-aware launch model.</b> Nova can present available display modes, session ownership, safe disconnects, and host-backed tuning before and during play.</td>
<td valign="top"><img src="docs/screenshots/glyph-local.svg" width="22" height="22" alt=""><br>
<b>Local-first and open.</b> Pairing state, permissions, library data, and core streaming remain on your host. Optional AI features use only the provider you configure.</td>
</tr>
</table>

## How isolation works

1. **Choose a game.** Launch from Polaris, Nova, or a compatible Moonlight
   client.
2. **Resolve where this session runs.** Private and Gamescope modes get a
   session-owned compositor; Host Virtual Display gets an extra output; Mirror
   Desktop deliberately uses the physical desktop. A Space runs its own Steam
   in a container, chosen per device. Polaris validates the matching capture
   and encoder path before admitting the stream.
3. **Observe and recover.** Mission Control reports what actually happened;
   Doctor suggests bounded corrections when live evidence needs attention.

Read the [runtime guide](https://papi-ux.com/docs/runtime/) for the detailed
Private Stream, virtual-display, and desktop-mirroring behavior.

## <img src="docs/screenshots/pulse-ready.svg" width="14" height="14" alt=""> Ready, live, and back to the library

The same Mission Control surface changes from an idle host with a paired client
to an active stream with live telemetry, Doctor status, and the observed
capture and encoder paths. Capture can involve CPU copies even when encoding
runs on the GPU; an idle capability forecast does not verify a live path.

![Polaris Aurora Mission Control during a live Android Handheld session, showing Doctor and the GPU-native runtime path](docs/screenshots/polaris-mission-control-live-v1.3.8.webp)

The Library keeps launch health and source context beside the artwork. Control
Ultimate Edition leads this representative Aurora capture.

![Polaris Aurora Library with Control Ultimate Edition and other ready games](docs/screenshots/polaris-library-control-v1.3.8.webp)

Every capture above and across [papi-ux.com](https://papi-ux.com/polaris/) comes from the tagged public release; the [pixel-level provenance manifest](https://papi-ux.com/images/products/showcase-v1.3.8-v1.3.6-provenance.json) ships with the site.

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

## Install and start a first stream

Use an official package from the [latest GitHub
release](https://github.com/papi-ux/polaris/releases/latest), then perform the
explicit host setup. On Bazzite, follow the [RPM installation guide](docs/bazzite.md)
and reboot into the staged deployment before setup:

```bash
sudo -H polaris --setup-host
polaris
```

**Fresh install:** open `https://localhost:47990/#/welcome` and create the web
account. **Upgrade or reinstall:** open `https://localhost:47990/#/login` and
use the existing account; package operations intentionally preserve credentials,
pairing keys, settings, and the library under `~/.config/polaris`. The
[quick-start guide](https://papi-ux.com/docs/quickstart/) contains the current
Fedora, Arch, SteamOS, Ubuntu, Bazzite, openSUSE, and source paths, and on a
Steam Deck the [SteamOS guide](https://papi-ux.com/docs/steamos/) also keeps
Polaris running in Game Mode. Only use
`polaris --setup-host --enable-kms` when the guide says your DRM/KMS capture path
needs it.

## Clients and compatibility

[Nova](https://papi-ux.com/nova/) is the enhanced client for Android and for
**x86_64 Linux desktops, laptops and handhelds**, including Steam Deck, as an
Alpha. It adds a host-backed Library, the Space chooser, Play Setup, Private
Stream choices, Command Center, NovaHUD, session ownership and tuning provenance.
Choose Android APKs or the standard Linux Flatpak from
[stable](https://github.com/papi-ux/nova/releases/latest) or
[beta releases](https://github.com/papi-ux/nova/releases); the
[Linux install guide](https://papi-ux.com/docs/nova/linux/) covers native
streaming and Steam Input. Steam Frame uses a separate experimental ARM64
development route; sustained 1080p/90 and color/clarity qualification remain open.

Nova reviews the launch plan before play.

![Nova Android 1.4.14-beta.1 candidate game page for Control Ultimate Edition, with the reviewed stream plan and matching secondary action buttons](docs/screenshots/nova-android-game-detail-v1.4.14-beta.1.webp)

*Nova beta candidate; [exact source and screenshot hashes](docs/screenshots/beta1-readme-provenance.json).
The Polaris console images above retain their labelled 1.3.8 provenance.*

Standard Moonlight-compatible clients remain supported for pairing, browsing,
launching, input, and streaming; the [client
table](https://papi-ux.com/docs/compatibility/#clients) says what they get next
to Nova. Check the maintained [compatibility
guide](https://papi-ux.com/docs/compatibility/) before choosing a distro, GPU,
capture path, HDR mode, or experimental Browser Stream setup.

### PyroWave and Doctor

PyroWave is experimental SDR and needs compatible host, client and network
capabilities. Nova Linux includes it in the standard 1.4.14 bundle, with a device
check on selection. Read the [PyroWave guide](https://papi-ux.com/docs/pyrowave/)
for request versus encoder units and limits; a suggested target does not prove
network capacity.

Doctor grades fresh media-loss evidence, not control-channel retransmissions.
A verified loss reduction can pause Live Tuning for that stream; Undo restores
bitrate and tuning without changing host settings. See
[Doctor](https://papi-ux.com/docs/doctor/).

Newly paired clients receive **Game Control** by default: enough access to browse,
launch, and play, without clipboard, file-transfer, or server-command permissions.
Existing clients keep their saved access until it is changed under **Devices**.

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

## Documentation and project links

- [Documentation](https://papi-ux.com/docs/) · [Spaces](https://papi-ux.com/docs/spaces/) · [Emulators](https://papi-ux.com/docs/emulators/) · [Play with Moonlight](https://papi-ux.com/docs/moonlight/) · [Launch modes](https://papi-ux.com/docs/launch-modes/) · [Doctor](https://papi-ux.com/docs/doctor/) · [FAQ](https://papi-ux.com/docs/faq/)
- [Roadmap](https://papi-ux.com/docs/roadmap/) · [Website changelog](https://papi-ux.com/docs/changelog/) · [GitHub changelog](docs/changelog.md)
- [Matrix community](https://matrix.to/#/#papi-ux:papi-ux.com) · [Releases](https://github.com/papi-ux/polaris/releases) · [Issues](https://github.com/papi-ux/polaris/issues) · [Discussions](https://github.com/papi-ux/polaris/discussions)
- [Security policy](SECURITY.md) · [Contributing](.github/CONTRIBUTING.md) · [Source](https://github.com/papi-ux/polaris)

## Acknowledgments

Polaris builds on the Apollo and Sunshine host lineage and stays protocol-compatible with the wider Moonlight ecosystem. Thanks to those maintainers and communities for the foundation.

## AI Transparency

Polaris is a maintainer-led project. I use AI-assisted tools as research,
debugging, comparison, and drafting aids, especially when validating unfamiliar
Linux compositor, packaging, and client behavior.

Those tools do not decide what Polaris is or what ships. I review changes,
test every aspect, and own the final decisions around validation,
trust boundaries, and release quality.

## Contributing

Before starting an imported objective, check the
[current product work and review queue](docs/product-objective-status.md) for
existing implementation, open PRs and remaining acceptance.

Contributions are welcome, especially focused fixes, docs, translations, packaging improvements, real-hardware testing, and careful feature work. Polaris is still a small maintainer-led project, so the easiest pull requests to review are the ones that explain the problem clearly, keep the change scoped, and say what was tested on Linux. See [CONTRIBUTING](.github/CONTRIBUTING.md) for the full workflow.

## License

Polaris is free and open-source software licensed under the [GNU General Public
License v3.0](LICENSE).

<div align="center">

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/polaris-icon-darkmode.svg">
  <img src="docs/screenshots/polaris-icon-lightmode.svg" width="56" alt="Polaris mascot">
</picture>

<sub>[Website](https://papi-ux.com/polaris/) · [Matrix](https://matrix.to/#/#papi-ux:papi-ux.com) · [Documentation](https://papi-ux.com/docs/) · [Releases](https://github.com/papi-ux/polaris/releases) · [Security](SECURITY.md)</sub>

</div>
