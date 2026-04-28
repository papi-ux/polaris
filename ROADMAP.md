# Roadmap

Polaris is public and usable today, but it is still early. This roadmap is meant to set expectations and make it easier to see where testing and contributions help most.

## Current Focus

- Keep Fedora and Arch release packages reliable.
- Validate Bazzite as the next host install path using the Fedora RPM through rpm-ostree.
- Promote Ubuntu 24.04 from source-build support to a tested DEB release path.
- Tighten Linux compositor, capture, and encoder behavior across more GPUs and distros.
- Improve Mission Control diagnostics so failures are easier to understand without digging through logs.
- Keep Nova integration clear while preserving compatibility with standard Moonlight clients.

## Near-Term Work

- Bazzite desktop-mode and gamemode validation, especially Bazzite host to Steam Deck/Moonlight clients.
- Ubuntu DEB validation on real desktop hosts, followed by broader Debian-family install guidance.
- More runtime diagnostics for VAAPI and software encode paths.
- Clearer recovery flows when Steam, MangoHud, or compositor startup misbehaves.
- More examples for safe web UI deployment on trusted networks.

## Later

- Wider distro packaging if the maintenance cost stays reasonable.
- Better hardware-specific guidance as more users report results.
- More automated smoke coverage for common launch and pairing paths.

## Known Limits

- Polaris is Linux-host-only today.
- Fedora 42, Fedora 43, and Arch have the most polished install paths; Bazzite is experimental; Ubuntu 24.04 packaging is staged; other distros are source-build oriented.
- NVIDIA/NVENC is the most heavily validated encoder path.
- Some richer live-session behavior depends on Nova.

## Useful Feedback

- Bazzite rpm-ostree status, desktop/gamemode state, and Steam Deck client results.
- Ubuntu package/source-build results with GPU, driver, desktop environment, and display server details.
- Distro, GPU, driver, and compositor details for successful installs.
- Logs and screenshots for failed pairing, launch, capture, or encoder setup.
- Reports that compare Nova with another Moonlight-compatible client on the same host.
