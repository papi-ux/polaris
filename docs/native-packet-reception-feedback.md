# Native packet reception feedback v1

Polaris #615 and Nova #442 use a private, explicitly negotiated client-to-host control
extension, `0x7F10`. This is a matched allocation, not an upstream registry reservation or
feature bit. It observes original RTP packets accepted by the native queue. It does not
provide a packet census, loss denominator, clean/recovery signal or capacity estimate.
No bitrate, FEC, Doctor, action, permission or policy writer consumes it.

The host offers `a=x-ss-video.packetReceptionFeedbackVersion:1` in this launch's DESCRIBE.
The client echoes `a=x-ml-video.packetReceptionFeedbackVersion:1` in ANNOUNCE. Exactly one
attribute and the single token `1` are accepted after surrounding ASCII whitespace trim;
all duplicate attributes, including identical duplicates, junk and extra tokens disable
only this extension. No offer/echo, unknown versions or malformed peers change streaming.
The host remembers its actual offer and freezes the negotiated result for this stream.

The Sunshine protocol, unique `ML_FF_SESSION_ID_V1` control connect-data mapping,
`controlProtocolType == 13` and `SS_ENC_CONTROL_V2` remain mandatory. Existing dispatch
rejects plaintext control and verifies GCM before invoking the handler. Both already
configured video modes are supported: no encryption setting or policy changes. Status
`media_authenticated` comes only from the host's actual video encryption flags;
`control_authenticated` becomes true only after a structurally accepted report arrives
through that verified encrypted control dispatch. `coverage` is `queue_accepted_original`.
Unencrypted media can be injected or modified on path; encrypted control reporting does
not turn it into authenticated media or independently prove the client's observations.

## Wire layout

All payload integers are big-endian, decoded bytewise. Exact payload length is
`52 + 10 * sample_count`, at most 212 bytes. The inner control header stays in the existing
control protocol's encoding, inside encrypted v2 ENet generic-channel framing. Reports
are unreliable/unsequenced, at most once per existing 100ms native control tick, without
retry, backlog, catch-up burst, probing or extra media.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | u8 | version, exactly 1 |
| 1 | u8 | flags: 1 HAS_ACCEPTED, 2 INCOMPLETE, 4 CLOCK_UNAVAILABLE, 8 TIMING_SAMPLE_LIMIT |
| 2 | u8 | sample count, 0..16 |
| 3 | u8 | reserved, zero |
| 4 | u32 | report sequence, starts at 1, advances for every scheduled send including failure, never wraps |
| 8 | u32 | actual interval microseconds |
| 12 | u64 | client connection-relative interval end, never wall time |
| 20 | u32 | completed receive-call progress, including timeout returns |
| 24 | u32 | accepted original data packets |
| 28 | u32 | accepted original parity packets |
| 32 | u32 | data UDP payload bytes |
| 36 | u32 | parity UDP payload bytes |
| 40 | u32 | age since last accepted commit; UINT32_MAX if unavailable/saturated |
| 44 | u32 | first accepted frame, zero without HAS_ACCEPTED |
| 48 | u32 | last accepted frame, zero without HAS_ACCEPTED |

Each sample is frame u32, RTP sequence u16, socket-read age u32. Counts and frame bounds
cover observer commits during the interval. Samples preserve the earlier userspace
socket-read time; an age greater than the interval is valid delayed verification or
acceptance, not a later fabricated arrival. Samples older than two seconds are omitted
and make the report INCOMPLETE. No reconstructed, duplicate or rejected packet is an
original arrival. Encrypted video still requires the native queue's existing successful
GCM path; this extension never bypasses it. Plain video retains ordinary queue admission.

Complete intervals have `0 < interval_us <= 2,000,000`. A long interval is accepted only
with INCOMPLETE and zero samples: actual positive duration, or UINT32_MAX on saturation,
and an actual advancing client end. It clears derived metrics and can later resume with
a normal complete interval. Clock failure has a separate zero-time sentinel:
INCOMPLETE|CLOCK_UNAVAILABLE, no samples, interval/end zero, last-accepted age UINT32_MAX.
It disables observation until a fresh native connection. Zero-duration snapshots are not
sent and consume no sequence. A report with zero accepted packets retains progress and
last-accepted age but clears prior delivery metrics. TIMING_SAMPLE_LIMIT alone means
sparse timing coverage; retained valid samples and full-interval counters can still join.

## Host correlation and readback

The host records before/after `steady_clock` bounds around actual batch/single send APIs.
Successful batch shards share bounds. API acceptance is not on-wire transmission. Exact
`(frame32, rtp16)` keys live in a per-generation, fixed 16,384 by 32-byte packet ledger
(512KiB), with bounded metadata and two-second expiry. Key reuse is ambiguous; collisions
can evict retention, never reconstruct missing sends. A scan after eviction is only a
retained subset, not an upper bound on sends. Two generation-local, per-class evicted-frame
high-water marks conservatively distinguish complete data/parity ranges. Counters above a
proved complete class total, or matched sample sums above claimed acceptance, are refused
without refreshing state. Excess above a retention-limited subset instead publishes a fresh
unavailable `retention_limited` observation and clears derived metrics. The constant-size
marks can conservatively limit older overlapping ranges; they do not guess missing sends.

