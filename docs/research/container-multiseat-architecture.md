# Container multiseat architecture spike

Status: architecture, an offline rootless-Podman backend, locked image inputs,
immutable per-seat runtime/data-plane bindings, typed workload plans, and a
supervisor/IPC routing proof. Nothing in this document enables multiseat,
launches a container, or changes the current single-workload runtime.

## Outcome

A container build is not the difficult part. The product boundary is allowing
two clients to run different workloads on one host without either seat owning
the other seat's compositor, capture, encoder, audio, input, process tree, or
persistent launcher state.

The implementation slices introduce executable admission, lifecycle,
reconciliation, and rootless-container contracts. They prove that two seats
can share one logical GPU while retaining different exact-generation
authority and different worker resource names. They also prove that stopping
one seat cannot stop the other, that a stale teardown cannot target a reused
slot, and that seat, profile, and encoder budgets fail closed.

It does not claim that two real games have been streamed yet.

## Current boundary

Polaris already has multiple RTSP/media sessions and one encoder per client.
The configured max_sessions value permits an owner and viewers of the same
active workload. It does not provide independent seats.

The next layer is still singleton:

| Concern | Current owner | Multiseat requirement |
| --- | --- | --- |
| Application and process tree | Global proc::proc | One worker per seat |
| Compositor and display mutation | Active proc_t generation | One namespace per seat |
| Audio launch routing | Active proc_t audio context | One sink per seat |
| Capture generation | Global active workload | One generation per seat |
| Input platform and gamepad ids | Process-global allocator | Seat-scoped devices and feedback |
| Doctor/session status | Aggregated active workload | Seat-keyed evidence and actions |
| Pause, resume, and teardown | Last network session | Exact seat generation |

Changing max_sessions alone would preserve these shared owners and create
cross-seat teardown hazards.

## Target topology

The durable design separates a stable host control plane from disposable seat
workers:

    Polaris control plane
      - pairing, profiles, app catalog
      - seat admission and GPU budgets
      - worker lifecycle and recovery
      - per-seat status routing
        |
        +-- seat worker A
        |     GPU 0, encoder lease 0
        |     capture display A -> nested Gamescope/compositor A
        |     worker-local capture -> encode -> media IPC A
        |     audio A, input/feedback A, process generation A
        |
        +-- seat worker B
              GPU 0, encoder lease 1
              capture display B -> nested Gamescope/compositor B
              worker-local capture -> encode -> media IPC B
              audio B, input/feedback B, process generation B

The first registry model lives outside proc_t on purpose. Folding a vector into
the current singleton before defining admission and teardown authority would
make shared global state look per-seat without actually isolating it.

## Seat identity and lifecycle

Admission returns an opaque handle:

- logical GPU id;
- slot number within that GPU's configured capacity;
- opaque controller epoch;
- monotonically increasing generation within that epoch.

The client and profile keys plus the typed workload plan are retained as
internal routing metadata but never appear in worker, runtime, Wayland, audio,
or input resource names; those names use only the controller epoch and
generation. That avoids leaking identity through host runtime paths. The
workload target is itself a bounded opaque catalog identifier.

The lifecycle is:

    reserved -> runtime selected -> starting -> running -> stopping -> released

Every mutation requires the complete handle. A slot can be reused only after
release, and the new generation makes every earlier handle stale. Teardown is
idempotent once stopping begins, but release is refused before stopping. A new
control-plane process must use a new epoch, so an old callback cannot target a
replacement registry even if its slot and generation numbers match. Startup
reconciliation and orphan cleanup remain worker-broker responsibilities.

## Worker-broker protocol

The backend boundary is deliberately smaller than a container-engine API. A
worker receives one immutable launch specification containing the exact seat
handle, opaque worker resources, opaque profile key, typed allowlisted workload
plan, typed runtime profile, display/data-plane topology, geometry and cadence,
chosen render node, concrete compositor, and encoder lease count. Client
identity, credentials, host paths, executable paths, arbitrary argv, shell
fragments, and container-engine authority are not part of that payload.

The backend exposes only three operations:

- launch one exact worker specification;
- stop one exact worker identity gracefully or forcibly;
- return an authoritative, complete inventory of this deployment's workers.

The control plane begins with admission closed. One successful inventory pass
must prove that no worker from an older controller epoch remains before any new
seat can start. Old workers are never adopted into a replacement registry.
They receive one exact graceful stop request, then one exact forced stop after
the configured monotonic deadline if still present. Admission opens only after
a later clean inventory confirms their absence. The broker does not repeat a
forced stop blindly; a worker still present after the force-confirmation
deadline is surfaced as stuck for a higher-level recovery policy.

