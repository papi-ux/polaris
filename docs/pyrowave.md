# Experimental PyroWave host support

PyroWave is an optional Vulkan compute encoder for compatible Nova clients on a
fast local network. It uses GameStream transport. Ordinary Moonlight clients
cannot select this codec.

## Build and selection

`POLARIS_ENABLE_PYROWAVE` defaults to `OFF`. To build the experimental encoder,
initialize the pinned dependencies and add `-DPOLARIS_ENABLE_PYROWAVE=ON` to the
normal Linux CMake configuration:

```sh
git submodule update --init --recursive third-party/pyrowave third-party/Granite
```

An enabled host advertises the codec to compatible clients. Nova must explicitly
select PyroWave; its Auto choice does not select it. Standard release packages
must not be assumed to include the encoder merely because their version is a
beta. Verify the build option for the package being tested.

The initial path converts CPU BGRA capture to full-range Rec.709 YUV420 and then
encodes on Vulkan. It supports SDR 8-bit 4:2:0 with even output dimensions from
16 through 4096 per axis. It scales capture to the requested dimensions with
aspect ratio preserved and even letterboxing. HDR, 4:4:4 and Spaces are outside
this route. Accepted frame rates and dimensions do not guarantee sustained
performance on a particular GPU or network.

## Transport compatibility

The shared Android/Linux profile uses PyroWave revision
`186f0393b77f7755953b5ecde994bb1cec2e4155` (C API 0.6.0) and Granite revision
`b6cffd5ce81f540f0855e6778428483e14763d9b`. The RTSP offer contains both
`a=rtpmap:99 PYROWAVE/90000` and
`a=fmtp:99 pyrowave-186f0393-sdr420-v1`. The client format is `0x10000`, the
server capability is `0x00800000`, and the selected `bitStreamFormat` is `3`.

Each GameStream frame carries one complete raw PyroWave bitstream, with the
coefficient packets concatenated in order. Each frame is independently coded.
The exact payload length excludes transport padding. Encoding retained capture
content still produces a new codec frame at the current bitrate budget; it must
not resend the previous codec sequence unchanged.

The encoder limits each frame to 3 MiB. Larger frames within that limit can lose
FEC parity protection under the transport's existing policy. This is not a
promise of recovery from every packet loss. Keep client and host dependency pins
and the profile token in agreement; the C API version alone does not establish
bitstream compatibility.

## Live tuning and diagnostics

The encoder accepts runtime bitrate updates without restarting a stream. Its
per-frame budget follows the negotiated rational frame rate and the transport
limit. Host bounds and encoder acknowledgement still apply. A user request, the
host's current target, the encoder's applied budget and measured received video
bitrate are different measurements.

PyroWave is intra-only, so sharpness can require substantially more bandwidth
than an inter-frame codec. The implementation does not add a PyroWave-specific
automatic quality floor or establish a universal bitrate recommendation.

Diagnostics report PyroWave as the codec and distinguish CPU capture from
Vulkan encoding. Encoder timing excludes the intentional wait for the next
frame. The high-refresh adaptive guard requires both encoder-budget pressure
and a delivery shortfall; pacing time alone is not evidence of encoder overload.

## Validation boundaries

Build and test both enabled and disabled configurations. Enabled coverage
includes whole-frame transport, retained-frame encoding, live budget changes,
resizing, letterboxing and colour conversion. The disabled binary must not gain
a PyroWave shared-library dependency. Run the matching client parser, Vulkan
decode and presentation tests against the host-produced frame as well.

Live automation needs one designated host owner and isolated capture and
playback audio services without access to physical outputs. Separate ports and
a null capture sink on the desktop audio service do not isolate client playback
or prevent changes to desktop routing. Refuse a competing host rather than
stopping someone else's service.

A completed soak, upgrade checks and physical display, input and audio testing
remain separate acceptance gates. Decoded-frame counters do not establish
physical presentation or audio quality, and an interrupted soak is not a pass.
