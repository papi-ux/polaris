# Changelog

This file tracks the public Polaris release line.

Older historical tags remain in the repository for continuity, but the current public product line
starts at `v1.0.0`.

## v1.3.9 - 2026-08-15

A Linux reliability and security patch for private-stream capture, compositor ownership, high-refresh cadence, and hostile-input boundaries.

- Enables VAAPI GPU-native capture only for the private headless ext-image-copy route when the captured DMA-BUF is explicitly `DRM_FORMAT_MOD_LINEAR`; tiled, invalid, missing, windowed-private, and direct-monitor modifiers remain on the SHM fallback, and Doctor preserves the exact fallback reason
- Makes capability-enabled Polaris work with `xdg-desktop-portal` by dropping capabilities before worker threads start and restoring ordinary same-user `/proc` access; explicit DRM/KMS capture retains its required capability on a separate process start
- Accepts capability-enabled owned Gamescope generations, warns when unsupported nested Gamescope WSI can hide a blocking dialog, and prevents owned Gamescope/Xwayland descendants from retaining Polaris listener sockets after host exit
- Creates and owns only an exact process-scoped Hyprland virtual output, verifies it before use, and fails closed instead of silently capturing a physical display
- Preserves high-refresh private-stream cadence, exposes launches that never attach, sees override-redirect windows during attach, and bounds preparation commands without releasing lifecycle ownership early
- Reapplies an explicit same-mode session choice when deterministic runtime, capture, or display companion state has drifted, while retaining the normalized no-op and teardown restoration paths
- Hardens Wayland frame ownership, VAAPI DRM PRIME descriptors, KMS render descriptors, portal/capture shutdown, session environment snapshots, and exact-generation cleanup boundaries
- Validates every client launch key, peer-declared control length, Steam app id, artwork URL/redirect hop, pairing PIN claim, and Doctor mutation boundary before use
- Keeps secondary-client telemetry while preventing duplicate control-channel network-risk samples
- Keeps `npm audit --audit-level=high` mandatory
- Retains exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official release assets
- Records the remaining field gates without treating CI as hardware proof: linear VAAPI headless capture still needs affected AMD 4K validation, Hyprland virtual-display ownership and same-mode session normalization need affected-host confirmation, and the supported Gamescope Stream route needs an end-to-end portal retest

## v1.3.8 - 2026-08-12

A safer private-stream daily driver: true-headless GPU-native capture, session-only launch choices, exact process ownership, an evidence-gated Doctor, bounded logs, benchmark controls, and a rebuilt web console.

- Enables a true-headless Vulkan/ext-image-copy path with a prefetched initialization frame
- Accepts a validated session-only `streamMode` launch override, re-evaluates capture sources around that session, reports display fallback explicitly, and leaves the persisted host default untouched; matching client support is versioned separately in Nova `v1.3.6`
- Owns detached-only workloads through exact PIDFD identity before detaching, signals and reaps only the captured session generation, stops retained private compositors safely, recaptures demonstrably transient `/proc` races to bounded quiescence while persistent or ambiguous attribution remains fail-closed, and hardens portal startup/cancellation and restore-token handling
- Exposes evidence-gated Doctor actions to recheck, perform one guarded bitrate reduction, restore a history-safe profile gradually, verify live telemetry, and Undo through the web console; matching Nova controls are versioned and released independently
- Caps runtime diagnostics at an 8 MiB active file plus one 8 MiB backup, bounds console/file queues, preserves record-time timestamps, and exposes an authenticated binary-safe tail API with a bounded browser view and truthful truncation state
- Adds authenticated bounded benchmark-run controls and T0-T2 host-stage evidence while keeping benchmark mode explicitly gated
- Rebuilds Mission Control around one status hero, one live strip, the Doctor, a safer preview, and five Nova-aligned themes
- Hardens render-node/GPU pairing, VAAPI-safe device selection, resume refresh restoration, HDR/YUV444 capability probes, virtual-display capture routing, and launch/resume status responses
- Records the bounded Retroid Pocket 6 release smoke of the exact source commit tagged as `v1.3.8`: true-headless `HEADLESS-1` capture, changing frames, session-only stream mode, durable Doctor apply/verify/Undo, exact-generation teardown, and clean restoration
- Keeps the remaining field-proof limits visible: the reporter's AMD 4K60 scenario and host-virtual-display route still need current-`v1.3.8` end-to-end confirmation, and SteamOS remains an experimental Desktop Mode package
- Keeps `npm audit --audit-level=high` mandatory
- Retains exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official release assets

