# PyroWave

PyroWave is an experimental video codec for a wired gigabit local network, and of the published
clients only the Nova for Android beta can play it. Nova is the client built alongside Polaris.
Moonlight cannot play it on any platform, and no iPhone, iPad, Mac or Windows client can: if you
stream from one of those, nothing changes and there is nothing to turn on. Every frame is a key
frame, so the client decodes each one much faster than H.264, HEVC or AV1, and the stream needs much
more bandwidth for the same picture. Details are in the [PyroWave reference](pyrowave-reference.md).

## Is it for you?

It suits you if fast decoding, or sharp text on a streamed desktop (Nova for Android streams it in
4:4:4), matters more to you than bandwidth. It does not work in a [Space](spaces.md), which exists
only if you set one up. You will need:

- **The Polaris 1.4.13 release on a Linux host**, not a Polaris beta, which reports the same
  version. If you install from the package repositories and never installed a beta by hand, you
  have the release ([otherwise, check](pyrowave-reference.md#a-host-that-ran-a-polaris-1413-beta)).
- **Nova for Android 1.4.13-beta.3 or newer on the phone or handheld**, a beta that installs beside
  stable Nova ([below](#nova-for-android)). So: the Polaris release on the host, a Nova beta on
  the client.
- **Or, on Linux or a Steam Deck, Nova 1.4.13's PyroWave Flatpak**
  ([how to install it](pyrowave-reference.md#nova-for-linux)): SDR 4:2:0 only, untested on a Deck.
- **Wired gigabit ethernet on every link, the client's included**, usually through a USB ethernet
  adapter or dock ([below](#the-network)). Nothing blocks Wi-Fi, but it usually stutters.
- **Vulkan 1.3 on the host's GPU and on the client's.** You can
  [check the host first](pyrowave-reference.md#check-the-hosts-gpu); the phone has no check to run
  first ([why](pyrowave-reference.md#the-phones-gpu)).

**Caution, KDE host with a display in HDR:** if capture goes through kms, PyroWave cannot stream at
all, SDR included. Capture can go through kms when **Force a Specific Capture Method** (`capture`,
**Settings > Advanced** in the web console) is **KMS**, or, on Autodetect, when the optional
polaris-kms add-on is installed and enabled; most hosts have neither. If yours does, turn HDR off on
every display before each PyroWave stream, or keep HEVC or AV1 for HDR; PyroWave on a KDE desktop is
SDR either way
([how to check, and other routes such as Private Stream](pyrowave-reference.md#a-kde-host-with-a-display-in-hdr)).

## What you need

### Nova for Android

Install Nova for Android 1.4.13-beta.3 or newer from
[Nova's releases](https://github.com/papi-ux/nova/releases), where betas are marked Pre-release. For
almost any current phone or handheld, take `Nova-Beta-Android-arm64-v8a.apk`
([other devices](pyrowave-reference.md#which-apk)). Stable Nova, 1.4.13 included, cannot select
PyroWave on Android: its release builds leave the codec out.

The beta installs beside stable Nova as a separate app, **Nova Pre** on the home screen (**Nova Beta**
in later betas), with its own settings and host list. Pair your host in the beta ([Pair and manage devices](devices.md))
before you stream; stable Nova keeps its own pairing.

### Polaris

Install the [Polaris v1.4.13 release](https://github.com/papi-ux/polaris/releases/tag/v1.4.13)
with the [update guide](updates.md) or the [package repositories](repositories.md), which never
serve a beta. Every release package contains the encoder: Fedora 44 (Bazzite takes this RPM), Arch,
Ubuntu 24.04 and SteamOS 3.8. Polaris 1.4.12 and older, and Windows and macOS hosts, have none.

**If you ever installed a Polaris 1.4.13 beta by hand**, an update will not replace it (the
version is the same), and on Fedora neither does `dnf install` of the release file. Beta.1 has no
encoder; beta.2 and beta.3 have it but can crash as a stream starts, so a working PyroWave stream
does not show that the release is installed.
[Compare build dates, and reinstall the release](pyrowave-reference.md#a-host-that-ran-a-polaris-1413-beta).

### The network

Every link should be gigabit: the host's port, any switch, and the client's USB ethernet adapter or
dock. Many cheap USB adapters are 100 Mbps, which leaves no headroom even at 1920x1080 at 60 fps, so
pick one sold as gigabit (1000 Mbps). Turn Wi-Fi off on the client while you stream; if Nova still
reaches the host, the adapter is carrying the stream.

## Turn it on

You choose PyroWave in the beta, not on the host: the Polaris web console has no PyroWave setting.
Nova for Linux has [its own steps](pyrowave-reference.md#turn-it-on-in-nova-for-linux).

1. Open the beta, not stable Nova, then **Settings > Client Stream Defaults**. Steps 2 to 5 are
   on that screen.
2. If you use a **Quality Preset** (the first item), pick it first: it sets resolution, bitrate and
   codec together.
3. Set **Video resolution** and **Video frame rate**, then raise **Video bitrate** to Nova's advice:
   about **91 Mbps** for 1920x1080 at 60 fps, **114 Mbps** for 2400x1080 at 60 fps, **162 Mbps**
   for 2560x1440 at 60 fps and **182 Mbps** for 1920x1080 at 120 fps
   ([other modes](pyrowave-reference.md#bitrate-advice)). Pick the figure for the resolution you set
   here, not the host's screen. The slider stops at 300 Mbps, below the advice for 2560x1440 at
   120 fps (about 323), and Nova does not raise the bitrate for you.
4. Open **Change codec settings** and choose **PyroWave (experimental)**.
5. Leave **Request HDR when host supports it** off. HDR on PyroWave has not been shown end to end.
6. Start a stream that is not a Space.

The codec applies to every host in the beta. While PyroWave is chosen, a stream to a Space, or to
a host that does not offer PyroWave, is refused, so choose another codec before you stream one.
Auto never picks PyroWave.

## Check that it is working

- **In Nova for Android**, turn on **Nova Stream HUD** under **Settings > Overlays & Controls** (off
  by default). During the stream it shows the codec as **PYRO**.
- **In Polaris**, [Mission Control](mission-control.md) in the web console names the codec of the
  running stream.

Nova never falls back from PyroWave to another codec: a stream started with PyroWave chosen is
PyroWave.

**Capture path.** [Doctor](doctor.md) shows whether frames reached the encoder on the GPU or in
host memory; either works ([what each frame costs](pyrowave-reference.md#what-each-frame-costs-the-host)).
On Polaris 1.4.13, its "CPU color conversion" label shows for every PyroWave stream; ignore it.
Newer builds say where the encoder converted colour.

**Bitrate.** Plan the link for the full bitrate you set. Polaris 1.4.14 and newer quote PyroWave's
own figure, from its author's quality model, with the target for a phone's or handheld's own screen
set by a check on a Retroid Pocket 6. On such a screen, set about 101 Mbps for 1920x1080 at 60 fps
in 4:2:0, 109 Mbps in 4:4:4, which Nova for Android streams, and 215 Mbps at 120 fps in 4:4:4, at
the host's default 10% FEC with stereo audio
([other figures, and their limits](pyrowave-reference.md#how-polaris-advises-and-tunes-pyrowave)).
When a stream sits more than a tenth below that on a clean network, [Doctor](doctor.md) offers one
tap to raise it, to at most 300 Mbps, with Undo. Where 300 Mbps or your `max_bitrate` is less than
the model asks, as at 3840x2160 and 120 fps in 4:4:4, and most frames still fill the codec's byte
budget, Doctor suggests a lower resolution or frame rate, or HEVC, instead. Live Tuning raises a
stream above your setting only to its 2 Mbps floor, and stops cutting at half the advice.

## If it does not work

| Symptom | Cause | Fix |
|---|---|---|
| No PyroWave in the codec list. | You opened stable Nova, or an older Nova beta. | Open the beta app, 1.4.13-beta.3 or newer ([Nova for Android](#nova-for-android)). |
| No codec choice in Play Setup. | Nova for Android 1.4.13-beta.3's Play Setup has none. | Use **Settings > Client Stream Defaults > Change codec settings**. |
| Nova for Linux: no PyroWave in **Video Codec**, or Play Setup says it cannot start. | Only `Nova-Linux-PyroWave-x86_64-alpha.flatpak` has the decoder. Otherwise, Play Setup names the reason. | [Install the PyroWave Flatpak](pyrowave-reference.md#nova-for-linux), or look up [Play Setup's message](pyrowave-reference.md#turn-it-on-in-nova-for-linux). |
| Nova says "This host does not offer the PyroWave profile this build of Nova can decode." | The host runs Polaris older than 1.4.13, 1.4.13-beta.1 or a build without the encoder, or its GPU lacks what the encoder needs. Or the stream is a Space. | Install the release ([over a beta](pyrowave-reference.md#a-host-that-ran-a-polaris-1413-beta)), then [check the host's GPU](pyrowave-reference.md#check-the-hosts-gpu). For a Space, choose another codec. |
| Nova says "Failed to start video stream establishment (error -2)" or "Video decoder failed to initialize...". | The phone's decoder could not start at this size, or the phone cannot decode PyroWave. | Lower the resolution; otherwise choose another codec ([the phone's GPU](pyrowave-reference.md#the-phones-gpu)). |
| No picture ever, and `systemctl --user status polaris` on the host shows Polaris failed or restarted. | A Polaris 1.4.13-beta.2 or beta.3 package, whose Vulkan video encoder crashes as a stream starts. | [Reinstall the release](pyrowave-reference.md#a-host-that-ran-a-polaris-1413-beta). |
| KDE host with a display in HDR: the host refuses the stream with a message naming HDR, or RTSP error 503, SDR too. On Polaris 1.4.13, Nova connects, shows no picture, then the stream ends. | Capture goes through kms, and KWin's HDR frames come in a format PyroWave cannot read. | Turn HDR off on the host display, or use HEVC or AV1 ([other fixes](pyrowave-reference.md#a-kde-host-with-a-display-in-hdr)). |
| Refused with **Request HDR when host supports it** on. | The host refuses PyroWave HDR unless the captured display is in HDR, and it has not been shown end to end. | Turn **Request HDR when host supports it** off. |
| Soft picture, and Nova says "PyroWave asks for about N Mbps at this resolution and frame rate." | The bitrate is too low, often from a **Quality Preset** (10, 20 or 50 Mbps). | Raise **Video bitrate**, or lower the resolution or frame rate. |
| Stutter or dropped frames. | The link cannot carry the bitrate: Wi-Fi, or a 100 Mbps adapter or port. | Wired gigabit on every link, Wi-Fi off on the client, or a lower mode and bitrate ([why](pyrowave-reference.md#limits)). |
| A stream used another codec after you chose PyroWave. | A **Quality Preset** picked afterwards set the codec. | Set **Video bitrate** and choose PyroWave again. |
