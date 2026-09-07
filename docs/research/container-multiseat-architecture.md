# Container multiseat architecture spike

Status: architecture, an offline rootless-Podman backend, locked image inputs,
immutable per-seat runtime/data-plane bindings, typed workload plans, a
supervisor/IPC routing proof, four isolated runtime providers, and a
host-brokered virtual-input authority contract. Nothing in this document
enables multiseat, launches a container, opens an input device, or changes the
current single-workload runtime.

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
- host-brokered uinput and uhid creation without exposing either creation
  endpoint to an untrusted launcher;
- only the exact generation's verified event nodes in each worker;
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
to the current user. Every GPU path is paired with the immutable filesystem,
inode, and character-device identity admitted by the trusted construction
boundary. The complete catalog is re-read against that baseline during launch
readiness and again immediately before `podman run`; replacement, aliasing, or
drift in an unrelated logical GPU therefore closes the whole admission edge.

Each opaque profile is configured with one typed runtime and one exact image
digest. The launch specification must match that mapping; neither the profile
key nor the image may be reinterpreted inside the worker. Each launch is
immutable and includes:

- a digest-pinned image with pulling disabled;
- a pre-created opaque profile volume, whose existence the backend verifies
  with `podman volume exists` immediately before launch because `podman run`
  would otherwise create it silently, mounted as the only persistent writable
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

The global input-device prototype has been removed. The backend now requires
an injected generation-fenced allocation from the host input authority. It
validates the complete allocation and its kernel metadata twice before command
invocation, then emits one `--device=host-event:fixed-worker-alias:rw` argument
per exact node. Raw `/dev/uinput`, `/dev/uhid`, a host-wide `/dev/input`, and a
caller-supplied input path are not representable in the Podman options or launch
specification.

The complete allocation has a canonical opaque SHA-256 fingerprint bound to
the seat handle, plan, host and worker paths, filesystem and character-device
identity, kernel name, phys, and host seat. That fingerprint is stored as an
immutable worker label. It is a reconciliation identifier, not an
authentication credential.
This moves the inspected worker-label contract to protocol 3 so an older
global-device prototype cannot reconcile as a current worker.

Inventory is two phase and bounded: an exact deployment-label listing returns
full immutable container IDs, then one JSON inspection validates every ID,
label, state, device binding, and cardinality. Podman may reconstruct an
equivalent input host path, so input inspection still requires its expected
major/minor plus the exact worker-local destination. GPU bindings are stricter:
their host path and full identity must match the immutable admitted baseline,
and the complete GPU catalog must still be disjoint. The complete GPU and input
binding set and manifest fingerprint must match current authority; missing,
extra, broadened, inaccessible, aliased, or changed bindings fail the inventory.
The one exception is a stopped worker with no exact allocation left to
authenticate against, because it was released or no longer resolves for that
seat label, plan, or fingerprint: it stays visible as stopped without input
authority, so the broker can release its seat and the container can be reaped
rather than failing every inventory until Podman removes it. A stopped worker
whose allocation still resolves keeps the full binding check. A
running container remains `starting` until its health check is explicitly
healthy. Invalid, truncated, contradictory, or oversized output fails the
complete inventory rather than returning a partial view. Graceful teardown
signals `TERM` to the inspected immutable container ID; forced teardown removes
that exact ID with force. Stop uses the identity-only inspection path so a
worker can still be removed after its input allocation has disappeared. It
never targets a name, latest container, wildcard, or all containers.

The backend, authority-manifest adapter, and live-host adapter compile into
Polaris, but are not wired into the current singleton runtime. Tests use
injected input authority, kernel observations, host character-device identity,
command results, and synthetic Podman JSON. Podman was not installed and no
container, volume, namespace, device, or profile was created during this
checkpoint. Rootless group-only device access and SELinux policy remain a live
deployment gate: this source slice does not preserve supplementary groups,
change host policy, or claim physical access to a mapped event node.

## Worker image and entrypoint checkpoint