An indeterminate launch is treated as potentially successful: its seat moves
to stopping and remains allocated until authoritative inventory confirms that
the exact worker is absent. Likewise, a worker that disappears from an
authoritative inventory releases only its own exact seat. Backend observation
failure closes admission without mutating registry state. A malformed,
unknown-state, or duplicate observation invalidates the complete snapshot;
the broker then issues no worker command and makes no lifecycle mutation from
that snapshot.

## GPU admission

A logical GPU declares two independent budgets:

- active seats;
- simultaneous encoder sessions.

Every admitted seat reserves both before a worker can start. A future hardware
probe may populate the limits, but an unknown or zero budget must not mean
unlimited. Scheduling and performance policy can become more sophisticated
later without weakening the initial invariant.

Rendering and encoding should stay on the same GPU when possible. Multi-GPU
placement belongs above the worker contract; a seat should receive one final
render/encode allocation rather than rediscovering devices inside the
container.

## Compositor policy

Gamescope is a required and continuously tested backend. It is not forced when
another compositor is more reliable for a workload.

The capture-producing display is the outer Wayland server. Gamescope (or the
selected alternative) is a nested client of that server and exposes a separate
app-facing Wayland socket. Gamescope compatibility therefore never grants
Gamescope ownership of capture. The raw-frame path stays inside the worker and
only encoded packets cross the authenticated media socket.

An explicit Gamescope request fails closed if Gamescope cannot be selected.
Automatic mode must bind one concrete compositor and retain a reason before
the worker enters starting state. That gives Nova, Doctor, and logs one
authoritative answer instead of a configured value that silently differs from
the launched runtime.

Expected candidates are:

- Gamescope for controller-first game sessions and its Xwayland boundary;
- Sway or labwc for launcher or desktop workflows that need robust
  multi-window behavior;
- a later evidence-based policy using launcher, GPU, driver, HDR, capture, and
  requested mode.

## Storage model

Safe to share read-mostly when the launcher permits it:

- immutable game payloads;
- installer/download caches;
- base runtime and compatibility-tool images.

Must remain per profile:

- credentials and tokens;
- Steam, Heroic, and Lutris databases;
- Wine and Proton prefixes unless explicitly cloned from a sealed base;
- shader/config writes;
- save data unless a separate synchronization contract owns it;
- runtime directories, D-Bus, Wayland, audio, and input endpoints.

Two active workers must never mount one mutable launcher home read-write. The
registry therefore rejects a second active seat for the same profile even
when it comes from a different client.

## Container boundary

The worker backend will eventually need a narrow contract for:

- the chosen GPU render and encoder devices;
- uinput and uhid access;
- udev visibility without granting the host seat access to virtual devices;
- an isolated runtime directory and session bus;
- audio and compositor startup;
- a persistent per-profile home;
- shared read-mostly game mounts;
- bounded logs and an exact health/ready signal;
- graceful stop followed by a generation-fenced forced stop.

A raw Docker socket and broad privileged mode are acceptable only for a
throwaway laboratory control. A supported deployment should use an allowlisted
broker and the smallest device and capability set that passes the acceptance
matrix.

## Rootless Podman backend checkpoint

The first Linux backend targets local rootless Podman through an explicit argv
runner. It forces remote mode off so inherited `CONTAINER_HOST` or connection
configuration cannot redirect authority to a Podman service. It does not use a
shell, a Docker-compatible socket, host networking, host PID/IPC/UTS
namespaces, wildcard devices, or privileged mode. It refuses to operate with
effective UID zero and fails launch before invoking Podman if the executable,
exact GPU/input character devices, or read-only game roots are not accessible
to the current user.

Each opaque profile is configured with one typed runtime and one exact image
digest. The launch specification must match that mapping; neither the profile
key nor the image may be reinterpreted inside the worker. Each launch is
immutable and includes:

- a digest-pinned image with pulling disabled;
- a pre-created opaque profile volume mounted as the only persistent writable
  home, with implicit volume creation disabled;
- explicit allowlisted GPU and virtual-input devices;
- read-only shared game roots at derived `/mnt/games/<opaque-name>` paths;
- private network, PID, IPC, UTS, cgroup, runtime, D-Bus, PipeWire/Pulse, and
  Wayland boundaries;
