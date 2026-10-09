# Recorder packet provenance (passive, version 1)

The stream recorder (#359) keeps a small **evidence record** beside every replay payload, both in
the live buffer and in a held save snapshot. The record holds what the encoded packet carried at the
recorder tap. It is passive:
- it changes nothing the recorder decides or writes;
- it is not a presentation timestamp;
- nothing outside tests reads it.

The output is still raw elementary video, labelled exactly as before: no container, audio or
timestamps.

## What is recorded

`stream_recorder::packet_provenance_t` (`k_version = 1`), supplied by `push_video()`:

| Field | Meaning |
|---|---|
| `frame_index` | The encoder's frame index as the tap read it (`packet->frame_index()`). **Empty** when the caller supplied none. Legacy `push_packet()` passes a `pts` that is often 0, so an empty index ("unsupplied") is kept distinct from a real index 0. |
| `frame_timestamp` | The packet's `frame_timestamp` (a capture or pacing time) exactly as it was at the tap, if it had one. The send path may fill a missing one *after* the tap; that later value is not recorded. An absent time stays absent: arrival time is never substituted. |
| `source_clock_timestamp`, `source_time_paced` | Copied from the packet. They are coordinate facts, never authority, proof of pacing, or VRR acceptance. |

The recorder stores this, with its version, as `detail::provenance_record_t` inside the same live
entry as the payload. Each record also gets **marks**:

| Mark | Set when |
|---|---|
| `index_unsupplied` | No index was supplied |
| `index_duplicate` / `index_regressed` / `index_gap` | The index is equal to, lower than, or more than one above the run's most recent *supplied* index |
| `time_missing` | No `frame_timestamp` at the tap |
| `time_duplicate` / `time_regressed` | The time equals, or is earlier than, the run's most recent *known* `frame_timestamp` |

How the comparisons work:
- **Unknown values are skipped.** A comparison runs only when both sides are known. An unsupplied
  index or a missing time is compared with nothing and does not move the baseline.
- **Overflow is impossible.** Index gaps are computed as an unsigned difference only when the index
  increased, so the comparison is exact at both ends of `int64`.
- **Gaps are evidence, not errors.** Real encoders can drop or skip frames, so a gap is recorded,
  never treated as a fault.
- **Baselines are per run.** Anything that already clears the run forgets them: a discontinuity, a
  codec change, a selection or configuration reset. The next run's first entry is compared with
  nothing.

## What it never does

- **No decisions.** It never refuses, trims, aligns, resets or cuts a run, and never counts a
  discontinuity. Retention, refusals, caps, trimming, keyframe alignment, resets, continuous writes
  and saved bytes are exactly those of `push_packet()`.
- **No timestamps.** It never infers or writes a presentation timestamp, cadence or order. How
  provenance might map to container time and order is a separate, explicitly reviewed later step.
- **No public surface.** It adds no public status, `save_result_t`, HTTP, Nova or UI field.
  Inspection is test-only:
  - `detail::replay_provenance_for_tests()`
  - `detail::snapshot_provenance_for_tests()`
  - `detail::provenance_sizes_for_tests()`
- **No change to the tap's guard or position.** The production tap in `src/stream.cpp` still runs
  only under the existing `is_recording() || current_mode() == replay_buffer` guard. It still sits
  before the parameter-set replacements. Building the record copies a few fields on the stack, with
  no lock, thread or allocation. The disabled fast path inside the recorder is unchanged.

## Ownership and lifetime

- **Live:** a record lives inside its replay entry, under the recorder lock, so payload and evidence
  are kept, trimmed and released together.
- **Held:** an admitted save copies the records into a provenance array, reserved and filled beside
  its payload-reference array, in the same order.
  - The held snapshot stays valid across live resets, session end and configuration reloads until
    its own lease releases it, exactly like the payload references.
  - The array is freed with the lease.
  - An admission that fails after the arrays were filled releases both with no accounting left
    behind.

## Memory (bounded accounting, not allocator or RSS cost)

- **Live:** each entry grows by the compiled size of the record plus any padding. There are at most
  `max_packets` live entries.
- **Held:** one array of `provenance_record_t` per held save, of its reserved capacity (at most one
  save at a time, at most `max_packets` entries).
- **Measured, not assumed:** the actual sizes are reported by the test
  `StreamRecorderProvenanceTests.ProvenanceStaysWithinThePacketCapAndRecordsCompiledSizes` as GTest
  XML properties:
  - the size of each type (`sizeofPacketProvenance`, `sizeofProvenanceRecord`,
    `sizeofLiveReplayEntry`, `sizeofSnapshotPayloadRef`);
  - a same-build mirror of the previous entry layout (`sizeofPreProvenanceEntryMirror`,
    `liveEntryDeltaVsMirror`);
  - the held capacities and the bounds derived from them.

  These are `sizeof` and vector-capacity facts for one compiler and build. They do not cover
  allocator overhead, heap totals or process RSS.

## Not covered

This slice proves none of the following:
- decoding, or which decoders and libraries are present;
- the actual bitstream framing of any codec lane (AV1 and PyroWave included);
- presentation timestamps, cadence, ordering or seeking;
- a container, audio, or A/V sync;
- device, live or playback acceptance.

Those are separate, reviewed slices. Recording remains off by default.