The worker overlay is intentionally small and shared by four independently
locked runtime profiles: a plain Gamescope-capable base, Steam, Heroic, and
Lutris. `containers/multiseat/images.lock.json` records immutable OCI index
digests for every runtime and for the Go build toolchain. Tags such as `edge`
and `latest` are not build inputs. The Containerfile performs no package,
module, or source download: it builds the standard-library-only worker,
dispatcher, and implemented providers with CGO disabled, copies those static
binaries into the chosen locked runtime, and overrides the image entrypoint.
Its final stage rejects a locked root that does not already contain the fixed
D-Bus/PipeWire tools and trusted configurations.

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
| nested compositor | outer parent Wayland socket, inner app Wayland socket, render node, width, height, refresh in mHz, HDR flag, concrete compositor |
| virtual input | input seat |
| encoder | logical GPU, render node, session count, worker-local media policy |
| launcher process tree | runtime profile, workload kind, opaque catalog target, inner Wayland socket, audio sink, input seat |

Environment is independently allowlisted per stage. Only the launcher receives
the persistent-home XDG paths and runtime-profile setting; for example, the
encoder receives only its render-node setting. The child does not inherit the
worker's ambient environment, standard input, output, or error streams.

`polaris-seat-runtime` implements the trusted dispatch boundary independently
of any one physical resource provider. The worker and helper
share one canonical parser and environment builder. The helper rejects extra
or reordered argv, non-canonical numbers, cross-stage fields, runtime/workload
mismatches, and all ambient environment entries before it touches the catalog.
It opens only the fixed `/run/polaris-auth/runtime-providers.json` path, without
following a final symlink, and requires a non-writable regular file owned by
the worker's effective UID. That fixed path is part of the existing
per-generation read-only auth mount, not the mutable launcher profile. Strict
bounded JSON rejects unknown or duplicate fields and ambiguous stage
selections. Nested-compositor entries bind the concrete compositor; launcher
entries bind both the runtime kind and exact opaque workload target.

The controller authority transaction now emits the matching catalog from only
those typed selectors. Seven fixed provider locations and empty literal
provider argv are compiled policy, not configuration input. A fully written and
synced temporary inode is linked under the final mode-0400 catalog name, its
SHA-256 is authenticated by the generation's signed recovery record, and the
live handle pins its inode and digest. Validation, restart recovery, and cleanup
all require the exact catalog alongside the token and record; wrong mode,
changed bytes, replacement inode, swapped catalog, symlink, or an unexpected
auth entry fails closed. The existing read-only auth bind mount is its only
container exposure.

The selected executable is subject to the same no-follow, root-owner,
non-group/world-writable checks. The helper executes that already-open file in
place through its descriptor, with a canonical `serve-resource-v1` argv and
only the stage environment. Catalog arguments remain literal elements after
an explicit `--`; no shell or executable path crosses from the controller.
Exec-in-place preserves the exact provider PID and initial process group
already owned by the worker. Directly supervised children receive a
parent-death kill and bounded TERM/KILL handling. A component such as Gamescope
can create its own descendant session, so the provider still owns normal
Xwayland shutdown and exact container teardown remains the final backstop.
Offline Linux tests exercise same-PID dispatch, strict catalog ownership, bad
readiness after provider start, descendant cleanup, literal hostile catalog
arguments, and two concurrent helper groups.

Three catalog targets are now concrete but still inert in production. The
session-bus provider validates a mode-0700, same-UID runtime directory, rejects
preexisting artifacts, starts a descriptor-pinned root-owned `dbus-daemon`,
validates its exact printed address, and completes an EXTERNAL-authenticated
D-Bus handshake before readiness. It captures the socket and any standard
private service directories by inode. On cancellation it bounds TERM/KILL,
reaps the daemon, and removes only the same captured empty artifact set.

The audio provider starts a private PipeWire core with desktop integration and
RAOP discovery disabled, creates one exact named null sink through `pw-cli`,
then starts Pulse-on-PipeWire from the fixed PipeWire binary and pulse
configuration. Its readiness probe speaks the Pulse protocol and accepts only
that sink and its monitor. Pulse and native PipeWire clients receive the same
typed route through `PULSE_SINK` and `PIPEWIRE_NODE`. It captures its socket,
lock, PID, and pulse-directory inodes, stops Pulse before the core, and refuses
to delete replacements or unexpected directory contents.

The display/capture provider starts descriptor-pinned `gst-launch-1.0` with a
scrubbed environment and no persistent plugin registry. `waylanddisplaysrc`
owns the outer headless compositor and negotiates the admitted dimensions and
rational refresh into an owner-only `unixfdsink` socket. The hardware pipeline
requires DMA-BUF; its bounded raw-frame socket name is derived from the runtime
namespace with domain-separated SHA-256, so the controller does not receive or
route raw frames. A later encoder provider can attach with `unixfdsrc` while
the transport remains inside that worker.

