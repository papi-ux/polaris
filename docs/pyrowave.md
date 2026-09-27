# PyroWave

PyroWave is an experimental Vulkan compute video codec. It decodes far faster
than H.264, HEVC or AV1 because every frame is coded on its own, and it wants a
great deal more bandwidth for the same picture for exactly the same reason. On a
wired local network that trade is often worth taking. Over anything slower it is
not.

It is host support for compatible Nova clients only. Ordinary Moonlight clients
cannot select it.

## Turning it on

**There is nothing to turn on in the Polaris console.** The host advertises the
codec and the client chooses it, so there is no PyroWave setting on this side and
nothing to change in your encoder selection. If you are looking for a switch and
not finding one, that is why.

The console also does not currently tell you whether your host can offer
PyroWave. Every Linux package from **v1.4.13** onwards contains the encoder, so
if you installed Polaris from the repository you have it.

### What you need

- **Polaris v1.4.13 or newer on Linux**, from the repository or a build with the
  submodules fetched. Windows and macOS hosts do not have it.
- **A Nova client with the decoder compiled in.** This is the part people get
  wrong; see below, because a beta version label is not enough.
- **A GPU on the host with the Vulkan features the encoder needs.** If it is
  missing, the host says `PyroWave: no GPU on this host has what the encoder
  needs` and the codec is simply not offered.
- **A fast wired link.** Start there rather than discovering the bitrate cost
  over Wi-Fi.

### Nova on Android

**Play Setup → Video codec → PyroWave (experimental)**.

**Auto will not choose it.** You have to select it by name. That is deliberate,
because it is experimental and expensive on bandwidth.

### Nova on Linux

The ordinary Linux Alpha Flatpak **does not contain the decoder**, whatever
version it is. It is built from the standard manifest. You need a bundle built
from `com.papi_ux.Nova.pyrowave.json`, which is published as its own asset.

In a build that has it: **Play Setup → Video Codec → PyroWave · Experimental**,
on the normal Desktop destination. The Linux device needs a compatible Vulkan
decoder of its own.

A beta release label alone is not sufficient, and neither is the version number.
Check which manifest your bundle came from.

### Moonlight

Not possible, and not a setting you are missing. Standard Moonlight clients do
not contain a PyroWave decoder, and no host option can add one. Moonlight keeps
using H.264, HEVC and AV1.

## What it costs

- **Bandwidth, substantially.** PyroWave is intra-only, so every frame is a key
  frame. Sharpness can need several times the bitrate an inter-frame codec would
  use for the same picture. Measure it on your own link rather than assuming a
  number: the host does not apply a PyroWave-specific quality floor and there is
  no single recommended bitrate.
- **Frame size is capped at 3 MiB.** Large frames below that cap can lose FEC
  parity protection under the transport's existing policy, so packet loss is
  less well protected exactly when frames are biggest.
- **GPU time on both ends.** It is a compute codec, so the win is in decode
  latency rather than in efficiency.

## Limits worth knowing before you test

- **Spaces cannot use it.** PyroWave is unavailable on that route.
- **Auto never selects it** on either client.
- **HDR needs the right capture input.** The host carries both an SDR 4:2:0 and
  an HDR 2020 PQ 4:2:0 profile, but see the capture format section below, because
  one common HDR desktop configuration cannot feed it at all.
- **Accepted frame rates and resolutions are not a performance promise.** A
  mode being negotiable does not mean a given GPU and network will sustain it.
- **A successful SDR stream is not HDR validation.** They are separate paths and
  separate proofs.

## Confirming it is actually running

Connect, then check the stream diagnostics: they name the codec, so **PyroWave**
appearing there is the confirmation. They also distinguish CPU capture from
Vulkan encoding, which matters because a host can run this codec while still
converting colour on the CPU, and that is much slower than the path is capable
of.

Two things the numbers do not mean. Encoder timing excludes the deliberate wait
for the next frame, so it is not a duty cycle. And the high-refresh adaptive
guard needs both encoder-budget pressure and a delivery shortfall, so pacing
time on its own is not evidence the encoder is overloaded.

A user's requested bitrate, the host's current target, the encoder's applied
budget and the measured received video bitrate are four different numbers. Do
not read one as another.

The encoder does accept bitrate changes live, without restarting the stream.

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

## Building it

`POLARIS_ENABLE_PYROWAVE` defaults to `ON`, so a normal Linux configuration
already builds the encoder and there is no flag to add. What it needs is the
pinned dependencies, which a plain clone does not fetch:

```sh
git submodule update --init --recursive third-party/pyrowave third-party/Granite
```

Pass `-DPOLARIS_ENABLE_PYROWAVE=OFF` to leave the encoder out. Do not assume a
package contains the encoder because its version is recent; verify the build
option for the package you are actually testing.

## Transport reference

The shared Android and Linux profile uses PyroWave revision
`186f0393b77f7755953b5ecde994bb1cec2e4155` (C API 0.6.0) and Granite revision
`b6cffd5ce81f540f0855e6778428483e14763d9b`. The RTSP offer contains
`a=rtpmap:99 PYROWAVE/90000` with the profile token in `a=fmtp:99`. The host
carries two tokens, `pyrowave-186f0393-sdr420-v1` and
`pyrowave-186f0393-hdr2020pq420-v1`. The client format is `0x10000`, the server
capability is `0x00800000`, and the selected `bitStreamFormat` is `3`.

Each GameStream frame carries one complete raw PyroWave bitstream, with the
coefficient packets concatenated in order, and each frame is independently
coded. The exact payload length excludes transport padding. Encoding retained
capture content still produces a new codec frame at the current bitrate budget;
it must not resend the previous codec sequence unchanged.

Keep client and host dependency pins and the profile token in agreement. The C
API version alone does not establish bitstream compatibility.

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

See the [Polaris update guide](updates.md) and the
[Nova update guide](https://papi-ux.com/docs/nova/updates/).
