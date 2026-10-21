# Play with Moonlight

Polaris speaks the Moonlight protocol, so any Moonlight client can pair with it: the official
Moonlight apps on Android, iOS, Windows, macOS, and Linux, the Steam Deck build, and forks such as
Artemis. This guide takes a standard Moonlight client from install to a tuned stream. Nova, the
client built alongside Polaris for Android and for Linux, has its own
[quick start](https://papi-ux.com/docs/nova/quickstart/) and adds what the Moonlight protocol cannot
carry: a launch mode per launch, Play Setup, Spaces, PyroWave, watching another player's stream, and
live tuning from the client. [What each client gets](compatibility.md#clients) compares them row by
row, and says what the host can set for a Moonlight player instead.

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

Moonlight always pairs with the PIN. The two other routes on the Devices page are for Nova only:
**Trusted Pair**, which skips the PIN when Nova asks from a subnet you trust, and **Nova QR**.
Moonlight cannot ask for either.

What the access presets mean, and how to change a device's access later, is in
[Pair and manage devices](devices.md).

## 4. Start a stream

Moonlight lists what Polaris publishes: the **Desktop** entry, **Steam Big Picture** if you use
it, and every game in your library. Pick one.

Which display the game lands on is decided by the host's launch mode, not by Moonlight. The
default **Private Stream** runs the game on a private display without touching the host
monitors; **Mirror Desktop** streams the visible desktop instead. The modes and how to choose
one are in [Launch modes and capture paths](launch-modes.md). Moonlight cannot switch modes per
launch the way Nova can; the host's saved choice applies, unless the app's **Launch as** names
another mode. That is how one Moonlight library offers more than one way to play
([Launch as](apps.md#launch-as)).

On a **Private Stream** host, a Steam game cannot start while Steam is open on the host desktop,
and the launch is refused. To have Polaris quit desktop Steam first and then start the game, turn
on the device's **Close Steam on the host to start games** switch under **Devices, Edit Access**
([Editing a device](devices.md#editing-a-device)). Anything unsaved in that Steam is lost.

Disconnecting does not end the game. Polaris keeps it paused for the resume window
(**Settings, Audio/Video, Disconnect Resume Timeout**, five minutes by default) so a reconnect
resumes the session. Moonlight cannot watch a stream another device started: it never asks to
watch, so while that device owns the stream the host refuses Moonlight's launch. Watching needs
Nova.

## 5. Match the stream to the device

Moonlight's own settings decide what it asks for; Polaris decides what it can honour. The
combinations that matter:

| Moonlight setting | What Polaris does with it |
|---|---|
| Resolution and frame rate | Treated as the ceiling. Polaris streams at the requested mode when the host can provide it, and never optimises above it. If the request cannot be met, the host's fallback mode applies; set it with the **Display Planner** under **Settings, Audio/Video**. |
| Bitrate | Used as the starting target. With **Auto Quality** on, the host lowers the bitrate when round-trip time or encoder load climbs and recovers it afterwards, live where the session supports it and otherwise at the next launch; the strip on the Audio/Video page shows the live value. Moonlight sends no media loss counts the host can use, so packet loss alone does not lower it. |
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
  [Fix a bad stream with Doctor](doctor.md). For a Moonlight stream it has network and host
  evidence only: decode and render timing come from Nova for Android.
- A launch the host refuses shows one line in Moonlight: what went wrong and, when there is one,
  the change that fixes it.
- Permission denied when starting a stream means the device has **Browse & Watch** access; see
  [the troubleshooting entry](troubleshooting.md#paired-client-gets-permission-denied-403-when-starting-a-stream).
- No controller in the game, a game on the wrong screen, a black Steam Big Picture, or missing
  audio each have an entry in [Troubleshooting](troubleshooting.md).

## Ports

Polaris listens on a block of ports derived from one base port, the **Port** setting (`port`)
under **Settings, Network**, which is 47989 by default. Every other port is a fixed offset from
it, so changing the base port moves the whole block. The port map on the same page shows the
block for the saved base port.

| Default port | Offset from the base port | Protocol | Used for | Needed on the LAN |
|---|---|---|---|---|
| 47984 | -5 | TCP | HTTPS for paired clients: the library, launch, resume and quit, and Nova's Polaris requests | Yes |
| 47989 | 0 | TCP | HTTP host information and PIN pairing. With a changed base port, this is the port to give a client that adds the host by hand | Yes |
| 47990 | +1 | TCP | Web console (HTTPS), and Nova's live session events | Optional |
| 47998 | +9 | UDP | Video | Yes |
| 47999 | +10 | UDP | Control and input | Yes |
| 48000 | +11 | UDP | Audio | Yes |
| 48010 | +21 | TCP | RTSP session setup | Yes |

With a base port of 50000, for example, the block is TCP 49995, 50000, 50001 and 50021, and UDP
50009 to 50011.

The web console port is the only optional one. Moonlight never uses it, and on the host itself the
console is at `https://localhost:<port + 1>`. Allow it from the local network only if you manage
Polaris from another device's browser, or if you use Nova, which follows live session events on
it during a stream; the stream itself uses the other ports. Who may use the console is a separate
choice, **Origin Web UI Allowed** on the same page, which allows the local network by default.

Automatic discovery is not part of this block. On Linux the host announces itself through Avahi
over mDNS (UDP 5353), and the announcement carries the base port. If a client does not list the
host by itself, add it by its address ([step 2](#2-add-the-host)), or allow mDNS from your local
network as shown below.

Across networks, forward those ports to the host or let **UPnP** under **Settings, Network** do
it, and set the encryption mode for WAN sessions on the same page. The web UI itself stays
reachable only from the host or the local network unless **Origin Web UI Allowed** on that page
allows more.

### Allow the ports through a Linux firewall

Polaris does not change the host's firewall on Linux, and neither does `polaris --setup-host`.
If the host runs firewalld or ufw, a client on the same network may not reach it until the
firewall allows the ports above. Keep the firewall on and allow those ports from your local
network only. Streaming inside your home network needs no router forwarding.

A firewall is one possible cause among several, so check it against a failed attempt. Note the
client's error and save the host log from that same attempt: a log that shows only a normal
startup does not say why a connection failed. A host that waited in vain for a client's first
video or audio packet logs a line beginning `Initial Ping Timeout: no video ping arrived on UDP`
(or `audio`) with the port, which points at a firewall or a UDP path problem; Doctor reports the
same start as described in [When the stream never started](doctor.md#when-the-stream-never-started).

The commands below are examples to adapt, not something Polaris runs. They assume the default
base port, IPv4, a local network of `192.168.1.0/24`, and a network interface named `enp5s0`.
Replace those with your own values, and with your ports from the table if you changed the base
port. `ip -brief address` lists each interface with its address: an interface showing
`192.168.1.20/24` is on the `192.168.1.0/24` network. To see which firewall is active, run
`sudo firewall-cmd --state` and `sudo ufw status verbose`, and use the one that reports `running`
or `Status: active`.

**firewalld.** Find the zone that holds the LAN interface, then add rich rules limited to your
network in that zone. `--permanent` saves the rules, and `--reload` makes the saved configuration
the running one.

```bash
sudo firewall-cmd --get-active-zones
sudo firewall-cmd --permanent --zone=ZONE \
  --add-rich-rule='rule family="ipv4" source address="192.168.1.0/24" port port="47984" protocol="tcp" accept' \
  --add-rich-rule='rule family="ipv4" source address="192.168.1.0/24" port port="47989" protocol="tcp" accept' \
  --add-rich-rule='rule family="ipv4" source address="192.168.1.0/24" port port="48010" protocol="tcp" accept' \
  --add-rich-rule='rule family="ipv4" source address="192.168.1.0/24" port port="47998-48000" protocol="udp" accept'
sudo firewall-cmd --reload
sudo firewall-cmd --zone=ZONE --list-rich-rules
```

Replace `ZONE` with the zone that `--get-active-zones` lists for `enp5s0`. The rules apply only in
that zone and only to clients inside `192.168.1.0/24`. For the optional web console, add the same
rule with `port port="47990" protocol="tcp"`. For discovery, if
`sudo firewall-cmd --zone=ZONE --list-services` does not include `mdns`, add the same rule with
`service name="mdns"` in place of the port. To undo a rule, repeat it with `--remove-rich-rule` in
place of `--add-rich-rule`, then reload.

**ufw.** Add rules limited to the LAN interface and your network. ufw writes a port range with a
colon. To see what a rule would add without adding it, put `--dry-run` straight after `ufw`.

```bash
sudo ufw allow in on enp5s0 proto tcp from 192.168.1.0/24 to any port 47984,47989,48010 comment 'Polaris'
sudo ufw allow in on enp5s0 proto udp from 192.168.1.0/24 to any port 47998:48000 comment 'Polaris'
sudo ufw status verbose
```

For the optional web console, add another TCP rule like the first with `port 47990` in place of
the port list. Upstream ufw's default rules already accept IPv4 mDNS, so discovery needs no rule
of its own unless your distribution changed them. To undo a rule, repeat it with `delete`
straight after `ufw`, or find its number with `sudo ufw status numbered` and run
`sudo ufw delete NUMBER`.

These examples cover IPv4, the default **Address Family**. If you set it to IPv4+IPv6 and clients
connect over IPv6, add the same rules for your IPv6 prefix: `family="ipv6"` with that prefix for
firewalld, or the prefix after `from` for ufw with IPv6 enabled in its configuration.

## MoonDeck on Steam Deck

[MoonDeck](https://github.com/FrogTheFrog/moondeck) launches your Steam library through Moonlight
from the Deck's own UI and needs its host sidecar, MoonDeck Buddy, on the Polaris machine. Buddy
confirms its `MoonDeckStream` app exists by reading Sunshine's `apps.json` straight off disk, and
on Linux it looks in `~/.config/sunshine/apps.json` by default, which a Polaris host does not
have. Polaris keeps the same file, with the same shape, at `~/.config/polaris/apps.json`.

Point Buddy at it: in Buddy's `settings.json` (`~/.config/moondeckbuddy/`) set
**Sunshine apps filepath** to the absolute path of `~/.config/polaris/apps.json`, add an app named
`MoonDeckStream` in Polaris whose command runs Buddy's `MoonDeckStream` executable, and leave
"Continue streaming if the application exits quickly" unchecked, exactly as Buddy's own Sunshine
guide says. Everything past that check is Moonlight pairing and launching, which Polaris answers
the same way Sunshine does.
