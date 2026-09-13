# Multiseat worker image inputs

Continuous H.264/Opus media is packaged with an explicit worker opt-in. See
[`container-multiseat-encoder-provider.md`](../../docs/research/container-multiseat-encoder-provider.md)
for its media contract and remaining acceptance. The
[Steam profile adapter](../../docs/research/container-multiseat-steam.md) adds
Big Picture or a typed game ID, private storage, and a dedicated Docker bridge.
GPU seats select NVENC or VA-API on their allocated render device. NVIDIA
variants include the pinned nvcodec plugin; software capture retains OpenH264.


Docker is the default build and worker engine. See
[the Docker backend decision](../../docs/research/container-multiseat-docker.md)
for the host trust boundary, image import, profile initialization, and acceptance.
The [saved profile catalog](../../docs/research/container-multiseat-profile-storage.md)
provides private Docker volume provisioning and paired device assignments through
the administrative CLI. The
[host launch integration](../../docs/research/container-multiseat-launch-integration.md)
owns the controller when explicitly enabled; production activation defaults off.

Building these images does not enable multiseat in the running Polaris service.

Steam images also build and test 32-bit and 64-bit controller compatibility
libraries from Polaris source. With controller permission, a Steam seat receives
one raw controller and one separately allocated Steam translation output. The
launcher verifies both libraries and the exact output identity before starting
Steam. Its private broker accepts bounded controller reports, releases held
controls on disconnect, and retires with the seat. The container receives the
output's exact event node; device creation stays on the host.

The translated output currently supports one controller per Steam seat. Software
tests cover both library ABIs, private broker admission, controller release, and
Docker device reconciliation.

On 2026-09-13, the Steam NVIDIA Docker image built from
`4eec5ced8392ce98ff405fbe3c5cbcab6a13746e` passed a bounded PEAK DX12
controller smoke through Nova on an RP6. Automated events on the RP6 controller
device exercised movement, camera control, menu navigation, and pause/resume with
Steam Input both enabled and disabled. The compatibility libraries loaded in
Steam and Proton, and the original Enable Steam Input override was restored.
A second bounded run with the same image reached Control Ultimate Edition
gameplay and PEAK's offline airport scene concurrently under two saved Steam
accounts. A local Moonlight client decoded Control at approximately 60 FPS at
1080p, while Nova on the RP6 streamed PEAK. Each worker had distinct profile
storage, a private network namespace, and five allocated input nodes with no
overlap. RP6 controller events moved PEAK and operated its menus while Control
remained paused; bounded writes to Control's verified seat keyboard moved its
character while PEAK remained paused. This does not validate a second physical
controller or keyboard transport through the local client.

After Control exited normally, disconnecting its client retired only that
worker. PEAK continued streaming and responding to the RP6 controller after the
peer worker was removed. Both games returned to Steam through their normal exit
menus. Final cleanup retained all three profile homes, preserved Enable Steam
Input for PEAK, removed the test workers and IPC, and restored the original
SELinux policy with enforcement and the regular Polaris service still active.

Rumble, AMD hardware, and sustained streaming quality still require acceptance.
The RP6's slow-connection warning also reproduced in Steam Big Picture before
PEAK started. Client logs recorded decoder watchdog flushes and audio queue
overruns while the local comparison stream remained stable during gameplay.
A bounded host capture contained the expected packets for frames the RP6
reported with incomplete data. That observation does not locate the loss or
distinguish late delivery from client handling. Streaming quality remains open;
screenshots, packet captures, and detailed receipts are retained privately.

A follow-up Steam NVIDIA image from
`1c25a2eb6e5ba5e9e73fdd00c70bf21f57f46b0e`, with Nova
`de88f759c3363c009af0455f6210dacf65875259`, negotiated a 4000 kbps stream
budget. NVIDIA confirmed a 3067 kbps video target after audio and transport
reservations, and Nova requested the required 5 ms Opus packets. The bounded
11 minute RP6 run reached PEAK's offline airport scene and verified controller
menu navigation, forward movement and camera rotation. The stream ended at its
configured test timeout with 39764 video frames, 133308 audio frames and no
worker discontinuities. Cleanup retained all three profile volumes and restored
the original policy. Client connection warnings, decoder watchdog flushes and
audio queue overruns still occurred; this validates startup bitrate selection,
not sustained streaming quality.

