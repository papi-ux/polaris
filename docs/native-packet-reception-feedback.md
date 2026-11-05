# Native packet reception feedback v1

Polaris #615 and Nova #442 use a private, explicitly negotiated client-to-host control
extension, `0x7F10`. This is a matched allocation, not an upstream registry reservation or
feature bit. It observes original RTP packets accepted by the native queue. It does not
provide a packet census, loss denominator, clean/recovery signal or capacity estimate.
By default no bitrate, FEC, Doctor, action, permission or policy writer consumes it.

## Experimental downward-only trial (default off)

`adaptive_bitrate_native_downward=false` is a separate config-file-only host setting. Settings
omits this option and preserves its existing file value when other settings are saved; the config
API rejects attempts to change it. Live Tuning On alone preserves the existing controller.
Explicitly enabling this experimental option permits one 15% downward
request after a frozen, rate-local four-second native reference and eight standing seconds. Data
sample elapsed bounds stay separate from ENet RTT; they are not one-way delay, loss or capacity.
The current exclusive stream, caller, nonce, writers, revisions, actual encoder receipt and accepted
frame tuple are rechecked before the existing encoder request path can act. Unsupported or busy
contexts refuse the trial. No FEC percentage changes or upward authority are added.

The policy owns the lower target in a distinct native hold and freezes legacy loss/RTT judgments.
Missing reports, flat ENet, ineffective cuts and sampled native relief cannot restore the prior rate
or start another cut. Actual backend application must be proved within six seconds of dispatch;
response observation has its own six-second bound. An explicit writer with a different target or
request/floor, or a new stream, is needed to resume adaptation. A same-value writer retains the
conservative hold. This option is a source candidate, not automatic-device or physical qualification.

The response window groups data-sample elapsed bounds by absolute steady second, as the reference
does. A sampled response needs four consecutive buckets, each with at least four reports spanning
500 ms, whose median upper bound is below the trigger's lower bound, all within six seconds of the
first report from the backend-applied tuple. That first report can arrive anywhere inside a second, so
the response's first bucket covers only the rest of it. If that one bucket is short it is discarded
with no credit; a first bucket that already meets the coverage rule counts as before. Every report
still passes the gap, order, data-sample, tuple and deadline checks. Any later short or skipped bucket,
a report gap, disorder, overflow or unavailable data still ends the trial unassessed, a changed tuple
or the six-second deadline ends it ineffective, and nothing restores the rate or makes another cut.

Each episode transition, including refusals that did not previously leave a history entry, adds a
`native_diagnostic_transition` entry whose cause names its static reason; each closed or discarded
response bucket adds a `native_diagnostic_bucket` entry naming its disposition. They are diagnostic
history entries in the existing open kind/cause fields, not native actions or response credit, and add
no serialized field.

### Reference-phase interruption diagnostic (host log only)

Before a cut, the episode's reference and standing window can be discarded without any phase change,
so without any history entry. That happens on a gap, disorder, unavailable data, a short, skipped or
overflowing bucket, a changed tuple, action revision or floor, the 60-second reference age, a refused
or unconsumed report, or a writer or scope that replaces the episode. The host counts these clears and
what each report did at the entry point. It writes bounded lines to the existing host log at info
level. It adds no history entry, trace row, wire or API field, or schema:

```
Live Tuning native diagnostic <summary|interruption|scope_end>: generation=<n> sequence=<n>
  native_downward=<on|off> entered=<n> busy_skips=<n> consumed=<n> pending_waits=<n> requests=<n>
  policy_refused=<n> refused=<n> first_refusal=<name> last_refusal=<name> refusals=[<name>:<n>,...]
  retired=<n> retired_owner=<n> retired_window=<n> reference_clears=<n> first_clear=<name>
  last_clear=<name> clear_counts=[<name>:<n>,...] last_lost_closed=<n> last_lost_reference=<n>
  last_lost_standing=<n> max_lost_standing=<n> summaries=<n>/64 interruptions=<n>/16 suppressed=<n>
```

(One line in the log; wrapped here.)

- **Refusals** are named in the entry point's own evaluation order:
  - `config_disabled`;
  - the permission groups `scope_owner`, `enablement`, `writer`, `context_unobserved`, `operator_revision`, `action_revision`, `applied_revision`, `rate`, `caller_flags`, `current_caller`, `fec` and `readiness`;
  - the receipt and consumption stages `no_receipt`, `receipt_not_current`, `signal_not_current`, `measurement`, `setting`, `caller_changed` and `policy_not_normal`.

  Each name comes from the check that already decided; no predicate or caller callback runs again for it. `consumed` and `pending_wait` are not refusals.
- **Clears** are named `gap`, `order`, `median_unavailable`, `bucket_coverage`, `bucket_second_skipped`, `bucket_overflow`, `source_changed`, `action_revision_changed`, `floor_changed`, `reference_expired`, `permission`, `not_consumed_receipt`, `not_consumed_measurement`, `not_consumed_setting`, `not_consumed_current`, `policy_refused` or `writer_or_scope`.
  - A clear is counted only when the window held observations.
  - `last_lost_*` gives how many closed, reference and consecutive standing buckets the latest clear discarded.
  - A window usually starts inside a second, so a short first bucket is a counted `bucket_coverage` clear with `last_lost_closed=0`.
- **A busy skip is not a clear.** A report that finds the state lock busy never enters. It returns as before and only increments a lock-free, process-wide counter. Its line reports the difference since the scope began as `busy_skips`.
  - Reports that never reach the entry point are outside these counts.
