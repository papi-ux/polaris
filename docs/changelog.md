# Changelog

This file tracks the public Polaris release line.

Older historical tags remain in the repository for continuity, but the current public product line
starts at `v1.0.0`.

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
- `Polaris-arch-x86_64.pkg.tar.zst`
