# Profile launch integration

Polaris can now own the Docker profile controller and route authenticated
GameStream launches into saved profiles. `multiseat_enabled` defaults to `false`.
An ordinary installation keeps its existing app list, host launch behavior,
display handling, and input path. This implementation is experimental.
Two simultaneous Moonlight instances have passed loopback playback checks;
independent physical clients and measured latency remain separate acceptance work.

## Explicit configuration

The host settings are `multiseat_enabled` and `multiseat_config`. The second is
an absolute path to an owned, mode 0600 JSON file in a private directory. Example
contents for one render device:

```json
{
  "schema": 1,
  "deployment_id": "polaris-profiles",
  "profile_catalog": "/var/lib/polaris/multiseat/profiles.json",
  "ipc_root": "/var/lib/polaris/multiseat/ipc",
  "selinux_type": "",
  "gpus": [
    {
      "id": "gpu-primary",
      "render_node": "/dev/dri/renderD128",
      "devices": ["/dev/dri/renderD128"],
      "max_seats": 2,
      "max_encoder_sessions": 2
    }
  ]
}
```

The example paths and GPU inventory must be adapted to the host. The dedicated
IPC directory must already exist, be owned by the service user, and have mode
0700. Keep both JSON files outside it. The GPU list is an exact device allowlist;
the factory validates physical device identities and rejects aliased capacity.
NVIDIA configurations require the exact associated DRM and NVIDIA nodes and
the installed `polaris_nvidia_worker_t` policy described in the
[Docker backend document](container-multiseat-docker.md). SELinux remains
enforcing. Workers receive their allocated input event nodes, never raw uinput,
a whole host input directory, privileged mode, or the Docker socket.

The configuration admits at most 16 GPUs, with 1 through 16 seats and encoder
sessions each and 64 devices per GPU. Unknown or duplicate fields, relative
paths, public files, symlinks, and mixed IPC authority directories are rejected.
It selects the existing local Docker Engine and runc defaults and enables worker
media. Runtime images must already be present by immutable image ID. No image
pull is part of startup. The current images and saved profiles require UID and
GID 1000.

Enable only after creating the private catalog with the
[administrative profile command](container-multiseat-profile-storage.md). An
empty catalog remains inert. Invalid enabled configuration or unavailable
authority stops startup with an error. Do not also enable the older
`multiseat_moonlight_input` owner. Routes are a snapshot for the controller's
lifetime, and its retained catalog lock prevents external administrative edits
until shutdown has proven cleanup complete. The Devices page can now change
assignments through the running controller while no profile session is active.
The existing `max_sessions` host limit still applies; its
default is two concurrent streams.

## Assigned device behavior

A paired device assigned to a profile receives one `Polaris Profile` app entry.
Its UUID and ID are stable public protocol identifiers, shared across profiles;
they do not select storage. Only the currently authorized paired device UUID
selects its private catalog route. A name, query parameter, or app ID cannot
select another person's profile.

Launching that entry queues startup on one controller owner thread. The runtime
reserves profile and GPU capacity, creates the authorized input device plan,
starts the worker, reconciles its state, and requires its authenticated media
connection before preparing RTSP. Launch permission, revocation, and immutable
client settings are checked again immediately before publication. Temporary
clients and watch requests cannot launch a profile. Clients must support
encrypted RTSP. The current shared RTSP handshake slot can reject overlapping
launch handshakes with a retryable conflict while established streams remain
independent.

The implemented media contract is SDR H.264 with 4:2:0 chroma, a whole frame
rate, and stereo audio in 5 ms packets. The request parser bounds dimensions to 320 through 4096 by 240 through
2160 with even dimensions, and frame rate to 1 through 240 Hz; these are admission limits, not measured
performance guarantees. ANNOUNCE must agree with the prepared dimensions and
frame rate. Host optimizer envelopes, HDR, HEVC, AV1, and surround audio are
rejected. Mapped devices receive a dedicated `worker_profile_v1` response from
the Nova optimizer endpoint. It resolves an SDR H.264 stream with a requested
bitrate budget capped at 8000 kbps, stereo audio and a whole frame rate from
15 through 240 Hz. Paired and configured host bitrate limits can reduce that
budget. Explicit display and bitrate limits are checked before returning the
contract. RTSP reserves audio and transport overhead before selecting the
worker's video encoder rate. Nova retains five millisecond audio packets for
profile streams even below its usual low bitrate threshold. Older clients that
request ten millisecond packets cannot use this worker contract. Profile status
reports the capability ceiling
separately from bitrate telemetry, which is not yet available.
Nova validates the complete response and applies it only for the reserved
profile app. Its launch includes `workerProfile` as an assertion against the
current assignment, checked again at queue admission. A stale assertion is
rejected even if the device has since become unassigned. It cannot select a
different profile. Moonlight can request the supported contract through a
manual stream preset without this Nova envelope.

