# Support and Compatibility

What Polaris supports today, how well each path is validated, and where the honest limits are. Status
words mean specific things here: **Recommended** paths have official package assets and the most
validation, **Experimental** paths ship but need broader real-hardware coverage, and
**Source-build** paths have no published package yet.

Polaris is Linux-only by design. Windows and macOS host ports are not planned.

## Host distributions

| Area | Status | Notes |
|---|---|---|
| Fedora 44 | Recommended | Official RPM asset and most validated release path. See the [Fedora guide](fedora.md). |
| Arch Linux | Recommended | Official package asset. See the [Arch guide](arch.md). |
| CachyOS / Arch derivatives | Expected via Arch package | Pacman-compatible derivatives should start there; report derivative-specific dependency or runtime gaps. |
| SteamOS 3.8 x86_64 | Experimental Desktop Mode package | Dedicated package; physical Steam Deck gameplay, Game Mode, suspend, and update persistence are not yet certified. See the [SteamOS guide](steamos.md). |
| Bazzite | Experimental | Layer the Fedora RPM with `rpm-ostree`; Desktop Mode validated on NVIDIA with Headless Stream, real Steam and Game Mode need more coverage. See the [Bazzite guide](bazzite.md). |
| Ubuntu 24.04 | Experimental tester path | The DEB asset exists but this path needs broader real-hardware validation. See the [Ubuntu guide](ubuntu.md). |
| openSUSE Tumbleweed | Source-build supported | Dedicated dependency and build guide plus CI build coverage; no published package asset yet. See the [openSUSE guide](openSUSE.md). |
| Debian-family distros | Source-build oriented | Ubuntu 24.04 is the only direct DEB asset today. |
| Other Linux distros | Source-build / community validation | Bring distro, GPU, driver, compositor, and package details when reporting success or breakage. |

## GPU and encoding

| Area | Status | Notes |
|---|---|---|
| NVIDIA / NVENC | Best-tested | The main fast path and most validated encoder and runtime combination. |
| AMD / VAAPI | Supported, expanding validation | Mesa VAAPI is the Linux AMD baseline; GPU-native DMA-BUF is preferred when available and reported truthfully when it falls back. |
| Software encode | Supported fallback | Useful for diagnostics and unsupported hardware, but not the performance target. |
| HDR / Main10 | Conditional | Main10 SDR can work when requested; true HDR requires real HDR metadata from the active capture path. |

## Clients

| Area | Status | Notes |
|---|---|---|
| Nova for Android | Best experience | Full launch contract, watch mode, tuning, and richer live state. |
| Standard Moonlight clients | Compatible | Core streaming works without Nova-specific UX. |
| Browser Stream | Experimental | Browser-based path using WebTransport and WebCodecs; best tested on Chromium-family browsers. |

## Feature status

| Feature | Status | Why it matters |
|---|---|---|
| Headless Stream runtime | Recommended path | Launches games into a stream-only compositor instead of rearranging your physical desktop. |
| Nova-aware launch contract | Supported | Lets Nova show Private Stream, Host Virtual Display, Mirror Desktop, watch and resume, and safety state before launch. |
| Mission Control | Supported | Shows runtime, capture path, encoder, clients, stream health, and host actions in one cockpit. |
| Game Control pairing preset | Supported / default for new devices | Trusted clients can browse, launch, and send input without clipboard, file-transfer, or server-command permissions. Existing devices keep their saved access until edited. |
| Doctor and optional AI explanation | Supported / optional | Deterministic telemetry drives diagnosis and safe actions. AI may explain that evidence, but cannot define launch settings or Doctor actions. |

## Best-tested first setup

For the smoothest first run:

- **Host distro**: Fedora 44 or Arch Linux / CachyOS.
- **GPU path**: NVIDIA with NVENC is the most validated; AMD with Mesa VAAPI is supported and uses the
  same Headless Stream flow, with capture-path truth visible in Mission Control.
- **Desktop**: KDE Plasma Wayland is the most exercised daily driver, but Headless Stream launches its
  own compositor and is not KDE-only.
- **Config**: `headless_mode = enabled`, `linux_use_cage_compositor = enabled`,
  `linux_prefer_gpu_native_capture = enabled`.
- **Client**: Nova on an ARM64 Android handheld or Android TV device, or a standard Moonlight client
  for the core stream path.

The [headless fallback matrix](runtime.md#linux-lts-headless-fallback-matrix) shows which capture path
to expect on older LTS hosts and what packages each one needs.

## Known limitations

- Polaris is a Linux-only host. Windows and macOS host ports are not planned.
- Fedora and Arch are the most validated package paths. CachyOS should use the Arch path first, but
  derivative-specific issues still need reports.
- Bazzite support is experimental. Desktop Mode has Headless Stream validation on NVIDIA and growing
  AMD Mesa VAAPI coverage; real Steam and Game Mode flows need more hardware reports.
- Ubuntu 24.04 DEB packaging is experimental; other Debian-family distros are still source-build
  oriented.
- openSUSE Tumbleweed has source-build guidance and CI coverage but no published package asset yet.
  Leap and other RPM distros should start from source.
- NVIDIA and NVENC are the most heavily validated hardware path. AMD and Mesa VAAPI are supported but
  still need broader real-hardware coverage before claiming parity.
- Some UX surfaced in Nova — explicit launch recommendations, watch mode polish, live tuning —
  depends on the Nova Android client.
- MangoHud can still be risky on Steam Big Picture and some Steam and Proton launches.