- a read-only root filesystem with Podman's writable compatibility tmpfs for
  `/dev`, `/dev/shm`, `/run`, `/tmp`, and `/var/tmp`, plus explicit size bounds
  for shared memory, the worker runtime directory, and `/tmp`;
- bounded process count, container log, and health-check log settings;
- no inherited proxy environment or host-derived `/etc/hosts` entries;
- no capabilities, `no-new-privileges`, a tiny init, and a worker-owned
  health command;
- exact seat, outer-capture and inner-app Wayland resources, runtime profile,
  typed workload selector, image, display/data-plane topology, compositor,
  render node, and encoder labels, but no client identity or profile key.

Inventory is two phase and bounded: an exact deployment-label listing returns
full immutable container IDs, then one JSON inspection validates every ID,
label, state, and cardinality. A running container remains `starting` until
its health check is explicitly healthy. Invalid, truncated, contradictory, or
oversized output fails the complete inventory rather than returning a partial
view. Graceful teardown signals `TERM` to the inspected immutable container ID;
forced teardown removes that exact ID with force. It never targets a name,
latest container, wildcard, or all containers.

The backend and live-host adapter compile into Polaris, but are not wired into
the current singleton runtime. Tests use an injected fake host, so they inspect
only generated argv and synthetic Podman JSON. Podman was not installed and no
container, volume, namespace, device, or profile was created during this
checkpoint.

## Worker image and entrypoint checkpoint

The worker overlay is intentionally small and shared by four independently
locked runtime profiles: a plain Gamescope-capable base, Steam, Heroic, and
Lutris. `containers/multiseat/images.lock.json` records immutable OCI index
digests for every runtime and for the Go build toolchain. Tags such as `edge`
and `latest` are not build inputs. The Containerfile performs no package,
module, or source download: it builds the standard-library-only worker with
CGO disabled, copies that static binary into the chosen locked runtime, and
overrides the image entrypoint.

Separating launcher variants matters for both maintenance and isolation. It
keeps one launcher update from silently changing every seat image and avoids
combining multiple writable launcher homes, credential stores, and runtime
stacks in one image. A future image workflow must build from an exact Polaris
revision, record the resulting Polaris-owned image digest, and configure the
Podman backend with only that final digest. Pinning the third-party base is an
input guarantee, not a substitute for pinning the produced worker image. The
current lock is not a publisher-trust claim: signature, attestation, SBOM,
vulnerability-policy, license-bundle, and retained-manifest gates still belong
in a future publication workflow.

The current `polaris-seat-worker` is a supervisor proof, not a game worker. It
validates the immutable seat allocation, securely reads its capability,
creates private control and media sockets, publishes a process-bound health
record, responds to authenticated heartbeats, and handles authenticated
shutdown. Health must complete mutual authentication and a heartbeat on both
sockets; the existence of socket nodes alone is never a ready signal.

An injectable worker-runtime layer models session bus, audio, the
capture-producing outer display, nested compositor, virtual input, encoder,
and launcher-process-tree readiness. It starts them in that dependency order,
admits IPC only after all seven are ready, propagates an unexpected terminal
signal as worker failure, and tears down in exact reverse order. The nested
compositor therefore exits before the outer display/capture owner. One
120-second total startup ceiling accommodates the established Gamescope
readiness budget; each stop has its own five-second bound so a blocked, failed,
or panicking adapter cannot starve later cleanup. Returned errors identify only
the stage and operation, not adapter-provided paths or diagnostics.

A process-backed adapter layer now supplies a concrete Linux supervision
boundary. Each stage becomes a literal, shell-free `polaris-seat-runtime
serve` argv, receives a fixed allowlisted environment, owns a new process
group, and cannot report ready until it writes the exact versioned record on an
inherited descriptor. TERM is directed to that group and escalates to KILL at
the component deadline. Tests execute this path with a synthetic subprocess,
including hostile literal argv, environment non-inheritance, bad or missing
readiness, early exit, descendant cleanup, and forced stop.

The helper protocol is intentionally stage-specific. Every invocation also
receives `--runtime-namespace`; no invocation receives the controller
capability, client key, profile key, worker name, or controller epoch.

