# Support and compatibility

What Polaris supports today, how well each path is validated, and where the honest limits are. Status
words mean specific things here: **Recommended** paths have official package assets and the most
validation, **Supported installation** paths have a maintained package/setup workflow with
feature-specific hardware limits, **Experimental** paths need broader real-hardware coverage,
**Unvalidated** paths have code but no release validation on that hardware, and **Source-build**
paths have no published package yet.

Polaris is Linux-only by design. Windows and macOS host ports are not planned.

## Host distributions

| Area | Status | Notes |
|---|---|---|
| Fedora 44 | Recommended | Official RPM asset and most validated release path. See the [Fedora guide](fedora.md). |
| Arch Linux | Recommended | Official package asset. See the [Arch guide](arch.md). |
| CachyOS / Arch derivatives | Expected via Arch package | Pacman-compatible derivatives should start there; report derivative-specific dependency or runtime gaps. |
| SteamOS 3.8 x86_64 | Supported on Steam Deck OLED | Dedicated package, proven in Desktop Mode and in Game Mode on a Steam Deck OLED. The Deck LCD, suspend, and update persistence are not yet certified. See the [SteamOS guide](steamos.md). |
| Bazzite 44 | Supported installation | Layer the Fedora 44 RPM with `rpm-ostree`; NVIDIA Desktop Mode has streaming evidence. Game Mode and driver-specific game flows need more coverage. The standalone system extension remains withdrawn. See the [Bazzite guide](bazzite.md). |
| Ubuntu 24.04 | Experimental tester path | The DEB asset exists but this path needs broader real-hardware validation. See the [Ubuntu guide](ubuntu.md). |
| openSUSE Tumbleweed | Source-build supported | Dedicated dependency and build guide plus CI build coverage; no published package asset yet. See the [openSUSE guide](openSUSE.md). |
| Debian-family distros | Source-build oriented | Ubuntu 24.04 is the only direct DEB asset today. |
| Other Linux distros | Source-build / community validation | Bring distro, GPU, driver, compositor, and package details when reporting success or breakage. |

## GPU and encoding