A later RP6 delivery check isolated a degraded wireless link. With no stream
running, a 30 second synthetic UDP test lost 46.2% of packets sent at 12 Mbps,
and the device reported an 8 Mbps receive link rate. Reconnecting to the same
saved network raised the reported receive rate to 648 Mbps; the identical
31962 packet test then had no loss. A 1 Mbps probe alongside the worker stream
went from 64.9% loss before reconnecting to no loss afterward.

Using the same NVIDIA image and Nova
`10a8389a5bd307630006b544783a27ab9dd5f19c` with
`-PnovaNativeDebugChecks=false`, the subsequent 507 second stream delivered
30431 video frames and 101438 audio frames. Native client logs recorded no
unrecoverable video frames, network frame drops or decoder watchdog flushes.
PEAK offline navigation, movement and camera input worked, and the game
returned to Steam through its own menus before the bounded test expired.
All three profile volumes remained and policy cleanup completed. There were
33 pending-audio messages, so this does not establish prolonged audio quality.
The cause of the degraded wireless state and its recurrence remain unproven.

Nova Debug normally enables native FEC validation that intentionally requires
an extra parity packet. Quality checks now explicitly disable that mode and
verify the packaged native library. The earlier comparison that accidentally
reused a debug native library was excluded. Detailed evidence remains private.

`images.lock.json` distinguishes immutable source roots, dependency locks, and
produced worker artifacts. The Gamescope, Steam, Heroic, and Lutris source roots
each receive an offline runtime dependency stage, a Wayland GStreamer plugin
compiled against that root's ABI, pinned Gamescope 3.16.19, and the same static `polaris-seat-worker`,
`polaris-seat-runtime` dispatcher, and private session-bus, audio,
display/capture, and nested Gamescope provider binaries. Keeping the launchers
separate avoids multiplying package and credential state inside one large
image.

Final build stages have no module or package download step. The worker, dispatcher,
and six providers use only the Go standard library, disable CGO and
module-network access, run their tests, then emit static Linux/amd64 binaries.
The final stage fails its build unless the locked root supplies the fixed D-Bus,
PipeWire, `pw-cli`, `pw-dump`, WirePlumber, `pactl`, GStreamer, Gamescope, and Xwayland executables; the
`waylanddisplaysrc`, `unixfdsink`, `unixfdsrc`, and `fakesink` elements; and both
trusted PipeWire configuration files. The image job builds all four Linux/amd64
profiles at an exact Polaris revision and exports downloadable OCI archives,
verified manifest and configuration digests, package manifests, CycloneDX SBOMs,
dependency lock hashes, and isolated real-provider receipts. See
[RUNTIME-IMAGES.md](RUNTIME-IMAGES.md) for the fetch/build split, NVIDIA physical
lane, acceptance scope, and lock refresh procedure.

These locks establish immutable byte identity, not publisher trust. No
signature, attestation, vulnerability policy, or complete license bundle is
claimed by this milestone. A publishable image must add those gates and retain the
resolved upstream manifests as provenance evidence before any lock refresh.

The entrypoint owns
private control and media sockets, mutual authentication, explicit data-plane
attachment, health state, and shutdown. The worker has an injectable lifecycle
contract for a session bus, audio, capture-producing outer display, nested
compositor, virtual input, worker-local encoder, and launcher process tree. A
Linux process-backed adapter set turns each stage into one literal, shell-free
`polaris-seat-runtime serve` request. Each request receives only opaque seat
resources, runs in its own process group with a scrubbed environment, and must
publish the exact `POLARIS-RUNTIME-READY/1` record on an inherited descriptor
before the stage is ready. Shutdown targets the owned process group with TERM
and escalates to KILL at the component deadline.

