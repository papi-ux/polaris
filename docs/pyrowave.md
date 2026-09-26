# Experimental PyroWave host support

PyroWave is an optional Vulkan compute encoder for compatible Nova clients on a
fast local network. It uses GameStream transport. Ordinary Moonlight clients
cannot select this codec.

## Test the beta

Polaris **v1.4.13-beta.3** includes the host encoder. There is no PyroWave toggle
in the Polaris console: the compatible client selects the codec for its stream.
Leave the host's normal encoder selection unchanged.

- **Nova Android beta:** select **Play Setup → Video codec → PyroWave
  (experimental)**. Auto does not select it.
- **Nova Linux:** use a build made with the separate
  `com.papi_ux.Nova.pyrowave.json` Flatpak manifest. The ordinary Linux Alpha
  bundle published with **Nova v1.4.13-beta.3** uses the standard manifest and
  does **not** include PyroWave. A beta release label alone is not sufficient.
  In an enabled build, select **Play Setup → Video Codec → PyroWave ·
  Experimental** on the normal Desktop destination. The Linux device needs a
  compatible Vulkan decoder; PyroWave is unavailable in Spaces.
- **Moonlight:** standard clients do not contain the PyroWave decoder. A
  host-side setting cannot add it; use a compatible Nova build to test this
  codec. Moonlight continues to use its supported H.264, HEVC and AV1 paths.

Use a fast wired local link for the initial test. Confirm that the stream
diagnostics report **PyroWave** after connecting. Nova Linux's shared profile is
SDR 4:2:0; do not treat a successful SDR stream as HDR validation. See the
[Polaris beta update guide](updates.md) and
[Nova beta update guide](https://papi-ux.com/docs/nova/updates/).

## Build and selection

`POLARIS_ENABLE_PYROWAVE` defaults to `ON`, so a normal Linux configuration already
builds the encoder and there is no flag to add. What it needs is the pinned
dependencies, which a plain clone does not fetch:

```sh
git submodule update --init --recursive third-party/pyrowave third-party/Granite
```

Pass `-DPOLARIS_ENABLE_PYROWAVE=OFF` to leave the encoder out.

An enabled host advertises the codec to compatible clients. Nova must explicitly
select PyroWave; its Auto choice does not select it. Standard release packages
must not be assumed to include the encoder merely because their version is a
beta. Verify the build option for the package being tested.

The shared Linux-client profile uses SDR 8-bit 4:2:0. The current host also has
GPU capture conversion and additional colour paths for compatible clients;
those do not add HDR or 4:4:4 selection to the Linux client. The host's HDR path
requires supported capture input; the beta.3 release notes describe the portal
path and its KMS limitation. Spaces remain outside this route. Accepted frame
rates and dimensions do not guarantee sustained performance on a particular
GPU or network.

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

## Capture formats the codec cannot read

PyroWave reads eight bit BGRA and RGBA and the four ten bit packed formats,
`XBGR2101010`, `ABGR2101010`, `XRGB2101010` and `ARGB2101010`. It cannot read the
sixteen bit float formats.

That matters on one configuration in particular. **KWin composites HDR as
`ABGR16161616F`**, so on a KDE host with `capture = kms` and the display in HDR
mode, the scanout PyroWave is handed is sixteen bit float and it has to refuse.
The stream ends with `capture is handing over a dmabuf in a format this codec
cannot read`, and the refusal is correct: it does not change while the display
keeps its mode.

Today that refusal arrives after the client has already negotiated the codec and
built a decoder, so the symptom is a session that starts and moves zero video
bytes rather than a codec that is never offered. Turning HDR off on the host
display, or capturing by another route, is what makes PyroWave available there.
A host scanning out ten bit packed HDR is unaffected.

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