The upstream compositor chooses an automatic `wayland-N` socket. Polaris keeps
that server-owned path and adds a no-replace hard-link alias at the exact
controller-allocated capture name. It publishes readiness only after the alias
resolves to the supervised child's PID and UID, a bounded Wayland registry and
callback round trip exposes compositor, shared memory, seat, XDG shell, and one
output global, the current mode exactly matches width, height, and mHz, and one
frame traverses `unixfdsrc`. Hardware additionally requires DMA-BUF protocol
version 3 or newer. HDR fails before process start because this upstream source
does not expose a typed HDR contract. The child inherits a `0077` umask, all
artifacts are captured by inode, and cleanup retains replacements while still
removing the other owned sockets and lock.

All four implemented providers accept only their canonical stage invocation
with an empty provider-argument tail. Every provider-owned top-level dependency
is opened no-follow as a root-owned, non-group/world-writable executable and
launched through that descriptor with a fixed environment and no shell. The
Gamescope provider additionally validates `/usr/bin/Xwayland` immediately
before start and gives Gamescope only `/usr/bin` as its child-search path;
Gamescope owns that child exec inside the immutable image root. The
readiness FIFO is sealed close-on-exec before any child starts. Real Linux tests
exercise D-Bus and audio plus the upstream compositor and raw-frame transport
in software mode, clean normal and partial failure, and prove simultaneous
private audio graphs and display transports remain independent. The software
display path does not open a GPU. The real display fixture for this checkpoint
was built from `gst-wayland-display` revision
`081feb5ab8057937b78104668bb1f507ce42e18d`; no plugin binary is vendored or
installed by this tree.

The fourth provider owns the nested Gamescope boundary without owning capture
or a game process. It consumes and re-probes the exact capture-host socket,
launches Gamescope's Wayland backend with one Xwayland server and the admitted
geometry, and uses Gamescope 3.16.25's readiness FIFO record to discover the
runtime-selected X and inner Wayland displays. Readiness then requires the
supervised PID and UID at both protocol endpoints, exact Wayland width, height,
and millihertz, matching X11 root dimensions, and unchanged parent-socket
identity. Fractional refresh is inherited and verified rather than rounded.
The worker's private `/tmp` must begin with no X display, so production
Gamescope must select `:0`; every runtime and X11 artifact is inode-fenced.

The request preserves and validates the admitted render-node path, but
Gamescope 3.16 does not expose an exact render-node-path selector. The
`POLARIS_RENDER_NODE` environment marker is evidence for child diagnostics, not
device-selection enforcement. Exact GPU authority therefore remains the
container backend's explicit character-device allowlist together with the
outer Wayland binding; the production adapter stays inert until that complete
boundary is activated and physically gated.

The software test backend is deliberately not represented as a production
nested smoke. Fedora's installed lavapipe can run Gamescope's headless backend
and real Xwayland/Wayland protocols, but it lacks the present-id and
present-wait extensions Gamescope requires for its Wayland backend. The
installed Xwayland still opens the host render node during initialization even
when glamor is explicitly disabled, so this opt-in test requires separate
DRM-device authorization. Fake-process tests pin the exact production argv,
parent socket,
readiness record, peer identity, geometry, failure cleanup, replacement
retention, and two-seat isolation. An authorized real-GPU test is still needed
before the Wayland backend can become a release gate.

The providers are built and copied into their fixed catalog locations, but the
production worker remains inert because `run` still injects no adapters or data
plane. The controller/coordinator catalog path also remains outside the current
singleton runtime. No OCI image was built during this checkpoint. The image
recipe now fails closed unless its locked root contains `gst-launch-1.0`,
`gst-inspect-1.0`, `waylanddisplaysrc`, `unixfdsink`, `unixfdsrc`, `fakesink`,
`gamescope`, and `Xwayland`; the existing locked application roots
have not yet passed that gate. Virtual input, encoder, and launcher providers
remain missing; Podman health still means only supervisor liveness.

## Host-brokered virtual-input authority checkpoint

Virtual input must be created before the worker launch, on the trusted host
side. This lets Podman map only known event nodes into the worker's private
`/dev`, while the host broker retains `/dev/uinput` and `/dev/uhid`. It also
avoids depending on hotplug delivery through the worker's private network and
mount namespaces. A launcher cannot manufacture or discover another seat's
nodes merely because it knows their host major/minor values.