The image carries the dispatcher and private session-bus, audio, outer capture,
nested Gamescope, verified input-reader, and experimental launcher providers.
The launcher accepts the image-owned `input-pong-v1` workload
with the Gamescope profile, and Steam Big Picture or a canonical numeric game
ID with the Steam profile. The small offline X11 game exercises keyboard,
pointer, optional gamepad, and private Pulse audio without launcher accounts.
Its executable is compiled against each profile's locked X11/GStreamer ABI;
image checks resolve its ELF dependencies and the SBOM records its source hash.
The launcher validates the allocated display protocols, retained compositor process lifetime, and input identities,
retains the private profile, and owns its entire descendant process tree,
including helpers that detach into another session.

The isolated physical game harness can also run bounded codec observations.
`POLARIS_PHYSICAL_ENCODED_GAME=1` checks captured game motion through OpenH264;
`POLARIS_PHYSICAL_ENCODED_AUDIO=1` checks the allocated sink monitor through
Opus at 48 kHz stereo with 5 ms packets capped at 1400 bytes. Both require
`POLARIS_PHYSICAL_GAME=1`. The audio check pins the private Pulse socket, checks
the selected monitor, and measures the fixed game's quiet 440 Hz tone after
decoding. Each enabled observation runs for both seats and again for the
surviving seat after its peer's container is removed. These observations prove
worker-local codec roundtrips; continuous media transport and client playback
remain separate acceptance gates.

The production `run --media=enabled` path supplies the implemented providers
and continuous encoder media source for Gamescope and Steam allocations.
Heroic and Lutris launcher implementations remain outstanding. A worker that announces a contract on
its media channel has it held against what the client negotiated, acknowledged,
and its frames carried to that client's own packet destination, with keyframe
requests and reference invalidations travelling back on control. Provider readiness proves a resource or
supervised process is available; it does not prove game frames reached a client.
Unit tests and isolated physical input receipts likewise do not establish
compositor input delivery or successful game streaming.

The controller has a host-brokered input authority and a
Linux inputtino lifecycle backend connected through the profile launch owner.
The backend creates virtual devices outside the untrusted
launcher boundary and derives the exact generation's event-node identity from
`fstat`, sysfs, and udev before returning fixed worker-local paths. The container
adapter consumes that allocation through a separate injected source, verifies
the full authority and kernel snapshot twice before command invocation, and
maps each host event node to only its fixed worker alias. The former global
input-device option is gone; generated worker commands cannot receive raw
`/dev/uinput`, `/dev/uhid`, or a host-wide `/dev/input` mapping.

Each worker carries an opaque SHA-256 fingerprint of its complete generation
manifest. Inventory compares that fingerprint and the inspected container device
bindings against the current authority. Podman may reconstruct a different
host path from the stored major/minor pair, so reconciliation accepts that path
only when the character-device identity and exact worker alias still match.
Stop remains available after input authority disappears so an orphaned worker
can still be removed. Offline tests inject the allocation, kernel probe, host
device metadata, command runner, and inspect JSON; no input node or container
engine is opened.

The injected authority now decodes one canonical typed event per route call.
Keyboard, relative and absolute pointer, pointer button and scroll, touch, pen,
and complete Xbox-style gamepad state are representable; every encoding is at
most 24 bytes. The host backend selects only the managed inputtino object held
by the exact generation and rejects stale handles, wrong input-seat names,
sequence gaps, unavailable device kinds, impossible opposing D-pad states,
duplicate key/button transitions, and invalid touch lifecycles. It never writes
an event node and never gives the worker an injection handle. An indeterminate
managed-device call poisons that generation's route and inventory until exact
teardown, so a possibly partial event cannot be replayed after reconciliation.

Xbox rumble callbacks have an explicit controller-facing shape: complete seat
handle, per-generation sequence, gamepad slot, and bounded low/high magnitudes.
Callbacks from a released generation are closed before its devices are dropped,
and any callback already in flight still carries the old generation identity.
The codec, managed devices, kernel observations, and feedback sink are injected
in tests; no physical input node is opened.