- **Bounds.** Each scope writes:
  - at most 64 `summary` lines: one at its first entered report, then at most one every 10 s;
  - at most 16 `interruption` lines, one per clear other than the routine `reference_expired`;
  - one `scope_end` line when the scope ends or resets, if it entered or skipped any report.

  Anything over a limit only increments `suppressed`. Running lines appear only while the opt-in is on. A disabled scope still ends with one `scope_end` line naming `config_disabled`, which shows whether reports arrived. Lines stay under 1536 bytes, and counts saturate at 2^32−1.
- **Privacy.** Lines carry only static names and numbers. `generation` and `sequence` are the existing opaque scope generation and report sequence. No app or session string, nonce, token, address or free text appears.
- **No authority.** No decision, permission, deadline, threshold, bucket, action, retry or learning reads these counts. Lines are formatted under the state lock and written after it is released.

To collect them for a case window: `journalctl --user -u polaris --since "<case start>" --no-pager | grep 'Live Tuning native diagnostic'`. For a desktop-launched instance, search that unit or the Polaris log file instead.

## Passive native signal and controller inspection (private trace 2.5)

Each native control callback now projects the newly admitted value copy into a stateless typed
`native_video_signal::signal_t`, with tracing either on or off. The existing native scope's internal
readback retains at most its current report's diagnostic value; public JSON and the wire payload do
not change. The projection itself grants no permission; only the separate experimental consumer above
can make a downward request after serializing and rechecking current authority. Doctor and AI do not consume it.

For matched sender API bounds `[B,E]`, admission host steady receive time `R`, and client socket-read
age `a` in microseconds, each sample carries signed elapsed bounds `[R-E-1000*a, R-B-1000*a]`.
These are admission-time durations, never recomputed at status-poll time. Negative bounds remain
signed and unavailable. Data/parity and full report counters remain separate from sparse timing
samples. Configured encoder bitrate and accepted bytes/interval are not capacity or a loss
denominator. No baseline, window, floor/background learning, congestion threshold or clean/recovery
credit is created. Absolute host/client clock offsets are not subtracted; client clock-rate error,
media/return-path and scheduling terms remain in the elapsed measurement.

Native/frame locks are released before one nonblocking controller `state_mutex` inspection.
The expected generation and app token are compared under that lock; only match booleans are copied,
never a token or address. A mismatch exports no foreign rates/writers/revisions. Busy context is
explicitly unobserved. The snapshot copies enabled/configured/paused, runtime support/in-place,
sole-scope and writer/pending facts, requested/base/applied values, revisions and applied age.
`floor_kbps` is the diagnostic maximum of the existing session/configuration floor and backend
in-place runtime floor; inspection changes neither floor nor any policy or target. Historical source
encoder lifetime/application/origin tuple stays separate from current controller revisions: a no-op
reaffirmation is not a new backend application.

`inspection_ready` describes observation-time checks only, not permission or a capability. Source
measurement availability is separate from readiness. Caller RUNNING/watch/input-only/negotiation,
verified control authentication and actual media-encryption facts remain separate. After inspection,
publication reacquires only the native scope lock and rechecks current report identity plus the
existing two-second report/oldest-sender/frame freshness. A newer report clears the prior projection;
refused callbacks cannot reuse it; reconnect/retirement is terminal. A future actual writer must
serialize and recheck its own current scope, permissions, freshness and writer state.

Opt-in private trace minor **2.5** adds two bounded companion row kinds; the original minor-4
`native_admission` (34 payload fields) and `native_admission_sample` (12) shapes stay unchanged.
One `native_signal_context` row has exactly 40 payload fields (the unchanged recorder maximum),
and up to 16 `native_signal_sample` rows have 12 each. Ordinary `type`, `order` and
`observed_steady_ns` metadata are additional. Companion keys join the same opaque callback and
report sequence to the unchanged raw admission/encoder tuple and raw sample rows.

Context keys are `trace_scope`, `session_generation`, `callback`, `report_sequence`,
`received_steady_ns`, `signal_reason`, `measurement_available`, `inspection_ready`, `sparse`,
`running`, `watch_or_input_only`, `negotiated`, `control_authenticated`, `media_authenticated`,
`context_reason`, `context_observed`, `inspection_steady_ns`, `generation_matches`, `app_matches`,
`exclusive`, `configured_enabled`, `runtime_enabled`, `paused`, `runtime_supported`, `in_place`,
`doctor_override`, `explicit_writer`, `pending_update`, `applied_known`, `base_kbps`,
`requested_kbps`, `floor_kbps`, `applied_kbps`, `operator_revision`, `action_revision`,
`applied_revision`, `applied_age_ns`, `native_availability`, `sample_count`, and `published`.
`applied_age_ns` is null when unknown; unobserved/busy context uses explicit default scalars and
inspection time -1, not proof of false exclusions or a zero floor. Report sequence is null for an
unattributed/refused callback. Zero opaque scope/callback IDs are null. Foreign or undispatched
callbacks cannot relabel another report's context or sample outcomes.

Sample-outcome keys are `trace_scope`, `session_generation`, `callback`, `report_sequence`,
`sample_index`, `frame_index`, `rtp_sequence`, `packet_kind`, `udp_bytes`, `elapsed_reason`,
`elapsed_lower_ns`, and `elapsed_upper_ns`. Invalid host ordering yields null elapsed bounds;
negative arithmetic remains signed with `negative_interval`. No payload, token, address or client
identity is emitted. Existing event/byte/time/sample/ledger budgets stay unchanged. Frozen 2.4/2.3
artifacts and readers remain byte-identical; a new reader must explicitly support this minor and
validate the companion relationship rather than upgrade an old reader's predicates.

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
