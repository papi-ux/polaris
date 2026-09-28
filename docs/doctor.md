# Fix a bad stream with Doctor

When a game feels wrong, keep the stream open and let Doctor measure it. Doctor separates network,
host, and client evidence, tells you what is actually confirmed, and offers the safest next step it
can perform. You should not need to translate graphs before getting back to the game.

Open **Command Center** in Nova or **Mission Control** in the Polaris web UI while the affected
stream is still running.

## Read the verdict

Doctor reports one of four plain outcomes:

| Verdict | What it means |
|---|---|
| **Network** | Fresh media-loss or round-trip evidence points to the connection. Control-channel observations alone do not prove a media problem. |
| **Host** | Capture cadence, frame pacing, encode time, or the host render path is missing the stream target. |
| **Client** | Received, decoded, or rendered evidence points to the playback device when those measurements are available. |
| **No confirmed issue** | The available evidence does not support blaming one stage. Unavailable measurements stay unknown instead of becoming a guess. |

Doctor also names the kind of client streaming, in its `client_family` evidence row: `nova`, or
`moonlight` for a client that speaks only the Moonlight protocol, such as Moonlight or Artemis. The
host reads it from the device's pairing record when the device launches or resumes, as the Devices
page does, so it cannot tell Nova for Android from Nova for Linux, or Moonlight from Artemis. For a
Moonlight-protocol client the row says what it cannot use. Polaris gets no media loss from it, so
the **Network** verdict rests on round-trip time, and the **Client** verdict needs decode and render
timing that only Nova for Android measures. PyroWave, Live Tuning from the client and choosing the
launch mode per launch are Nova only, though Artemis can ask for Host Virtual Display; Live Tuning
on Mission Control still tunes the stream. Diagnostics carry `client_family` for the stream and for
each client, and Session Snapshot on **Doctor & Support** shows it as **Client type** ([what each
client gets](compatibility.md#clients)).

Static menus and repeated frames do not by themselves prove a pacing fault. Doctor collects six
complete video telemetry windows after startup and requires a warning threshold in two consecutive
windows before grading frame pacing. While that window is still filling, pacing evidence stays
**Unknown** instead of briefly blaming startup work. Confirmed warning evidence remains visible
instead of appearing beside a contradictory stable verdict.

SHM or system-memory capture is capability context, not a failure by itself. Doctor keeps the path
visible in Advanced evidence, but leaves the overall verdict healthy when delivered cadence,
capture latency, and encoder time remain inside the active stream's real FPS budget. It warns only
when those measurements show pressure; it does not lower a healthy 120 FPS stream merely because
its compatibility path is CPU-backed.

**Capture source size** compares a delivered frame with the size encoded for that stream. For
example, a 3840 × 2160 source feeding a 1920 × 1080 stream has four times as many pixels before
scaling. On a CPU capture path, Doctor notes sources with at least twice the stream's pixel count
and suggests checking the active output mode and the adapter's supported modes. Lowering the client
resolution alone may leave the capture source unchanged. This is context for investigation: it
does not prove a frame-rate bottleneck or authorize an automatic change.

Source sizes appear after a real frame reaches the encoder and belong to that stream generation;
probe images and late updates from retired connections do not supply them. Diagnostics include
`capture_source` for each client and for the primary client at the top level, or `null` when no
source has been observed. Doctor shows the size comparison while one client is active, so it does
not present several clients' capture paths as a single diagnosis.

Each client in the diagnostics also carries `capture` once the display it encodes from has opened
and that client's stream has published it. Until then `capture` is absent, and absent means unknown,
never a default. `preference` is the capture setting polaris.conf held when the stream started,
where empty is Autodetect. `requested` is the backend the stream asked for, read the way Polaris
reads the setting, so `drm` is `kms` and `kwin` is `portal`, and an alias alone is no override.
`opened` is the backend whose display initialization succeeded: `kms`, `wlr`, `portal`, `x11` or
`nvfbc`. A stream in Polaris' own labwc session asks for `wlr` and captures labwc by direct
screencopy, so it reports `wlr` as both `requested` and `opened`, with the `private_compositor`
reason below when polaris.conf named another backend. No live stream reports `cage`: the portal
backend would give its own screencopy of labwc that name, and a labwc stream never opens the portal
backend. `route` is the path inside it: for the portal, `portal_screencast`, `portal_gamescope_node`
or `portal_kwin_node`, and for every other backend the same value as `opened`. Both describe the
display as it came up. A portal capture that stops while frames are flowing opens the display again,
and `route` follows it. One that stopped between the display coming up and its first frame is
started again as capture begins and can take another route, and `route` keeps the one the display
came up on until the display is opened again.

`mode_override_reason` appears when one of the host's rules set polaris.conf's choice aside for this
stream, and is absent otherwise:

| Value | The rule |
|---|---|
| `private_compositor` | A private labwc session can only be captured through wlroots. |
| `substituted` | The configured backend captured nothing in this mode, and the host's capture evaluation substituted another. |
| `gamescope_session` | A launch into a Gamescope session is captured through the portal. |
| `dongle_session` | A dongle session captures the host desktop through the portal once the topology is swapped. |
| `desktop_discovery` | Mirror Desktop, including a host in Steam Game Mode, lets capture discovery choose instead of pinning wlroots. |
| `virtual_display_backend` | A Host Virtual Display, set in polaris.conf or by a launch, is captured the way its backend exposes it. |

`route_fallback_reason` appears when the portal took a ScreenCast instead of the local node it asked
for first, and is absent otherwise:

| Value | What happened |
|---|---|
| `gamescope_node_missing` | The gamescope node was asked for and gamescope exported none. |
| `gamescope_node_failed` | The gamescope node was there and capture on it did not start. |
| `kwin_node_unavailable` | KWin withheld its screencast protocol from Polaris, or opened no output stream. |
| `kwin_node_failed` | KWin opened the output stream and capture on its node did not start. |

A host whose compositor is not KWin has no KWin output to fall back from, so a ScreenCast there
carries no `route_fallback_reason`.

`transport`, `residency` and `format` come from the frames that client's encoder accepted from the
display `opened` names, and read `unknown` until that display delivers one. A display opened again,
such as a DMA-BUF capture that fell back to shared memory, starts over at `unknown` rather than
keeping the last display's answer, while `capture_source` still shows the last frame any display
delivered. None of these say that a combination is supported, or that frames stayed on the GPU.

With a single client streaming, the same answers repeat at the top level as
`capture_backend_preference`, `capture_backend_requested`, `capture_backend_opened`,
`capture_backend_route`, `capture_mode_override_reason` and `capture_route_fallback_reason`, the
last two only when the client carries them. With no client, or with two, they are absent, because
one client's answer is no answer for another. `stream_instance_id` names one stream for a report
about it. It is observational only and never accepted as input.

Each client also carries `encoder_backend`, the encoder its own stream's encode loop last sampled,
such as `nvenc`, `vaapi`, `vulkan`, `software` or `pyrowave`. It is absent until that stream's first
sample, and `last_session` keeps it when the stream ends. With a single client streaming it repeats
at the top level as `encoder_backend`, taken from that client. With no client, or with two, it is
absent there. The host also keeps its own record of the encoder, which is whichever encode loop
sampled last, Browser Stream's included, and no `encoder_backend` is taken from it. That record is
still served. `linux_gpu_profile.capture_forecast.encoder` names it whenever it holds one, from an
encode loop's first sample until the stats are reset when streaming stops, because the forecast asks
which encoder the next stream is likely to get. It appears in these diagnostics, under the Doctor's
`advanced_evidence`, and in the stream policy that the session status and the other paired
endpoints serve. The session status also answers from it a request that has no stream of its own,
as below.

A PyroWave stream's client also carries `pyrowave_route`, how that stream's own encoder took its
last frame with a picture in it: `zero_copy` when the encoder imported the captured DMA-BUF and
converted its colour on the GPU, with no CPU upload at its input, `gpu_upload` when the frame was
copied from host memory and converted on the GPU, and `cpu_convert` when its colour was converted on
the CPU, which is what `POLARIS_PYROWAVE_GPU_INPUT=off` asks for and what a host falls back to when
the GPU path cannot start. The route is what the encoder saw at its input: `zero_copy` says nothing
about how capture filled the buffer, so it is no proof that the whole capture stayed on the GPU. It
is absent until the stream encodes a captured frame, and for every other codec. It starts
over when the stream's display is opened again, and stays absent until the encoder built for the new
display takes a frame, so neither the reason nor `last_session` gives a display that delivered no
frame the route of the display before it. The encoder selection reason that
`/polaris/v1/session/status` serves for a PyroWave stream is built from it and opens with where the
colour is converted. A paired client is answered about its own stream, the one the response names as
`session_generation`, including while Watch Stream runs beside it, its reconnect overlaps the
stream it replaced, or a Browser Stream, which lists no client of its own, runs beside it. Whether it
gets a PyroWave reason at all follows that stream too, as do the `codec`, `active_backend`,
`effective_backend` and `fallback_allowed` beside it, so a PyroWave stream and an HEVC watcher are
each answered about their own whichever started first, and a reconnect is answered about its new
stream before that stream's first frame. Asked from the host, the reason gives the route every
stream that reported one shares, and says so when they differ.

The session health served beside it, and by the other paired endpoints, takes the same stream's
codec and encoder for `active_encoder` and `encoder_selection`, for the AV1 part of `decoder_risk`,
for `safe_codec` and for the CUDA-disabled NVENC finding. What follows from those follows the same
stream: `issues`, `recommendations`, `primary_issue`, `limiting_factor`, `auto_action`, `grade`,
`summary`, `recovery_profile` and `relaunch_recommended`, the `safe_bitrate_kbps` and
`safe_target_fps` a worse grade lowers, the `recovery_policy` built from them, and the Doctor built
from that health, its `encoder_selection` evidence and its `result_id` included. The session status
serves that `recovery_policy` as `auto_quality` too, and its `profile_state` reads the same answers:
its `state`, `label` and `reason` follow `auto_quality` and the grade, its `last_result` carries the
health's `grade` and `primary_issue`, and its `current_profile.preferred_codec` is the encoder
block's `codec`. Everything else those answers weigh is still the host's, whichever encoder wrote it
last: the encoder's `target_device`, `target_residency` and `target_format`, which decide the 10-bit
part of `decoder_risk`, the frame rate, bitrate, encode time and pacing, and the codec in the Doctor
v2 effective settings. A request with no stream of its own among the diagnostics is answered from
the host's records throughout.

The Doctor in the live stream stats carries the encoder selection too, which is where the web
console's Troubleshooting page reads the **Detected GPU driver**, **Encoder policy**, **Preferred
encoder**, **Fallback encoder**, **Fallback used** and **Selection reason** under **Advanced / raw
diagnostics**. With one stream running it is that stream's own, as its client is told it in the
session status. With more than one it is the selection they share, and it is absent when they
differ, because one stream's selection is no answer for another. With none running it is the
host's own. The host's own record of the encoder, which is whichever stream sampled last, never
decides which stream it describes.

When a stream ends, the diagnostics keep what it captured as `last_session`: `state` is `ended`,
with its `stream_instance_id`, `client_name`, and `started_at` and `ended_at` in UTC. `capture`
carries the fields above as they stood when the stream ended, and is absent when its display never
opened. `codec`, `encoder_backend` and `pyrowave_route` appear when that stream reported them.
`last_session` is the most recently ended session on this host, including a viewer that ends while
another client keeps streaming, so a report about one stream should match its `stream_instance_id`.
It stays after the last stream ends, until another session ends or Polaris restarts, and nothing
else changes it: not a restored capture setting, not an encoder probe, and not a late update from
the ended stream. It is the capture outcome only. Packet loss and latency are not kept, because
nothing yet records how fresh each measurement was when the stream ended.

On a Linux host configured for `gamescope_stream`, Doctor & Support also shows a **Gamescope Session
Helper** card. It names the `polaris-gamescope-session` launcher this Polaris will run, warns when a
stale copy under `~/.local/bin` or `/usr/local/bin` sits ahead of it on PATH, and fails when the
launcher or its runtime library was installed from a different checkout than the running build,
which is the state that makes already-fixed session bugs reappear. Reinstall the helpers from this
Polaris version, or install the distro package, and the card turns green.

Polaris runs the launcher beside its own binary before one on PATH, which keeps a stale copy from an
old install out of the way. A packaging that ships its own wrapper, one that exports the gamescope
build and tools the session should use, names it with `POLARIS_GAMESCOPE_SESSION` in the Polaris
service's environment, and that launcher runs whenever it is an absolute path to an executable file
with no spaces or quotes. The Nix module sets it to its wrapper, unless its `environment` option
names another. Any other name is logged as a warning when the session starts, and the usual
launcher runs.

## Check Spaces

On Linux, expand **Spaces checks** in **Doctor & Support** to read the host's container setup,
gaming runtime checks and reported Space activity. The section starts collapsed and checks only
when opened or when you select **Recheck**. It shows when that snapshot was read.

Missing or invalid evidence is shown as unavailable. Runtime driver mismatches, unsupported Space
launchers and failed runtime changes point back to **Spaces**, where you can inspect or change the
setup. Disabled Spaces remains informational for a host that uses normal desktop streaming.
A reported running session does not verify its video or input.

These checks read diagnostics without changing host setup or the stream's Doctor verdict and Auto
Fix action. Opening **Spaces** is the next step when a setup finding needs attention.

## When the stream never started

A client that negotiates a stream and then cannot build its decoder leaves during video setup, so
there is no stream left to measure. For fifteen minutes after such a start, while nothing streams,
Doctor's primary issue is `stream_failed_to_start`. It names the client and the codec it
negotiated, and the Fix My Stream checklist and the support report lead with it. When the codec was
PyroWave, the client could not start its PyroWave decoder: choose HEVC or H.264 for that device, or
update the client. For any other codec, the client's own error message names the cause.

The host log names that start in one line that begins
`Stream failed to start for [<client>]: the client left during video setup`. A client that stays
connected while its video never arrives, or leaves only after waiting several seconds for it, logs
`Initial Ping Timeout` instead, with the UDP port it waited on. That usually means a firewall or a
UDP path problem between the client and the host.

## Pick the offered action

Doctor uses a small action vocabulary so the button says what will happen:

- **Auto Fix** changes one reversible setting in the current stream: the live bitrate. Under
  confirmed loss or latency that is one guarded step down. On a clean network it is steps of at most
  a quarter back up to the bitrate the stream opened at, or, for a starved PyroWave stream, up to
  the codec's advice ([PyroWave reference](pyrowave-reference.md#how-polaris-advises-and-tunes-pyrowave)).
  Doctor verifies the encoder and the next evidence window after each step, then restores the
  previous live target if verification fails.
- **Recheck** gathers a fresh read-only measurement. It does not change the stream.
- **Manual** explains the next check when Polaris cannot safely act for you.
- **Undo** restores the previous live bitrate while the same stream generation still owns it.

A change that needs a new stream is not an Auto Fix. Fresh-launch experiments are a separate future
**Run a trial** workflow and are not enabled in this release.

## Follow the result

After Auto Fix, Doctor keeps the outcome visible:

| State | What to do |
|---|---|
| **Watching** | Keep playing for a few seconds while Doctor measures the changed stream. |
| **Verified** | The target metric improved without a guardrail regression. Keep playing. |
| **Restored** | Verification failed and the previous live target was restored. Review the next guidance. |
| **Needs attention** | Polaris stopped rather than stacking another guess. Reconnect if encoder restoration could not be confirmed, then follow the manual guidance. |

Auto Fix is scoped to the active owner, app, and stream generation. A reconnect, second controller,
or newer manual bitrate choice retires the old receipt instead of letting it mutate a different
stream.

## Launch preset is separate

**Auto**, **Quality**, **High FPS**, and **Stability** are deterministic launch presets. They resolve
the next launch from your explicit choice, paired-client settings, and current host capabilities.
Doctor history and AI output do not silently rewrite them, and Doctor never changes Private Stream,
Host Virtual Display, or Mirror Desktop topology.

## Optional AI explanation

AI is optional. When configured, it can turn Doctor's structured evidence into a shorter plain-
language explanation. The AI response is informational: it cannot define the action, target
bitrate, confidence used by Doctor, or next-launch policy. Deterministic Polaris code remains the
only authority for the button and its settings.

If the verdict still does not match what you see, copy the diagnostics from Command Center or export
a redacted support bundle from Polaris before ending the stream. That preserves the measurements
needed to investigate the real bottleneck.