An offline adapter now parses one complete decrypted Moonlight packet without
casting caller storage to a packed native structure. It validates the mixed
wire endianness, declared and exact packet sizes, reserved fields, fixed
controller sentinels, normalized finite touch/pen values, permissions, and the
complete seat generation before assigning the next authority sequence.
Malformed, denied, ignored, and unsupported packets consume no sequence.
Representable keyboard, pointer, scroll, touch, pen, and Xbox-state packets are
converted through the canonical 24-byte codec. Non-normalized keyboard input,
Unicode text, touch cancel-all/hover, pen button-only/tool-unknown events, Xbox
extended buttons, and controller touch/motion/battery remain explicitly
unsupported rather than being silently misrepresented.

The matching controller queue accepts only exact-generation, contiguous typed
rumble callbacks and converts them to Polaris' existing Moonlight feedback
message. It retains at most one latest state per each of sixteen gamepad slots,
so a zero-magnitude stop replaces an older pending rumble without growing an
unbounded queue. Remaining states drain in source-sequence order; teardown can
close and clear the queue permanently.

An offline session bridge now owns those two directions for one exact
authenticated control lifetime. A trusted source atomically claims the
existing non-secret launch ID plus process-local session generation and returns
an exclusive lease containing the seat handle, input permissions, and rumble
permission. The bridge never accepts or stores a session token. It refuses an
unprepared seat, permissions broader than the allocation's device plan, a
duplicate session claim, or missing feedback dependencies.

Feedback registration is also an owned subscription rather than a callback
capturing the bridge. The callback holds only a weak state reference, and
delivery peeks then acknowledges the queue so a retry remains within the same
fixed sixteen slots. A newer same-slot state may supersede the state being sent
without being erased by its older acknowledgement. Teardown stops new work,
synchronously detaches feedback publication, waits for already-admitted work,
clears queued feedback, and only then releases the authenticated lease. A
closed or throwing sender fails the session closed.

The production ownership adapters provide bounded process-local implementations. An authenticated
session registry accepts at most 256 immutable bindings, rejects duplicate
keys and any reuse of the same GPU seat slot, grants one exclusive bridge
claim, and lets the session owner retire a registration while synchronously
waiting for that claim to leave. A feedback hub supplies the inputtino backend
with a weak, non-throwing sink and routes typed feedback only to the exact seat
generation. Subscription detach and hub shutdown wait for callbacks already in
flight, including a detach racing global shutdown.

The matching concrete sender verifies the complete authenticated binding and
bounded rumble shape before submitting to a typed, session-owned mailbox.
Mailbox retry and close results map directly to the bridge without exposing an
ENet peer or session secret. The `stream::session_t` mailbox endpoint and
profile launch owner connect this route only for an authenticated worker binding.

Rumble delivery still needs acceptance through simultaneous client playback.
The worker's older opaque input/feedback test adapter is
deliberately not treated as injection authority. Steam controller translation
and its current physical evidence are described above. The optional rootless
Podman backend requires trusted crun, the actual launching UID and `keep-groups`.
The optional policy under `selinux/` labels only reserved
multiseat event nodes. Policy installation remains explicit; the isolated
harness must establish device access for each selected final image.

The dispatcher and worker share one canonical stage parser and environment
builder. Before it can touch a provider, the dispatcher rejects reordered or
extra argv, non-canonical numbers, a runtime/profile mismatch, and every
ambient environment setting. It then opens the fixed
`/run/polaris-auth/runtime-providers.json` path without following a final
symlink, requires a non-writable regular catalog owned by its effective worker
UID, and selects one exact stage entry. This location is inside the existing
per-generation read-only auth mount; the mutable launcher home cannot replace
it. Nested compositors are selected by their concrete compositor; launcher
entries are selected by the exact runtime kind and opaque workload catalog ID.
Duplicate selections, unknown JSON fields, duplicate JSON fields, moving
executable paths, and control bytes fail closed.

The selected provider must likewise be a root-owned, executable regular file
with no group/other write access. The dispatcher opens it without following a
final symlink and executes that already-open file in place, with one canonical
provider argv and the same scrubbed stage environment. No shell is involved.
The provider therefore retains the helper PID and worker-owned process group;
it, not the dispatcher, must publish the exact readiness record only after its
real resource is usable. Catalog-supplied arguments remain literal argv after
an explicit `--` delimiter. Offline tests prove same-PID dispatch, descriptor
readiness, ambient-secret removal, literal hostile arguments, partial-ready
cleanup, descendant cleanup, and two-helper process-group isolation.

