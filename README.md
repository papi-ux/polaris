<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/Polaris_fulllogo_2.svg">
  <img src="docs/screenshots/Polaris_fulllogo_2_light.svg" width="250" alt="Polaris">
</picture>

**Stream games from your Linux PC without giving up your desktop.**

Polaris turns a Linux gaming PC or a Steam Deck into your own game-streaming
host. Play on Nova for Android, Moonlight or another compatible client while
your desktop stays exactly as you left it.

[![Stars](https://img.shields.io/github/stars/papi-ux/polaris?style=for-the-badge&color=7c73ff&labelColor=1f1d31)](https://github.com/papi-ux/polaris/stargazers)
[![Matrix](https://img.shields.io/badge/Matrix-Join_chat-0dbd8b?style=for-the-badge&logo=matrix&logoColor=white&labelColor=1f1d31)](https://matrix.to/#/#papi-ux:papi-ux.com)
[![License](https://img.shields.io/github/license/papi-ux/polaris?style=for-the-badge&color=4c5265&labelColor=1f1d31)](LICENSE)
[![Release](https://img.shields.io/github/v/release/papi-ux/polaris?style=for-the-badge&color=c8d6e5&labelColor=1f1d31&label=latest)](https://github.com/papi-ux/polaris/releases/latest)

[**Get Polaris**](https://papi-ux.com/downloads/?product=polaris) ·
[Explore Polaris](https://papi-ux.com/polaris/) ·
[Quick start](https://papi-ux.com/docs/quickstart/) ·
[Join the Matrix community](https://matrix.to/#/#papi-ux:papi-ux.com)

</div>

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

![Polaris Mission Control in the default Aurora theme: Control Ultimate Edition is next up, 5 of 5 launch checks are ready and the mode is Private Stream](docs/screenshots/beta2/polaris-mission-control-aurora.webp)

## Why Polaris

Most streaming hosts borrow your desktop. They change your resolution, wake
your monitors and leave the cleanup to you. Polaris gives each game a place of
its own and tells you plainly how the stream is going.

- **Your desktop stays yours.** Private Stream runs the game in its own
  compositor, so your monitors, windows and layout stay put. When you do want
  the real desktop, Mirror Desktop and virtual displays are there too, each
  named for what it does to your screens.
- **You can see what's happening.** Mission Control, the web console, shows
  what's playing, who's connected, how healthy the stream is, and which
  capture path and encoder Polaris really chose. When the network struggles,
  Doctor can offer a lower bitrate, check whether it helped and undo it.
- **Your library comes along.** Steam, Heroic, Lutris and ROM collections
  arrive with their artwork, and each game keeps a launch check you can see
  before you press play.
- **Step away without losing your place.** Disconnect keeps the game running so you can pick
  it up later. End Session stops only the game that session started. A second
  device can watch without taking over.
- **It works with what you have.** Nova gets the richest experience, and
  Moonlight and other compatible clients still pair, browse and play. There's
  no cloud account or subscription: pairing, settings and your library stay on
  your PC.

Prefer charcoal and red? 1.5.0-beta.2 adds `< Congratulations, Director >`, a
theme shared with Nova. Nothing changes until you pick it, and Polaris Aurora
stays the default.

![Polaris Mission Control in the < Congratulations, Director > theme: charcoal, warm white and red](docs/screenshots/beta2/polaris-mission-control-director.webp)

## Get started

[**Get Polaris**](https://papi-ux.com/downloads/?product=polaris) helps you pick
the right package. If you'd rather choose yourself:

| Your Linux host | Start here |
| --- | --- |
| Fedora 44, Arch Linux, CachyOS | [Quick start](https://papi-ux.com/docs/quickstart/), with signed package repositories |
| SteamOS 3.8 or 3.9 on a Steam Deck | [SteamOS guide](https://papi-ux.com/docs/steamos/); stable has a 3.8 package, and 1.5.0-beta.2 adds a separate 3.9 package |
| Bazzite | [Bazzite guide](https://papi-ux.com/docs/bazzite/) |
| Ubuntu 24.04 | [Ubuntu guide](https://papi-ux.com/docs/ubuntu/), a tester package |
| NixOS | [NixOS guide](https://papi-ux.com/docs/nixos/), a source build with community validation |
| Something else | [Compatibility](https://papi-ux.com/docs/compatibility/) and [building from source](https://papi-ux.com/docs/building/) |

Most packages then need a one-time host setup:

```bash
sudo -H polaris --setup-host
polaris
```

**Fresh install:** open `https://localhost:47990/#/welcome` and create your account.
**Upgrade or reinstall:** sign in at `https://localhost:47990/#/login`. Your
pairing, settings and library stay in `~/.config/polaris`.

Add `--enable-kms` only when your capture guide asks for it. Setup names any
missing permission or dependency, but only a real stream shows your hardware
works. Then pair [Nova](https://papi-ux.com/nova/) or a Moonlight client and
play.

## Good to know

- **Linux only, on purpose.** Windows and macOS hosts aren't planned. Clients
  connect from their own platforms; the [client table](https://papi-ux.com/docs/compatibility/#clients)
  shows what each one gets.
- **Graphics.** NVIDIA is the most tested. AMD and Intel work through VAAPI,
  with software encoding as a fallback.
- **Stable or beta.** [Latest](https://github.com/papi-ux/polaris/releases/latest)
  is always stable; betas are opt-in prereleases. This page follows the source,
  which can be ahead of the newest package. The
  [1.5.0 release notes](docs/release-notes/v1.5.0.md) cover 1.5.0-beta.2 and
  what's still being checked.
- **Experimental and preview.** PyroWave runs only when a client asks for it.
  Source-paced VRR (Nova doesn't request it yet) and Recording (video only, no
  audio) stay off until you turn them on. Spaces is a preview.

Looking for the client? [Get Nova](https://papi-ux.com/downloads/?product=nova)
for Android, or take its [latest release](https://github.com/papi-ux/nova/releases/latest).
Its native Linux and Steam Deck builds are alphas, and Steam Frame has a
separate experimental alpha.

## Learn more

- [Documentation](https://papi-ux.com/docs/) · [How the runtime works](https://papi-ux.com/docs/runtime/) · [Launch modes](https://papi-ux.com/docs/launch-modes/) · [Mission Control](https://papi-ux.com/docs/mission-control/) · [Doctor](https://papi-ux.com/docs/doctor/) · [Spaces](https://papi-ux.com/docs/spaces/) · [Emulators](https://papi-ux.com/docs/emulators/) · [Play with Moonlight](https://papi-ux.com/docs/moonlight/) · [FAQ](https://papi-ux.com/docs/faq/)
- [Roadmap](https://papi-ux.com/docs/roadmap/) · [Website changelog](https://papi-ux.com/docs/changelog/) · [Changelog](docs/changelog.md) · [Releases](https://github.com/papi-ux/polaris/releases)
- [Matrix community](https://matrix.to/#/#papi-ux:papi-ux.com) · [Issues](https://github.com/papi-ux/polaris/issues) · [Discussions](https://github.com/papi-ux/polaris/discussions) · [Security policy](SECURITY.md) · [Contributing](.github/CONTRIBUTING.md)

<sub>Screenshots: 1.5.0-beta.2 ([details](docs/screenshots/beta2/provenance.json)).
Website gallery: [image sources](https://papi-ux.com/images/products/showcase-v1.3.8-v1.3.6-provenance.json).</sub>

## Acknowledgments

Polaris builds on the Apollo and Sunshine host lineage and stays compatible
with the wider Moonlight ecosystem. Thanks to their maintainers and
communities for the foundation.

## AI Transparency

Polaris is maintainer-led. I use AI-assisted tools for research, debugging,
comparison and drafting, especially around unfamiliar compositor, packaging
and client behavior. They don't decide what Polaris is or what ships: I review
and test changes, and I own the final calls on validation, trust boundaries
and release quality.

## Contributing

Focused fixes, docs, translations, packaging and real-hardware testing are
welcome. Explain the problem, keep the change scoped and say what you tested on
Linux. Check the [current work and review queue](docs/product-objective-status.md)
first, then see [CONTRIBUTING](.github/CONTRIBUTING.md).

## License

Polaris is free and open-source software under the [GNU General Public License v3.0](LICENSE).

<div align="center">

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/polaris-icon-darkmode.svg">
  <img src="docs/screenshots/polaris-icon-lightmode.svg" width="56" alt="Polaris mascot">
</picture>

<sub>[Website](https://papi-ux.com/polaris/) · [Matrix](https://matrix.to/#/#papi-ux:papi-ux.com) · [Documentation](https://papi-ux.com/docs/) · [Releases](https://github.com/papi-ux/polaris/releases) · [Security](SECURITY.md)</sub>

</div>
