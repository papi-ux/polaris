# Offline upstream validation

The combined changes cover input/certificate hardening, explicit pairing approval,
encoder/capture lifecycle, conditional probe reuse, rational timing and PipeWire
negotiation, nanors runtime dispatch, optional VA-API controls, stable KMS names,
and isolated multiseat input/runtime images. Production multiseat activation and
worker adapters remain unwired. Runtime dependency/provider success is not game
streaming acceptance.

## Private capture regression

Build the full native video suite with CUDA support, then run on an NVIDIA host
with labwc, wlr-randr, and vkcube installed:

```sh
python3 tools/tests/video_lifecycle_harness.py \
  --test-binary build/tests/test_polaris_video \
  --render-device /dev/dri/renderD128 \
  --evidence build/private-video-evidence
```

The evidence directory must be new. The harness creates its own home and Wayland
runtime, starts an animated private compositor, and exercises nine capture cycles:
abort before the first frame, repeated reconnects, explicit 59.940 FPS, a 60 FPS
stream on a 120 Hz output, SHM fallback, and successful DMA-BUF retry. It asserts
the observed transport, checks render-descriptor stability after warmup, and
retains an encoded packet through encoder/compositor teardown to check ownership.
It starts no streaming server, input authority, launcher, or game.

A process-local child subreaper and retained pidfds provide bounded cleanup,
including reparented descendants and termination signals. The receipt records
forced cleanup and remaining resources; either prevents an acceptance pass.
Private logs/configuration remain for inspection. The test skips in ordinary
native runs unless explicitly enabled through this harness.

## Measured comparison

Three interleaved runs compared the same test against lifecycle baseline
`7cdc9c90e712f1b5113e50dc69212897ca0289a4` and the combined implementation on an
RTX 4090, NVIDIA 610.57.04, Core i9-14900K, using GCC 16, CUDA 13.2, Debug builds,
1280x720 H.264 NVENC at 10,000 kbps, and the same private animated compositor.
These are capture/encoder measurements; they exclude network and client latency.

| Warm reconnect median | Baseline | Combined implementation |
| --- | ---: | ---: |
| Probe call | 51.74 ms | 5.25 ms |
| Capture-to-encode completion | 1.460 ms | 1.473 ms |
| Capture stop | 41.57 ms | 41.47 ms |
| Delivered cadence | 60.02 FPS | 60.14 FPS |
| Resident memory | 305.3 MiB | 307.9 MiB |

Each nine-cycle run performed nine full probes on baseline and five full probes
plus four reuse hits on the implementation. Output and capture-transport changes
required full probing. Warm probe time fell about 90%; the encode/stop/cadence
results do not establish a performance improvement. Short frame samples do not
resolve the 0.060 FPS difference between 59.940 and 60. Separate private PipeWire
producer measurements cover rational cadence directly.

The baseline leaked three render descriptors on each DMA-BUF reconnect. Its
render-descriptor count grew from 5 to 26 over the nine cycles; the implementation
held at 2. The baseline deliberately fails that regression while completing all
frame/transport checks. The implementation retains additional bounded provider
proofs: warm total descriptors were 237 versus 80, and final median RSS was
322.0 MiB versus 315.3 MiB. Probe reuse trades retained provider references and
memory for fewer live probes; it is not a memory optimization.

The independent nanors benchmark measured a 22.8–33.0% reduction in parity encode
CPU time, with identical parity, and a small codec-creation cost increase. See
[fec-runtime-dispatch.md](fec-runtime-dispatch.md) for its separate method and
limitations. No end-to-end game-streaming gain is claimed.

## Remaining hardware boundaries

VA-API controls preserve automatic defaults and have driver/codec option tests;
AMD/Intel hardware evidence is required before promoting new defaults. KMS
qualified and numeric selection have physical capture evidence; disconnect and
ambiguous binding behavior also have native DRM boundary regressions. The four
runtime profiles have isolated provider/input evidence, including independent
workers and cleanup. Worker-local encoding, launcher management, production
media routing, seat-aware status, and real concurrent game streams remain the
next milestone.