## v1.3.7 - 2026-08-07

A use-after-free in VAAPI DMA-BUF capture that AMD hosts on DRM/KMS have been running, and CI that finally builds the nix packaging it patches.

- Fixes a use-after-free at the VAAPI DMA-BUF import boundary: the VRAM converter destroyed its imported surface before importing the replacement, while an in-flight conversion could still be using it. Reachable on DRM/KMS capture and the non-cage wlroots VRAM path, which is what AMD hosts have been running
- Fails closed on an invalid surface or texture selection rather than converting it, and closes duplicated DMA-BUF descriptors once ownership transfers
- Keeps the gamescope-polaris patch stack and the packaged compositor in step, so the `+polhdr2` stamp a session negotiates against is one a build proved
- Checks the vendored patch stacks on every push and builds the nix packages in CI, so a patch that cannot apply fails there rather than on a host
- Keeps `npm audit --audit-level=high` mandatory
- Retains exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official release assets

## v1.3.6 - 2026-08-07

Client-facing fixes for Nova, host setup advice that works on ostree systems, and gamescope session and HDR capture hardening.

- Prints the input-group command that works on ostree hosts, where the group lives in `/usr/lib/group` and `usermod -aG input` cannot find it; Bazzite gets `ujust add-user-to-input-group`
- Reports what the artwork resolve endpoint actually did, which Nova requires and no shipped build had ever sent, so library artwork update reported the host as unsupported
- Serves `platform` and `runtime` on library entries from the Lutris runner recorded at import, and only where the runner determines them
- Lets a manual artwork match decide which game a completion estimate is for, without falling back to the Steam app id that made the wrong estimate confident
- Records the fields Polaris serves to Nova in `docs/nova-contract.json`, derived from the source that serves them, and serves `vaapi_vendor` so a crash report names the driver generation
- Recovers gamescope session teardown from a dead nested marker, a non-leader attach, and an incomplete attach generation that refused later launches until restart
- Negotiates PipeWire capture formats against what the stream encodes, so 10-bit PQ cannot feed an SDR encode
- Claims the stream sink as the session default while streaming, and releases that claim only from the session that took it
- Keeps `npm audit --audit-level=high` mandatory
- Retains exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official release assets

## v1.3.5 - 2026-08-06

Package-update safety, Linux host integration owned by the package, and library playtime and completion estimates.

