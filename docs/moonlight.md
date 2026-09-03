# Play with Moonlight

Polaris speaks the Moonlight protocol, so any Moonlight client can pair with it: the official
Moonlight apps on Android, iOS, Windows, macOS, and Linux, the Steam Deck build, and forks such as
Artemis. This guide takes a standard Moonlight client from install to a tuned stream. Nova, the
Android client built alongside Polaris, has its own [quick start](https://papi-ux.com/docs/nova/quickstart/) and adds
launch modes, watch mode, and live tuning that the Moonlight protocol cannot carry.

## 1. Install Moonlight on the device

Install the Moonlight client for your platform from the [Moonlight project](https://moonlight-stream.org/)
or your platform's store. Any current release works; Polaris needs nothing beyond the standard
protocol, and there is no Polaris-specific client plugin.

Make sure the client and the host share a network, or that the host is reachable through the ports
listed in [Ports](#ports) if you stream across networks.

## 2. Add the host

Open Moonlight. If the host and the client are on the same network and **Enable Discovery** is on
under **Settings, Network**, the Polaris host appears by itself. If it does not, add it by hand
with the host's IP address or hostname.

Moonlight shows a four-digit PIN and waits.

## 3. Pair with the PIN

In the Polaris web UI open **Devices**, choose **Manual PIN**, type the PIN Moonlight is showing,
give the device a label if you want one, keep **Game Control** selected, and choose **Send**.

Moonlight finishes the handshake and shows the host as paired. If its library does not appear
immediately, refresh the host in Moonlight.

Two alternatives:

- **Trusted Pair** skips the PIN for devices already inside a subnet you list under
  **Settings, Network, Trusted Subnet Auto-Pairing**. Use it only on networks you control.
- **Nova QR** is for Nova only; standard Moonlight clients cannot scan it.

What the access presets mean, and how to change a device's access later, is in
[Pair and manage devices](devices.md).

## 4. Start a stream

Moonlight lists what Polaris publishes: the **Desktop** entry, **Steam Big Picture** if you use
it, and every game in your library. Pick one.

Which display the game lands on is decided by the host's launch mode, not by Moonlight. The
default **Private Stream** runs the game on a private display without touching the host
monitors; **Mirror Desktop** streams the visible desktop instead. The modes and how to choose
one are in [Launch modes and capture paths](launch-modes.md). Moonlight cannot switch modes per
launch the way Nova can; the host's saved choice applies.

Disconnecting does not end the game. Polaris keeps it paused for the resume window
(**Settings, Audio/Video, Disconnect Resume Timeout**, five minutes by default) so a reconnect
resumes the session. A second client can join an existing stream as a viewer when
**Browse & Watch** access allows it and `max_sessions` is above one.

## 5. Match the stream to the device

Moonlight's own settings decide what it asks for; Polaris decides what it can honour. The
combinations that matter:

| Moonlight setting | What Polaris does with it |
|---|---|
| Resolution and frame rate | Treated as the ceiling. Polaris streams at the requested mode when the host can provide it, and never optimises above it. If the request cannot be met, the host's fallback mode applies; set it with the **Display Planner** under **Settings, Audio/Video**. |
| Bitrate | Used as the starting target. With **Auto Quality** on, the host lowers the bitrate under packet loss and recovers it afterwards, live where the session supports it and otherwise at the next launch; the strip on the Audio/Video page shows the live value. |
| Video codec | H.264, HEVC, and AV1 are offered when the host encoder supports them and the matching option is on under **Encoder Profiles**. Let Moonlight choose automatically unless a device decodes one codec badly. |
| HDR | Needs a Main10-capable codec on both sides and the host side set up as described in [Linux HDR and Main10](configuration.md#linux-hdr-and-main10). SDR handhelds should leave it off. |
| Audio configuration | Stereo, 5.1, and 7.1 are all served. The host captures the sink chosen under **Settings, Audio/Video, Host audio capture**; leave it empty to let Polaris pick a virtual sink that matches the client's channel count. |
| Controller | Moonlight sends controller input; the host emulates a gamepad for the game (see **Settings, Input**). Controllers plugged into the host are hidden from private streams by default so the emulated pad is the only one the game sees. |

Start with Moonlight's defaults, stream once, then open **Mission Control** on the host: the live
strip shows latency, frame rate, loss, and bitrate, and Doctor names the limiting factor if there
is one. Change one Moonlight setting at a time and watch that strip.

## 6. When it does not look right

- Open **Doctor & Support** on the host while the stream is still running. Doctor separates
  network, host, and client evidence and offers the safest fix; the details are in
  [Fix a bad stream with Doctor](doctor.md).
- Permission denied when starting a stream means the device has **Browse & Watch** access; see
  [the troubleshooting entry](troubleshooting.md#paired-client-gets-permission-denied-403-when-starting-a-stream).
- No controller in the game, a game on the wrong screen, a black Steam Big Picture, or missing
  audio each have an entry in [Troubleshooting](troubleshooting.md).

## Ports

Polaris listens on a block of ports derived from its base port (47989 by default). The web UI
lists the live map under **Settings, Network**.

| Port | Protocol | Purpose |
|---|---|---|
| 47984 | TCP | HTTPS pairing and control |
| 47989 | TCP | HTTP discovery and launch |
| 47990 | TCP | Web UI |
| 48010 | TCP | RTSP session setup |
| 47998 to 48000 | UDP | Video, control, and audio |

Across networks, forward those ports to the host or let **UPnP** under **Settings, Network** do
it, and set the encryption mode for WAN sessions on the same page. The web UI itself stays
reachable only from the host or the local network unless **Web UI origin** on that page allows
more.
