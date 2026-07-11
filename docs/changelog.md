# Changelog

This file tracks the public Polaris release line.

Older historical tags remain in the repository for continuity, but the current public product line
starts at `v1.0.0`.

## Unreleased

- Replaced the Linux Portal capture placeholder with a reusable PipeWire video stream that negotiates real frame formats, dimensions, and buffer ownership while preserving supported system-memory fallback
- Added fail-closed same-GPU DMA-BUF capture for Portal streams, including exact DRM-render-node-to-CUDA ordinal binding and encoder reinitialization when transport or dimensions change

## v1.3.0 - 2026-07-11

Feature release focused on self-service stream diagnostics, safer release visibility, display planning, and more resilient Linux capture startup.

- Added a manual Update Center with release metadata, package guidance, and a visible update call to action in the web console
- Added deterministic Polaris Doctor diagnostics and privacy-safe support reports for host readiness, active streams, and post-session troubleshooting
- Added optional AI Doctor explanations that translate deterministic findings without replacing the local-first diagnostic source of truth
- Added native network-path probes for route, latency, packet-loss, and reachability evidence in support workflows
- Added native controller, isolation, and haptics diagnostics so input-path failures can be separated from client or game behavior
- Added a display resolution planner that explains requested, host, capture, and output-mode compatibility before launch
- Expanded Mission Control and Troubleshooting self-tests, support bundles, and issue-draft generation with clearer remediation steps
- Improved Linux desktop capture startup by self-healing stale Wayland, display, and session-bus environment values
- Hardened headless DMA-BUF capture so conversion failures fall back cleanly instead of leaving private streams stranded
- Clarified NVIDIA, AMD/VAAPI, GPU-native, and fallback guidance across the public setup and troubleshooting docs

## v1.2.1 - 2026-07-10

Patch release focused on CachyOS/Arch Settings reliability, Linux audio/capture diagnostics, and safer Moonlight-compatible host troubleshooting.

- Fixed Settings saves being blocked by an internal SteamGridDB clear-key flag, allowing unrelated Network and trusted-subnet changes to save normally
- Improved Settings pending-change handling so cancelled SteamGridDB key clears no longer reappear as phantom unsaved edits
- Added clearer backend error details for failed Settings saves so support can identify rejected config keys instead of treating every failure like a filesystem permission issue
- Improved PipeWire audio overrun diagnostics so stream support bundles and logs better explain audio-path failures during launch/connect attempts
- Clarified GPU-native Stream relaunch/fallback reporting for Linux hosts, including AMD/VAAPI SHM fallback messaging and vendor-neutral capture diagnostics
- Hardened Linux private/windowed compositor capture policy so cage/labwc runtime probes happen against the intended streaming runtime instead of a missing display context
- Refreshed public docs, screenshots, and install guidance for Moonlight-compatible users arriving through Fedora, Arch/CachyOS, Ubuntu, and Bazzite paths

## v1.2.0 - 2026-07-03

Feature release focused on Nova-ready private/headless streaming, Portable Chrome cockpit polish, safer launch contracts, Linux input/capture hardening, and broader package coverage.

- Added the Portable Chrome web theme with a dimmer Moonlight-grey early-2000s retro-futurist skin, stronger chrome panel depth, restrained green status accents, and generic theme-toggle cycling across every registered skin
- Improved npm run smoke:web so release smoke gates can target live Polaris or built static web assets, check hashed JS/CSS assets plus the unauthenticated login page, and report a clear preflight when the live HTTPS server is not running
- Added a guided AI Auto Quality optimizer setup checklist with clearer provider/auth/runtime cards and actionable draft test feedback
- Polished release accessibility/mobile readiness with named icon controls, live status regions, trapped confirmation-dialog focus, and non-sticky handheld review bars
- Polished Mission Control live-session hierarchy with a stronger top summary for stream quality, latency, FPS, loss, bitrate, capture path, and runtime mode plus collapsible secondary live panels
- Added a sticky Library import staging summary and review drawer with source counts, per-game removal, clear-all staging, and already-imported confirmation
- Reduced idle web console polling pressure by deduplicating overlapping system/stream stats fetches and backing off transient fallback failures
- Added a Settings pending-changes review drawer with safe before/after values, save/apply impact labels, jump links, and per-setting reset controls
- Added confirmation dialogs and async toast feedback for host-affecting web actions such as disconnecting clients, recovery controls, stale display cleanup, and restart-sensitive quick toggles
- Hardened Linux lock-screen dismissal so a failed loginctl unlock-session attempt continues through other graphical user sessions before falling back to loginctl unlock-sessions
- Added Polaris v1 client/session surfaces for Nova: client settings advertisement, session status/stop integration, stream event queueing, and paired-client launch/input permission hardening
- Hardened private/headless launch policy around desktop Steam, mirror-desktop intent, strict gamepad isolation, host virtual-gamepad metadata, and headless bwrap setup
- Improved NVIDIA/Linux capture contracts with CUDA/GPU-native capability checks, DMA-BUF fallback diagnostics, virtual display output preservation, and headless VAAPI capture reinitialization
- Added openSUSE Tumbleweed build coverage and refreshed package/release workflows for the current Fedora, Ubuntu, and Arch asset line

