# Multiseat worker image inputs

This directory defines an offline-reviewable image recipe. It does not enable
multiseat or make the current Polaris process a container controller.

`images.lock.json` contains the only accepted build and runtime inputs. Every
reference names an immutable OCI index digest; moving tags are intentionally
absent. The plain Gamescope-capable base and the Steam, Heroic, and Lutris
variants all receive the same static `polaris-seat-worker` binary. Keeping the
launchers separate avoids multiplying package and credential state inside one
large image.

The build stage has no module or package download step. The worker uses only
the Go standard library, disables CGO and module-network access, runs its tests,
then emits a static Linux/amd64 binary. A future image job must select a runtime
reference from the lock, build at an exact Polaris revision, record the
resulting image digest, and hand only that final digest to the Podman backend.

These locks establish immutable byte identity, not publisher trust. No
signature, attestation, SBOM, vulnerability policy, or license bundle is
claimed by this spike. A publishable image must add those gates and retain the
resolved upstream manifests as provenance evidence before any lock refresh.

The current entrypoint is intentionally a supervisor and IPC proof. It owns
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

That adapter set is a concrete supervision boundary, not the missing media
implementation. No `polaris-seat-runtime` helper is built or copied into the
image yet. The production `run` command injects no adapters and therefore does
not start Gamescope, Steam, Heroic, Lutris, audio, capture, encoding, or virtual
input. Treating its healthy supervisor as a streaming-capable worker would
still be a false gate. Tests exercise the real Linux subprocess/readiness/group
teardown path with a synthetic helper and never invoke a launcher or device.

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

The locked Games on Whales images remain useful application roots, but their
launcher scripts couple compositor and application startup and do not provide
one uniform private session-bus, PipeWire, capture, encode, and virtual-input
service contract. The Polaris helper must own those boundaries explicitly; the
worker must not infer readiness from a GoW entrypoint or from the existence of
a Wayland socket alone. The controller binds each opaque profile to one typed
runtime and one exact final image digest, carries the requested display and
data-plane topology through admission and reconciliation, and gives the worker
canonical width, height, refresh, and HDR values. It also supplies an exact
allowlisted workload plan: a typed Gamescope, Steam, Heroic, or Lutris selector
plus a bounded opaque catalog target. No executable path, arbitrary argv, or
shell fragment crosses that boundary. The missing helper must resolve the
target through a trusted catalog and reject any plan that does not match the
selected runtime profile.

Wolf's working data plane uses a capture-producing outer Wayland compositor
with Gamescope nested beneath it, plus separate audio, virtual-input, and
GStreamer services. This contract makes the same ownership edge explicit:
applications use the nested compositor's inner Wayland socket, while capture
uses the outer socket and raw frames remain worker-local through encoding.
Only encoded video/audio and stream markers may cross the authenticated media
channel. Controller input crosses the attached control channel and feedback
returns there; every routed item repeats the exact seat generation and
cross-seat output is rejected. This is still a supervision and routing proof.
A placeholder helper that only creates socket nodes and reports ready would
not make this image streaming-capable.

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
read-only and contains the controller-generated mode-0600 capability file plus
a mode-0600, capability-authenticated identity record used only for bounded
controller-crash recovery. The record binds the exact seat identity and
runtime namespace; it does not grant authority without the capability. Neither
file is placed in argv, environment, labels, or container inspection metadata,
and the worker cannot rewrite them through either mount.

The local coordinator owns the live authority handle and authenticated client
until a complete backend inventory proves that exact worker absent. On restart
it audits at most 256 root entries through no-follow descriptors and recovers
only valid signed records absent from that authoritative inventory. Active,
ambiguous, malformed, replaced, unexpected, or live-socket state is retained
and blocks admission rather than being deleted by name or recursively.