The session-bus provider starts one fixed `dbus-daemon` against the seat's
mode-0700 runtime directory. It does not publish readiness until the daemon's
exact address is validated and an EXTERNAL-authenticated D-Bus handshake
completes. The audio provider starts a private PipeWire core with RAOP
discovery disabled, creates one exact named null sink, starts
Pulse-on-PipeWire, and requires the Pulse protocol to expose only that sink and
its monitor. Pulse clients route through `PULSE_SINK`; native PipeWire clients
receive the same sink through `PIPEWIRE_NODE`.

The display/capture provider starts the fixed `gst-launch-1.0` executable with
a scrubbed environment and no persistent GStreamer registry. Its hardware path
is `waylanddisplaysrc` with the admitted render node and exact width, height,
and rational refresh, followed by a DMA-BUF caps boundary and `unixfdsink`.
The raw-frame endpoint is a bounded SHA-256-derived Unix socket inside the
seat's private runtime; a later encoder provider can consume it through
`unixfdsrc` without moving raw frames through the controller. The compositor's
automatic `wayland-N` socket is given a no-replace hard-link alias at the exact
controller-allocated capture name while retaining its original server-owned
path. A child-inherited `0077` umask makes every socket and lock owner-only.

Readiness is protocol-level. Polaris requires the aliased socket's peer PID and
UID to match the supervised producer, completes Wayland registry and callback
round trips, requires compositor, shared-memory, seat, XDG shell, and one
output global, requires DMA-BUF version 3 or newer on the hardware path, and
compares the current output mode with the admitted dimensions and mHz. It then
receives one frame through a separate `unixfdsrc` pipeline before publishing
the provider-ready record. Socket nodes alone, a wrong mode, a non-producing
pipeline, or a missing DMA-BUF global cannot report ready. HDR is rejected
before process start because the selected upstream compositor does not yet
expose a typed HDR contract. Cleanup stops the producer first and removes only
the same captured socket and lock inodes; replacements are retained and
reported while other owned artifacts are still cleaned.

The nested-compositor provider accepts only the typed `gamescope` selection.
It first re-probes the exact outer socket and admitted mode, then launches the
fixed Gamescope binary as a Wayland client with one Xwayland server, exact
inner and outer dimensions, no application command, and a dedicated 120-second
startup ceiling. Integer refresh rates are supplied explicitly. Fractional
rates are never rounded: Gamescope must inherit the parent cadence and the
post-start probe must report the exact admitted millihertz or readiness fails.
HDR remains rejected because the outer provider has no typed HDR contract.
The admitted render-node path is validated and retained as diagnostic evidence,
but Gamescope 3.16 has no exact render-node-path selector. Exact GPU authority
therefore remains the container backend's explicit device allowlist and outer
Wayland binding, not an environment-variable claim.

Gamescope 3.16.19's `--ready-fd` option is a FIFO path despite its name. It
writes exactly one `DISPLAY WAYLAND_DISPLAY` line only after its Xwayland
server and compositor context initialize. Polaris creates that FIFO and the
limiter file under the private runtime, rejects any pre-existing Gamescope
artifacts, requires the reported inner socket to be `gamescope-0`, adds a
no-replace hard-link at the controller-allocated app socket, and then performs
native Wayland and X11 setup handshakes. Both peers must be the supervised
Gamescope PID and UID; Wayland must advertise the exact width, height, and mHz,
and the X11 root screen must match the admitted dimensions. A mode-0600,
generation-derived session record preserves the selected X display and the
allocated app-facing Wayland name for the future launcher provider.

The production container must give each worker a private `/tmp` as well as its
private runtime. The provider requires an empty X11 namespace and therefore
requires Gamescope to select `:0`; it captures both X socket and lock inodes and
validates that the lock names the supervised PID. Gamescope runtime, alias,
readiness, limiter, session, and X11 artifacts are removed only when their
captured inodes still match. Replacements and unexpected artifacts are retained
and reported.