`multiseat::input::authority_t` and the injected Linux
`inputtino_host_backend_t` are the first executable boundary for that model.
They remain outside the singleton runtime, Podman command construction, and
the worker protocol. Tests replace both inputtino creation and kernel I/O, so
they open no real input device. Their contract:

- starts admission closed and requires one complete, unambiguous backend
  inventory before any device creation, with both expected and observed
  allocation sets bounded at 256;
- binds every allocation to the full controller epoch, GPU, slot, generation,
  and opaque input-seat name;
- always requires one keyboard and both event nodes created by inputtino's
  relative/absolute mouse pair, optionally admits touch and pen, and bounds
  generic Xbox-style gamepad slots at sixteen;
- accepts only canonical `/dev/input/eventN` host nodes with unique filesystem
  inode and character-device identities, the kernel's event-number-to-minor
  mapping (static minor 64 + N for event0 through event31, dynamic minor N
  from event256 upward, nothing in between),
  Linux input major 13, an exact generation-derived kernel name, and host seat
  `seat-polaris`;
- maps those nodes to fixed worker-local paths such as
  `/dev/input/polaris-keyboard`, `/dev/input/polaris-mouse-relative`, and
  `/dev/input/polaris-gamepad-0`, which reveal no client or profile identity;
- derives each identity through `O_PATH|O_NOFOLLOW`, `fstat`, root-owned
  non-writable sysfs attributes, and bounded `/run/udev/data` parsing; a
  missing udev record is retryable rather than proof of isolation;
- validates, but does not expose, an optional `/dev/input/jsN` child returned
  for an Xbox-style gamepad;
- cannot accept `/dev/uinput`, `/dev/uhid`, a duplicate or noncanonical node,
  a contradictory nonempty phys marker, or a seat0 allocation as authoritative;
- accepts one canonical big-endian typed event per route call, bounded at 24
  bytes, with strict nonzero sequencing; malformed values do not consume a
  sequence, while stale generations and indeterminate backend results fail
  closed;
- retains ambiguous teardown identities as reconciliation tombstones, removes only an
  exact unambiguous orphan, and confirms orphan removal through a second
  authoritative inventory.

The production factory now wraps inputtino device lifetimes, but no production
code constructs the backend. The pinned inputtino commit accepts
`DeviceDefinition::device_phys` but does not write it for its uinput-backed
keyboard, mouse, touch, pen, or Xbox device. The dedicated udev rule therefore
matches only the reserved `Polaris multiseat *` kernel-name namespace. Polaris
hashes the opaque generation name into that bounded namespace, requires exact
name and `ID_SEAT=seat-polaris` readback, accepts an empty phys only for this
known dependency behavior, and still rejects any nonempty phys that contradicts
the expected isolation marker.

This is a trusted lifecycle, identity, and injection backend, not a worker-owned
virtual-input provider. The route codec has a fixed version and closed event
vocabulary: keyboard transition, relative pointer, absolute pointer, pointer
button, two-axis scroll, touch contact, pen tool, and complete Xbox-style
gamepad state. It rejects trailing bytes, nonzero reserved fields, unsupported
keyboard codes and buttons, out-of-range coordinates or deltas, contradictory
D-pad directions, noncanonical touch release fields, and events for an
unallocated device kind or gamepad slot. Absolute, touch, pressure, distance,
and tilt values use bounded integer representations; no native struct layout or
floating-point bit pattern crosses the boundary.

The backend independently repeats the full-handle, input-seat, and sequence
checks before selecting the one managed inputtino object retained by that exact
generation. It tracks key and pointer-button transitions plus no more than
sixteen live touch contacts before calling inputtino, so duplicate transitions,
unknown releases, or a seventeenth contact cannot reach the dependency. A
rejected managed-device call does not advance its backend sequence. A throwing
or otherwise indeterminate call poisons routing and inventory for that
generation until exact teardown, preventing a possibly partial event from
being replayed after reconciliation.

Xbox rumble is the only feedback kind in this checkpoint. The production
wrapper converts inputtino's callback to an explicit event containing a bounded
gamepad slot and two 16-bit magnitudes. A generation-local gate adds the full
seat handle and its own nonzero monotonic sequence before invoking an injected
controller sink. The gate stays closed during partial creation, closes before
teardown, rejects callbacks from a non-gamepad object or wrong slot, and leaves
any callback already in flight stamped with the old generation so a later
controller adapter can reject it safely.

