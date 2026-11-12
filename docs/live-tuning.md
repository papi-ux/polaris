# Live Tuning

Live Tuning is the host's adaptive bitrate preference. It does not require an AI
provider or a subscription. Doctor explanations remain a separate feature.

Live Tuning is on by default from 1.5.0. Older releases wrote the old Off default
into `polaris.conf` on every settings save, so the first start of 1.5.0 turns a
saved Off on once and records `live_tuning_on_by_default` in
`settings_migrations.json` beside the settings file. This includes a host that
chose Off deliberately before 1.5.0. Turn it off after the upgrade and it stays
off. An already enabled setting stays on. An unreadable migration record changes
nothing, and a settings file the store refuses is tried again at the next start.
Live Tuning still adjusts only encoders that accept a bitrate change mid-stream.

With Live Tuning on, the default, adaptive parity is active for a Nova stream on
a Linux host using NVENC when Nova reports partly lost frames. It can adjust FEC
during the stream while lowering the encoder rate to keep video plus parity
within the client's request; other streams keep configured FEC.

The switch in Quick Controls and Audio/Video settings saves immediately. Nova's
Command Center uses the same preference. Its HUD shows tuning separately from
stream health. A saved On preference can be waiting for a stream, measuring,
adjusting, applying a bitrate, stable, or unavailable for the current encoder.
Disconnected or invalid status is Unknown; it is not permission to change settings.

The displayed applied bitrate comes from the encoder acknowledgement, not the
requested target. Turning tuning off holds the last confirmed bitrate. Choosing
an explicit fixed live bitrate turns tuning off for the rest of that stream and
supersedes a pending Doctor bitrate action. Nothing is saved: tuning reads as off
until the stream ends, the next stream starts with the saved preference, and
switching tuning on during the stream resumes it. Auto, Quality, High FPS, and Stability launch presets still
apply at the next explicit launch.