The existing 4,096-frame ledger records the immutable encoder tuple before sending.
Only the new const inclusive-range access is added; its historical exclusive join and
consumers are unchanged. Every frame in first..last must be retained, fresh, successful
and exactly tuple-equal, including encoder lifetime, stream generation, application
sequence, authority revision, configured kbps and receipt origin. Same kbps with a new
application is mixed. Failed/partial/fallback/cancelled sends are unavailable, including
macOS's existing batch fallback; no platform send behavior is repaired here.

Optional `native_reception` appears only in the paired current controller owner's session
status, with matching active timing scope and app session, never through a global latest
fallback. Waiting, zero acceptance, no samples, incomplete, stale, missing and ambiguous
states remain explicit. Report gaps and receive-loop stalls are separate facts. Rejected
reports never refresh them; reconnect/stop retires the old instance terminally. Snapshots
recheck sender and report freshness and retain raw aged facts without stale derived
values. There is no last-good reuse. This object is separate from frame receipts and
legacy Java/media counters; no atomic cross-layer join is claimed.

Matched samples may expose an API-to-socket-read dispersion interval. For host API bounds
[bA,eA],[bB,eB] and receiver time difference dR, it is
`[dR-(eB-bA), dR-(bB-eA)]`. No absolute host/client clocks are subtracted. Reordered pairs
carry no derived dispersion. Bounds include batching, kernel queues, receiver scheduling
and clock-rate skew; they are not absolute one-way delay, media RTT or isolated network
queue delay. Offered/accepted traffic under app-limited production does not establish
spare capacity. Silence never proves clean delivery or identifies a pause cause.

The bounded work per admitted report is at most 16,384 packet slots, 4,096 frame slots and
16 samples. No per-packet allocation or unbounded report queue is added. Encrypted control
framing is at most 240 bytes/report (2,400 B/s at 10Hz) before ENet/UDP/IP/link overhead;
whole-link cost, lock/scheduling cost and timing discrimination require later measurement.
Pure tests exercise real parser, ledgers, negotiation and JSON plus the peer production
C serializer's four frozen golden encodings. Production RTSP/control/status and actual
stream behavior require full required CI and matched runtime acceptance.

## Private host trace rows (schema 2.4)

Only while the existing opt-in `POLARIS_VRR_TRACE_V2` recorder is running does the `0x7F10`
callback also record raw admission rows. The trace minor becomes 4 because they are a new row
family. Readers that accept exactly 2.3, including
`scripts/validation/live-tuning/inspect_frame_records.py`, keep refusing 2.4 unchanged, and
none of their qualifiers widen. With the recorder off, the callback performs the same `accept()`
after a single enabled check, with no copy or join. Status, JSON, contract and wire are
unchanged, and nothing in Live Tuning, FEC, Doctor or the policy reads these rows.

Every callback writes one fixed 34-field `native_admission` row:
- the opaque trace scope, session generation, native scope generation and callback ids;
- the host's own admission result (or `not_dispatched` when the existing guard refused), which
  is never a statement about wire loss;
- the host steady time `accept()` judged it at;
- the ENet peer's smoothed `roundTripTime`, read in that same callback.

Report facts are attributed (`report_attribution=admitted_payload`) only to a newly admitted
report whose nonzero scope generation equals the callback's session generation. An admitted
snapshot from any other or unknown generation is marked `generation_mismatch` and carries no
report, tuple or samples; a refusal is `none`. For an attributed report the row also carries its sequence, `end_us`, interval, flags,
receive-loop tick and state, gap count, accepted counters, frame range, sample count,
availability and frame reason. The full immutable encoder tuple appears only when that report's
availability joined. Each refused, duplicate, malformed or contradicting payload copies nothing
from an earlier report, because `accept()` fills `admission_trace_t` under its own lock only
when this payload's sequence and end were just published.

At most 16 fixed 12-field `native_admission_sample` rows follow, only for that attributed joined report.
Each carries:
- the sample's frame, RTP sequence and client socket-read age (client connection microseconds);
- the retained sender API begin/end bounds the join used (host steady nanoseconds);
- packet kind and UDP payload bytes.

The two clock domains are never subtracted in the trace. The facts include sender kernel and
receiver scheduling, so they are not queue delay, one-way delay, capacity or loss. Rows never
contain an address, session token, client identity or name, certificate, key or packet buffer.

The recorder's 40-field, 512-slot queue, 2,000,000-event, 256 MiB and 30-minute limits are
unchanged. These rows add at most 17 per callback, and loss is counted in the existing footer.