Exact Podman manifest binding and bind-time identity revalidation remain a
separate injected checkpoint: the adapter queries the authority by exact
generation, verifies the full allocation and live kernel snapshot twice, maps
only event nodes to fixed aliases, fingerprints the full manifest, and
reconciles that fingerprint plus inspected device identities. Because the host
retains the inputtino handles, production input must terminate at this authority
rather than granting the worker an injection endpoint. The current generic
worker input/feedback adapter remains only a synthetic protocol fixture and is
not wired to this route.

The next injected edge parses one complete decrypted Moonlight input packet
without dereferencing packed caller storage. It treats the wire format as an
independent protocol: the big-endian declared size, little-endian magic,
per-field mixed endianness, exact packet length, reserved bytes, finite
normalized floats, fixed controller sentinels, and target event semantics are
all checked before conversion. A packet then passes an explicit keyboard,
mouse, touch, pen, or controller permission before the generation-bound
adapter assigns its next authority sequence. Invalid, denied, no-op, and
unsupported packets consume no sequence.

The representable subset is keyboard transitions with normalized VK codes,
relative and absolute pointer motion, five pointer buttons, both scroll axes,
touch down/move/single-contact release, pen hover/contact, and the base
Xbox-style gamepad state. Modifier metadata is validated but not synthesized;
Moonlight's actual modifier transitions remain authoritative. Non-normalized
keyboard codes, Unicode text, touch hover/cancel-all, pen button-only or
unknown-tool events, extended controller buttons, and controller-associated
touch, motion, and battery packets return an explicit unsupported result. A
fixed preallocated gamepad receives an all-zero state for a canonical inactive
controller packet instead of allowing the packet to destroy host authority.

The reverse adapter accepts only typed rumble from its complete seat handle and
the exact next generation-local sequence, then converts it to the existing
Moonlight feedback message. Its fixed sixteen-entry storage keeps at most the
latest unsent state for each gamepad slot. Same-slot updates coalesce, including
the zero state which stops rumble, while retained cross-slot states drain in
their source-sequence order. Closing the queue clears it permanently, and a
stale generation, gap, malformed event, or callback after close cannot enter.

The offline session-ownership edge binds those adapters to one existing
authenticated control lifetime without moving authentication into the input
parser. Its key is the non-secret launch-session ID plus the process-lifetime
session generation already assigned after launch authentication. A trusted
binding source must atomically claim that exact key and return one exclusive
lease containing the full seat handle and authoritative input/feedback
permissions. No raw session token crosses or persists in this module. Opening
also requires the exact input allocation to remain admitted and refuses touch,
pen, controller, rumble authority, or feedback slots absent from its device
plan.

When feedback is authorized, a second injected source returns an owned
subscription for the exact seat generation. Its callback captures only weak
shared state, so retaining or invoking an old function cannot dereference a
destroyed bridge. The queue supports peek plus sequence acknowledgement:
retryable sends retain the state inside the same fixed sixteen slots, while a
newer same-slot callback can supersede an in-flight value without the older
acknowledgement erasing it. Sends are serialized and carry the complete
authenticated binding. A closed sender or exception closes the bridge rather
than leaving input alive without its control owner.

Close order is explicit. The bridge first rejects new input, feedback, and
drain operations. It then synchronously detaches the feedback subscription,
waits for every already-admitted operation, clears the queue, and only then
releases the authenticated-session lease. Lifecycle locks are not held while
calling the input authority or feedback sender, allowing synchronous rumble
publication during an input route without deadlock. The sender is forbidden
from re-entering close or drain on the same bridge.

Concrete process-local ownership adapters now implement the three injected
edges without activating them. The authenticated binding registry is bounded
to the same 256-entry ceiling as input authority, rejects duplicate control
keys and physical seat-slot reuse across generations, and grants only one
claim at a time. Retiring a registration first makes it undiscoverable and then
waits for the bridge lease to detach. Registry shutdown similarly rejects new
authentication and waits for every outstanding claim. Registry destruction is
non-blocking but retires every entry, allowing retained leases and registration
owners to unwind safely without dereferencing the destroyed source object.

The feedback hub exposes a weak, non-throwing sink suitable for the existing
inputtino backend constructor. It holds at most one subscriber per physical
seat slot, routes only an exact full-generation handle, catches callback
exceptions, and makes both per-subscription detach and hub close wait for all
callbacks already in flight. Its global in-flight count also covers the race
where a subscription removes itself while hub shutdown is waiting. A retained
backend sink becomes a harmless no-op after hub destruction.