While tuning is on and the encoder can change its rate live, network pressure is
tuning's to act on, sustained video frame loss and round trip time alike. Doctor does
not take a bitrate step of its own then: it rechecks and says what tuning found
([Doctor](doctor.md#pick-the-offered-action)).

## How it decides

Live Tuning reads two things: each client media report's frames, expected and lost after FEC
recovery, and the control channel's round trip time, about ten readings a second. It decides on
reports, not on the clock, so a pause in reports pauses it, and a step it was trying when the
reports stopped is taken back without counting against it. So is a return to the request that no
report has judged yet: if reports stop for 2.5 seconds right after it, perhaps because the return
overloaded the link, the stream goes back to the rate it left and tries the request again once
reports are back. A probe that had only begun when reports stopped, perhaps because the link is
overloaded, takes one deep sample and goes no further.

- **A change in loss.** It learns the stream's usual loss and how bursty that loss is, and starts a
  probe only when loss rises clearly above it and two of the last three reports lose 3% of their
  frames or more. Loss under 3% never starts a probe, and one burst never cuts.
- **A link limit, or not.** A probe steps to 60% of the rate. If the loss goes, it checks the rate
  it left, since the loss may simply have ended, and either climbs straight back or narrows in on
  the highest clean rate. If the loss stays, it takes one sample at an eighth of the rate, never
  below twice the floor. Clean there means a link limit somewhere between. The same loss there means
  the loss does not depend on the bitrate: the stream goes back to its rate, and Live Tuning learns
  that loss and leaves it alone, with the reason `loss_independent_of_bitrate`. Only loss that is
  still clearly more than background costs even there, over about a tenth of the frames, probes on
  toward the floor. At the floor, loss over a fifth of the frames that is also clearly over the
  tenth background can cost is the link's: the stream holds at the floor and climbs back only in
  judged steps of at most half again, never straight back to its rate, which would only load a link
  that cannot carry the floor.
- **Under a link.** Settled under a link it found, it tries the rate above after 8 reports and then
  about every 20, and climbs back toward the request when that is clean, with the reason
  `holding_under_the_link`. When the link itself falls, so that a rate it held clean starts losing,
  Live Tuning stops trusting the rates the old link carried. It climbs back from where the new link
  holds it, a judged step at a time, and never jumps straight back to the rate it held before.
- **Queues.** A round trip time that rises and stays, 10 ms or more over the minute's lowest reading
  and not falling for 8 seconds, is a queue at the link: Live Tuning cuts 15% at a time until it
  drains and holds under it, with the reason `holding_under_a_queue`. A short rise is not a queue. A
  Steam Deck's Wi-Fi background scan lifts the reading to about 83 ms for 5 seconds every five
  minutes and then lets it fall, and 1.4.13 cut the stream for each one. A raise that starts a queue
  fails once the reading rises twice in a row to that far over its lowest.

Every verdict is a sequential likelihood-ratio test on frame counts, scaled by how bursty the
stream's loss is, so a noisy second does not decide. Live Tuning reads only ENet's smoothed round
trip time: ENet keeps no sample behind it, and the host cannot tell how many fed each change.
The policy and its replays are `src/live_tuning_policy.h` and
`tests/unit/test_live_tuning_policy.cpp`; the reference model they follow is
`tools/tests/live_tuning_policy_reference.py`. `tests/unit/test_live_tuning_acceptance.cpp` holds
the policy to a 26-cell acceptance bench. It passes 22. The 4 it misses are a link that shrinks
just as the client's reports pause, which needs a congestion signal that keeps arriving while
reports are missing.

## Experimental native step-down trial

`adaptive_bitrate_native_downward` is a separate host option, off by default. To
opt in, set this in `polaris.conf` before starting Polaris:

```ini
adaptive_bitrate_native_downward = enabled
```

Use `disabled` to leave the trial off. Settings does not expose this option and
preserves its file value when other settings are saved; the configuration API
refuses changes to it. Turning ordinary Live Tuning on does not enable the trial.

With Live Tuning on, matched Nova native packet reports and current stream,
writer and encoder checks can permit one floor-clamped 15% downward request after
a four-second reference and eight standing seconds. The lower target is then held:
those reports cannot start another cut or automatically raise it. A live bitrate
writer with a different target, a changed request or floor, or a new stream is
needed to resume ordinary tuning. The reports do not establish loss, a clean link
or capacity. See [native packet feedback](native-packet-reception-feedback.md#experimental-downward-only-trial-default-off)
for the eligibility checks and hold behavior.

One downward response was observed in a scoped RP6 test; full device qualification
and any delivery-quality benefit remain unconfirmed.

## Range

Live Tuning lowers the bitrate on measured loss, latency or encoder load, and recovers
it toward the client's own request. That request is the bitrate the stream opened at,
which is what the client asked for less FEC and audio overhead, or a later live
change. The floor is `adaptive_bitrate_min`. There is no ceiling of its own: the
client's request is the ceiling, and `max_bitrate` caps what a client may request.
Doctor's bitrate step and its Undo start from the bitrate the encoder runs at, so one
step cuts at most 20% of that rate and Undo puts that rate back.

A PyroWave stream has a floor of its own: half what the codec's model advises for it on a
device's own screen, at the encoder, and never above the client's request. At that floor
Live Tuning stops cutting, and Doctor suggests HEVC or a lower mode rather than another
cut. Live Tuning does not cut PyroWave for a slow encode, which takes as long at any
bitrate. Live Tuning raises a stream above the request only to lift one below
`adaptive_bitrate_min` to that floor. Doctor's raise for a starved PyroWave stream is the other
change that can, as one tap with Undo
([PyroWave reference](pyrowave-reference.md#how-polaris-advises-and-tunes-pyrowave)).

The floor is the one place the bitrate can sit above the request. A client that asks
for less than `adaptive_bitrate_min` starts the controller at the floor. With Live
Tuning on, the stream rises to the floor at the encoder's first bitrate check. With it
off, the stream stays at the request, but Doctor works from the floor rather than from
the rate the encoder runs at.

A live bitrate from a paired client, such as Nova's Deck HUD, applies as asked up to
500000 kbps, the most Polaris's own endpoints take from a client, with `adaptive_bitrate_min` as
its floor. Capabilities announce that limit as `manual_bitrate_max_kbps`, and a host without it
takes 300000. The RTSP handshake a Moonlight client uses is not one of those endpoints, and
`max_bitrate` alone bounds it.
`max_bitrate`, when set, caps it at the encoder, the rate that setting's description
names. A launch applies `max_bitrate` to the client's request before FEC and audio come
off, so a launch at the cap encodes a little below a live change at the cap. The same holds
at the 500000 kbps limit: a launch there runs the encoder at about 449000 kbps at the default
10% FEC with stereo audio, and a live change there at 500000. The reply
reports the target the host set after the cap and floor, not an encoder
acknowledgement; the applied bitrate still comes from the encoder. Nova's 1.5.0 beta converts
a request-labelled control through the host's known FEC/audio split before it posts a live change.
Its 500 Mbps request therefore remains about 449 Mbps of video in this example, rather than a
500 Mbps raw encoder write. If the split is absent, video units remain video units. Automatic
recommendations still stop at 300 Mbps; this manual endpoint limit does not raise that ceiling.

`adaptive_bitrate_max` used to cut a client's request down to it, 100 Mbps unless
changed. A PyroWave stream Nova asked to run at 180 Mbps or more dropped to 100 Mbps
half a second in when adaptive bitrate was on, and a live bitrate write clamped the
same way even with it off. It no longer limits anything. Polaris still reads it, so
an existing settings file loads unchanged, and logs a warning that names `max_bitrate`
as the cap to use instead. The status field `adaptive_max_bitrate_kbps` reports the
ceiling the controller holds, which a stream raises to its own request. Like
`adaptive_base_bitrate_kbps`, it keeps the last stream's value until the next stream
starts.

## Bitrate units

A client's bitrate request covers the video, the FEC that protects it, the audio and the
packet overhead. The handshake takes FEC off first, then the audio (never more than a fifth
of what is left), then 500 kbps of overhead (never more than a tenth), and hands the encoder
the rest. While a stream runs, `GET /polaris/v1/session/status` says how that went for it, on
every codec, in `bitrate_units`. Capabilities announce it as `bitrate_units_v1`.

| Field | What it is |
|---|---|
| `requested_kbps` | What the client asked for: the total it configured, or its maximum when it configured none. 0 when it sent neither. |
| `warp_factor` | How many times the host multiplied the request because `limit_framerate` renders faster than the client streams. 1 when it did not. |
| `cap_kbps` | The cap that cut the request, or null when none did. PyroWave meets only `max_bitrate`; every other codec meets the bitrate its launch resolved from the Stability preset, a device profile or a saved paired profile, or `max_bitrate` when it resolved none. |
| `cap_source` | Where that cap came from, such as `max_bitrate` or `stability_preset_selected`. Null when `cap_kbps` is. |
| `split_kbps` | The total the handshake split: `requested_kbps` times `warp_factor`, cut to `cap_kbps`. Null when the encoder rate is no split of this stream's own request: a watcher, which encodes at its owner's rate, or a client that sent no bitrate. |
| `encoder_kbps` | Where the handshake left the encoder. |
| `live_encoder_kbps` | The rate the encoder runs at now, after a live bitrate, Live Tuning or Doctor. |
| `audio_kbps` | What the stream's audio costs: 256 kbps a channel in high quality, 96 otherwise. |
| `fec_percentage` | The FEC share the stream was split with, the host's when it started. Above 80 no FEC comes off. |
| `formula` | `stream_bitrate_v1`, the arithmetic above. |

Whenever `split_kbps` is a number, `encoder_kbps` is exactly what `stream_bitrate_v1` makes of
it with that audio and FEC, so a client can work out what to ask for to land the encoder on a
given rate. When `split_kbps` is null the formula says nothing about the stream. `version` is 1.

A client with a stream here is answered about that stream alone. While it is still in its
handshake, or in the moment its teardown takes, the object is left out rather than filled with
another stream's figures. A client with no stream here is answered about the first stream that
has recorded its handshake. A stream in a Space reports through its worker and carries no
`bitrate_units` yet, and capabilities answered for a Space's profile do not announce it.

The web console's stream stats, `GET /api/stats/stream` and its event stream, show the same object
for each streaming client as `clients[].handshake_bitrate_units.units`, beside that row's
`stream_instance_id`. Each row answers from its own handshake only. While the row is still in its
handshake, `available` is false, `reason` is `handshake_not_recorded` and `units` is null. In a
snapshot where nothing streams, `reason` is `not_streaming`. A row is never given another stream's
figures, and a watcher's null `split_kbps` stays null. `basis` is `rtsp_handshake_record`,
`snapshot` is `as_of_diagnostic`, and `action_authority` is always false. `requested_kbps`,
`warp_factor`, `cap_kbps`, `cap_source`, `split_kbps`, `encoder_kbps`, `audio_kbps`,
`fec_percentage` and `formula` are the mapping the handshake negotiated and recorded, and a later
live change does not rewrite them. `live_encoder_kbps` is different: it is the rate the row's own
encode loop last reported when the stats were read, or `encoder_kbps` until the first report, so it
can change after a live bitrate, Live Tuning or Doctor change. Neither acknowledges a particular
bitrate write or shows what media was delivered. There is no top-level copy, even with one client,
and a row with no `stream_instance_id` carries none. The object is observational only and never
accepted as input. The paired session status and capabilities are unchanged.

## API contract

`GET /api/live-tuning` requires web administrator authentication. Its response
contains `status` and `live_tuning`. `POST /api/live-tuning` accepts exactly one
boolean field, `enabled`, and requires the quoted `configuration_revision` in
`If-Match`. Cookie-authenticated mutations require the normal CSRF token. A valid
configured bearer credential or an authorized verified client certificate bypasses
cookie CSRF validation; the endpoint still enforces its authentication checks.

A stale or absent revision returns HTTP 412 without changing the controller.
A failed durable commit returns an error without changing the preference. Refresh
the state before asking the user to retry; do not automatically replay the save.
A settings file the settings store refuses returns HTTP 503 with `code`
`config_unreadable` and changes nothing. On `POST /api/live-tuning` the reply also
carries `error`, `path`, `reason` and `fix`, the fields `GET /api/config` answers
such a file with; the paired route carries the code alone.
`GET /api/config` returns contents and revision from one secure file snapshot.
Configuration saves preserve the live preference when it is omitted.

The existing paired `/polaris/v1/session/adaptive-bitrate` route accepts
`configuration_revision` alongside the existing application session ID and
generation. Its sole-owner, permission, revocation and session-generation checks
remain mandatory. Legacy paired callers may omit the revision; new Nova sends it.

The additive `live_tuning` version 1 object is shared by web statistics, paired
session status and session events. It contains:

- `enabled`, `scope` (`host`), `state`, `supported`, and `reason`;
- `runtime_enabled`, `pending`, `quality_limit_kbps`, `requested_bitrate_kbps`,
  and `applied_bitrate_kbps`;
- `app_session_id`, `session_generation`, `configuration_revision`,
  `host_instance`, and increasing `sequence`.

A shared encoder or a mismatched session cannot claim a supported live actuator
or another session's applied bitrate. Clients reject malformed values, old
sequences and retired host instances. The shared conformance examples are in
`tests/fixtures/live-tuning-v1.json`, also consumed by Nova's unit tests.

Session status advertises `events_https_port`. Nova uses that HTTPS endpoint with
its existing pinned authenticated client, disables redirects, cancels blocked
reads on teardown, and requests a fresh status after connection loss or an event
sequence gap. Events are snapshots, not a replay log. Old connection callbacks
cannot update a replacement session. Missing or invalid canonical tuning becomes
Unknown until a successful resynchronization.

## Validation boundary

Unit and isolated transport tests cover conditional saves, failed persistence,
configuration contention, encoder acknowledgement races, recreation followed by
disable, owner/session attribution, shared fixtures, and stale UI delivery.
Physical game streaming and device-specific encoder behavior remain separate
acceptance checks. This change does not enable container multiseat production.
