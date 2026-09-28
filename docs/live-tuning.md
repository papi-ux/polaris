# Live Tuning

Live Tuning is the host's adaptive bitrate preference. It does not require an AI
provider or a subscription. Doctor explanations remain a separate feature.

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
300000 kbps, the most the endpoint takes, with `adaptive_bitrate_min` as its floor.
`max_bitrate`, when set, caps it at the encoder, the rate that setting's description
names. A launch applies `max_bitrate` to the client's request before FEC and audio come
off, so a launch at the cap encodes a little below a live change at the cap. The reply
reports the target the host set after the cap and floor, not an encoder
acknowledgement; the applied bitrate still comes from the encoder.

`adaptive_bitrate_max` used to cut a client's request down to it, 100 Mbps unless
changed. A PyroWave stream Nova asked to run at 180 Mbps or more dropped to 100 Mbps
half a second in when adaptive bitrate was on, and a live bitrate write clamped the
same way even with it off. It no longer limits anything. Polaris still reads it, so
an existing settings file loads unchanged, and logs a warning that names `max_bitrate`
as the cap to use instead. The status field `adaptive_max_bitrate_kbps` reports the
ceiling the controller holds, which a stream raises to its own request. Like
`adaptive_base_bitrate_kbps`, it keeps the last stream's value until the next stream
starts.

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