All four providers open fixed root-owned top-level executables without
following a final symlink and execute the open descriptors without a shell.
The Gamescope provider also validates `/usr/bin/Xwayland` immediately before
start and supplies only `/usr/bin` as Gamescope's child-search path; Gamescope
itself owns that child exec inside the immutable image root. Descendants cannot
inherit the readiness descriptor, and each directly supervised child receives
a parent-death kill; Gamescope owns its Xwayland lifecycle, with exact container
teardown as the final backstop. TERM is bounded and escalates to KILL. Cleanup
removes only captured, same-inode
socket, lock, PID, and private service-directory artifacts; replacements or
unexpected directory contents are retained and fail the provider. Linux tests
run the real D-Bus and audio protocols plus the real upstream Wayland
compositor in software mode, prove two audio graphs, display transports, and
modeled nested stacks do not see or stop each other, and verify clean teardown.
An opt-in test can exercise the installed Gamescope with its real Xwayland and
inner Wayland protocols through the headless backend under lavapipe. That
lab-only substitution is explicit: this host's lavapipe lacks the
`VK_KHR_present_id` and `VK_KHR_present_wait` extensions required by
Gamescope's Wayland backend. Xwayland glamor is disabled, but this installed
Xwayland still opens the host render node during initialization, so the test
requires a separate DRM-device authorization and was not accepted as a
GPU-free gate. A real outer-to-Gamescope Wayland smoke remains a later
GPU-authorized gate. No image was built and no host service was installed,
restarted, or reconfigured for this checkpoint.

The injected contract starts those seven resources in dependency order and
publishes worker health only after every adapter reports ready. Startup has one
120-second ceiling so a real Gamescope adapter is not accidentally constrained
by the old five-second application timeout. An unexpected component exit fails
the worker. Shutdown attempts launcher-process-tree, encoder, virtual-input,
nested-compositor, display-capture, audio, and session-bus cleanup in that exact
reverse order, with a separate five-second bound per component; a timeout,
error, or panic cannot starve the remaining cleanup. Concrete resource helpers
must honor cancellation, remain in their owned process group, and be
idempotent. The controller waits 45 seconds before forcing an exact worker
generation: 35 seconds for seven serial five-second component bounds, plus the
existing five-second authenticated-shutdown I/O budget and five-second
backend-command budget.

Polaris builds each application runtime from a pinned official Ubuntu base and
owns session-bus, PipeWire, capture, encoding and input supervision. Readiness
requires each provider's authenticated contract and live artifacts.
The controller binds each opaque profile to one typed
runtime and one exact final image digest, carries the requested display and
data-plane topology through admission and reconciliation, and gives the worker
canonical width, height, refresh, and HDR values. It also supplies an exact
allowlisted workload plan: a typed Gamescope, Steam, Heroic, or Lutris selector
plus a bounded opaque catalog target. No executable path, arbitrary argv, or
shell fragment crosses that boundary. The dispatcher resolves the exact
runtime kind and target through its trusted provider catalog and rejects any
plan that does not match the selected runtime profile.

The display provider owns the outer headless compositor and nests Gamescope
beneath it. Audio, input and encoding have separate lifetimes. In this contract,
applications use the nested compositor's inner Wayland socket, while capture
uses the outer socket and raw frames remain worker-local through encoding.
Only encoded video/audio and stream markers may cross the authenticated media
channel. Controller input crosses the attached control channel and feedback
returns there; every routed item repeats the exact seat generation and
cross-seat output is rejected. The concrete encoder emits H.264 and Opus only
after both codecs produce valid media, and the controller binds that media to
the exact selected worker connection. Production activation remains default-off;
client playback and launcher integration still need their acceptance gates.

Authentication does not attach either data channel. Health probes authenticate
and heartbeat without consuming media. A streaming controller explicitly
attaches control and media as one all-or-nothing operation, and at most one
owner may attach each channel. Losing an attached channel cancels that exact
worker generation so its runtime tears down instead of leaving an orphaned
headless workload.

