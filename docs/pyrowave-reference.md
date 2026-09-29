# PyroWave reference

This page is for looking up one PyroWave detail at a time; to find out whether PyroWave is for you
and how to turn it on, start with [PyroWave](pyrowave.md).

## A KDE host with a display in HDR

If no display on the host is in HDR, none of this applies. On a KDE desktop with a display in HDR,
PyroWave cannot stream the desktop at all, SDR included, when capture goes through kms. KWin sends
HDR frames in a format PyroWave cannot read. This happens with **Request HDR when host supports it**
on or off in Nova. The format check comes before any HDR decision, so both end the same way.

From Polaris 1.4.14 the host reads the display's format before the stream starts and refuses it. A
Nova that tells the host at launch that it will ask for PyroWave shows the reason, which names HDR;
an older Nova shows an RTSP handshake failure with error 503. Polaris 1.4.13 lets Nova connect, show
no picture, then end the stream. If the host can run Private Stream, which captures its own session
rather than the desktop, it keeps offering PyroWave for that; otherwise it stops offering PyroWave
while the desktop is in HDR.

### Does capture go through kms?

If you set **Force a Specific Capture Method** (`capture`, under **Settings > Advanced** in the
Polaris web console) to **KMS**, it does; skip to [Confirm the failure](#confirm-the-failure).
Capture can also go through kms with that setting on **Autodetect (recommended)** on a host where
polaris-kms is set up. `polaris-kms` is a
separate package that lets Polaris capture straight from the graphics driver; most hosts never
install it. A host has it set up when the package is installed and
`sudo -H polaris --setup-host --enable-kms` has been run. As a quick first check, see whether the
package is installed with `rpm -q polaris-kms`, `pacman -Q polaris-kms` or `dpkg -s polaris-kms`.

To see which route capture takes, start a stream with any codec, then run
`journalctl --user --since '10 min ago' | grep 'capture_transport='` on the host. A line containing
`kms: capture_transport=` means capture went through kms. `portal: capture_transport=` or
`wlr: capture_transport=` means it did not.

### Confirm the failure

After the refused stream, run `journalctl --user --since '10 min ago' | grep 'pyrowave_capture_unreadable'`
on the host. On Polaris 1.4.13, grep for `cannot read` instead; it shows `fourcc 1211384385`, and
Doctor there does not flag this and labels the capture format `bgra8`, so it looks normal. Newer
Doctor raises `capture_format_unreadable_by_pyrowave` once any stream has captured the desktop.

### Fixes

Pick one of these:

- **Turn HDR off on the host.** HDR is a per display option in KDE System Settings, Display &
  Monitor. If you are not sure which display Polaris captures, turn HDR off on every display before
  you test, then start a new stream. Turning HDR back on brings the failure back, so this is an
  ongoing cost: turn HDR off before each PyroWave stream, and back on afterwards if you want it.
  Leave **Request HDR when host supports it** off in Nova as well: with the display out of HDR, the
  host refuses a PyroWave HDR request ([Limits](#limits)).
- **Use HEVC or AV1 instead of PyroWave.** The failure is PyroWave's own (its log line says
  `this codec cannot read`), so to keep the host's monitor in HDR, stream this host with HEVC or AV1
  instead. This changes nothing on the host, so HEVC and AV1 keep streaming this host as they do
  now. In the Nova for Android beta the codec is chosen in **Settings > Client Stream Defaults >
  Change codec settings** and applies to every host, so choose PyroWave again before streaming
  another host. In Nova for Linux, choose HEVC in **Play Setup > Video Codec** for this host and
  game.
- **Take capture off kms.** **Force a Specific Capture Method** offers **Autodetect (recommended)**,
  **NvFBC**, **wlroots**, **KMS** and **X11**, with no portal choice. Autodetect keeps capture off
  kms only when polaris-kms is not set up, so you may need both steps:
  1. If the setting is on **KMS**, set it to **Autodetect (recommended)**.
  2. If polaris-kms is set up, also run `sudo -H polaris --setup-host --disable-kms`.
  3. Start a stream and confirm the route with the `capture_transport=` log line above.

  This applies to every codec, HEVC and AV1 included, and it gives up HDR: on a KDE desktop, capture
  off kms gets no HDR frames (desktop portal capture gives none outside a Gamescope HDR session; see
  [Limits](#limits)), so HEVC and AV1 stop streaming the desktop in HDR too. PyroWave on a KDE host
  in HDR after this change has not been tested.
- **Change the launch mode** in **Settings > Audio/Video > Where games run** in the Polaris web
  console. That is the host's saved mode for the next launch, so it applies to every client and
  every codec ([Choose where games run](launch-modes.md)).
  - **Private Stream** (a launch mode that runs a game in its own session) captures with wlr
    whatever **Force a Specific Capture Method** says, so you do not need to change it first. It
    does nothing for streaming the desktop. On a KDE host in HDR it has streamed PyroWave; that test
    ran with the setting on **Autodetect (recommended)**.
  - **For the desktop**, the Desktop entry (the library entry that streams your desktop) can run on
    a Host Virtual Display instead of mirroring your screen (new in Polaris 1.4.13). On KDE that
    adds a new screen to your desktop session, sized to the client. It does not show your HDR
    monitor, and it is SDR only, because KWin virtual screens carry no HDR
    ([Choose where games run](launch-modes.md)). Polaris captures that screen through the desktop
    portal whatever **Force a Specific Capture Method** says. This route has not been tested with
    PyroWave on a KDE host in HDR.

Whatever you choose, PyroWave on a KDE desktop streams SDR: no desktop route gives it HDR frames, and
HDR on this codec has not been shown end to end. Polaris cannot yet convert KWin's HDR format for
this codec.

## A host that ran a Polaris 1.4.13 beta

The package repositories never serve a beta, so a host that only ever installed Polaris from
repo.papi-ux.com runs the release. If you installed a Polaris 1.4.13 beta package by hand, the
betas carry the same version, so an update will not replace them. The order is: check which build
is installed, reinstall the release if the build dates differ (on Ubuntu or SteamOS, reinstall
anyway), restart Polaris, then check again.

Which command replaces a beta depends on the distribution. On Fedora, `dnf install` of the release
file over a beta says the package is already installed and changes nothing; if you installed the
release that way, it did not take, and `dnf reinstall` does. On Arch, `pacman -U` of the release
file reinstalls it. On Ubuntu, `apt install` of the release file replaced the beta in a test, and
`apt install --reinstall` is the command below.

- Polaris 1.4.13-beta.1 has no encoder at all.
- Polaris 1.4.13-beta.2 and beta.3 can crash as a stream starts: reinstall the release on any host
  that ran either, whatever its GPU. The 1.4.13 release fixes it. The crash is in Polaris's Vulkan
  video encoder, not in PyroWave, and any stream that starts that encoder can take Polaris down
  before its first frame. On an AMD GPU, Polaris's automatic encoder choice tries it first for
  Private Stream (a launch mode that runs a game in its own session). These are Polaris betas;
  Nova's 1.4.13 betas are a different app.

### Is the release installed?

The version cannot tell a beta from the release, so compare build dates. First download the release
package from the
[Polaris v1.4.13 release](https://github.com/papi-ux/polaris/releases/tag/v1.4.13), then run these in
the folder you saved it to. On Fedora, compare
`rpm -qi polaris | grep 'Build Date'` with
`rpm -qip ./Polaris-fedora44-x86_64.rpm | grep 'Build Date'`. On Arch, compare
`pacman -Qi polaris | grep 'Build Date'` with
`pacman -Qip ./Polaris-arch-x86_64.pkg.tar.zst | grep 'Build Date'`. If the dates match, the release
is installed. Bazzite uses the Fedora comparison. On Ubuntu or SteamOS this page has no way to tell
a beta from the release, so reinstall anyway; it does no harm, but nothing here confirms it took.
After a reinstall on Fedora, Bazzite or Arch, the same comparison confirms that it worked.

If the host also has `polaris-kms`, compare it too. That package carries its own copy of Polaris,
encoder included, and on a host set up with `--enable-kms` the service runs that copy. Download the
release's `polaris-kms` file and compare `rpm -qi polaris-kms` with
`rpm -qip ./Polaris-kms-fedora44-x86_64.rpm`, or `pacman -Qi polaris-kms` with
`pacman -Qip ./Polaris-kms-arch-x86_64.pkg.tar.zst`, each piped to `grep 'Build Date'`.

A match covers the installed files, not the copy that is running. If Polaris has not restarted since
the install, restart it as described at the end of [Reinstall the release](#reinstall-the-release).

### Reinstall the release

In the same folder:

```sh
sudo dnf reinstall ./Polaris-fedora44-x86_64.rpm                 # Fedora
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst                 # Arch
sudo apt install --reinstall ./Polaris-ubuntu24.04-x86_64.deb    # Ubuntu
```

If the host also has `polaris-kms` (`rpm -q polaris-kms`, `pacman -Q polaris-kms` or
`dpkg -s polaris-kms` names it), download the release's `polaris-kms` package as well and reinstall
it:

```sh
sudo dnf reinstall ./Polaris-kms-fedora44-x86_64.rpm                 # Fedora
sudo pacman -U ./Polaris-kms-arch-x86_64.pkg.tar.zst                 # Arch
sudo apt install --reinstall ./Polaris-kms-ubuntu24.04-x86_64.deb    # Ubuntu
```

On Bazzite, replace the layered package with the release RPM as in
[Update on Bazzite](bazzite.md#update), then reboot. That step has not been tested over a beta, so
run the Build Date comparison after the reboot.

On SteamOS, run the SteamOS 3.8 command from the Install section of the
[v1.4.13 release page](https://github.com/papi-ux/polaris/releases/tag/v1.4.13) again. If the host
has `polaris-kms`, download `Polaris-kms-steamos3.8-x86_64.pkg.tar.zst` and add it to that block's
`pacman -U` line.

Then restart Polaris: `systemctl --user restart polaris`. If you started Polaris from the desktop
instead of as the service, quit it and start it again, because that command does not restart the
copy you launched. A package check covers the installed files, not the running copy:
`systemctl --user status polaris` should show the service active since a time after the reinstall.

## Check the host's GPU

Polaris encodes PyroWave with Vulkan compute rather than with the GPU's video engine, so the
encoder needs a Vulkan 1.3 GPU with the features its shaders use. The host checks its GPU once per
Polaris run, the first time a client or the web console asks what it serves.

From Polaris 1.4.14 the web console answers without a stream. Open **Settings > Encoder Profiles**;
on a Linux host every encoder tab starts with **Advertised codec support**, and its PyroWave row says
**Available** or **Not available**. When it is not available, the row shows the host's reason and
message, the same ones a client is given. When the host offers PyroWave only to stream modes with
their own compositor, such as Private Stream, the row also shows the refusal a launch into the
host's own mode gets. While a PyroWave stream runs, the row shows its route and the bitrate advice
in [How Polaris advises and tunes PyroWave](#how-polaris-advises-and-tunes-pyrowave).

On an older Polaris, or to read the GPU the host picked, check the log. To check a host before you
set up a PyroWave client:

1. Quit any Polaris you started from the desktop, then run `systemctl --user restart polaris`, so
   Polaris runs as the service and logs to your user journal. `systemctl --user is-active polaris`
   prints `active` when the service is the copy running.
2. Start a stream that is not a Space, from any client, with any codec. Stable Nova or Moonlight is
   fine.
3. On the host, as the user who runs Polaris and without sudo, run
   `journalctl --user --since '15 min ago' | grep 'PyroWave:'`. Run this within 15 minutes of
   step 1, or widen `--since` to reach back to it. Lines from before step 1 belong to the previous
   run.

These lines are logged at the Info level, which the default **Log Level** (**Settings > General** in
the Polaris web console) shows. If you changed Log Level, set it back to the default for this check.

- `PyroWave: encoding on <GPU>, queue family ...` means the host can serve PyroWave.
- `PyroWave: no GPU on this host has what the encoder needs`, or a line saying there is no Vulkan
  loader or no Vulkan 1.3 instance on this host, means it cannot.
- If a stream ran after the restart and there is still no PyroWave line, the running Polaris has no
  encoder: it is 1.4.12 or older, Polaris 1.4.13-beta.1, or a source build without the encoder.

## Nova for Android

PyroWave needs Nova for Android 1.4.13-beta.2 or later. The [PyroWave](pyrowave.md) page asks for
beta.3 or newer because its [Turn it on](pyrowave.md#turn-it-on) steps were written for beta.3; a
phone already on beta.2 can play PyroWave, but its screens may differ from those steps.

### Which APK

Nova's betas are on [Nova's releases](https://github.com/papi-ux/nova/releases), marked Pre-release.
For almost any current phone or handheld, take the `arm64-v8a` APK, which on Nova 1.4.13-beta.3 is
`Nova-Beta-Android-arm64-v8a.apk`. `armeabi-v7a` is for older 32 bit devices and `x86_64` is for
Android on Intel or AMD hardware. Android will ask you to allow installs from the browser or file
manager you open the APK with.

On an Android device that uses 16 KB memory pages, the PyroWave library in Nova 1.4.13-beta.2 and
beta.3 may not load. PyroWave still appears in Settings, but a PyroWave stream then fails as it
starts, with the error described under [The phone's GPU](#the-phones-gpu). Other codecs and the app
itself are unaffected. Neither of the Android devices PyroWave was tested on uses 16 KB pages, so
this has not been confirmed on one.

The beta is a separate app with its own settings and host list. On the home screen the beta is
called Nova Pre (Nova Beta in later betas); stable Nova is called Nova.

### The phone's GPU

The phone decodes PyroWave with Vulkan compute. The codec asks for a Vulkan 1.3 GPU, and Nova checks
the phone in the background by decoding a known frame. In Nova 1.4.13-beta.3, a failed check is
recorded, but it does not hide PyroWave in Settings or block choosing it, it does not stop a stream
by itself, and it has no message of its own. So there is no check to run on the phone first.

A decoder that cannot initialize is a separate failure, and it does show a message. The stream
fails as it starts, and Nova says "Failed to start video stream establishment (error -2)". If the
stream's video surface is still valid, Nova also shows "Video decoder failed to initialize. Your
device may not support the selected resolution or frame rate." This is read from Nova
1.4.13-beta.3's source; no phone that fails has been tried. Try a lower resolution if the size is
the problem; otherwise choose another codec. On a phone with 16 KB memory pages, beta.2 and beta.3
can fail the same way ([Which APK](#which-apk)).

### 4:4:4 text

The sharp text comes from 4:4:4. 4:4:4 keeps full colour detail for every pixel, which is what keeps
small coloured text sharp. 4:2:0, which ordinary H.264 and HEVC streams use, stores colour at a
quarter of the resolution. Nova for Android asks for 4:4:4 whenever it asks for PyroWave, and takes
it when the host offers it, which a Polaris 1.4.13 host serving PyroWave does. Nova for Linux streams
SDR 4:2:0 only.

### Bitrate advice

Set **Video bitrate** in **Settings > Client Stream Defaults** before the first stream. Nova's advice
scales with the number of pixels and the frame rate: about 91 Mbps × (width × height ÷ 2,073,600) ×
(frame rate ÷ 60). That is about 91 Mbps for 1920x1080 at 60 fps, about 114 Mbps for 2400x1080 at
60 fps, and about 182 Mbps for 1920x1080 at 120 fps. The slider stops at 300 Mbps, which is below
Nova's advice for 2560x1440 at 120 fps (about 323 Mbps) and for 3840x2160 at 60 fps (about
364 Mbps). Nova does not raise the bitrate for you; when it is too low, Nova names its figure as the
stream starts: "PyroWave asks for about N Mbps at this resolution and frame rate." Every frame is a
key frame, so a small budget goes on the frame rather than on the detail.

A **Quality Preset**, the first item in Client Stream Defaults, sets resolution, bitrate and codec
together, and some presets set 10, 20 or 50 Mbps. After any change of preset, set the bitrate and
the codec again.

That advice is Nova 1.4.13's, a flat 0.73 bits per pixel judged by eye. From Polaris 1.4.14 the host
quotes the codec author's own model instead, which asks for more at small sizes and less at 4K
([How Polaris advises and tunes PyroWave](#how-polaris-advises-and-tunes-pyrowave)).

### One codec for every host

The codec chosen in **Settings > Client Stream Defaults > Change codec settings** applies to every
host in the beta, and Play Setup in Nova 1.4.13-beta.3 has no codec choice to override it. While
PyroWave is chosen, a stream to a Space, or to a host that does not offer PyroWave, is refused.
Choose another codec there before you stream one. On a newer Nova beta, if the codec is not under
Client Stream Defaults, look for a codec choice in Play Setup.

Auto never picks PyroWave, so it has to be chosen by name. The PyroWave entry is in Nova's Settings
whatever host you use. Whether the host can serve it shows when a stream starts, and on the host in
the web console ([Check the host's GPU](#check-the-hosts-gpu)).

### Nova Stream HUD

**Nova Stream HUD**, under **Settings > Overlays & Controls**, is off by default. During the stream
it shows the codec as PYRO, and a long press on it opens Command Center.

## Nova for Linux

Nova 1.4.13 attaches an experimental PyroWave build of Nova for Linux to
[its release](https://github.com/papi-ux/nova/releases/tag/v1.4.13):
`Nova-Linux-PyroWave-x86_64-alpha.flatpak`, beside the standard `Nova-Linux-x86_64-alpha.flatpak`,
which is built without the decoder. Earlier Nova releases, the 1.4.13 betas included, attach only
the standard Flatpak, which was named `Nova-Deck-x86_64-alpha.flatpak` before 1.4.13.

The PyroWave build is an Alpha, like the standard Flatpak. It negotiates SDR 4:2:0 only. No
PyroWave stream decoded by it on a Steam Deck has been recorded yet, so treat a Deck as untested.

### Install it

Download `Nova-Linux-PyroWave-x86_64-alpha.flatpak` and its `.sha256` file from the
[Nova 1.4.13 release](https://github.com/papi-ux/nova/releases/tag/v1.4.13). Close Nova, then run
this in the download folder (on a Deck, in Desktop Mode):

```sh
sha256sum -c Nova-Linux-PyroWave-x86_64-alpha.flatpak.sha256
flatpak install --user ./Nova-Linux-PyroWave-x86_64-alpha.flatpak
```

The PyroWave build has the same app ID as the standard Flatpak, `com.papi_ux.Nova`, so it replaces
that app and keeps its pairing and preferences. It has no automatic update feed. That holds when
both go into the same installation. If `flatpak list --app --columns=application,installation`
shows `com.papi_ux.Nova` under `system`, a `--user` install sits beside that copy rather than
replacing it; remove one of them, for example with `flatpak uninstall --system com.papi_ux.Nova`. To
go back, install `Nova-Linux-x86_64-alpha.flatpak` from the same release with
`flatpak install --user`, keeping the app data. The PyroWave build still offers H.264 and HEVC, and
Auto never picks PyroWave in it.

You can also build it yourself from `com.papi_ux.Nova.pyrowave.json`, beside `com.papi_ux.Nova.json`
in `clients/deck/packaging/flatpak/`, with the commands in
[Nova's Flatpak README](https://github.com/papi-ux/nova/blob/v1.4.13/clients/deck/packaging/flatpak/README.md).
A build from a later Nova tag works with a Polaris 1.4.13 host only while its
`clients/deck/pyrowave/protocol.h` still names `pyrowave-186f0393-sdr420-v1`.

### Turn it on in Nova for Linux

1. Open **Play Setup** for the game you want to stream.
2. Choose **Video Codec**, then **PyroWave · Experimental**. The choice is saved for that host and
   game only. The **Encoder** row then shows **PyroWave · Vulkan**.
3. Start the stream. If PyroWave cannot start, Play Setup says why:

| Play Setup says | Why |
|---|---|
| "This PC does not offer compatible PyroWave support. Use a matching enabled Polaris build or choose another codec." | The host does not offer PyroWave: Polaris older than 1.4.13, 1.4.13-beta.1, or a GPU that lacks what the encoder needs ([Check the host's GPU](#check-the-hosts-gpu)). |
| "PyroWave is not available in Spaces. Choose Auto or H.264." | The stream is a Space. |
| "PyroWave Vulkan decoding is unavailable on this Linux device. Choose another codec." | This device cannot decode PyroWave. |
| "This stream size exceeds this device's PyroWave decoder limits. Choose a smaller size or another codec." | The resolution is above what this device's decoder takes. |
| "This Nova build does not include PyroWave. Use an enabled experimental build or choose another codec." | PyroWave is still chosen in a build without the decoder, such as the Alpha. |

To stop using PyroWave for that host and game, choose H.264 or HEVC in the same place; those are the
alternatives the build offers.

Nova for Linux gives no PyroWave bitrate figure and does not warn when the bitrate is low, so set the
bitrate yourself: **Bitrate** in Play Setup before the stream, or **Live Bitrate** in Command Center
during it. The figures under [Bitrate advice](#bitrate-advice) are Nova for Android's advice. This
page has no verified figure for the Linux build. As a starting point only, Nova for Android's
formula gives about 45 Mbps for a Steam Deck's 1280x800 at 60 fps and about 67 Mbps at 90 fps.

The Nova Stream HUD is Nova for Android's. Nova for Linux's own HUD has a **CODEC** reading, which
its source at `v1.4.13-beta.3` sets to PyroWave during a PyroWave stream. This page has not checked
it on a live stream, so confirm the codec in Polaris [Mission Control](mission-control.md) as well.

## What each frame costs the host

You do not need to know your capture route to use PyroWave, unless the host is a KDE desktop with a
display in HDR ([above](#a-kde-host-with-a-display-in-hdr)). The route decides whether each frame is
copied to the GPU before it is encoded.

The host side is cheap. Even on a route whose frames arrive in host memory, a 1080p Private Stream
on an RTX 4090 measured 0.35 ms to copy each frame to the GPU plus 0.95 ms to encode it. Capture that
delivers frames already on the GPU skips the copy; the routes that do are listed next.

The encoder gets a frame already on the GPU only when capture hands it over while it is still in GPU
memory (a DMA-BUF). That happens with PipeWire capture that negotiated DMA-BUF, such as a KWin Host
Virtual Display, and with `capture = kms` on an eight bit or ten bit packed scanout, the frame the
graphics driver sends to the display (not a KDE display in HDR; see
[A KDE host with a display in HDR](#a-kde-host-with-a-display-in-hdr)). Private Stream, wlroots and
X11 capture, and PipeWire capture that fell back to shared memory, deliver frames in host memory,
which are copied to the GPU and converted there. Private Stream and Host Virtual Display are chosen
in the Polaris web console under **Settings > Audio/Video > Where games run**
([Choose where games run](launch-modes.md)).

A frame can cost more than that for two reasons. If the GPU path cannot start, colour conversion
falls back to the CPU (SDR only; the host log says so). An ultrawide source makes the copy bigger,
because the whole source is copied to the GPU every frame. A KWin Host Virtual Display avoids the
copy.

[Doctor](doctor.md), the stream check in Nova's Command Center and in Mission Control, names the
codec and reports the capture transport and whether frames were on the GPU or in host memory. The
stream stats also carry how the encoder took its frames, `pyrowave_route` (`zero_copy`,
`gpu_upload` or `cpu_convert`), and the encoder selection reason says the same in words. That is the
encoder's input only, so `zero_copy` does not prove that capture stayed on the GPU. Polaris 1.4.13
gives every PyroWave stream the reason "PyroWave is encoding with Vulkan after CPU color
conversion." instead, even though conversion runs on the GPU by default; there, go by the host log.

## Read the host log

`journalctl --user --since '10 min ago' | grep -E 'PyroWave:|Refusing (launch|resume|RTSP setup) \['`
shows the last stream. Run it as the user who runs Polaris, without sudo: Polaris runs as a user
service, so its log is in your user journal. The refusals below start with `Refusing` and name
their code in brackets; every other line below contains `PyroWave:`.

| Log line | What it means |
|---|---|
| `encoding on <GPU>, queue family ...` | The host check passed: this GPU can run the encoder. It appears once per Polaris run. |
| `no GPU on this host has what the encoder needs` | The host cannot serve PyroWave and does not offer it. `no Vulkan loader on this host` and `no Vulkan 1.3 instance on this host` mean the same. |
| `encoding a 1920x1080 frame where capture left it, on <GPU>` | Capture handed over a DMA-BUF, and the encoder read the frame where capture left it, with no copy. |
| `encoding straight from a 1920x1080 picture on <GPU>` | The frame arrived in host memory, was copied to the GPU, and its colour was converted there. |
| `falling back to converting frames on the CPU` | The GPU path could not start, so colour conversion runs on the CPU. This happens for SDR only. |
| `POLARIS_PYROWAVE_GPU_INPUT is off, so frames are converted on the CPU` | That environment variable forced the CPU converter. If this run's log has no such line, it is not set. |
| `Refusing launch [pyrowave_capture_unreadable]: PyroWave cannot ...`, or the same after `Refusing resume` or `Refusing RTSP setup` | The display capture would read is in a format PyroWave cannot read, so the stream was refused before it started; the rest of the line says which. See [A KDE host with a display in HDR](#a-kde-host-with-a-display-in-hdr). |
| `Refusing resume [capture_in_use_by_other_codec]: Another stream on this host is running ...`, or the same after `Refusing RTSP setup` | Another stream on the host is capturing for a different codec, one PyroWave and the other not, and one capture cannot serve both. |
| `Error: PyroWave: capture is handing over a dmabuf in a format this codec cannot read (fourcc ...)` | Capture hands over a format PyroWave cannot read, identified by its fourcc (the four character code of a pixel format), and the stream ends. From Polaris 1.4.14 this is a backstop for a display whose format changed after the stream started. See [A KDE host with a display in HDR](#a-kde-host-with-a-display-in-hdr). |
| `this client negotiated HDR and the captured display is not in HDR` | The client asked for HDR and the captured display is not in HDR, or its HDR metadata could not be read, so the stream is refused, whatever the capture route. See [Limits](#limits). |
| `over 300 frames, ... ms and encode ... ms a frame` | The average cost of a frame, after 300 frames (five seconds at 60 fps), then every 18,000 frames (five minutes). |

One more line matters, and that grep leaves it out: `Skipping FEC for oversized encoded frame(s)`
means the largest frames went out without their error correction (see [Limits](#limits)).

## How Polaris advises and tunes PyroWave

From Polaris 1.4.14 the host carries PyroWave's own bitrate model. The codec's author ran four
lossless game clips through it at every 16:9 size from 1280x720 to 3840x2160, scored the results
with PSNR-HVS-M-H, an objective metric weighted for how far away the picture is watched, and fitted
the bitrate each quality needed. Polaris reads that fit at two distances, each with its own quality:

- **A device's own screen**, 2.875 picture heights away, gets the far figure, the lower one, at
  31 dB. That target is calibrated by eye: on a Retroid Pocket 6, Control at 1920x1080, 120 fps and
  4:4:4 looked soft at 50 Mbps and right at 200, where the author's 35 dB asks about 400 and 31 dB
  asks 215.
- **A television or monitor**, 2 picture heights away, gets the near figure at 35 dB, the level the
  author calls good quality, until it is checked on a big screen.

The host computes these figures itself, and a fixture in Polaris's tests pins them at both
targets. Nova's estimator ports the same model. Mirroring the own screen target there is in review,
and until it lands Nova still reads both figures at 35 dB, so for 1920x1080 at 120 fps in 4:4:4 it
quotes about 400 Mbps where the host quotes 215.

Every figure below is the model's, as the request a client sets, at the host's default 10% FEC with
stereo audio in high quality, rounded up to a whole Mbps. More FEC or surround audio asks a little
more for the same picture. These are the model's readouts, not what Polaris recommends: Doctor
raises no higher than 300 Mbps, and nothing Polaris recommends on its own goes past that. A client
can set up to 500 Mbps by hand, so the television figures past 500 Mbps are beyond every Polaris
endpoint.

| Stream | Own screen (far, 31 dB) | Television or monitor (near, 35 dB) |
|---|---|---|
| 1280x720 at 60 fps, 4:4:4 | 106 Mbps | 185 Mbps |
| 1920x1080 at 60 fps, 4:2:0 | 101 Mbps | 246 Mbps |
| 1920x1080 at 60 fps, 4:4:4 | 109 Mbps | 298 Mbps |
| 1920x1080 at 120 fps, 4:4:4 | 215 Mbps | 594 Mbps |
| 2560x1440 at 60 fps, 4:4:4 | 126 Mbps | 381 Mbps |
| 2560x1440 at 120 fps, 4:4:4 | 251 Mbps | 761 Mbps |
| 3840x2160 at 60 fps, 4:4:4 | 165 Mbps | 350 Mbps |
| 3840x2160 at 120 fps, 4:4:4 | 328 Mbps | 699 Mbps |

The model is an objective metric on four game clips of about ten frames each, scored on luma only,
sampled at 16:9 and measured on SDR. The own screen target rests on one check by eye, on one device
and one game, and the television target on none. A picture smaller than 1280x720 or larger than
3840x2160 takes the bits per pixel of the nearest of those two sizes.

At 31 dB the own screen figure in 4:4:4 does not rise steadily with picture size. At 60 fps it
climbs from 106 Mbps at 1280x720 to about 114 at 1280x800 and 113 at 1600x900, falls to 109 at
1920x1080, then rises again, so a 1280x800 handheld streaming 4:4:4 is advised a little more than
the Retroid Pocket 6 at 1920x1080. That is the shape of the author's fit one dB above its lowest
quality, not a Polaris rounding. The 4:2:0 figures, and every figure at 35 dB, rise with size, so
Nova for Linux, which streams 4:2:0, is unaffected.

**Where clients read it.** While a PyroWave stream runs, `GET /polaris/v1/session/status` carries
`pyrowave_bitrate`, and `GET /polaris/v1/pyrowave/advice?width=&height=&fps=&chroma=420|444`
answers the same figures before a launch. Capabilities announce both as `pyrowave_advice_v1`, and
`docs/nova-contract.json` lists every field. Both name what they assume in `assumes`: the FEC share
the stream started with and its own audio, the figures its `bitrate_units` carry. The advice route
uses the asking client's own stream when it has one running, and the host's FEC share with stereo
in high quality when it does not, which is also what session status assumes until a stream's
handshake is recorded. Session status also carries `bitrate_units` for every stream, PyroWave or
not ([Live Tuning](live-tuning.md#bitrate-units)).

**Starved.** A stream is starved when what it runs at, as a request, is more than a tenth below the
figure Doctor would raise it to. The Retroid Pocket 6 check at 200 Mbps is 93% of its 215, and
healthy. Session status carries that rate as `request_kbps`, beside the encoder's own
`encoder_kbps`, and the console and Doctor's evidence quote it the same way. The host also keeps the
share of the last 240 frames, about four seconds at 60 fps, that left at 99% or more of PyroWave's
byte budget, and reports it. That share never makes a stream starved.

**Doctor.** A starved stream on a clean network, packet loss at most 2% and latency under 45 ms, the
limits Doctor's quality restore verifies with, gets a Doctor finding. It ranks below every network,
encoder and capture failure. Doctor quotes one figure, as the request a player sets: the far
figure, or 300 Mbps or `max_bitrate` when that is lower, and it says which held it. It offers to
raise the bitrate to that figure in steps of at most a quarter, each verified for 8 seconds, with
Undo. That is the one way Polaris raises a stream above the bitrate the player asked for. While Live
Tuning is on, Doctor says what to set instead of acting: the same figure at the encoder, as the live
bitrate, because a live bitrate applies at the encoder with no FEC or audio taken off. For 1920x1080
at 120 fps in 4:4:4 that is about 193 Mbps live for the request of 215. A stream cut below a
request that already meets the far figure climbs back to that request, by Doctor's ordinary quality
restore or by Live Tuning's own recovery, and Doctor never asks for less than the player set.

**More than Doctor raises to.** Where 300 Mbps or `max_bitrate` holds Doctor's figure below the far
figure, a stream within a tenth of that limit and still below the far figure, with more than 80% of
its last 240 frames at the byte budget, gets a finding of its own,
`pyrowave_needs_more_than_allowed`, under the same clean network rule and ranked below a starved
stream. Doctor names the model's figure and the limit and suggests a lower resolution or frame rate,
or HEVC. It changes no bitrate there. The far figure is past the limit, so no raise Doctor offers
reaches it, and a cut would only soften the picture further. Where only the 300 Mbps cap holds the
stream, Doctor adds that a player can set more by hand, up to 500 Mbps or `max_bitrate` if that is
lower. 3840x2160 at 120 fps in 4:4:4 on a device's own screen is one such stream, where the model
asks 328 Mbps and Doctor stops at 300. A stream set by hand at the far figure or above has
what the model asks, and a stream whose goal is the far figure itself gets no finding however full
its budget: that figure is calibrated to where the Retroid Pocket 6 looked right, and a full budget
says only that the codec would use more bits. A stream Doctor's quality restore would bring back to
the bitrate it launched at gets that restore instead.

**Live Tuning.** Live Tuning raises a stream above the player's request only to lift one below
`adaptive_bitrate_min` (2 Mbps by default) to that floor. On a PyroWave stream
it cuts no lower than half the far figure at the encoder, about 45 Mbps for 1920x1080 at 60 fps in
4:2:0, which Doctor quotes as a request of 51 Mbps, or no lower than the request when that is lower
still. At that floor it stops, and Doctor
suggests HEVC or a lower mode instead of another cut. It does not cut PyroWave for a slow encode,
because the encode takes as long at any bitrate. A live bitrate set by hand turns Live Tuning off
for that stream only.

**Caps.** A launch decides its bitrate before it knows the codec, so the Stability preset's 15 Mbps
cap, a device profile's rate and a saved paired profile (Keep in Step) are sized for H.264. A
PyroWave stream keeps its client's own request over all of them. Only `max_bitrate` caps it, and
the host log and session status name any cap that applied or was set aside. A client can set up to
500 Mbps by hand, above the 300 Mbps Doctor raises to, so the own screen figure for 3840x2160 at
120 fps is within reach and the television figures past 500 Mbps are not. That high, a frame can
outgrow its error correction ([Frame budget and FEC](#frame-budget-and-fec)).

## Limits

- **HDR has not been shown end to end.** It needs ten bit frames with HDR metadata and the GPU input
  path. Desktop portal capture (the desktop's screen sharing service) never provides those frames
  outside a Gamescope HDR session on the host, so a PyroWave HDR request on a KDE or GNOME desktop
  is refused, and the host logs `this client negotiated HDR and the captured display is not in HDR`.
  The host makes that refusal whenever the captured display is not in HDR, whatever the capture
  route, kms included. A KDE display in HDR under kms fails before that check, for a different
  reason, SDR included: see [A KDE host with a display in HDR](#a-kde-host-with-a-display-in-hdr).
- **A KDE host with a display in HDR cannot stream PyroWave at all when capture goes through kms**,
  SDR included ([A KDE host with a display in HDR](#a-kde-host-with-a-display-in-hdr)). Polaris
  cannot yet convert KWin's HDR format for this codec.
- **Spaces cannot use it.**
- **Auto never selects it**, on any client.
- **Among published clients, only the Nova for Android beta can use it**
  ([Is it for you?](pyrowave.md#is-it-for-you)). The Nova for Linux PyroWave build is unpublished
  and negotiates SDR 4:2:0 only.
- **Other clients are unaffected.** Polaris adds PyroWave to the codecs it offers every client, when
  its GPU can run the encoder and the session is not a Space. Moonlight and stable Nova ignore it and
  keep using H.264, HEVC or AV1.
- **It costs bandwidth.** It is intra only, so every frame is a key frame. From Polaris 1.4.14 the
  host quotes the codec author's own model, which asks about 101 to 761 Mbps across the modes in
  [How Polaris advises and tunes PyroWave](#how-polaris-advises-and-tunes-pyrowave), and recommends
  no more than 300 Mbps on its own. Nova 1.4.13
  advises about 91 to 364 Mbps from a flat figure, and its bitrate slider stops at 300 Mbps
  ([Bitrate advice](#bitrate-advice)). Valve quotes 100 to 500 Mbit/s and at least gigabit ethernet
  for the same codec in Steam Remote Play. Steam Remote Play's PyroWave and Polaris's are separate streams and cannot connect to each
  other.
- **Wi-Fi is not blocked, but it rarely keeps up.** Nothing stops PyroWave on Wi-Fi, but Wi-Fi
  usually cannot carry these bitrates, so expect stutter there. A wired host does not help the
  client's Wi-Fi link. A 100 Mbps adapter or port is below Nova's advice for anything above
  1920x1080 at 60 fps, and leaves no headroom even there.
- **There is no PyroWave frame size cap.** Each frame's budget is the bitrate divided by the frame
  rate, at least 4096 bytes. The largest frames are sent without their error correction, so packet
  loss is least protected when frames are largest.
- **An ultrawide source costs more when frames arrive in host memory**, because the whole source is
  copied to the GPU every frame. A 32:9 source is letterboxed, and the bars cost nothing.
- **Polaris 1.4.13 diagnostics do not show where colour was converted.** Use the host log there.
- **A mode that negotiates is not a performance promise.** A given GPU and network may not sustain
  it.
- **A working SDR stream is not HDR validation.** They are separate paths and need separate proof.

## Reference for implementers

This section is for people building Polaris from source or writing a PyroWave client.

### Building from source

`POLARIS_ENABLE_PYROWAVE` is on by default for Linux, so a normal configuration builds the encoder
with no flag to add. It needs two pinned submodules that a plain clone does not fetch:

```sh
git submodule update --init --recursive third-party/pyrowave third-party/Granite
```

With the option on and either submodule empty, configuration stops with an error that prints this
command. Pass `-DPOLARIS_ENABLE_PYROWAVE=OFF` to leave the encoder out, which builds exactly as the
tree did before the codec existed. [Building](building.md) covers the rest of a source build.

### Transport and profile tokens

Polaris pins PyroWave revision `186f0393b77f7755953b5ecde994bb1cec2e4155` (C API 0.6.0) and Granite
revision `b6cffd5ce81f540f0855e6778428483e14763d9b`. Keep client and host pins and the profile token
in agreement; the C API version alone does not establish bitstream compatibility. A Nova for Linux
build from a Nova tag later than `v1.4.13-beta.3` works with a Polaris 1.4.13 host only while its
`clients/deck/pyrowave/protocol.h` still names `pyrowave-186f0393-sdr420-v1`.

The RTSP offer contains `a=rtpmap:99 PYROWAVE/90000` and `a=fmtp:99` followed by the profile tokens
the host can serve, separated by spaces: `pyrowave-186f0393-sdr420-v1`, plus
`pyrowave-186f0393-hdr2020pq420-v1` when the GPU input path is available, which is the default. A
client looks for its own token as one whole element of that list. The host sends both lines only
when a GPU on it can run the encoder, and never for a Spaces session.

In `ServerCodecModeSupport` the host sets `0x00800000` for PyroWave and `0x01000000` for PyroWave
4:4:4, and `0x02000000` for PyroWave HDR10 only when the GPU input path is available. Nova's client
video format for PyroWave is `0x10000`, and the selected `bitStreamFormat` is `3`.

The host refuses the ANNOUNCE, with a reason in its log, when the codec cannot run on it, when the
dynamic range is neither SDR nor HDR10, when HDR is asked for without the GPU input path, when the
colour mode is not full range, when an SDR request names a colourspace other than Rec. 709 or an HDR
request one other than Rec. 709 or BT.2020 (the HDR stream is BT.2020 PQ either way), or when the
width or height is odd.

Each GameStream frame carries one complete raw PyroWave bitstream, with the coefficient packets
concatenated in order, and each frame is coded independently. The exact payload length excludes
transport padding. Encoding retained capture content still produces a new codec frame at the current
bitrate budget; it must not resend the previous codec sequence unchanged.

### Frame budget and FEC

Each frame's budget is the bitrate divided by the frame rate, at least 4096 bytes, with no upper
limit. There is no PyroWave frame size cap and no payload size minimum. A frame that needs more FEC
blocks than the transport allows is sent without FEC parity, and the host logs
`Skipping FEC for oversized encoded frame(s)`. At 500 Mbps and 60 fps a frame is about 1.04 MB,
inside the four-block envelope of about 1.30 MB at 10% FEC and 1392-byte packets. At 45 fps or
fewer, or with 1024-byte packets, a frame that fills its budget outgrows it and goes out without FEC
parity. The encoder accepts bitrate changes live, without restarting the stream. A frame that leaves
at 99% of its budget or more counts toward `ceiling_frame_share` in session status. The encode
happens where the frame is handed to the codec,
and from Polaris 1.4.14 that time counts in `encode_time_ms`. Live Tuning does not cut the bitrate
for it, since a lower bitrate does not shorten it.

### Capture routes and the `capture` setting

Private Stream and the other private compositor modes capture with wlr whatever `capture` says. A
Host Virtual Display made by KWin, EVDI or kscreen-doctor is captured through the portal whatever
`capture` says. When `capture` names another backend, the host logs
`cannot be captured through [<backend>]; this session uses [portal]` and restores the setting at
teardown.

The kms, portal and wlr backends each log one line per capture with the prefix `kms:`, `portal:` or
`wlr:` followed by `capture_transport=`, `frame_residency=` and `frame_format=`, naming the backend
that actually captured.

### Colour conversion and capture input

Colour conversion runs on the GPU by default. A frame captured into host memory is copied to the GPU
and converted in the encode pass, and a DMA-BUF from capture is imported without a host copy. The
CPU converter is used only when `POLARIS_PYROWAVE_GPU_INPUT` is `0`, `off`, `no` or `false`, or when
the GPU path cannot start on an SDR stream. It is eight bit, so it cannot carry HDR: with the
variable off, the host offers no HDR token and no HDR10 bit, and an HDR session that cannot use the
GPU path ends.

A DMA-BUF can be read in `XRGB8888`, `ARGB8888`, `XBGR8888`, `ABGR8888`, `XBGR2101010`,
`ABGR2101010`, `XRGB2101010` or `ARGB2101010`. A DMA-BUF in any other format, including the sixteen
bit float formats, ends the stream on its first frame, because capture keeps producing that format
for as long as the display keeps its mode. A KDE display in HDR scans out `ABGR16161616F`, fourcc
1211384385, printed `AB4H`, and kms capture hands it over as it is, so the stream ends with
`Error: PyroWave: capture is handing over a dmabuf in a format this codec cannot read (fourcc
1211384385); ending the stream, because that does not change while the display keeps its mode`.
This check runs before any dynamic range decision, so an SDR request ends the same way.
Polaris 1.4.13's diagnostics and its `kms:` log line label that format `bgra8`. From Polaris 1.4.14
the log names it `AB4H`, Doctor raises `capture_format_unreadable_by_pyrowave`, and the host reads
the scanout's format before the stream and refuses it with `pyrowave_capture_unreadable`. That
refuses earlier and says why; it is not a fix, because the conversion for sixteen bit float is not
written.

The encoder selection reason is built from `pyrowave_route` and says where the encoder converted
colour: after importing a DMA-BUF (`zero_copy`), after copying a frame from host memory to the GPU
(`gpu_upload`), or on the CPU (`cpu_convert`). It describes the encoder's input, not capture, so it
is no proof of a GPU native capture path. Polaris 1.4.13 gives every PyroWave stream the fixed
reason "PyroWave is encoding with Vulkan after CPU color conversion." The log lines under
[Read the host log](#read-the-host-log) are the observed path.

### Reading the numbers

Encoder timing leaves out the deliberate wait for the next frame, so it is not a duty cycle. The
bitrate the client asked for, the host's current target, the encoder's applied budget and the
measured received bitrate are four different numbers; do not read one as another.

### Validation boundaries

Build and test both enabled and disabled configurations. Enabled coverage includes whole-frame
transport, retained-frame encoding, live budget changes, resizing, letterboxing and colour
conversion. The disabled binary must not gain a PyroWave shared-library dependency. Run the matching
client parser, Vulkan decode and presentation tests against the host-produced frame as well.

Live automation needs one designated host owner and isolated capture and playback audio services
without access to physical outputs. Separate ports and a null capture sink on the desktop audio
service do not isolate client playback or prevent changes to desktop routing. Refuse a competing
host rather than stopping someone else's service.

A completed soak, upgrade checks and physical display, input and audio testing remain separate
acceptance gates. Decoded-frame counters do not establish physical presentation or audio quality,
and an interrupted soak is not a pass.
