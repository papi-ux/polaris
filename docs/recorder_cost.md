# Recorder cost probe

`tests/unit/test_stream_recorder_cost_probe.cpp` (#357) measures what the stream recorder's
admission, locking, keyframe-reservation and save paths cost on synthetic input. It is
**report-only**:
- it asserts deterministic correctness invariants;
- its timings are recorded, never compared with a threshold.

It is off by default. Configure with
`-DBUILD_FULL_TESTS=ON -DPOLARIS_ENABLE_RECORDER_COST_PROBE=ON` on Linux; turning it on anywhere
else stops the configure with an error, rather than silently building nothing. When on, it builds
its own executable, `test_polaris_recorder_cost`, from the shared full test runtime, and registers
the CTest of the same name. Ordinary full builds, `test_polaris_stream` and every required suite
are unchanged.

## What it does and does not touch

- **It does:** it calls the real recorder through its existing test seams: `push_packet`,
  `detail::push_packet_at`, the continuous and save hooks, `set_continuous_limits_for_tests`,
  `keyframe_stats_for_tests` and `recorder_mutex_try_lock_for_tests`.
- **It does not:** it starts no capture, encoder, stream, client session, live keyframe (IDR)
  request or anything on a host.
- **Fixture-only state:**
  - sessions are recorder registrations only;
  - keyframe events are fixture `safe::mail_raw_t` events;
  - files are written only in the test's own temporary directory and the configured output
    directory.
- **Clean up:** every case restores config, seams, sessions and writers when it ends, pass or
  fail.

## Inputs

| Variable | Required | Meaning |
|---|---|---|
| `POLARIS_RECORDER_COST_OUTPUT_DIR` | yes (set by CTest to `build/tests/recorder-cost`) | Existing absolute directory |
| `POLARIS_RECORDER_COST_SOURCE_SHA` | yes (set by CTest from `git rev-parse HEAD` at configure time, or `-DPOLARIS_RECORDER_COST_SOURCE_SHA=`) | 40 or 64 hex digit source commit, recorded as caller-reported (`sourceShaOrigin`), not as proof of what was compiled |
| `POLARIS_RECORDER_COST_SAMPLES` | no, default 64, range 1..10000 | Timed samples per subrecord |
| `POLARIS_RECORDER_COST_WARMUP` | no, default 8, range 0..1000 | Untimed warm-up iterations |
| `POLARIS_RECORDER_COST_RUNNER` | no | Recorded verbatim; `null` when unset |

A missing or malformed required value, or an out-of-range parameter, fails the case. Nothing is
skipped.

**Outputs:**
- Each case creates `recorder-cost-<Case>.json` with an exclusive, no-follow creation, and fails
  if the file already exists. It never truncates or replaces a file.
- **To run again,** use a new output directory.

## Cases

The synthetic train is encoded video frame payloads, **before RTP fragmentation**:
- 60 fps;
- a 256 KiB keyframe every 120 frames;
- dependent frames cycling 16/32/48/64 KiB;
- HEVC-labelled bytes that are never decoded.

The 60 fps interval is an artificial arrival-clock step used by replay fixtures. Calls are
unpaced when measuring wall-clock cost. Keyframe comparisons use fixed 32 KiB dependent payloads;
each fixture-dispatch invalid packet follows an untimed 256 KiB recovery keyframe. Their subrecords state
these overrides and use the actual steady clock.

| Case | Subrecords |
|---|---|
| `DisabledTap` | `production_tap_reads`: the exact `is_recording() \|\| current_mode() == replay_buffer` check `src/stream.cpp` makes per frame (it never calls `push_packet` while disabled). `direct_disabled_push`: a direct `push_packet` call on the disabled fast path, which the real tap never makes. |
| `ReplayAdmission` | `replay_push`: replay-mode admission for the owner session (one owned payload copy per frame under the recorder lock). The timed frames continue the warm-up's train: frame numbers, keyframe positions and arrival times run on from the warm-up frames. |
| `ContinuousAdmission` | `storage_free_writer` and `native_tmp_writer`: admission with the writer drained between frames, so a drop can only be a defect. `throttled_writer`: a small queue (512 KiB, 8 slots) whose writer completes one write call per permit granted every 4 frames. It reports the real admitted, dropped and refused counts and the peak queue. |
| `LockContention` | `producer_push` and `reader_0`/`reader_1`: replay admission while readers call `recorder_status()`, plus try-lock failures. Overlap is not guaranteed for every call. |
| `KeyframeReservation` | `unwired_owner_push`: no channel, the production shape (continuous admission takes the writer lock; keyframe reservation returns before its additional lock). `fixture_channel_owner_push`: a fixture channel, with one additional reservation lock per owner frame. `fixture_dispatch`: an invalid packet models a discontinuity, then reserves and raises the fixture event; this is not observed network loss. **Controlled condition:** each times only the production `push_packet`. The storage-free writer is drained, untimed, before the next frame, and each recovery keyframe is admitted and drained untimed before the invalid packet. Drain waits and backpressure are excluded; a woken writer can still contend for its mutex during the timed push. |
| `ReplaySaveSnapshot` | `admit_snapshot`: snapshot admission under the lock, then dropped. `complete_to_owned_dir`: completion into the temporary directory (`min(samples, 4)` real files, recorded with `warmup: 0` because no completion warm-up runs). |

## Record schema (`schema: 1`)

- **Identity:** `probe`, `case`, `sourceSha`, `timing` ("report-only").
- **Parameters:** samples and warm-up values, each with its source (`default` or `environment`).
- **Inputs and environment:** `train`, `machine` (uname, CPU model or an explicit `null` with a
  note, hardware concurrency, uid), `build` (compiler `__VERSION__`, `__cplusplus`, NDEBUG,
  optimized) and `runner`.
- **Measurements:** `subrecords[]` (name, what it measures, the warm-up that subrecord actually ran, sample count, raw `ns[]`),
  `outcomes` and `invariants[]` (name, passed).
- **Memory:** `ru_maxrss` before and after.

## Reading the numbers

- **Payload counts** are the recorder's ownership accounting, not allocator measurements.
- **`ru_maxrss`** is the process's high-water mark, not current RSS and not recorder-only memory.
- **The unwired production path** still runs `keyframe_reserve`'s early checks. It avoids the
  additional reservation lock and dispatch because no channel is registered; ordinary continuous
  admission still takes the writer lock. The unwired path has a measurable cost.
- **What synthetic runs on a CI runner cannot show:** the effect on real broadcast-thread pacing
  and client latency, real encoder frame sizes and rates, the user's disk and memory under real
  bitrates, or live IDR cost.
- **Still open:** audio, a container with timestamps, decode and playback qualification, a
  reviewed permission-scoped Nova client contract, and live keyframe recovery. Production registers
  sessions without a keyframe request channel, so a bound continuous file that drops a frame waits
  for the stream's next keyframe; the replay buffer has no request path at all, so a run that loses
  its starting keyframe can stay empty until one arrives (see `docs/configuration.md`, "Replay
  window and keyframes"). None of these probes completes a clip.
