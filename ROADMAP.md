# Roadmap

Polaris is public and usable today, but it is still early. This roadmap is meant to set expectations and make it easier to see where testing and contributions help most.

## Current Focus

- Keep Fedora and Arch release packages reliable.
- Tighten Linux compositor, capture, and encoder behavior across more GPUs and distros.
- Improve Mission Control diagnostics so failures are easier to understand without digging through logs.
- Keep Nova integration clear while preserving compatibility with standard Moonlight clients.

## Near-Term Work

- Broader Debian-family install guidance and source-build cleanup.
- More runtime diagnostics for VAAPI and software encode paths.
- Clearer recovery flows when Steam, MangoHud, or compositor startup misbehaves.
- More examples for safe web UI deployment on trusted networks.

## Later

- Wider distro packaging if the maintenance cost stays reasonable.
- Better hardware-specific guidance as more users report results.
- More automated smoke coverage for common launch and pairing paths.

## Known Limits

- Polaris is Linux-host-only today.
- Fedora 42 and Arch have the most polished install path; other distros are source-build oriented.
- NVIDIA/NVENC is the most heavily validated encoder path.
- Some richer live-session behavior depends on Nova.

## Useful Feedback

- Distro, GPU, driver, and compositor details for successful installs.
- Logs and screenshots for failed pairing, launch, capture, or encoder setup.
- Reports that compare Nova with another Moonlight-compatible client on the same host.