| Stage | Additional literal arguments |
| --- | --- |
| session bus | none |
| audio | audio sink |
| display capture | outer capture Wayland socket, render node, topology, worker-local media policy, width, height, refresh in mHz, HDR flag |
| nested compositor | outer parent Wayland socket, inner app Wayland socket, render node, concrete compositor |
| virtual input | input seat |
| encoder | logical GPU, render node, session count, worker-local media policy |
| launcher process tree | runtime profile, workload kind, opaque catalog target, inner Wayland socket, audio sink, input seat |

Environment is independently allowlisted per stage. Only the launcher receives
the persistent-home XDG paths and runtime-profile setting; for example, the
encoder receives only its render-node setting. The child does not inherit the
worker's ambient environment, standard input, output, or error streams.

The resource helper itself does not exist yet. It is deliberately not faked by
calling a GoW launcher entrypoint: the referenced GoW launch scripts couple
compositor and application startup, while the locked application roots do not
expose one uniform private D-Bus, PipeWire, virtual-input, capture, and encoder
contract. Polaris now binds the exact runtime profile, typed workload plan,
data-plane topology, two Wayland identities, and display mode from admission
through the selected image and helper argv. A real helper still must implement
the actual stage semantics and resolve the plan through a trusted catalog. The
production command injects no adapters or data plane, so Podman health proves
only that the supervisor contract is alive.

The upstream Wolf data plane informed, but does not dictate, this contract.
Wolf uses `gst-wayland-display` as an outer headless
compositor that exposes a framebuffer, nests Gamescope as a Wayland client for
its Xwayland boundary, creates virtual audio sinks through a standalone audio
service, uses inputtino plus fake udev for virtual-device lifecycle, and sends
the captured frames through GStreamer. Gamescope compatibility therefore does
not imply that Gamescope itself should own the capture boundary. The Polaris
stage graph now makes the same ownership edges explicit without prematurely
choosing GStreamer over a Polaris-native capture/encode implementation. A
process that merely opens expected sockets and reports ready still would not
satisfy that contract.

The broker's default graceful-stop deadline is now 45 seconds. It covers the
worker's 35-second worst-case serial reverse teardown plus the controller's
default five-second authenticated-shutdown I/O budget and five-second backend
command budget before an exact-generation forced removal is allowed.

## Local control and media IPC checkpoint

`--network=none` remains the supported boundary. A pre-created, mode-0700
runtime root is the only external filesystem prerequisite. Polaris now
exclusively creates one mode-0700, generation-scoped authority directory
beneath that root, plus private `ipc` and `auth` children. `ipc` is
bind-mounted read-write at `/run/polaris-ipc`; `auth` is bind-mounted
read-only at `/run/polaris-auth`. The latter contains a controller-generated
mode-0600, 32-byte capability encoded as canonical lowercase hex and a
capability-authenticated binary identity record, while the IPC mount contains
two mode-0600 Unix sockets:

- `control.sock` is reserved for lifecycle, input, feedback, status, and
  bounded error messages;
- `media.sock` is reserved for encoded video/audio packets and discontinuity
  markers.

The capability value never appears in argv, environment, labels, logs, or
container inspection metadata. Each connection must have the same effective
UID as the rootless controller and complete mutual HMAC-SHA256
challenge/response. Proofs bind the role, channel, controller epoch, logical
GPU, slot, generation, and worker name, so a valid control proof cannot be
replayed on the media socket or another seat. Fresh random challenges prevent
cross-connection proof replay, and each direction begins at sequence one and
then advances exactly without gaps or reuse.

Every frame has a fixed 32-byte big-endian header. Parsing validates the magic,
channel, message shape, zero reserved flags, exact slot/generation, nonzero
sequence, and advertised length before allocating a payload. Control payloads
are capped at 64 KiB and media payloads at 16 MiB. Authentication alone does
not activate data flow: a controller must explicitly attach each channel, and
only one attached owner per channel is permitted. Health probes authenticate
and heartbeat without attaching, so they cannot consume a stream.

After attachment, input is accepted only on control and acknowledged only
after the exact seat adapter accepts it. Feedback returns on control; encoded
video/audio plus end-of-stream and discontinuity markers return on media. Raw
frames never cross this IPC boundary. Each adapter output repeats the complete
seat identity and is rejected if it names another generation, while the frame
header independently fences every incoming packet. Losing either attached
channel cancels the exact worker so reverse teardown cannot leave a headless
workload running. Adapter errors are bounded and redacted at the supervisor
boundary.