The concrete mailbox sender accepts only the exact immutable authenticated
binding and a nonzero, in-range rumble state. It maps typed queued, retry, and
closed mailbox outcomes to the session bridge. The mailbox interface is the
deliberately narrow future boundary to `stream::session_t`'s existing
control-thread queue; no ENet peer, encryption key, session token, or raw
network send is representable here.

An explicitly activated production owner now joins registration, bridge,
feedback subscription, and the existing control-thread mailbox for one live
session. `stream::session_t` derives the non-secret per-stream key and immutable
input permissions from its already-authenticated launch state; callers can
supply only an admitted seat handle, authority, registry, and typed feedback
hub. A selected multiseat session never falls back to singleton input after
rejection or close. Teardown closes the mailbox edge, quiesces and detaches the
bridge, then retires registration, while permission changes require a fresh
session. ENet transmission remains on the existing control thread.

The production construction boundary is now present but default-off.
`stream::session::start()` asks one process-global activation gate before it
closes input selection or allocates singleton input. With no installed gate,
ordinary sessions follow the unchanged singleton path. An explicitly enabled
gate accepts only an already-admitted seat and keys that pending selection to
the authenticated launch ID plus its lifecycle generation. The exact RTSP
session converts that selection into the process-lifetime-monotonic stream key;
a reused launch ID with a stale generation remains unselected. Duplicate
session or physical-seat selections are rejected, selection is bounded, and a
selected bind failure remains a fail-closed tombstone instead of falling back.

One process-local coordinator now owns the feedback hub, injected host backend,
input authority, binding registry, activation gate, installation, and pending
launch selections in dependency-safe order. Its backend factory receives the
retained feedback sink before constructing the authority. Configuration is a
typed option which defaults to disabled: that state installs no gate and cannot
reconcile, create, release, or select input. The enabled test path derives each
selection key from the real authenticated launch object rather than accepting
an identity from the media or input packet. Reconciliation cannot omit a seat
while any launch selection still retains its exact authority. The coordinator
owns that exact shared launch object until explicit retirement and rejects
another object even if it repeats the same numeric identity, so a copied stream
key cannot outlive the authoritative cancellation edge.

Cancellation keeps an exact fail-closed tombstone while the RTSP launch remains
admissible. The coordinator will retire it only after the same launch object is
atomically cancelled and no selected stream remains bound to the registry.
Until the registry exposes a seat-keyed claim query, that retirement check is
deliberately process-wide and conservative. Shutdown first closes the
still-installed activation gate, waits for live bindings and their feedback
callbacks to detach, closes the registry and hub, and releases every exact input
allocation. An indeterminate input teardown retains the closed gate for an
explicit retry while the coordinator remains alive. A future production owner
must observe a successful shutdown report before destroying the coordinator.
If a direct owner ignores that contract, the controller, the Moonlight runtime,
and the coordinator apply one fail-closed policy: each detaches the
process-global entry points this graph installed (the runtime lifecycle target
and the activation gate), refuses further selection, and deliberately retains
the complete implementation graph while a stream, activation, or exact input
cleanup is still pending. No bridge can outlive its raw authority reference,
and new sessions take the ordinary single-seat path instead of reaching a graph
nobody owns. The process-exit owner in `main.cpp` only retains, which is moot
because the process is leaving. Final input cleanup walks the
authority in place rather than allocating a snapshot, and the public `noexcept`
shutdown edge converts any remaining exception into an explicit retryable
incomplete report.

The Linux-only `multiseat_moonlight_input` configuration now constructs this
owner through the main runtime, but it defaults to false. The disabled path
returns before creating the production device factory, backend, coordinator, or
activation gate. The web configuration model preserves that default without
exposing a checkbox. Enabling the option constructs the host-input owner and
installs an empty authenticated-launch gate; it does not select a seat. The only
selection entry point requires the exact shared RTSP launch object, an immutable
seat handle, and the worker's exact input-seat name. The allocation must match
both pieces of authority. There is deliberately no request-facing caller for
that entry point at this checkpoint.