The worker H.264 stream uses one reference frame. The pinned NVIDIA plugin
patch configures both the encoder's reference storage and prediction lists;
changing only an already encoded SPS would leave invalid slice dependencies.
The real provider check inspects initial and recovery SPS headers before
decoding. The patch remains recorded in the source lock, SBOM and image receipt.

The input plan provides keyboard and pointer devices and one gamepad when
controller input is authorized. The current compositor has no native touch or
pen mapping, so profile launches omit those devices and the corresponding RTSP
capability. Clients can use mouse emulation for a touch screen. Pairing
permissions stay unchanged; native touch and pen remain future provider work. Profile
launches clear host client commands and suppress host clipboard reads and
server commands. Server info reports only the requesting device's profile
session. Unassigned devices retain the ordinary app list and launch path.

The profile session status endpoint reports only the requesting device's
session token and admitted media settings. It does not reuse desktop capture
counters, encoder selection, another client's state or host Doctor findings.
Worker measurements and live tuning are not yet exposed through this endpoint.
Profile capabilities disable host diagnostics, host settings synchronization
and host media controls. A profile stop request checks the current paired
identity and exact session token before cancelling that device's worker.

## Profile assignment interface

The Devices page shows separate gaming profiles only when a profile controller
is configured. Permanently paired devices with launch permission can be moved
between provisioned profiles or returned to Standard streaming. The first Steam
runtime setup remains an administrative command. After that setup, the panel
can create additional Steam profiles. Multiple devices may share a profile's
games and settings, with one active stream for that profile.

The panel separates the current assignment from an unsaved choice, names the
other devices assigned to a shared profile, and explains Steam sign-in in
expandable help. Refreshing the catalog or saving one device preserves valid
draft choices for other devices. Empty catalogs and devices without profile
access have explicit explanations; an existing assignment can still be removed
after its device loses launch permission.

A successful write is shown as saved only after a fresh, valid catalog reports
the requested assignment and an available controller. Pending changes remain
pending. Failed or malformed reads retain the last displayed snapshot and
disable edits until a successful refresh. An initial read failure offers a retry;
a successful response with multiseat disabled keeps ordinary device setup
unchanged. The panel does not infer whether Steam is signed in.

Create Steam profile accepts a name and an existing configured Steam profile as
the runtime source. It uses that immutable local image for fresh private storage
and a dedicated profile network, always opening Big Picture. Sign-ins, saves,
game downloads and device assignments are not copied from the source profile.
The new profile is unassigned until the administrator chooses a device below.
An empty or non-Steam catalog cannot provision the first runtime through this UI.

Creation uses the same owner thread, idle requirement, catalog lease release,
durable transaction and controller replacement as assignment changes. The
administrator request contains exactly `request_id`, `source_profile_id` and
`name`; it cannot supply an image, volume, mount, command or paired device.
The lower-case UUID request ID identifies the new catalog profile. Concurrent
identical requests share one operation. A repeated committed request confirms
the same profile and preserves any subsequent assignments, without Docker work.
Conflicting profile metadata is rejected. Orphan storage is never adopted on retry.

A bounded HTTP wait may return 202 while creation remains owned by the service.
The UI retains the request and offers a status check or a retry of that same
request. It reports success only after the available controller's catalog contains
the expected profile. Failed or uncertain provisioning retains its resources
for administrative review and does not publish a usable partial profile.

The assignment API requires Web UI administrator authentication or the
administrator API key. A paired streaming certificate alone is insufficient.
Cookie based writes also require the existing CSRF token. Assignment requests
supply only a paired device UUID and an existing profile ID, never an image,
path, volume or command. Creation uses the same administrator authentication
and CSRF boundary.