The Linux controller authority store now implements that filesystem
lifecycle without recursive deletion. It walks the pre-created root without
following symlinks, rejects effective UID zero, uses exclusive creation and a
CSPRNG-generated capability, pins directory, token, and signed-record inodes
in a move-only handle, and validates their bytes as well as ownership, type,
mode, size, and the complete child-name allowlist. The record binds the exact
controller epoch, GPU, slot, generation, worker name, and runtime namespace to
the capability; it is recovery evidence, not a second credential. Cleanup
refuses a renamed or replaced generation, a mutated token or record, an
unexpected entry, or an actively listening socket. It removes only inactive
allowlisted socket nodes, the exact private files, and the exact pinned
directory hierarchy. Dropping a handle cleanses its in-memory capability but
deliberately does not guess at filesystem cleanup.

This establishes continuity and fail-closed cleanup against stale, malformed,
or raced filesystem state. It is not a security boundary against a process
that has already compromised the controller's effective UID and can read its
mode-0600 capability; host account and container isolation remain required.

The controller-side client connects with bounded nonblocking deadlines,
requires stable mode-0600 socket nodes and a same-effective-UID `SO_PEERCRED`
peer, and completes mutual authentication independently on both channels.
Connection is all-or-nothing: a missing or rejected media channel closes an
already authenticated control channel. Data-plane attachment is also
all-or-nothing. Input acknowledgement, feedback, encoded media, heartbeat, and
shutdown responses are strictly sequence-checked; media or feedback arriving
before an acknowledgement is retained in a bounded exact-channel queue rather
than confused with that acknowledgement. Handshake/ack phases impose their own
zero- or 32-byte payload limits before allocation rather than accepting the
larger general channel limit.

The backend-neutral Linux coordinator now joins these pieces without depending
on Podman. It creates authority before asking the broker to launch, owns the
move-only handle and controller session together, and uses broker hooks to
require two-channel authentication before Backend Ready can become Registry
Running. Graceful backend stop is preceded by authenticated shutdown whenever
a live session exists. Running sessions heartbeat both channels, and a failure
stops only that exact generation.

The broker exposes active identities only on a complete, validated inventory.
The coordinator uses that snapshot to audit the authority root on every clean
reconciliation. A signed authority belonging to an active worker is retained;
an absent one is reopened through no-follow descriptors and removed through
the same inode/liveness fences as a live handle. An unknown root entry,
malformed or duplicate record, changed token, replacement inode, active socket
contradicting inventory, failed inventory, or more than 256 entries blocks the
entire recovery pass and admission. Recovery never derives deletion authority
from a filename and never recursively deletes. Current-format crash leftovers
are therefore recoverable, while ambiguous or partially damaged state remains
for explicit operator inspection.

These components compile into Polaris but remain outside the singleton runtime.
No configuration path constructs the coordinator, and no worker/container is
started by this checkpoint.

## Launcher acceptance comes second

Steam, Heroic, and Lutris are all first-class targets, but installing their
packages does not prove multiseat. After two generic workers pass the isolation
contract, each launcher must pass:

1. first-run setup and login persistence;
2. install or discover a real title;
3. direct title launch at requested geometry and cadence;
4. controller, keyboard, mouse, audio, and video;
5. disconnect and reconnect;
6. clean stop with the other seat still running;
7. a second launch without stale compositor, prefix, or process ownership.

## Executable proof in this slice

The test_multiseat_runtime target covers:

- two seats sharing one GPU and encoder pool;
- matching typed Gamescope, Steam, Heroic, and Lutris workload plans plus
  runtime profile, data-plane topology, and display mode required at admission
  and preserved through the broker and worker allocation;
- unique worker/runtime/outer-Wayland/inner-Wayland/audio/input identities;
- concurrent admission without duplicate slot allocation;
- independent stop and release;
- stale-generation rejection after slot reuse;
- stale-controller rejection across a control-plane restart;
- separate seat and encoder capacity failures;
- one active seat per client;
- one active seat per persistent profile;
- concrete Automatic selection and fail-closed explicit Gamescope;
- resource names that do not contain client or profile identifiers;
- mandatory clean inventory before worker launch;
- two independent fake workers reaching ready state and stopping separately;
- restart reconciliation that blocks admission while an old-epoch worker exists;
- graceful orphan cleanup followed by deadline-bounded forced cleanup;
- fail-closed handling of rejected and indeterminate launches;
- exact-seat release when one authoritative worker disappears;
- observation failure that closes admission without changing a reserved seat;
- malformed or duplicate inventory that closes admission without guessed
  commands or lifecycle mutation;