## v1.1.0

Feature release focused on the Polaris web console, Library workflows, safer pairing defaults, and NVENC split-frame hardening.

- Polished Mission Control degraded-state handling and Library management flows so active sessions, imports, and app editing are easier to scan and manage
- Renamed Auto Quality UI surfaces toward clearer stream-profile language
- Added a server-authoritative `Game Control` pairing access preset for QR/OTP and manual PIN pairing, granting list/view/launch plus input permissions without clipboard, file transfer, or server-command access
- Labeled paired clients with the exact Game Control permission mask as `Game Control` instead of `Custom Access`
- Hardened NVENC split-frame defaults by explicitly passing FFmpeg's disabled split-frame value when split-frame encoding is disabled
- Improved Linux lock-screen dismissal so Polaris falls back from a systemd manager session to the graphical login session before running `loginctl unlock-session`
- Added web, pairing, and video regression coverage for the new access preset and split-frame default behavior

## v1.0.18

Hotfix release focused on keeping cached AI launch profiles from forcing capable clients back to 720p.

- Bounded cached AI display-mode optimization by the explicit client request so Shield, Retroid, and Android TV launches that request 1080p keep a 1080p headless compositor
- Preserved history-safe recovery behavior so confirmed recovery profiles can still lower resolution or FPS when a recent session needs it
- Smoke-tested a Shield direct Steam launch with Nova `v1.0.10`, confirming the client request, Polaris-selected mode, labwc headless runtime, and Android decoder all used `1920x1080x60`

## v1.0.17

Hotfix release focused on session lifecycle cleanup after client End and terminate flows.

- Cleared stale paused/resumable session state when an app is explicitly terminated after the last stream client disconnects
- Emitted a terminal `stream_ended` lifecycle event from the terminate cleanup path so Nova can remove stale Active Session/Resume UI
- Made the session shutdown-request flag thread-safe between HTTPS controls and stream cleanup
- Added regression coverage for paused app termination, connected-client guards, duplicate idle suppression, and streaming cleanup handoff

## v1.0.16

Stability hotfix release focused on client certificate verification during reconnect and disconnect flows.

- Fixed a crash in HTTPS client certificate verification by giving each verification request its own OpenSSL `X509_STORE_CTX`
- Protected paired-client certificate state while concurrent HTTPS threads verify Nova/Moonlight clients
- Added regression coverage for concurrent certificate verification against the pairing certificate chain
- Rebuilt local master with CUDA enabled and smoke-tested a Retroid Pocket 6 direct Steam launch through headless labwc, DMA-BUF GPU capture, CUDA conversion, and NVENC
- Confirmed the direct Retroid launch path stayed clear of inherited MangoHud and cleaned up the isolated session without a new coredump

## v1.0.15

Hotfix release focused on keeping MangoHud out of Linux headless stream runtimes unless a game explicitly opts in.

- Suppressed inherited and session-pacing MangoHud injection for direct Steam game launches inside the headless cage compositor
- Kept explicit per-game MangoHud support available for direct game launches while continuing to block MangoHud for Steam Big Picture sessions
- Cleared `MANGOHUD_CONFIG` from the isolated `labwc` and XWayland runtime so compositor/helper processes do not inherit stale FPS-cap configuration
- Added policy coverage for direct headless cage launches and Steam Big Picture suppression
- Smoke-tested a direct Retroid Pocket 6 launch under a forced parent `MANGOHUD=1` environment and verified stream child processes did not retain `MANGOHUD*`