The container retains `--network=none`. Beneath a pre-created mode-0700
runtime root, the controller exclusively creates one inode-fenced,
mode-0700 authority directory per exact worker generation. Its `ipc` child is
mounted read-write for the two Unix sockets, while its `auth` child is mounted
read-only and contains the controller-generated mode-0600 capability file,
mode-0400 provider catalog, and mode-0600 capability-authenticated recovery
record. The controller derives the catalog from the concrete compositor and
exact admitted workload pair; executable locations and empty provider argv are
compiled policy rather than configuration text. The catalog appears under its
final name only after its complete bytes and restrictive mode are synced. Its
SHA-256 is part of the signed record alongside the exact seat identity and
runtime namespace, and the handle pins all three file inodes. None of these
files is placed in argv, environment, labels, or container inspection metadata,
and the worker cannot rewrite them through the read-only mount.

The local coordinator owns the live authority handle and authenticated client
until a complete backend inventory proves that exact worker absent. On restart
it audits at most 256 root entries through no-follow descriptors and recovers
only valid signed records absent from that authoritative inventory. Active,
ambiguous, malformed, replaced, unexpected, or live-socket state is retained
and blocks admission rather than being deleted by name or recursively.

## Isolated input acceptance with crun

The experimental input provider verifies every fixed alias using read-only
evdev descriptors, generation-specific kernel names and physical identities.
It rejects unexpected aliases, duplicate devices, missing core devices and
noncontiguous gamepad slots. Descriptors remain owned until shutdown; periodic
checks fail on namespace replacement or source-device removal. The display
request can explicitly select an input seat. That path supplies only verified
keyboard/pointer aliases to the pinned compositor plugin and requires that the
actual producer retain them before reporting ready. Requests without an input
seat preserve the existing capture-only behavior. The production entrypoint
still supplies no runtime adapters.

The opt-in SELinux policies permit only the three evdev identity queries
(`EVIOCGVERSION`, `EVIOCGNAME`, `EVIOCGPHYS`) in addition to event reads. Broader
compositor capability queries and real game input delivery need separate
physical verification; the new provider does not yet establish that acceptance.

The Linux worker backend defaults to Docker with runc, explicit numeric UID/GID
and supplementary groups, and private temporary filesystems. Launch rechecks
actual process groups and exact device identities immediately before invocation.
A user service must inherit the required groups. Stopping an existing worker
remains available after input access disappears. SELinux stays enforcing.

The retained Podman option explicitly selects crun and uses `keep-id` and
`keep-groups`; its OCI annotation checks remain specific to that engine. Earlier
rootless Podman receipts must be repeated on Docker before claiming physical
acceptance for the default backend.

`MultiseatPhysical.TwoWorkersReadOnlyTheirAllocatedInputAndStopIndependently`
is an opt-in acceptance test, defaulting to Docker. Set `POLARIS_MULTISEAT_PHYSICAL=1`, an exact
`POLARIS_PHYSICAL_IMAGE`, a private `POLARIS_PHYSICAL_IPC_ROOT` parent, and
two distinct pre-created profile volumes through `POLARIS_PHYSICAL_VOLUME`
and `POLARIS_PHYSICAL_VOLUME_B`. `POLARIS_PHYSICAL_IMAGE` must be a manifest
digest reference, `name@sha256:<64 hex>`, or a full Docker image ID
`sha256:<64 hex>` matching the verified artifact; a tag is refused before launch.
Always add `--gtest_output=xml:<private receipt path>` to retain RecordProperty
diagnostics.

Set `POLARIS_PHYSICAL_LIVE_MEDIA=1` to exercise continuous worker media on
Docker with the Gamescope input game. Leave `POLARIS_PHYSICAL_ENCODED_GAME`
and `POLARIS_PHYSICAL_ENCODED_AUDIO` unset: those flags run separate probes.
The live mode starts the controller-owned worker runtime, obtains each launch's
authenticated connection, and consumes its H.264 and Opus through the real host
media pump. It decodes every video packet to 1080p SDR and every audio packet to
5 ms stereo, checks changing frames and audible samples, requests keyframes,
and repeats input isolation and continued decoding after the first seat stops.
Missing frames fail acceptance. Opus is decoded continuously; bounded video
receipts are decoded at stop using the system OpenH264 GStreamer plugin and
Python 3. XML properties retain the counts and exact image identity. This isolated harness opens no network listener
and does not establish client playback, latency, or production activation.

