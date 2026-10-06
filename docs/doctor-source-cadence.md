# Doctor source cadence evidence

Doctor v2 keeps fixed-grid grading for older hosts and for a valid `vrr` policy
whose `effective` value is false. A VRR request or capture capability alone does
not change that grading. The admission/timestamp foundation currently reports
`effective: false`; it does not enable source-time pacing.

This consumer also reserves an optional, host-only `source_cadence` aggregate
for a future source-time producer. The current foundation does not emit this
aggregate. Missing, stale, malformed or differently scoped evidence leaves the
diagnosis collecting rather than reporting a healthy source-time stream.

## Policy identity

The private authenticated Doctor evidence uses `vrr.schema: 1` and the exact
nonzero `session_generation` of the requesting stream. Its `requested`,
`requested_from`, `capture_pacing`, `pacing_mode`, `effective` and
`fallback_reason` fields must describe a consistent policy. Numeric identities,
versions, times and counts must be exact integers; booleans and floating point
identities are rejected.

A source-time policy requires `requested: "true"`, `effective: true`,
`pacing_mode: "source_time"`, `capture_pacing: "source_driven"` and a null
fallback reason. A fixed-grid policy requires a fallback reason consistent with
the request and capture route. `watch_or_input_only` takes precedence over any
request; `request_malformed` requires a malformed request; `not_requested`
requires an absent or false request. Route, clock and presentation reasons
require a true request. `route_unreported` requires unknown capture pacing,
`route_fixed_interval` requires fixed-interval capture, and clock or presentation
fallbacks require source-driven capture.

## Future aggregate contract

The host producer must supply the following fields in `source_cadence`:

| Field | Required evidence |
|---|---|
| `schema`, `session_generation` | Version 1 and the same stream generation as the policy and client reports. |
| `clock_domain_verified`, `clock_epoch_id` | A true host clock-verification result and a nonempty epoch identity. Doctor consumes that result; it does not independently verify clock equivalence. |
| `window_start_ms`, `window_end_ms` | Server monotonic milliseconds matching the first and last client report receipt in Doctor's current comparison window exactly. At least one second of coverage; the end must be no more than two seconds old and never in the future. |
| `first_source_ns`, `last_source_ns` | Exact ordinary-source presentation times mapped into the verified server monotonic clock, in nanoseconds. Both must be inside the comparison window and the first must precede the last. Missing edge evidence cannot qualify. Window milliseconds must be representable as signed 64-bit nanoseconds. |
| `source_frames`, `encoded_source_frames` | At least three ordinary source frames and the count encoded from those frames. The latter cannot exceed the former. |
| `invalid_source_frames`, `routine_repeat_frames` | Both zero to qualify source-time measurements. Keepalive policy is separate and must not inflate these ordinary-source counts. |
| `source_fps` | Positive, finite FPS matching `(source_frames - 1) * 1000 / window_duration_ms` within one percent. |
| `max_source_gap_ms` | Finite, nonnegative maximum interior source interval, at least the mean over the first-to-last source span and no greater than that span (one nanosecond tolerance). |
| `max_delivery_interval_error_ms`, `timestamp_error_ms` | Finite, nonnegative measurements from source/delivery correlation. |
| `limits` | Explicit finite, nonnegative `max_source_gap_ms`, `max_delivery_interval_error_ms` and `max_timestamp_error_ms` limits. The source-gap limit must be positive. |

For qualified measurements, Doctor compares rendered FPS with the measured
source FPS instead of the requested stream ceiling. It retains source stalls,
timestamp errors, delivery jitter, network pressure, decoder/render pressure
and encoder drops. Source-stall grading includes the leading window-to-first
gap and trailing last-to-window gap as well as the interior maximum; averaging
FPS over the window cannot hide a boundary stall. These derived edge gaps appear
in the diagnostic observation. Encoding fewer than 90 percent of ordinary source frames
also reports encoder pressure. Static/duplicate hints cannot hide an observed
source stall or timestamp error.

This first consumer qualifies only sources at or below the stream ceiling. A
faster source can legitimately lose frames to the ceiling, and this aggregate
has no verified ceiling-drop accounting yet. Doctor leaves that case collecting
rather than blaming the encoder or capture cadence.

Source-time evidence cannot authorize a fixed-rate pacing trial. Trial readiness
is false and the fixed-grid pacing-error metric is unavailable for such a
stream. This diagnostic does not grant VRR activation, bitrate authority or
receiver-presentation acceptance. Its observation explicitly reports
`physical_acceptance: false` and `receiver_presentation_verified: false`.

The private JSONL trace checker remains a separate contract. This aggregate
consumer does not replace correlated source/send/presentation traces, queue
growth and latency measurements, or the physical VRR acceptance gates.