- rootless Podman launch arguments with pinned images, explicit devices,
  private namespaces, writable compatibility surfaces, explicit worker
  runtime/temp/shared-memory bounds, and no broad authority;
- one digest-pinned image selected by each configured opaque profile, with
  exact profile, image, geometry, cadence, and HDR reconciliation labels;
- health-gated two-worker inventory with exact ID/label validation;
- exact-ID graceful and forced teardown plus created-worker cleanup;
- fail-closed root, missing-device, unknown-allocation, timeout, ambiguous
  launch output, truncation, inventory bound/cardinality/ID, and malformed
  inventory paths.
- language-neutral IPC authentication and framing golden vectors;
- strict capability syntax, seat/channel proof separation, payload bounds,
  and replay/gap rejection;
- two in-process Linux workers authenticating both channels, exchanging
  heartbeats, and stopping independently;
- private-file, symlink, socket-mode, health-identity, and stale-node checks;
- controller-owned exclusive authority creation, CSPRNG/token validation,
  inode-fenced allowlist-only cleanup, replacement/tamper rejection, and
  active-socket preservation;
- all-or-nothing controller authentication of both Unix channels, bounded
  connect/handshake/I/O deadlines, phase-specific allocation limits, strict
  response sequencing, explicit data-plane attachment, input acknowledgement,
  bounded asynchronous media queues, cross-generation rejection, and
  independent two-worker shutdown;
- capability-authenticated authority records, bounded no-follow restart scans,
  active-orphan retention, inventory-proven inactive recovery, and refusal of
  malformed, duplicate, unexpected, replaced, or live-socket state;
- coordinator ownership from pre-launch authority creation through endpoint
  authentication, heartbeat, authenticated shutdown, backend absence, and
  exact cleanup, including two independently managed seats;
- a real process-level C++ controller to Go worker fixture that authenticates
  and attaches both Unix channels, routes input, feedback, and encoded video,
  heartbeats both channels, then drives exact reverse-order cleanup through
  graceful shutdown of the injected offline runtime;
- a real Linux process-supervision fixture proving exact readiness framing,
  literal shell-free argv, scrubbed environments, descendant group teardown,
  partial-start ownership, and bounded TERM-to-KILL escalation;
- adversarial offline runtime coverage for complete and partial startup,
  malformed leases, early exits, panics, errors, bounded timeouts, exact
  reverse teardown, redacted failures, and two-seat independence;
- two simultaneous injected data planes with exact input, feedback, encoded
  video/audio routing, duplicate-owner rejection, cross-seat input/output
  rejection, route-failure rollback, and teardown isolation;
- digest-only image locks for Gamescope, Steam, Heroic, Lutris, and the static
  worker toolchain, plus a no-network Containerfile build contract.

This remains an offline control-plane/backend proof. A later container
integration test must build and pin the final worker images, pre-create profile
volumes and the private runtime root, instantiate the coordinator behind an
explicit opt-in configuration, and run two real supervisor containers. Before
physical game testing, the missing `polaris-seat-runtime` implementation must
bind the modeled session bus, audio sink, outer display/capture owner, nested
compositor, virtual-input lifecycle, worker-local encoder, trusted workload
catalog, and launcher process tree without weakening the proven adapter,
routing, and teardown contracts. Concurrent real frame/audio/input traffic,
not more synthetic heartbeats, is then the next acceptance boundary.

## Upstream references

- Wolf architecture:
  https://games-on-whales.github.io/wolf/stable/dev/how-it-works.html
- Wolf custom Wayland compositor and framebuffer boundary:
  https://games-on-whales.github.io/wolf/stable/dev/wayland.html
- Wolf GStreamer pipeline:
  https://games-on-whales.github.io/wolf/stable/dev/gstreamer.html
- Wolf fake-udev input isolation:
  https://games-on-whales.github.io/wolf/stable/dev/fake-udev.html
- Wolf configuration and per-profile storage:
  https://games-on-whales.github.io/wolf/stable/user/configuration.html
- Games on Whales application images:
  https://github.com/games-on-whales/gow
- Headless Sunshine/Steam Docker proof of host plumbing:
  https://github.com/numsu/headless-sunshine-steam-docker
- Podman rootless container and device model:
  https://docs.podman.io/en/stable/markdown/podman-run.1.html
- Podman JSON container inspection:
  https://docs.podman.io/en/stable/markdown/podman-container-inspect.1.html