Each profile volume must be mode 0700 and owned by the controller UID. See the
[Docker profile initialization recipe](../../docs/research/container-multiseat-docker.md#images-and-profile-state).
Missing volumes and driver redirection are refused by admission; private root
ownership is enforced by the worker at startup.

`POLARIS_PHYSICAL_PROFILE` selects gamescope,
steam, heroic, or lutris. The image must contain the separately packaged
`polaris-seat-input-probe` acceptance helper. GPU device paths must belong to
the explicit catalog supplied through the physical harness environment.

That catalog must include the GPU's DRM primary node, not only its render node.
Gamescope's Vulkan backend checks `VkPhysicalDeviceDrmPropertiesEXT::hasPrimary`
whenever the backend does not present through a Vulkan swapchain, which is the
case for the Wayland backend this provider uses, and exits with `physical device
has no primary node` when the node is absent from the container. The harness
derives the default from the render node's sysfs sibling and refuses a catalog
without one, because the symptom otherwise arrives as a nested compositor whose
runtime helper never became ready.

The harness creates a unique deployment and authority root, opens every
allocated event alias inside both workers, checks major/minor identity and
absence of other input nodes, and sends bounded synthetic input through the
host authority. It checks the second worker again after stopping the first.
Any forced container cleanup fails acceptance; cleanup only targets captured
container IDs and never recursively removes caller-provided authority roots.
Passing this test establishes isolated input and worker lifecycle. Production
provider selection and multiseat activation remain off; it does not establish
successful game streaming.

The worker UID is passed explicitly so an image `USER` cannot override it.
Docker inventory requires the exact Config.User and numeric group list; Podman
inventory also checks OCI process.user.uid and retained-group evidence. Use a short private IPC parent for the
physical harness so both generated Unix socket paths fit Linux's 108-byte limit.

Experimental compositor input uses the fixed native `capture-input` producer.
The Go provider verifies and passes already-open, read-only keyboard and mouse
descriptors in a fixed order. A bounded native decoder sends the pinned plugin's
existing input events. It performs no device discovery, device writes, or input
ioctls and needs no host udev metadata. The initial mapping covers keyboard,
relative/absolute pointer, buttons, and wheel; gamepads remain direct workload
readers. Touch and pen allocations fail this experimental admission until their
mappings are implemented. Source retirement, dropped kernel events, excessive
input backlog, or stalled capture fails the provider and tears down its stream.
Capture allows up to five seconds between frames so a brief Steam launch or
game presentation transition can recover. A longer stall still retires the
seat, and input descriptor failures remain immediate during that interval.

A post-creation X11 directory failure without a retained inode leaves cleanup
unproven. Startup fails and the worker's private tmpfs must be destroyed before
reuse. There is no same-worker retry path; unidentified or replacement paths
must never be removed by provider cleanup.

The private audio provider supervises WirePlumber 0.5.8 with a fixed `polaris`
profile. Hardware discovery, D-Bus integration, saved routing, default-device
fallback, and stream movement are disabled. Before launcher admission it captures
the allocated null sink's object serial, proves that the policy process is
attached to this private server, and verifies both stereo playback and monitor
ports. Dynamic streams may link only to that original sink; a replacement with
the same name does not inherit its authority. Policy exit retires the audio
provider, which stops Pulse and policy before the PipeWire core.

Each profile adds the hash-locked `wireplumber_0.5.8-1_amd64.deb` from Ubuntu's
signed Plucky release archive. This supplemental package uses the explicit
archive URL in its lock entry; the existing January 20 snapshot inputs retain
their versions and hashes. Offline installation against all four locked source
roots and runtime package sets adds only WirePlumber: its library, Lua, and
PipeWire dependencies are already covered. Final builds remain network-free.
