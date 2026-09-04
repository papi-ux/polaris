# Container multiseat architecture spike

Status: architecture contract only. Nothing in this document enables
multiseat, launches a container, or changes the current single-workload
runtime.

## Outcome

A container build is not the difficult part. The product boundary is allowing
two clients to run different workloads on one host without either seat owning
the other seat's compositor, capture, encoder, audio, input, process tree, or
persistent launcher state.

The first implementation slice therefore introduces an executable admission
and lifecycle contract. It proves that two seats can share one logical GPU
while retaining different exact-generation authority and different worker
resource names. It also proves that stopping one seat cannot stop the other,
that a stale teardown cannot target a reused slot, and that seat and encoder
budgets fail closed.

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
        |     Gamescope or selected compositor
        |     capture A, audio A, input A, process generation A
        |
        +-- seat worker B
              GPU 0, encoder lease 1
              Gamescope or selected compositor
              capture B, audio B, input B, process generation B

The first registry model lives outside proc_t on purpose. Folding a vector into
the current singleton before defining admission and teardown authority would
make shared global state look per-seat without actually isolating it.

## Seat identity and lifecycle

Admission returns an opaque handle:

- logical GPU id;
- slot number within that GPU's configured capacity;
- opaque controller epoch;
- monotonically increasing generation within that epoch.

The client, profile, and workload keys are retained as internal routing
metadata but never appear in worker, runtime, Wayland, audio, or input names;
those names use only the controller epoch and generation. That avoids leaking
identity through process listings and host runtime paths.

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
handle, opaque worker resources, opaque profile and workload keys, chosen
render node, concrete compositor, and encoder lease count. Client identity,
credentials, host paths, and container-engine authority are not part of that
payload.

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

Two active workers must never mount one mutable launcher home read-write.

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
- unique worker/runtime/Wayland/audio/input identities;
- concurrent admission without duplicate slot allocation;
- independent stop and release;
- stale-generation rejection after slot reuse;
- stale-controller rejection across a control-plane restart;
- separate seat and encoder capacity failures;
- one active seat per client;
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
  commands or lifecycle mutation.

This is still an in-memory control-plane proof. A later container integration
test must bind the same protocol to two real workers and demonstrate
concurrent frame, audio, and input heartbeats before physical game testing can
begin.

## Upstream references

- Wolf architecture:
  https://games-on-whales.github.io/wolf/stable/dev/how-it-works.html
- Wolf configuration and per-profile storage:
  https://games-on-whales.github.io/wolf/stable/user/configuration.html
- Games on Whales application images:
  https://github.com/games-on-whales/gow
- Headless Sunshine/Steam Docker proof of host plumbing:
  https://github.com/numsu/headless-sunshine-steam-docker
