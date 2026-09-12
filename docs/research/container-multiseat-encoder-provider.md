# Continuous seat media

The worker now has a continuous encoder and a concrete source for its existing
`seatDataPlane`. The earlier fake-only diagnosis was incorrect: the missing
piece was the connection to a real encoder. This document describes the local
implementation, not acceptance of a client game stream.

## Process and media ownership

Each seat owns one Go encoder supervisor and one native GStreamer helper. The
helper consumes that seat's descriptor-pinned capture socket and private Pulse
sink monitor. Its request includes the allocated render node, encoder session
reservation, audio sink, width, height and refresh rate. It never discovers a
replacement GPU or attaches to the desktop audio server.

The codec contract is H.264 constrained baseline at an 8 Mbps ceiling, with
stereo 48 kHz Opus in five millisecond packets. Software capture and device-free
checks use OpenH264. GPU capture requires a matching hardware encoder: NVENC
for NVIDIA or VA-API for AMD and Intel. Missing hardware support fails readiness;
a GPU seat does not silently fall back to software or another GPU.

The retained DRM descriptor establishes PCI identity. NVIDIA's CUDA device is
matched by PCI domain, bus, device and function, then a per-device GStreamer
factory must report that same CUDA ordinal. Automatic GPU-selection factories
are excluded. VA-API factories must expose a character device with the same
identity as the admitted render node. Selection is checked again on the running
encoder before publishing readiness.

Both hardware paths use CBR, no B frames and one frame of VBV/CPB capacity.
NVIDIA also disables lookahead and selects the ultra-low-latency tune. Its
minimum I/P quantizer is 10: OpenH264 2.6 silently dropped recovery IDRs from
our lower-QP constrained-baseline streams, while FFmpeg decoded every frame.
The floor made the exact-frame-count and concurrent-stop tests pass with
OpenH264. It deliberately trades the lowest quantizer values for compatibility;
it is not a claim of optimal visual quality for every workload. These
settings follow the [NVENC](https://gstreamer.freedesktop.org/documentation/nvcodec/nvh264enc.html)
and [VA-API](https://gstreamer.freedesktop.org/documentation/va/vah264enc.html)
plugin interfaces. They are configuration choices, not measured latency claims.

The allocation admits even SDR dimensions from 16 through 3840 pixels and
refresh rates from 1 through 240 Hz, subject to the selected encoder's actual
caps. HDR, configurable bitrate and additional codecs remain unfinished.

The GPU capture path shares the retained GBM/EGL context and explicit texture
conversion used by the encoded capture probe. Imported GPU memory must pass
texture-target checks before download. All pipelines retire before releasing
the GPU descriptor or EGL objects. Hardware encoding still uses this verified
CPU download and upload path; zero-copy capture is not established.

## Readiness and transport

The helper publishes a contract only after observing a real H.264 IDR/SPS and
a valid encoded Opus packet. The supervisor verifies that contract against the
allocation before signaling readiness. It creates a mode-0600 encoded Unix
socket inside the seat's mode-0700 runtime directory. One consumer is accepted;
there is no reconnect or second reader for the same generation.

The private connection uses a 12-byte `PME1` header with a packet kind and a
bounded big-endian payload length. Contract and frame bodies use the existing
worker media format. Video is bounded to 16 MiB including its prefix; audio is
bounded to 1400 encoded bytes. Raw video never crosses this connection.

The worker pins and authenticates the endpoint, reads its contract, and
announces it on the authenticated controller media channel. It sends the local
Start command only after the controller acknowledges that contract. Each
stream has monotonic frame indices and the first video frame is an IDR. Capture
timestamps are zero because capture clock provenance is not established;
encode timestamps use the shared kernel's monotonic clock.

IDR requests use an independent control writer while frame reads are blocked.
Reference invalidation requests conservatively force a complete IDR. Malformed
packets, changed contracts, invalid sequence metadata, timeouts and cancellation
retire the connection. Provider teardown stops the producer before closing its
pipes and removes only the endpoint whose inode it still owns.

## Explicit worker activation

The Docker backend has an internal `media_enabled` option, defaulting to false.
Only that option appends the final literal `--media=enabled` worker argument.
Container inventory verifies the executed argument as part of recovery.

The enabled path authenticates the seat's private authority before starting
providers and installs the real process adapters and encoder source. It
currently admits the Gamescope `input-pong-v1` acceptance workload in SDR.
Input and rumble continue through the host's existing generation-bound input
authority; the media plane does not inject input a second time.

This is an explicit worker integration path. The host production controller
still has no activation caller, profile configuration or client launch routing.
Single-device streaming keeps its current behavior. Steam, Heroic and Lutris
launchers still need concrete catalog-backed implementations and acceptance.

## Verification and remaining acceptance

`test-encoder-gpu.c` checks PCI identity mismatches, CUDA ordinals, per-device
factory names, VA device substitutions and symlink refusals without a GPU.
`test-encode-media.py` checks actual software H.264 decoding, bounded Opus
packets, the acknowledgement gate, requested IDRs, malformed controls and
normal termination. The real Go provider test streams two synthetic seats and
proves that stopping one removes its endpoint while the other continues. The
image builder requires this test to pass without skipping in the produced
runtime filesystem. Unit and race checks cover contract validation, endpoint
ownership, cancellation and controller acknowledgement order.

Passing `--render-node=/dev/dri/renderD128` to `test-encode-media.py` explicitly
selects a synthetic hardware-codec check. It encodes generated video and audio,
checks acknowledgement and IDR recovery, and decodes H.264. Add `--two-seats`
to start both sessions together, stop one and require 30 further video frames
and continued audio from its peer. This generated-media test does not capture
a game or establish two-client streaming. Use only the allocated GPU devices
and the exact NVIDIA image matching the host driver, or the matching VA image.

These checks do not validate GPU game capture or client playback. The existing
physical encoded-game probe still skips the continuous encoder and runs its
own bounded native codec observation. Its `RecordProperty` receipts require
`--gtest_output=xml`; they cannot substitute for exercising this data plane.

The next acceptance lane must exercise two exact-image Docker workers through
the continuous media channel, decode both streams, verify input isolation and
IDR recovery, stop either seat, and demonstrate uninterrupted output from its
peer. Hardware encoding must then receive its own AMD and NVIDIA measurements
before any performance or latency claim. Host activation and profile routing
follow that evidence, with enabling multiseat and no configured profiles leaving
ordinary streaming unchanged.