A narrow worker-to-Moonlight adapter now supplies that seam without treating a
raw handle as authority. The worker coordinator revalidates the exact running
generation, its private authority record, and its authenticated dual-channel
control session while holding its lifecycle lock through selection. The adapter
also requires the paired-client UUID on the retained launch to equal the seat's
admitted client key, passes the coordinator-owned input-seat name, and derives
controller-feedback intent from the launch's authenticated permissions. A
stale, merely starting, unauthenticated, tampered, cross-client, or cross-wired
seat therefore cannot register a selection. The adapter is compiled but is not
constructed or invoked by HTTP, configuration, or the singleton launch path.

RTSP handshake cleanup and authenticated launch completion are now separate
lifecycle edges. Connecting the control channel may release pending handshake
state, but it does not cancel the selected launch. Rejection, timeout, every
rejected RTSP ANNOUNCE path, failed RTSP setup, failed stream start, and normal
selected-stream stop all retire the exact launch-generation selection. Stream
teardown closes the live input bridge
and releases its physical-seat claim before reporting launch completion. The
last selected stream also sweeps conservative cancelled tombstones left while
another process-wide seat claim was live.

Main shutdown first closes and uninstalls the activation gate, waits for calls
already admitted through that gate, then shuts down the coordinator and its
input dependencies. If exact input cleanup remains indeterminate, the runtime
keeps the already-closed owner alive for process exit instead of destroying
dependencies beneath a retryable cleanup edge.

No worker, container, HTTP, or configuration path activates a real selection
yet; the authenticated adapter remains an uncalled process-local boundary, and
ordinary singleton input remains unchanged whether the option is disabled or
an enabled empty gate has no selection. Physical client
smoke at this checkpoint can prove that the production lifecycle wiring does
not regress ordinary streaming, controller input, or disconnect teardown; it
cannot prove multiseat input isolation. Crash-persistent node discovery,
rootless group/SELinux deployment policy, and physical container proof remain
required.

Steam Input also needs a separately mediated creation path; granting its
container raw uinput would reintroduce the authority this contract removes.

The upstream Wolf data plane informed, but does not dictate, this contract.
Wolf uses `gst-wayland-display` as an outer headless
compositor that exposes a framebuffer, nests Gamescope as a Wayland client for
its Xwayland boundary, creates virtual audio sinks through a standalone audio
service, uses inputtino plus fake udev for virtual-device lifecycle, and sends
the captured frames through GStreamer. Gamescope compatibility therefore does
not imply that Gamescope itself should own the capture boundary. Polaris now
uses that upstream compositor for the outer display and GStreamer's Unix-FD
transport for the handoff, preserving DMA-BUF-capable frames between separate
provider processes. The later encoder remains a distinct policy boundary. A
process that merely opens expected sockets and reports ready still does not
satisfy the contract.

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
mode-0600, 32-byte capability encoded as canonical lowercase hex, a mode-0400
provider catalog, and a capability-authenticated binary identity record. The
record binds the catalog digest as well as the generation identity. The IPC
mount contains two mode-0600 Unix sockets:

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

The trusted controller composition owner now closes the local lifecycle graph
without changing that default-off boundary. Disabled creation does not invoke
its dependency factory. Enabled, injected construction owns the registry,
worker authority store, Moonlight input runtime, worker backend/coordinator,
and authenticated worker-to-Moonlight adapter in dependency-safe order. Initial
and retry reconciliation always establishes authoritative worker absence before
it may prune host input. Starting a seat prepares its exact host input before
launching the worker; a proven worker rejection rolls that allocation back,
while an indeterminate launch retains it until inventory proves the generation
absent. Shutdown first quiesces Moonlight selection and registry claims without
waiting, and it does not begin worker teardown while an activation or claimed
stream remains. A caller retries after those owners close. Stop and shutdown
similarly refuse to release input while an exact worker or stream can still own
it. There is still no production caller, configuration switch, HTTP path,
container activation, or device mutation.

A concrete Linux dependency factory now proves that this owner can be composed
from one trusted GPU catalog without duplicating capacity or device authority.
It derives both registry capacity and the rootless Podman allowlist from each
catalog entry, rejects cross-entry path overlap, and verifies that every
allowlisted path resolves to a character-device identity owned by exactly one
logical GPU before constructing either capacity view. It persists those exact
path/identity pairs in the Podman catalog and revalidates the complete baseline
before launch and authoritative inventory, preventing a later device-node
replacement from receiving an independent seat or encoder budget. Per-GPU
device lists are exclusive; a future legitimately shared global node must use a
separate, budget-neutral owner rather than being duplicated across GPU entries. The
factory creates the private worker authority store and binds the Podman input
manifest source to a read-only, exact-generation allocation view owned by the
Moonlight runtime. The view stays readable through quiesce and a pending
shutdown, because the authoritative worker inventory that proves a stopped
worker is gone runs after Moonlight quiesce and before input close; it returns
no authority after release or once shutdown has closed. Disabled construction
returns before catalog validation or any factory;
enabled tests inject the UUID, input backend, Podman host, kernel probe, and a
fail-closed worker transport, so they execute no process, container, or device
operation. A complete-source-tree guard keeps this factory outside production
callers. The real dependency defaults exist only behind this uncalled factory;
wiring it to configuration and admitting a seat remain later, separately
reviewed work.

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

