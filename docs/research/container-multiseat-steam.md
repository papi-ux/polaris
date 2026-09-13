# Steam profiles

Saved Docker profiles can launch Steam Big Picture or one canonical Steam game
ID through the experimental profile streaming path. A Steam profile owns its
persistent home and one dedicated Docker bridge. Production activation remains
off by default. An isolated NVIDIA/Docker run has reached the Big Picture
sign-in screen through Nova while a separate profile streamed to Moonlight.
Steam game playback still requires acceptance.

## Provision and assign

Build the Steam runtime from the current committed source using the
[locked image workflow](../../containers/multiseat/RUNTIME-IMAGES.md). An older
image can contain the installer while lacking this launch adapter. Use the new
artifact's immutable local `worker_reference`, and the ordinary Polaris service
user with UID and GID 1000:

```sh
polaris --multiseat-profiles create-steam "$catalog_path" "Steam room" "$worker_image_id"
polaris --multiseat-profiles create-steam "$catalog_path" "One game" "$worker_image_id" 570
polaris --multiseat-profiles assign "$catalog_path" "$profile_id" "$paired_device_id"
```

Omitting the game ID selects `big-picture-v1`. An explicit ID must be a positive
decimal integer no greater than 4294967295, without leading zeroes. URLs, paths,
shell text, account credentials, and additional Steam options are rejected.
An ID selects a game; it does not install it, establish ownership, or bypass
Steam authentication. The [saved catalog](container-multiseat-profile-storage.md)
holds names and assignments privately. Each profile permits one active seat.

The image supplies the packaged installer. The client downloads into the
profile's home on first use, and the user completes prompts and sign-in through
the streamed UI. Polaris never accepts credentials in the catalog or launch
request. Initial installation needs network access; offline operation also
requires prior setup as described by [Valve's Linux instructions](https://github.com/ValveSoftware/steam-for-linux)
and [Steam offline mode guidance](https://help.steampowered.com/en/faqs/view/0E18-319B-E34B-B2C8).
Client updates and games are mutable profile data, separate from the locked
runtime image's package provenance.

## Network ownership

Provisioning initializes the private volume without networking, then creates
`pn-<profile-id>` on the same local Docker daemon. The bridge has one opaque
ownership label, IPv4 masquerading enabled, and inter-container connectivity
disabled. IPv6, overlay networking, ingress, published ports, custom DNS
overrides, and additional network attachments are not admitted.

The controller inspects an empty bridge before launch, selects it by immutable
network ID, and checks that identity again immediately before Docker runs.
Recovery requires the same network policy and only the exact worker as its
member. The worker receives no Docker socket or network administration
capability. The Gamescope validation workload keeps `--network=none`.

This is ordinary Docker outbound networking for client downloads, sign-in, and
game traffic. It is not a destination firewall: host and LAN services may remain
reachable, subject to the host's routing and firewall. Docker's
[bridge documentation](https://docs.docker.com/engine/network/drivers/bridge/)
describes the underlying connectivity policy. Streaming media still travels
through authenticated local IPC to Polaris; worker ports are not published.

Only successful volume and bridge verification publishes the profile. Failed
or interrupted provisioning can retain labeled resources; errors report their
opaque names when available. Neither failure nor worker teardown deletes the
profile's volume or network. Inspect ownership and catalog state before manual
recovery. A missing or changed network blocks launch/recovery but does not
prevent stopping the exact owned container.

## Process lifetime

The provider executes the trusted image's Bash interpreter with the fixed
`/usr/games/steam` package script and `-gamepadui`. A selected game adds only
`-applaunch` and its validated numeric ID. The package script keeps its normal
path so Steam can retain a usable launcher path across updates and restarts.
The root filesystem stays read only; writable client files remain in the
private home. The environment contains only the seat's display, audio, input,
and profile settings.

The launcher verifies the nested compositor's identity and protocols before
starting Steam. Its subreaper retains owned descendants if the initial script
exits while Steam continues. Once all owned processes exit, the launcher exits.
Seat cancellation stops and reaps that tree, including detached helpers, while
other seats retain their own process and resource lifetimes. Readiness proves
supervision has started, not successful login or rendered game frames.

## Steam sandbox requirements

Steam workers require the matching installed
[Polaris seccomp policy](../../containers/multiseat/seccomp/README.md). It adds
the nested user namespace and mount operations used by Steam's runtime to a
pinned Docker default policy. The outer worker still drops all capabilities
and uses no new privileges. Missing, writable, or changed policy files refuse
launch; recovery also verifies the exact policy reported by Docker.

The optional NVIDIA SELinux domain permits outbound TCP, ephemeral client
socket binding, and TCP helper listeners over the profile's private network.
It also permits tmpfs remounts inside Steam's nested sandbox. The outer worker
retains no capabilities and exposes no published ports. The policy retains
enforcing SELinux without the broad container networking domain attribute.
The ordinary container domain still denies the required tmpfs remount on the
tested Fedora host; other GPU and distribution lanes require their own
acceptance.

## Acceptance still required

The current stream contract requires a compatible manual SDR H.264 4:2:0 preset,
whole frame rate, and stereo audio with 5 ms packets. The worker produces
128 kbps constant bitrate Opus with DTX disabled, giving 80 byte packets for
Moonlight audio recovery. The host rejects an older or faulty worker if its
packet size changes before forwarding the changed packet to a client. Moonlight can request this
contract. Nova can resolve an assigned profile and use its supported stream
preset. See the
[launch integration](container-multiseat-launch-integration.md) for device
permissions, revocation, cancellation, and host configuration.

Physical testing on the NVIDIA/Fedora lane exercised the first-use installer
and client update while resolving the sandbox denials. The final rebuilt
Steam image reached Big Picture sign-in through Nova at 1080p60, and touch
input opened Steam's keyboard. A separate Gamescope profile streamed to
Moonlight concurrently. Disconnecting the Steam seat preserved that stream;
both workers then retired and left no IPC resources. The run retained
enforcing SELinux, no outer capabilities, private networking, and the exact
compiled seccomp policy. No account credentials were entered.

Game installation, authenticated Big Picture use, Proton, game audio,
controller behavior in games, and two simultaneous games still require
acceptance using the produced images. The NVIDIA Steam layer supplies and
checks amd64 and i386 vendor
libraries, generic graphics loaders, and their dynamic dependencies. Real
32 bit rendering remains part of game acceptance. Image checks and
process tests do not establish game compatibility or latency. Heroic and Lutris
launch adapters and Nova optimizer integration remain separate work. Profile
assignment is available in the host UI. The ordinary one-person, one-device
flow remains unchanged.