- Writes each mutable-distro download to the exact package filename with `wget --output-document`, preventing a pre-existing file from redirecting the new payload to `.1` or `.2` while a stale unsuffixed package is installed
- Makes Fedora, Arch, and Ubuntu update commands short-circuit after download, package-install, `sudo -H polaris --setup-host`, or restart failures
- Adds executable regression coverage for failed download, install, setup-host, and successful command paths across all three mutable package families
- Documents the v1.3.4 bootstrap caveat and provides exact-output commands for the first upgrade to v1.3.5
- Installs the udev rules and modules-load configuration as package files under `/usr/lib`, so the package manager owns them and removes them on uninstall
- Retires an unmodified `/etc` copy from an older install that would otherwise shadow the packaged rules, and keeps an edited copy with a warning naming the file in effect
- Lets `--setup-host` exit without root when the package already provides everything and the virtual input nodes are usable
- Adds a `polaris-debug` package to the Arch and SteamOS builds so `coredumpctl info polaris` yields a real backtrace
- Closes a `systemd-inhibit` process leaked on every session, and keeps a private session's virtual keyboard and mouse out of the desktop session logged in at the machine
- Warns at startup when seat isolation is enabled and the account Polaris runs as is not in the `input` group, which otherwise leaves the isolated devices unopenable by Polaris and by the streamed game
- Warns when `back_button_timeout` is shorter than the 100ms Home press it emulates, since the setting is milliseconds and a value like `2` turns nearly every Back/Select press into Home
- Reports the playtime Steam and Lutris already record on disk, and serves completion estimates from a local dataset first, with `beat_times_lookup` controlling the How Long To Beat fallback; both are announced through `/polaris/v1/capabilities` so clients can adopt them explicitly
- Adds a transactional custom artwork workflow with an authenticated resolver, bounded downloads, and atomic caching
- Surfaces per-app environment variables in the web UI
- Releases the host loopback when a session turns host audio off, instead of leaving an earlier session's loopback loaded for the life of the process
- Applies the session's requested resolution and refresh when preparing the streaming display
- Keeps `npm audit --audit-level=high` mandatory and clears the advisories that were failing every web build
- Retains exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb` as the official release assets

## v1.3.4 - 2026-07-31

Patch release adding a dedicated SteamOS 3.8 package lane and tightening Linux package, path, setup, and import safety.

- Added fail-closed packaged binary path validation with explicit source-prefix remapping and retained validation receipts
- Added secure Bazzite support for the `/home` to `var/home` layout without broad canonicalization
- Dispatched setup-host early and standardized public host-integration commands on `sudo -H polaris --setup-host`
- Made ImageMagick discovery locale-safe during Steam cover import
- Added a dedicated SteamOS 3.8 x86_64 package built against Valve's versioned repositories, with failure-safe installation and Desktop Mode package and startup validation only
- Kept physical Steam Deck gameplay, Game Mode, OLED 90 Hz, suspend and resume, and update persistence outside the certified support claim pending hardware evidence
- Kept `npm audit --audit-level=high` mandatory and the forbidden `webtransport-go v0.10.0` dependency absent
- Standardized the official release on exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and `Polaris-ubuntu24.04-x86_64.deb`

## v1.3.3 - 2026-07-30

Patch release focused on configuration-save hygiene, controller boundaries, Linux recovery guidance, and Nix session reliability.

- Stripped response-only runtime, stream-path, credential-presence, and virtual-display metadata before configuration saves so valid v1.3.2 settings edits no longer fail backend validation
- Kept display-planner presets idempotent across repeated selection and stale dongle-discovery responses
- Made tray URL launching locale-safe on Linux desktops
- Rebound preallocated gamepad controller feedback after device reuse so rumble and related output follow the active client
- Added default-disabled client-gamepad seat isolation with distinct virtual-controller identities and explicit Linux seat-policy limits
- Documented how Bazzite users can restore Sunshine after testing or uninstalling Polaris
- Kept Nix-composed idle/session scripts ShellCheck-clean and removed temporary authority files after failed atomic runtime publication
- Retained the exact release assets `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, and `Polaris-ubuntu24.04-x86_64.deb`; Fedora 44 remains the sole Fedora package lane and `npm audit --audit-level=high` remains mandatory

## v1.3.2 - 2026-07-30

Reliability patch focused on stream lifecycle, Linux private-session isolation, reconnect recovery, and truthful host diagnostics.

- Hardened RTSP follow-up control admission, live-session command ownership, session teardown, and serialized PulseAudio operations
- Prevented private-stream controller input from also navigating host Steam Big Picture while preserving normal game input
- Hardened private Steam teardown ownership and bounded interrupted process waits without broadly terminating desktop Steam
- Prevented private-session relaunch races by waiting for the prior Steam singleton to be fully released
- Preserved authenticated web sessions across host restarts and improved recovery from transient host outages
- Tolerated near-target stream FPS so healthy sessions are not mislabeled as degraded
- Exposed clearer Linux GPU probe topology diagnostics for capture-path troubleshooting
- Hardened Dashboard smoke navigation so release checks do not issue duplicate route requests

### Security and release packaging