| Area | Status | Notes |
|---|---|---|
| NVIDIA / NVENC | Best-tested | The main fast path and the most validated encoder and runtime combination. |
| AMD / VA-API | Supported, expanding validation | Mesa's radeonsi VA-API driver is the AMD baseline. Capture copies each frame through system memory on purpose, and Mission Control reports it as SHM; the DMA-BUF route stays off until it is proven safe ([why](launch-modes.md#if-you-have-an-amd-card)). |
| Intel / VA-API | Unvalidated | Intel encodes through the same VA-API code as AMD, with Intel's own VA-API driver. No Intel GPU has been through release validation, so nothing on Intel is known to work or known to fail. Expect SHM capture, set `adapter_name` on an Arc card, and please report what you find. |
| Vulkan Video | Experimental | `encoder = vulkan` asks for it on a GPU whose driver can encode with Vulkan Video. On AMD, Auto tries it first for Private Stream and for Gamescope Stream through the portal, with VA-API as the fallback. It carries no AV1 in this build, and no HDR on Gamescope Stream; `encoder = vaapi` keeps VA-API. See [Vulkan Encoder](configuration.md#vulkan-encoder). |
| PyroWave | Experimental | Encoded with Vulkan compute rather than the video engine, so it needs a Vulkan 1.3 GPU that passes the host's check, of any vendor. The published measurements are from NVIDIA. Only Nova plays it ([Clients](#clients)). See [PyroWave](pyrowave.md). |
| 4:4:4 | Limited | Of the standard codecs, only the software H.264 encoder offers 4:4:4 on a Linux host, on the CPU; NVENC, VA-API and Vulkan Video stream 4:2:0. PyroWave is the only way to get 4:4:4 from the GPU, on a GPU of any vendor that passes the host's check. The 4:4:4 row under **Settings > Encoder Profiles** says which of these a host has. |
| Software encode | Supported fallback | Useful for diagnostics and unsupported hardware, but not the performance target. |
| HDR / Main10 | Conditional | 10-bit SDR (Main10) works where the encoder offers it. True HDR needs real HDR metadata from the capture path, which only some stream modes have: see [HDR by stream mode](#hdr-by-stream-mode). |

## Desktops and compositors by stream mode

What each stream mode needs from the host session, as the code supports it. KDE Plasma on Wayland
is the most exercised desktop; the others need more reports. [Choose where games run](launch-modes.md)
describes the modes themselves.

| Stream mode | Desktops it can run on | Needs |
|---|---|---|
| Private Stream | Any desktop, and a host with nobody signed in | `labwc` and `wlr-randr`. Polaris starts its own labwc session and captures it directly. |
| Private Stream (GPU-native) | A running Wayland desktop | Its labwc runs as a window under the host desktop, so a desktop session has to be running. |
| Gamescope Stream | Any desktop | `gamescope`. Polaris joins an idle Gamescope or starts its own, and captures it through the portal. |
| Mirror Desktop | KDE Plasma, GNOME, wlroots desktops such as Hyprland and Sway, X11 | Portal capture on KDE Plasma and GNOME; wlroots capture or the portal on wlroots desktops. An X11 session is captured by X11 capture, which copies every frame through system memory. |
| Host Virtual Display | KDE Plasma with output-pinned KWin capture; Hyprland with its native virtual-output backend | Creation and capture must both be available. EVDI and kscreen-doctor need KWin capture; the native Hyprland backend needs its wlroots capture protocols. GNOME Wayland, Sway and other desktops without those routes cannot stream this mode, even if EVDI can create a connector. |
| Desktop Takeover | Hyprland only | A live Hyprland session, `hyprctl`, and an EVDI or Hyprland virtual output. |
| Headless Dongle | KDE Plasma | A dummy plug. Polaris moves the desktop onto it with kscreen-doctor. |

`capture = wlr` needs a wlroots compositor. KDE Plasma and GNOME have none of its protocols, so there
it can serve only the two Private Stream modes, which capture Polaris's own labwc. A launch in any
other mode with `capture = wlr` is refused with `capture_backend_unavailable`, which says so. Leave
**Force a Specific Capture Method** (under **Settings, Advanced**) on Autodetect unless you know you
need one.

## HDR by stream mode

True HDR needs HDR metadata from the capture path, a 10-bit encoder, and a client that asks for HDR.
Everything else streams SDR.

| Stream mode | HDR |
|---|---|
| Mirror Desktop with `capture = kms` | HDR10 from an HDR monitor, proven on KDE Plasma with NVIDIA. It needs `sudo -H polaris --setup-host --enable-kms` and the rest of [the recipe](runtime.md#the-recipe-that-works-today). |
| Gamescope Stream | HDR10 when its gamescope carries Polaris's 10-bit PQ capture patch. The Nix package builds that gamescope; elsewhere, `POLARIS_GAMESCOPE_BIN` points the session at one. Proven on NVIDIA; AMD has no release validation, and Auto's Vulkan Video route on AMD carries no HDR. The patch is not in upstream gamescope yet ([ValveSoftware/gamescope#2270](https://github.com/ValveSoftware/gamescope/pull/2270)), so a distribution's gamescope streams SDR. |
| Private Stream, both forms | SDR. Polaris's own labwc session reports no HDR metadata, so Polaris never advertises HDR there. |
| Host Virtual Display on KDE Plasma | SDR. KWin's virtual screens carry no HDR. |
| PyroWave | SDR. HDR on PyroWave has not been shown end to end, and on a KDE host with a display in HDR and capture through kms, PyroWave cannot stream at all yet ([details](pyrowave.md)). |

## Host requirements

- **systemd is optional.** Nothing in the stream path needs it: `polaris` started directly streams on
  any init system, and the application menu entry falls back to starting it directly. The user
  service, headless boot (`--enable-headless-boot`) and the Gamescope Stream helper units are
  systemd units, and host sleep asks logind (systemd-logind or elogind) to suspend. No distribution
  without systemd has been through release validation.
- **Avahi drives discovery.** Polaris announces itself on the network through avahi-daemon. Without
  it, or with publishing turned off, clients do not find the host on their own: add it by its
  address. SteamOS ships Avahi with publishing off ([SteamOS guide](steamos.md#connect-a-client)).
- **Each stream mode's tools**, listed in the table above: `labwc` and `wlr-randr` for Private
  Stream, `gamescope` for Gamescope Stream, and a supported creator plus output-pinned capture
  provider for Host Virtual Display. EVDI alone does not add GNOME Host Virtual Display streaming.

## Clients

Polaris speaks the Moonlight protocol, so any Moonlight client can pair and stream. Nova, the client
built alongside Polaris for Android and for Linux, also reads the host's own API, which is where
most of what follows lives. Nova for Linux is an Alpha. Browser Stream is experimental: H.264 and SDR
only, no controller yet, and only on the local network.

| Feature | Nova for Android | Nova for Linux | Moonlight | Artemis | Browser Stream |
|---|---|---|---|---|---|
| Pairing | QR, Trusted Pair, PIN | Trusted Pair, PIN | PIN | PIN; QR unverified | None: it opens from the signed-in web console |
| Launch mode per launch | Yes | Yes | No: the host's mode applies, or the app's Launch as | Host Virtual Display only, from its virtual display option, on an app set to Host default | No: the host's mode applies, or the app's Launch as when that is a Private Stream mode |
| Play Setup | Yes | Yes | No | No | No |
| Spaces | Yes | Yes | Unverified | Unverified | No |
| PyroWave | A Nova beta, 1.4.13-beta.3 or newer | The PyroWave Flatpak, SDR and 4:2:0 only | No | No | No |
| 4:4:4 | With PyroWave | No | Software H.264 only | Unverified | No |
| HDR | Yes | Not yet | Yes | Yes | No |
| Watch Stream | Yes | Not yet | No | No | No |
| Live Tuning | Yes, from Command Center | Yes, from Command Center | Host side only | Host side only | Unverified |
| Doctor evidence | Network, host, decode and render | Network and host | Network and host | Network and host | Unverified |
| Host sleep | Yes | Yes | No | No | No |
| Wake-on-LAN | Yes | Not yet | Yes | Yes | No |
| Refusal detail | Reason, fix and code | Text only | Text only | Text only | Unverified |

What the rows mean, and where they come from:

- **Pairing.** Trusted Pair is a request only Nova sends: from a subnet listed under **Settings,
  Network, Trusted Subnet Auto-Pairing**, the host approves it without a PIN. Nova QR carries a
  one-time PIN that Nova for Android scans. The code is an `art://` link, the scheme Artemis uses
  for Apollo's one-time PIN, so Artemis may pair with it too; no Artemis pairing has been tested.
  Every other client types the PIN into **Devices, Manual PIN**
  ([Pair and manage devices](devices.md)).
- **Launch mode per launch.** Moonlight never asks for a mode, so the mode saved under **Where
  games run** applies to every Moonlight launch, unless the app's **Launch as** names another mode.
  Artemis's virtual display option asks for Host Virtual Display, which the host grants unless
  its own mode is Private Stream or the app's Launch as names another mode.
- **Spaces.** A device given a Space sees that Space as its only app. Choosing titles inside a Space
  is a Nova feature, and no Moonlight or Artemis launch of a Space has been tested. A Space streams
  H.264, SDR and stereo only, and PyroWave does not work in one.
- **PyroWave.** Stable Nova for Android leaves the codec out; a Nova beta, 1.4.13-beta.3 or newer,
  installs beside it and has it. On Linux it comes in Nova 1.4.13's separate PyroWave Flatpak ([PyroWave](pyrowave.md)).
- **4:4:4.** Nova for Android asks for 4:4:4 whenever it asks for PyroWave. A Moonlight client that
  offers 4:4:4 gets it only from the host's software H.264 encoder ([GPU and encoding](#gpu-and-encoding)).
- **HDR** follows the host's stream mode ([HDR by stream mode](#hdr-by-stream-mode)). Nova for Linux
  launches SDR today.
- **Watch Stream** needs a client that asks to watch. Moonlight never does, so while another device
  owns the stream the host refuses a Moonlight launch instead of adding it as a viewer.
- **Live Tuning** is a host setting that applies to every stream. Only Nova can also switch it from
  the client, and Nova for Linux can set a live bitrate as well. The host sees media loss only from
  Nova for Android, so for every other client it tunes on round-trip time and encoder load.
- **Doctor** has network and host evidence for every stream. Decode and render timing come only from
  Nova for Android, so only there can it blame the playback device. Doctor names which kind of
  client a stream is, Nova or Moonlight / Artemis, and what a Moonlight-protocol client cannot use
  ([Doctor](doctor.md)).
- **Host sleep** is a request only Nova sends, and the host accepts it only while **Allow Clients To
  Sleep This Host** is on. **Wake-on-LAN** is the client's own magic packet, sent to the MAC the
  host reports.
- **Refusal detail.** A refused launch carries a reason, the change that fixes it, and a stable
  code. Moonlight, Artemis and Nova for Linux show the reason and the fix as one line of text,
  without the code; Nova for Android shows all three.
- **Artemis** also gets two Apollo extras. Its scale factor sets the resolution the host renders at.
  Its clipboard action reaches the host, but Polaris has no clipboard on Linux yet, so a read comes
  back empty and a write fails.

### What the host can do for a Moonlight player

Moonlight asks only for a resolution, a frame rate, a bitrate, a codec, HDR and the audio channels.
Everything else is set on the host, where it applies to every Moonlight launch:

- **Where games run** under **Settings, Audio/Video** is the launch mode a Moonlight app set to Host default gets.
- **One app entry per way of playing.** Each app's **Launch as** can name its own mode. The
  built-in Desktop entry is Mirror Desktop. An entry set to Host Virtual Display gets a screen
  sized to the client, on a Private Stream host too. One Moonlight library can offer a private
  game, the real desktop and a desktop on its own screen ([Launch as](apps.md#launch-as)).
- **The device's Display Profile**, under **Devices, Edit Access**, pins what that one device gets:
  a display mode in place of the one it asks for, a host output, the color range and HDR
  ([Editing a device](devices.md#editing-a-device)).
- **Close Steam on the host to start games**, in the same editor and off until you turn it on,
  lets a Moonlight launch of a Steam game quit desktop Steam on a Linux host instead of being
  refused, which Moonlight cannot ask for itself.
- **Live Tuning**, in Quick Controls on Mission Control, tunes a Moonlight stream's bitrate from the
  host, and Mission Control shows the result live. Doctor there names the limiting stage from
  network and host evidence.

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
- **Client**: Nova for Android on an ARM64 handheld or Android TV device, or a standard Moonlight
  client for the core stream path. [Clients](#clients) lists what each one gets.

The [headless fallback matrix](runtime.md#linux-lts-headless-fallback-matrix) shows which capture path
to expect on older LTS hosts and what packages each one needs.

## Known limitations

- Polaris is a Linux-only host. Windows and macOS host ports are not planned.
- Fedora and Arch are the most validated package paths. CachyOS should use the Arch path first, but
  derivative-specific issues still need reports.
- Bazzite uses the supported Fedora RPM installation. Desktop Mode has NVIDIA streaming evidence;
  Game Mode and AMD/Intel game flows need additional hardware validation. The withdrawn `.raw`
  system extension is a separate packaging path and is not promoted by this status.
- Ubuntu 24.04 DEB packaging is experimental; other Debian-family distros are still source-build
  oriented.
- openSUSE Tumbleweed has source-build guidance and CI coverage but no published package asset yet.
  Leap and other RPM distros should start from source.
- NVIDIA and NVENC are the most heavily validated hardware path. AMD and Mesa VAAPI are supported but
  still need broader real-hardware coverage before claiming parity. No Intel GPU has been through
  release validation.
- Launch mode per launch, Play Setup, Spaces, PyroWave, watching another player's stream and host
  sleep need Nova. [Clients](#clients) lists what each client gets, and what the host can set for a
  Moonlight player instead.
- MangoHud can still be risky on Steam Big Picture and some Steam and Proton launches.
