# Steam profiles

Saved Docker profiles can launch Steam Big Picture or one canonical Steam game
ID through the experimental profile streaming path. A Steam profile owns its
persistent home and one dedicated Docker bridge. Production activation remains
off by default; this implementation has not passed real Steam or two-client
playback acceptance.

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

## Acceptance still required

The current stream contract requires a compatible manual SDR H.264 4:2:0 preset,
whole frame rate, and stereo audio with 5 ms packets. See the
[launch integration](container-multiseat-launch-integration.md) for device
permissions, revocation, cancellation, and host configuration.

Real Steam bootstrap, Big Picture focus and input, game installation, Proton's
runtime sandbox under the unchanged Docker security policy, game audio, and
two simultaneous clients still require acceptance using the exact produced
image. The NVIDIA layer currently supplies amd64 userspace; matching i386 NVIDIA
libraries remain required work for 32 bit NVIDIA games. Image checks and
process tests do not establish game compatibility or latency. Heroic and Lutris
launch adapters, profile assignment UI, and Nova optimizer integration remain
separate work. The ordinary one-person, one-device flow remains unchanged.
