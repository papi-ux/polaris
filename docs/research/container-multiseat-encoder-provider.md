# The encoder provider: what is already decided, and the one thing that is not

Companion to `container-multiseat-architecture.md` and
`container-multiseat-user-model.md`. Those settle how a seat is allocated and
what it is to the person using it. This one settles the last provider, because
it is the piece the architecture calls "the one thing standing between a worker
and a stream" and it is the only stage with no implementation.

## The seam is already cut, on both sides

Almost nothing here is an open question. Every surface around the encoder exists
and agrees:

- **The controller already asks for it.** The worker catalog it writes contains
  `provider("encoder", executable("encoder"))`, so a seat is already told to run
  `/usr/libexec/polaris-seat/encoder`.
- **The protocol already speaks it.** `StageEncoder` carries
  `--logical-gpu-id`, `--render-node`, `--sessions` and `--media-pipeline`, plus
  `POLARIS_RENDER_NODE` in its environment, and the argv parser round-trips all
  four.
- **The catalog already validates it.** An encoder entry is accepted with an
  empty selector and target, like every non-launcher stage.
- **Its input already exists.** Display capture runs
  `waylanddisplaysrc render-node=… ! <caps> ! unixfdsink socket-path=…`, so the
  encoder reads `unixfdsrc` from that socket. The capture provider's own probe
  proves the consumer side works.
- **Its output is already typed.** `routedOutput{Identity, messageVideo, Payload}`
  delivered through `workerDataPlane.NextMedia`, under the rule written into
  `data_plane.go`: raw captured frames never leave the worker-local pipeline,
  only encoded packets cross.

What is missing is four files' worth of work, not a design.

## What is actually missing

1. **`RunEncoder`** in `internal/seatprovider`, following the shape every other
   provider uses: parse the invocation, open the readiness writer, normalise
   options, take a signal context, run.
2. **`cmd/polaris-seat-encoder`**, the eight-line entrypoint the other five
   stages each have.
3. **A production `workerDataPlane`.** This is the real gap. The interface has
   one implementation and it is a test fake, and `newWorkerServer` passes `nil`.
   The server already pumps `NextMedia`; nothing fills it.
4. **The image.** The Containerfile builds seven provider binaries and the
   encoder is not among them, which is precisely why the validation run found no
   encoder binary in a locked root.

## The one decision worth making deliberately

**Where the encoder process ends and the data plane begins.**

Option A, a provider process per seat, consistent with the other five. The
encoder owns its GStreamer pipeline, signals readiness, and publishes encoded
packets on a seat-private socket. The worker's data plane reads that socket and
answers `NextMedia`.

Option B, encode inside the worker process. Fewer moving parts and no third
socket, but it breaks the pattern every other stage follows and it weakens the
rule stated in `data_plane.go`, that no implementation may share an encoder
instance across seats. A process per seat makes that structural rather than
something a reviewer has to keep checking.

**Recommendation: A.** The isolation rule is the deciding argument; consistency
with the existing five is the supporting one. The cost is one more socket whose
identity has to be verified the way the capture socket already is.

## What this does not deliver

A stream a person can watch. After the encoder provider there is still no
activation key, and the user model is explicit that enabling multiseat with no
profiles configured must be a no-op rather than a behaviour change. The order is
encoder, then activation, then a seat is reachable.

## How it gets proven

The physical harness reaches the encoded lane now, which it could not do before
the DRM primary node was bound. `POLARIS_PHYSICAL_ENCODED_GAME=1` already
observes encoded frames per seat, with `source: worker-capture`, and already
checks that the surviving seat keeps encoding after its peer stops. That is the
gate this work has to pass, and it exists.
