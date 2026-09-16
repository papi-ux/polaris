# Spaces preview status, September 2026

The evidence behind the [Spaces guide](../spaces.md)'s "Preview limits"
section: what has been exercised, on what, and what has not. Moved here from
the guide on 2026-09-16 so the guide can stay in the order a player meets it.

## What the preview ships

The Spaces tab has host prerequisite checks, Docker installation guidance and
management for configured Steam Spaces. Creating additional Spaces from an
existing Steam setup is supported by the development backend.

First Space preparation is a persistent background job. It connects the
verified runtime download to a new private Steam home, with stop and retry
controls in Spaces. It does not report download progress; the runtime install
is a Docker pull with no byte count the job can read. The preview catalog is
empty until a runtime completes publication and review, so this build shows
that the download is unavailable. The next step selects a detected graphics
card, saves Spaces configuration and offers an explicit restart. The initial
configuration permits one active Space; simultaneous Spaces need a separately
reviewed graphics budget, which the host now publishes to clients as
`capacity`.

The current runtime requires the Polaris service account to use UID and GID
1000.

## What has been validated

| Host packaging | Spaces evidence |
| --- | --- |
| Fedora RPM | Fresh Fedora 44 VM package, Docker, security setup, reboot and removal checks passed without a GPU; an existing Fedora NVIDIA host has Steam, controller and simultaneous-stream evidence |
| Arch package | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| Ubuntu DEB | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| SteamOS package | Native host packaging does not imply Spaces installation support on the system image |

| Capability | Current boundary |
| --- | --- |
| NVIDIA encoding | Exercised in the development Steam runtime |
| AMD encoding | Do not infer Spaces support from native Polaris VA-API support; equivalent runtime acceptance is still needed |
| 60 FPS | Two 1080p streams completed 15 minutes; Shield Ethernet listening passed, RP6 Wi-Fi audio underruns remain unresolved |
| 120 FPS | Two streams stayed connected through a 15 minute test with one at 120 FPS; uneven presentation and audio gaps remain unresolved |
| Browser Stream | Separate experiment; it does not open a Space's isolated stream |
| Automatic recovery | Do not apply global host adjustments to a Space; isolated telemetry and verified session-scoped repair are still needed |

Registry publication and first runtime download remain release gates. A fresh
NVIDIA graphics installation has not been validated. The
[September 15 acceptance report](container-multiseat-acceptance-20260915.md)
records fresh Fedora installation, simultaneous 60 FPS streams, Shield
listening and Space reopening; the
[sustained streaming report](container-multiseat-sustained-streaming.md)
records the earlier frame rate measurements; the
[audio timing investigation](container-multiseat-audio-timing.md) records the
measured audio results and their limits. Each states its remaining limits.

## Measuring sound and stuttering

For a comparison, set the device's display settings in Polaris to 1920 × 1080
at 60 FPS. In Nova, open Play Setup, choose Auto for Frame Rate and Device
Settings for Resolution. Check the game during play with one Space, then
repeat with the other intended Spaces running.

1. Note the time, Space name, game, frame rate, bitrate and whether the client
   uses Wi-Fi or Ethernet. Note whether picture or controls also paused.
2. Save the game before reconnecting; the current runtime ends the game
   session when its stream disconnects.
3. Compare the same game and settings over Ethernet on the same client when
   available, otherwise another access point. Change one thing at a time.
4. Repeat with heavy downloads, builds and updates paused on the host, then
   compare one Space with the intended number of simultaneous players.
5. Keep the observations for Doctor & Support.

A low average latency or zero reported video packet loss does not rule out
brief audio delivery pauses. The preview has recorded both host scheduling
stalls and delays on wireless client paths. A quiet wired comparison narrows
the investigation; it does not identify a router or prove every client
reliable.

## Catalogs, selections and networking

Catalogs with explicit Desktop Access use schema 4. Catalogs with extra Space
access grants use schema 3. Archived-only catalogs use schema 2; catalogs with
neither use schema 1. Older builds reject newer schemas instead of guessing
their permissions. Selections live in a private file beside the catalog with a
`.selections` suffix, itself schema 1. A selection is never an access grant:
it is revalidated against the catalog before use. Keep the catalog, selections
and volume backups together.

Each running Space has a private Docker address on its own bridge network.
Nova connects to the Polaris host address and selects a Space by name; there is
no extra IP address per player. Outbound connections share the host network's
public address.
