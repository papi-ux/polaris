# Roadmap

Polaris is public and usable today, but it is still early. This roadmap is meant to set expectations and make it easier to see where testing and contributions help most.

## Current Focus

- Keep Fedora 42/43/44 and Arch release packages reliable.
- Keep CachyOS and other pacman-compatible Arch derivatives close to the Arch package path.
- Validate Bazzite as a tester host path using the Fedora RPM through rpm-ostree.
- Promote Ubuntu 24.04 from experimental DEB support to a better-tested Debian-family path.
- Keep openSUSE Tumbleweed source-build guidance current while watching whether a release package is worth the maintenance cost.
- Tighten Linux compositor, capture, input, and encoder behavior across more GPUs and distros.
- Improve Mission Control diagnostics so failures are easier to understand without digging through logs.
- Keep Nova integration clear while preserving compatibility with standard Moonlight clients.

## Near-Term Work

- Bazzite Desktop Mode and Game Mode validation, especially Bazzite host to Steam Deck/Moonlight/Nova clients.
- Ubuntu DEB validation on real desktop hosts, followed by broader Debian-family install guidance.
- More runtime diagnostics for VAAPI and software encode paths.
- Clearer recovery flows when Steam, MangoHud, input isolation, or compositor startup misbehaves.
- Better examples for safe web UI deployment on trusted networks.
- Issue templates and support-bundle guidance that capture distro, GPU, compositor, client, launch mode, and logs up front.

## Later

- Wider distro packaging if the maintenance cost stays reasonable.
- Better hardware-specific guidance as more users report results.
- More automated smoke coverage for common launch, pairing, stop/cleanup, and watch/resume paths.
- Continued Browser Stream validation once the Chromium/WebTransport path proves useful outside development.

## Known Limits

- Polaris is Linux-host-only today.
- Fedora 42/43/44 and Arch have the recommended package paths.
- CachyOS generally follows the Arch package path, but derivative-specific gaps still need reports.
- Bazzite and Ubuntu 24.04 are tester package paths.
- openSUSE Tumbleweed is source-build supported; other distros are source-build/community-validation oriented.
- NVIDIA/NVENC is the most heavily validated encoder path; AMD/Mesa VAAPI is supported and should stay visible while broader validation catches up.
- Browser Stream is experimental.
- Some richer live-session behavior depends on Nova.

## Useful Feedback

- Bazzite rpm-ostree status, Desktop Mode/Game Mode state, GPU, and Steam Deck/Moonlight/Nova client results.
- Ubuntu package/source-build results with GPU, driver, desktop environment, and display server details.
- openSUSE Tumbleweed source-build results, dependency quirks, and optional local package notes.
- Distro, GPU, driver, compositor, encoder, and launch-mode details for successful installs.
- Logs and screenshots for failed pairing, launch, capture, encoder setup, input forwarding, or cleanup.
- Reports that compare Nova with another Moonlight-compatible client on the same host.