- Updated Browser Stream to `webtransport-go v0.11.1` and `quic-go v0.60.0`, fixing remote memory exhaustion from unknown capsule buffering (CVE-2026-57497 / GHSA-g35j-m5xg-vh3q)
- Removed vulnerable, unnecessary web fixture-server dependencies and added `npm audit --audit-level=high` as a permanent CI gate
- Standardized the official release on exactly `Polaris-arch-x86_64.pkg.tar.zst`, `Polaris-fedora44-x86_64.rpm`, and `Polaris-ubuntu24.04-x86_64.deb`; Fedora 42/43 remain historical rather than current package lanes
- Added explicit Arch `vulkan-headers` / `vulkan-icd-loader` and Fedora `vulkan-loader-devel` package requirements
- Cleared GCC 15 warning-as-error blockers in Browser Stream setup, dormant preview diagnostics, and Linux display-topology helpers so exact-tag package validation builds cleanly

### Linux stream modes / private runtime foundation
- Add first-class `linux_stream_mode` and `linux_private_runtime` config (Private Stream, Host Virtual Display, Mirror Desktop, GPU-native preference, Gamescope Stream, Headless Dongle).
- Keep legacy `headless_mode` / `linux_use_cage_compositor` / `linux_prefer_gpu_native_capture` as a compatibility mapping; UI and client-settings write both.
- Centralize mode resolve/apply/labels in `stream_display_policy`; path availability probes `gamescope` on PATH (and dongle outputs at apply).
- Introduce `stream_runtime` interface with labwc and gamescope adapters so process session start does not hard-code cage forever.
- Add `stream_path` registry (runtime × capture × topology) with reserved slots for community EVDI/Family Mode paths; honest `runtime_backend` for portal/host/gamescope/labwc.
- Document the path plugin contract in `docs/stream-paths.md`.
- Enable **Headless Dongle** path (`headless_dongle`): privacy/extended swap via kscreen-doctor (`display_topology`, `headless_swap_mode`); DRM sysfs connector discovery + `/api/linux/display-outputs` auto-suggest.
- Enable **Gamescope Stream** ownership: attach idle `gamescope-0` (start `polaris-gamescope-idle` if needed) or spawn owned headless; wrap app launches into that runtime; never use `gamescope-1` for portal.
- Harden portal/PipeWire capture: disconnect under loop lock; keep restore_token with invalidate+retry on SelectSources failure; wait for `AvailableCursorModes` ≠ 0; shared ownership so release cannot UAF negotiate/capture waiters.
- Solid-base stop path: Moonlight `/cancel` responds before nested teardown; owner cancel ignores stale sessiontoken (case-insensitive UUID); Browser Stream signals shutdown, **releases portal/PipeWire**, then joins capture (bounded) **before** pidfd-killing gamescope/labwc; `terminate_impl` and WebUI disconnect share the same prepare path.
- Harden Gamescope orphan recovery and nested teardown with generation-pinned socket reclamation, shared shell/C++ ownership locking, atomic session credentials, exact private-SID/pidfd cleanup, and fail-closed portal/idle rebinding.
- Dashboard preview tries labwc, gamescope-0/1, host Wayland (grim), then spectacle — works across stream paths.
- Web UI: selectable path cards write full config (including dongle outputs and gamescope/portal capture).
- SB-5 mode-neutral Steam apps (issue #5): migration v9 + load-time normalize unwrap `polaris-gamescope-session` hardwires to `steam-appid` + detached `rungameid`; gamescope path applies attach X11 env (no host Wayland) via `stream_runtime::wrap_cmd` / process. Optional Steam Big Picture may keep nested WSI shell.

## v1.3.1 - 2026-07-12

Security and pairing-state patch focused on current cryptography dependencies, durable client authorization, and clearer paired-device history.

- Updated the Browser Stream helper's Go cryptography and supporting modules, clearing the associated Dependabot alerts
- Added localized Added and Last seen values for paired clients while keeping unknown timestamps truthful for legacy records
- Hardened canonical X.509 client identity, revocation, duplicate-state validation, and authenticated request-time authorization snapshots
- Hardened paired-client persistence with private, cross-process atomic state replacement
- Improved paired-client controls so failed mutations remain visible instead of reporting false success in the web console

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

- `Polaris-fedora44-x86_64.rpm`
- `Polaris-ubuntu24.04-x86_64.deb`
- `Polaris-arch-x86_64.pkg.tar.zst`