The offline test suite covers:

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
- atomic mode-0400 provider catalogs derived from typed compositor/workload
  selection, with a Go/C++ schema golden, record-bound SHA-256, inode-fenced
  validation and cleanup, and tamper/swap/replacement recovery refusal;
- real private D-Bus and PipeWire/Pulse providers with protocol-level
  readiness, exact sink routing, descriptor-pinned dependencies, bounded
  TERM/KILL, inode-fenced artifact cleanup, failure containment, and
  simultaneous two-seat audio isolation;
- a real capture-producing outer Wayland provider with exact peer, global, and
  output-mode validation, a one-frame Unix-FD readiness proof, owner-only and
  inode-fenced artifacts, fail-closed HDR, and simultaneous software-rendered
  display isolation without opening a GPU;
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
- production live-session ownership with exact registration and seat claims,
  routed input, typed control-mailbox feedback, retry retention, bridge-open
  rollback, and callback-quiescent close;
- default-off production activation with exact launch-generation selection,
  selected-versus-unselected isolation, duplicate seat/session refusal,
  stale-launch fencing, fail-closed bind tombstones, and RAII installation;
- coordinator-owned backend, authority, registry, feedback, activation, and
  launch-selection lifetimes, including disabled no-op construction,
  selected/unselected coexistence, cancellation tombstones, registry-first
  nonblocking quiesce, claimed-stream retry, direct-owner fail-closed retention
  with one detach policy at the coordinator, runtime, and controller,
  and allocation-free retryable exact input cleanup;
- serialized worker-to-launch authorization with exact running-generation,
  private-record, dual-channel, paired-client, input-seat, and permission
  checks, without a request-facing caller;
- trusted default-off controller composition with input-before-worker startup,
  worker-first reconciliation, proven-rejection rollback, indeterminate-owner
  retention, paired-client launch selection, and activation/stream-before-worker
  shutdown barriers, using only injected fake backends and no production caller;
- default-off concrete dependency composition from one GPU catalog, including
  lexical and character-device alias rejection, factory-free disabled
  construction, immutable physical-identity revalidation, failure unwind,
  offline Podman reconciliation, a quiesced shutdown that reconciles a listed
  stopped worker through the still-readable manifest view, and
  exact-generation read-only input manifest lookup with no production caller or
  live host mutation;
- digest-only image locks for Gamescope, Steam, Heroic, Lutris, and the static
  worker toolchain, plus a no-network Containerfile build contract.

This remains an offline control-plane/backend proof with four locally
exercised runtime providers and an injected host-input authority. A later
container integration test must build and pin the final worker images,
pre-create profile volumes and the private runtime root, construct the
production dependencies behind the trusted controller owner, and run two real
supervisor containers. Before physical game testing, the
remaining providers and immutable catalog must bind
the nested compositor, virtual-input lifecycle, worker-local encoder, exact
workload target, and launcher process tree to the implemented private session
bus, audio sink, and outer display/capture owner without weakening the proven
dispatcher, adapter, routing, and teardown contracts. Concurrent real
frame/audio/input traffic, not more synthetic heartbeats, is then the next
acceptance boundary.

## Upstream references

- Wolf architecture:
  https://games-on-whales.github.io/wolf/stable/dev/how-it-works.html
- Wolf custom Wayland compositor and framebuffer boundary:
  https://games-on-whales.github.io/wolf/stable/dev/wayland.html
- Wolf GStreamer pipeline:
  https://games-on-whales.github.io/wolf/stable/dev/gstreamer.html
- Validated `gst-wayland-display` revision:
  https://github.com/games-on-whales/gst-wayland-display/tree/081feb5ab8057937b78104668bb1f507ce42e18d
- GStreamer Unix-FD frame transport:
  https://gstreamer.freedesktop.org/documentation/unixfd/index.html
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
