# Changelog

This file tracks the public Polaris release line.

Older historical tags remain in the repository for continuity, but the current public product line
starts at `v1.0.0`.

## Unreleased

## v1.0.10

Patch release focused on Linux streaming diagnostics and host-session isolation.

- Gate true HDR streaming on display HDR metadata instead of client dynamic-range requests alone
- Add Linux true HDR diagnostics to logs, session status, and support data
- Document the KMS/DRM HDR validation path and current headless labwc SDR behavior
- Fixed Linux shader runtime path packaging so packaged builds can find shader assets correctly
- Isolated Linux headless audio routing so Polaris can capture its virtual stream sink without leaving the host desktop default sink redirected
- Added troubleshooting notes for Linux headless audio routing and HDR metadata validation

## v1.0.9

Patch release focused on Linux headless color correctness and support clarity.

- Fixed AMD/Mesa true-headless SHM color channel handling for reported 3bpp and 4bpp formats
- Prevented the first-run welcome wizard from staying on `Saving...` when browser-side credential saving fails
- Clarified Headless Stream vs Desktop Display behavior on Linux
- Clarified current Linux HDR/Main10 limits and the recommended SDR VAAPI validation path
- Documented that Polaris can be installed alongside Sunshine, but both hosts should not run on the same default GameStream ports at the same time
- Clarified that Moonlight is not inherently capped to 60 FPS; Polaris respects the FPS explicitly requested by the client

## v1.0.8

Patch release focused on Bazzite headless stream validation and host-session isolation.

- Fixed Linux EVDI virtual display output detection when Polaris opens a pre-created EVDI DRM card
- Documented the Bazzite EVDI `initial_device_count=1` setup path for virtual display validation
- Replaced the Bazzite service override heredoc with a `systemctl --user edit --stdin` command and documented the validated headless labwc success markers
- Clarified the recommended Bazzite Headless Stream optimization, expected SHM/RAM capture warnings, and host-input isolation success markers
- Routed headless labwc virtual input through the labwc socket and blocked host uinput fallback when the headless runtime is active
- Cleared host `DISPLAY` from cage-launched app and Steam follow-up commands so launched processes stay inside the stream runtime

## v1.0.7

Patch release focused on the first-run credential wizard.

- Documented the Bazzite composefs workaround for KMS capture: copy Polaris to `/usr/local/bin/polaris-kms`, apply `setcap` there, and run the user service from that writable copy
- Prevented the first-run credential wizard from staying on `Saving...` forever when the browser-side password request fails

## v1.0.6

Patch release focused on first-run Bazzite setup clarity and web credential routing.

- Redirect first-run `/login` and `/recover` visits to the welcome wizard when no web credentials exist
- Clarified first-run setup URLs in the README and Bazzite guide
- Updated the Bazzite guide to run `sudo polaris --setup-host --enable-kms` for the DRM/KMS capture path

## v1.0.5

Patch release focused on cleaner Bazzite/Fedora 44 packaging and headless runtime dependency coverage.

- Added the Linux headless runtime helpers to package dependencies: `labwc`, `wlr-randr`, Xwayland, and `xdpyinfo`/`x11-utils`
- Avoided GPU DMA-BUF capture when the build lacks a matching GPU upload path, preventing invalid CPU frame conversion in headless labwc sessions
- Added Fedora 44 RPM release assets for Bazzite 44 and Fedora 44 users
- Simplified the Bazzite install guide around one matching Fedora RPM layered through `rpm-ostree`

## v1.0.4

Patch release focused on Bazzite tester feedback and credential recovery.

Highlights:

- Fixed BGR0 CPU-frame conversion fallback by inferring packed row stride when capture reports `row_pitch = 0`
- Prevented headless `labwc` fallback paths from failing encode conversion with `src_stride=0`
- Updated the Bazzite guide with `labwc`/`wlr-randr` layering, Desktop Mode-first validation, and known log-message guidance
- Marked Bazzite and Ubuntu package paths as extremely experimental tester paths until broader real-hardware validation is complete
- Clarified web credential recovery: run `polaris --creds` as the same user, restart Polaris afterwards, and avoid shell-confusing placeholder commands

## v1.0.3

Highlights:

- Added an experimental Bazzite install path using the Fedora RPM through `rpm-ostree`
- Added a Bazzite validation checklist for desktop mode, gamemode, GPU, pairing, and headless behavior
- Added Ubuntu 24.04 DEB packaging: `Polaris-ubuntu24.04-x86_64.deb`
- Added an Ubuntu install guide with package, source-build fallback, and validation notes
- Polaris config saves now stay isolated from legacy Sunshine config paths

## v1.0.2

Patch release focused on validated Linux release packages.

Highlights:

- Fedora 42 and Fedora 43 RPMs now build with package-style install paths under `/usr`
- Fedora RPM smoke tests install the generated RPM, verify the packaged binary, and check shared-library resolution
- Fedora 43 is now part of the official release asset validation matrix
- Arch package validation now checks the package against the current distro Boost runtime before release upload
- Public install docs were refreshed for Fedora 43 and current Arch package dependency behavior

## v1.0.1

Patch release focused on dual-distro packaging, Linux runtime hardening, and public web-console polish.

Highlights:

- Arch joins Fedora as a first-class GitHub release package target, with refreshed `v1.0.1` assets for both distro paths
- Headless `labwc` startup, preview routing, and Mission Control preview-source labeling are improved
- Mission Control charting is split out and the web shell is hardened against local self-signed HTTPS chunk-load failures
- The web console now pauses polling and live telemetry while hidden, reducing idle browser and host load
- Browser smoke coverage now checks the key public routes against a live Polaris instance
- Trusted Pair diagnostics and trusted-subnet matching are clearer, including IPv6 support
- Client-requested display modes now cap AI/session optimization upshifts instead of being silently exceeded
- Control-shell AI cache handling is less noisy for Steam UI and desktop-style sessions

## v1.0.0

First public Polaris release.

Highlights:

- Linux-first host with a dedicated streaming compositor path
- Web UI for Mission Control, library management, pairing, settings, security, and troubleshooting
- Trusted Pair, QR pairing, and manual PIN pairing flows
- Live session preview, runtime telemetry, diagnostics, and quick controls
- Steam, Lutris, and Heroic library import flows
- Nova-aware launch modes, watch mode, and richer session-state integration
- Adaptive bitrate, AI optimizer support, and per-title tuning

## Release assets

Current official public assets:

- `Polaris-fedora42-x86_64.rpm`
- `Polaris-fedora43-x86_64.rpm`
- `Polaris-fedora44-x86_64.rpm`
- `Polaris-ubuntu24.04-x86_64.deb`
- `Polaris-arch-x86_64.pkg.tar.zst`