## v1.0.14

Patch release focused on Steam launch reliability, encoder/runtime polish, and safer Linux capture setup.

- Improved Steam library launch behavior, including direct Steam launch mode and non-default Steam library discovery
- Added NVIDIA NVENC split-frame encoding support, prepared FFmpeg wiring, configuration validation, and user-facing docs
- Improved Auto Quality and Adaptive Bitrate behavior so paired-client bitrate, recovery profiles, and clamp edge cases are handled more safely
- Added AMD GPU telemetry support and clearer dashboard handling for optional vendor-specific metrics
- Improved Linux unlock fallback, session cleanup, AMD headless DMA-BUF handling, and runtime diagnostics
- Added safe local development cleanup tooling with script coverage and building-guide documentation
- Hardened display selection so capture setup handles empty display lists without clamping against an invalid range

## v1.0.13

Patch release focused on AI Auto Quality, Nova coordination, and Linux stream pacing diagnostics.

- Added richer Nova/Polaris settings sync so launch optimization, applied stream settings, presentation state, adaptive bitrate status, and optimizer health are visible across both sides
- Merged adaptive bitrate behavior into the AI Auto Quality path so recovery decisions can consider network pressure, host frame pacing, encode pressure, and session history together
- Improved AI optimizer feedback handling so short low-confidence sessions do not incorrectly relax safe FPS caps or poison game profiles
- Added safer history-based recovery profiles, including FPS fallback behavior and clearer host-render-limited session grading
- Improved Linux headless stream reporting for DMA-BUF capture, CUDA conversion, encoder target, frame residency, and SHM/CPU fallback reasons
- Added resumable disconnect handling and cleanup improvements for Steam and isolated cage sessions
- Expanded optimizer, adaptive bitrate, stream stats, process migration, and web UI coverage for the new Auto Quality flow

## v1.0.12

Patch release focused on corrected Fedora/Bazzite NVIDIA release assets.

- Rebuilt Fedora 42, Fedora 43, and Fedora 44 release RPMs with CUDA enabled so NVIDIA/NVENC hosts can use the validated GPU-native upload path
- Added release validation that fails Fedora RPM packaging if a tagged release reports `Build features: cuda=disabled`
- Normalized the Fedora CUDA toolkit header patching flow for CUDA 13.2 headers and Fedora 42/43/44 release builds
- Fixed release dispatch packaging dependencies and Arch release validation so patch-release asset rebuilds are repeatable
- Kept the v1.0.11 Browser Stream and Linux runtime diagnostics behavior otherwise unchanged

## v1.0.11

Patch release focused on Browser Stream validation and Linux stream-runtime polish.

- Added experimental Browser Stream using WebTransport and WebCodecs, with `/browser-stream` routing and `/webrtc` compatibility aliases
- Added the Polaris-launched WebTransport helper, browser session API, WebCodecs playback, and browser keyboard, pointer, wheel, and touch input routing
- Added Browser Stream UI modes for Game Mode, in-game expansion, pop-out streaming, stream profiles, latency statistics, and unsupported-browser messaging
- Improved Linux stream runtime diagnostics, stream display policy reporting, and GPU-native/headless capture path explanations
- Improved Linux launcher integration, labwc refresh-rate handling, headless preview diagnostics, and stream cleanup behavior
- Fixed Browser Stream close handling so the helper, transport, cage runtime, and launched Steam game are cleaned up together
- Fixed Steam handoff after Browser Stream cleanup so Nova/Moonlight launches are not blocked by stale Steam child processes
- Fixed isolated Linux audio routing so game audio streams that move back to the host sink are returned to the Polaris virtual stream sink
- Added isolated Linux process cleanup for Steam-launched children that escape the direct app process group
- Fixed SHM capture color handling for reported wlgrab pixel formats and expanded unit coverage for the copy path
- Tightened HDR metadata gating, web config save behavior, and Browser Stream route/config/status test coverage

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
