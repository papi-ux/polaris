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

- **Choose where the game runs.** Private Stream, Gamescope Stream, virtual
  displays and desktop modes state their physical-screen impact. Unavailable
  routes refuse with a reason; per-game **Launch as** keeps host defaults intact.
- **Keep ownership clear.** Disconnect leaves a game available to resume;
  End Session can stop only the title the session owns. Heroic on Host Virtual
  Display still has unconfirmed-shutdown cases: prefer Private Stream there.
- **Build a library.** Steam, Heroic, Lutris and ROM imports retain their
  launcher identity, artwork and launch-health checks.
- **Spaces are a preview.** A player's own sign-in, library and saves in a
  container; one Space runs at a time. See [Spaces](https://papi-ux.com/docs/spaces/)
  and [emulators](https://papi-ux.com/docs/emulators/).
- **Doctor uses evidence.** Fresh media loss can justify one reversible bitrate
  step with verification and Undo. Missing data stays unknown; a named GPU
  encoder does not prove capture avoided CPU copies.

This README describes **1.5.0-beta.2** features. A source branch can lead
available packages: match assets and instructions to an actually published
release. Latest remains stable. The [release notes](docs/release-notes/v1.5.0.md) and the
[changelog](docs/changelog.md) record features, changes and remaining limits.

## Choose a launch mode

| Mode | What happens to the host display |
| --- | --- |
| Private Stream | A session-owned compositor runs the game separately from your desktop |
| Gamescope Stream | A gamescope session provides the stream; hardware and portal support still matter |
| Host Virtual Display | An added or borrowed output; its backend determines layout and privacy impact |
| Mirror Desktop | Streams the visible desktop deliberately |
| Desktop Takeover | Temporarily moves a Hyprland desktop and blanks its physical displays; recovery must be provable |
| Headless Dongle | Uses a configured dummy plug; panel blanking needs the supported setup |

[Launch modes](https://papi-ux.com/docs/launch-modes/) and the
[runtime guide](https://papi-ux.com/docs/runtime/) explain providers, restrictions
and capture paths. A Space has its own runtime; it is not another display mode.

## Mission Control, Director and mobile access

Mission Control features the current or next game, recent titles, live readings
and one Diagnostics section for frame health, pacing, host vitals and launch
checks. A missing reading shows its reason rather than zero. Open the web
console from a trusted browser on the host, phone or tablet; access policy and
authentication still apply.

Polaris Aurora stays the default. **`< Congratulations, Director >`** offers
charcoal, warm white and red; saved themes stay. Narrow screens use a drawer,
and portrait/landscape layouts support enlarged text. Browser-emulated mobile
checks passed; physical phone/tablet console validation remains pending.
Nova Android's embedded Host Console can hand a launch back to Nova without
changing the console's saved theme. See [Mission Control](docs/mission-control.md).

![Polaris Aurora Mission Control during a live Android Handheld session, showing Doctor and the GPU-native runtime path](docs/screenshots/polaris-mission-control-live-v1.3.8.webp)

The Library keeps launch health and source context beside the artwork. Control
Ultimate Edition leads this representative Aurora capture.

![Polaris Aurora Library with Control Ultimate Edition and other ready games](docs/screenshots/polaris-library-control-v1.3.8.webp)

Every capture above and across [papi-ux.com](https://papi-ux.com/polaris/) comes from the tagged public release; the [pixel-level provenance manifest](https://papi-ux.com/images/products/showcase-v1.3.8-v1.3.6-provenance.json) ships with the site.

<img src="docs/screenshots/divider-aurora.svg" width="100%" height="3" alt="">

## Install and start a first stream

Choose an official package from [Latest](https://github.com/papi-ux/polaris/releases/latest)
for stable, or the exact published prerelease for a beta. A stable package URL
does not select a beta.

| Linux host | Installation path |
| --- | --- |
| Fedora 44, Arch/CachyOS | [Quick start](https://papi-ux.com/docs/quickstart/); signed repositories serve stable |
| Bazzite | [RPM guide](docs/bazzite.md): stage through rpm-ostree, reboot, then setup |
| SteamOS 3.8 / 3.9 | [SteamOS guide](https://papi-ux.com/docs/steamos/); beta.2 uses separate matching packages |
| Ubuntu 24.04 | [Ubuntu guide](docs/ubuntu.md); experimental tester package |
| NixOS | [Flake/module guide](nix/README.md); separate system/per-user configuration, community validation limits |
| Other distributions | [Compatibility](https://papi-ux.com/docs/compatibility/) and [source build](docs/building.md) |

For the package paths that call for host setup:

```bash
sudo -H polaris --setup-host
polaris
```

**Fresh install:** open `https://localhost:47990/#/welcome` to create the account.
**Upgrade or reinstall:** use `https://localhost:47990/#/login`; package operations preserve
credentials, pairing, settings and library in `~/.config/polaris`.
Only add `--enable-kms` when your capture guide requires it. Diagnostics name
missing permissions, dependencies or providers; a successful setup is not a
live-stream or hardware pass.

On Steam Deck, follow the service/Game Mode steps in the guide. Leaving Game
Mode before a private launch is a separate **default-off** per-app beta opt-in
requiring SteamOS Manager 26.4.1 (SteamOS 3.9); 3.8 refuses before changing state.
Focused checks on Deck OLED covered H.264 around 60 fps and owned return to
Game Mode; they do not qualify HEVC, HDR or 120 fps on that host.

## Clients and compatibility

[Nova](https://papi-ux.com/nova/) adds a host-backed Library, Play Setup,
Command Center, NovaHUD and session ownership. [Stable Android](https://github.com/papi-ux/nova/releases/latest)
and [published betas](https://github.com/papi-ux/nova/releases) have separate
channels. Native x86_64 Linux is an Alpha; see the
[Linux install guide](https://papi-ux.com/docs/nova/linux/).

Steam Frame has a separate experimental ARM64 alpha in releases that include it.
Short checks covered 1080p SDR at requested 60 fps, H.264/HEVC/PyroWave picture,
sound, controllers and readable Command Center actions. Sustained performance,
higher rates, color/clarity, XR and HDR remain unqualified; this is not Steam
Frame Verified support. The [beta.2 Frame guide](https://github.com/papi-ux/nova/blob/v1.5.0-beta.2/clients/deck/docs/steam-frame.md#public-alpha)
is available when that tag is published; use the matching alpha pair only if
it is attached to that release.

Nova reviews the launch plan before play.

![Nova Android 1.4.14-beta.1 candidate game page for Control Ultimate Edition, with the reviewed stream plan and matching secondary action buttons](docs/screenshots/nova-android-game-detail-v1.4.14-beta.1.webp)

*Nova 1.4.14-beta.1 candidate; [exact source and screenshot hashes](docs/screenshots/beta1-readme-provenance.json).
The Polaris console images above retain their labelled 1.3.8 provenance.*

Standard Moonlight-compatible clients remain supported for pairing, browsing,
launching, input, and streaming; the [client
table](https://papi-ux.com/docs/compatibility/#clients) says what they get next
to Nova. Check the maintained [compatibility
guide](https://papi-ux.com/docs/compatibility/) before choosing a distro, GPU,
capture path, HDR mode, or experimental Browser Stream setup.

### PyroWave, pacing and diagnostics

PyroWave is experimental and needs compatible host, client and network paths.
Nova Linux's 1.5 beta standard bundle includes SDR support with a device check
on selection. [PyroWave](https://papi-ux.com/docs/pyrowave/) explains requested,
encoded and network bitrate units; advice is not a capacity measurement.

Source-paced VRR is a **default-off** beta opt-in; Nova does not request it yet.
The inspected compositor feedback is not a measured panel-cadence result.
Host Recording is experimental raw video without audio/timestamps; playback
is unqualified. The host's Tailscale peer fact describes its control connection,
not the media route. [Doctor](https://papi-ux.com/docs/doctor/) keeps stale or
missing evidence separate from measured zero and explains reversible actions.

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