Assignments change on the same owner thread as resource operations. New profile
launches are fenced while an edit is pending. Active or queued profile launches
reject the edit without cancelling a stream. After cleanup is proven, the old
controller closes and releases its catalog lease, one atomic catalog transaction
moves the assignment, and a replacement controller loads the saved routes.
Polaris itself does not restart. A failed write restores the prior catalog when
possible. Uncertain durability or failed reconstruction preserves affected
routes as unavailable, preventing fallback to host capture. The UI then requires
configuration review and restart.

## Cancellation and shutdown

Worker lifecycle generations are independent of host application generations.
Worker startup and teardown do not resume, pause, terminate, or reconfigure the
host application or display. Host process exit stops its connected host streams;
the shared control transport remains available for worker streams and new
insertions until the broadcaster shuts down.

Requests have a 25 second preparation deadline and the service retains at most
64 live requests or sessions. Expiration, failed publication, cancellation,
revocation, and abandoned launches are reconciled against their exact seat.
Cancellation is scoped to the authenticated device and optional session token;
an old token cannot cancel a replacement session or another device's seat.
`resume` starts a fresh assigned profile if it is free. It does not adopt or
reconnect to an already active profile. A disconnected worker seat is torn down,
while its private Docker volume persists.

Shutdown first closes admission and marks tracked launches cancelled, then
drains HTTP and RTSP before stopping the controller. The controller quiesces
stream bindings before worker and input teardown. If authority or absence
cannot be proven, the existing runtime quarantine retains dependencies and the
catalog lease until process exit. A mapped client cannot fall back to host
capture while shutdown is pending.

## Validation and remaining work

Offline tests exercise request queuing, one resource owner thread, independent
tokens, cancellation, deadlines, incomplete shutdown, strict configuration,
current pairing authority, revocation and permission replacement during startup,
RTSP rollback, worker input allocation, and host versus worker exit behavior.
Injected controller tests do not prove Docker startup, media delivery, controller
latency, or client playback on their own.

The NVIDIA physical harness has exercised two Gamescope workers with changing
video, decoded audio, isolated keyboard, pointer and controller input, and
independent teardown. Two Moonlight instances with separate paired identities
also launched assigned profiles through the normal protocol, decoded and
rendered approximately 60 frames per second, and cancelled independently. That
client check used one host and loopback networking.

A physical Android client also launched its assigned profile through Nova over
LAN Wi-Fi at 1920x1080 and 60 FPS, using hardware H.264 decoding and stereo audio
transport. Changing Pong video and touch mouse input were observed while a
separate Moonlight client streamed another profile over loopback. Disconnecting
Android left the second stream running; its authenticated session stop then
removed the remaining worker. The Android menu displayed the profile identity
without desktop diagnostics. Session status hid the active session from another
paired device sharing the occupied profile.

Administrator assignment checks covered live catalog reload, rejected changes
while streams remained active, reassignment after cleanup, and rejection of
stale launch assertions after moving or unassigning a device. These checks used
temporary hosts and profiles with SELinux enforcing and verified worker, volume
and IPC cleanup. They do not establish audible playback, controller latency,
or acceptance with two independent physical client devices.

Available workloads are Gamescope `input-pong-v1` and the experimental
[Steam launcher](container-multiseat-steam.md), with Big Picture or a typed game
ID. Bounded NVIDIA validation on September 13 exercised Control Ultimate
Edition gameplay and controller input alongside the comparison stream near
60 FPS. Two fresh stop cycles completed without provider errors, kernel faults
or owned coredumps. Those cycles used native host revision `775cdaa5`, worker
revision `872e4186`, and Nova revision `7f435a9e`.

Profile assignment UI checks cover delayed and failed read-back, pending
changes, stale catalogs, retained drafts, access removal, and empty states.
A browser preview with fixture data also checks assignment saves, keyboard
navigation, and layouts from 320 to 1280 pixels wide. These checks do not
substitute for live game acceptance.

Two independent commercial games, initial runtime setup UI, AMD hardware, audio
quality, rumble and two independent physical clients remain pending, including
reconnect and measured latency.
The UI must preserve the existing flow for one person with one device.
Runtime images use Polaris builds from official Ubuntu. Required third party
source and license notices remain intact until those dependencies are replaced.
