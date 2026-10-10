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
entry as the payload. Each record also gets **marks**, computed once, at admission, by comparing it
with the **adjacent retained entry**. That is the buffer's last entry after this admission's own
expiry, trimming and keyframe alignment, just before the new entry is stored:

| Mark | Set when |
|---|---|
| `index_unsupplied` | No index was supplied |
| `index_duplicate` / `index_regressed` / `index_gap` | The index is equal to, lower than, or more than one above the adjacent retained entry's index |
| `time_missing` | No `frame_timestamp` at the tap |
| `time_duplicate` / `time_regressed` | The time equals, or is earlier than, the adjacent retained entry's `frame_timestamp` |

How the comparisons work:
- **Only adjacent known values are compared.** A coordinate is compared only when both the new
  entry and its adjacent retained entry have it. An unsupplied index or a missing time on either
  side means that coordinate is not compared. There is no earlier "last known" baseline that
  reaches past an unknown neighbour.
- **Overflow is impossible.** Index gaps are computed as an unsigned difference only when the index
  increased, so the comparison is exact at both ends of `int64`.
- **Gaps are evidence, not errors.** Real encoders can drop or skip frames, so a gap is recorded,
  never treated as a fault.
- **An empty buffer means no comparison.** Whatever leaves the buffer empty before the entry is
  stored means the entry is compared with nothing:
  - a run clear (discontinuity, codec change, selection or configuration reset);
  - duration expiry;
  - a cap trim whose keyframe realignment removed everything.
- **Marks are fixed at admission.** Later trimming of a neighbour does not recompute them. A held
  snapshot keeps them as admitted.

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
  - Tests inject an admission failure (a `bad_alloc` thrown by a test-only save hook) at three
    boundaries. Each releases everything, leaving no accounting or reservation behind. No real
    allocation is made to fail. The boundaries are:
    - before the capture state is allocated (`before_admission_allocation`);
    - between the payload array's reserve and the provenance array's reserve
      (`before_snapshot_provenance_allocation`, added for #359);
    - after both arrays were reserved and filled (`after_snapshot_payloads`).

## Memory (bounded logical entries, measured capacity; not allocator or RSS cost)

- **Logical entries are bounded:**
  - **Live:** every live entry holds one record, and there are at most `max_packets` live entries.
  - **Held:** a held save holds one record per captured entry, at most `max_snapshot_packets`
    (= `max_packets`), with at most one save at a time.
  - **Total:** `metadata_entries` (live + held) stays within `max_metadata_entries`.
- **Byte cost is not bounded by these counts:**
  - **Live:** each entry grows by the compiled size of the record plus padding, but the live entries
    sit in a `std::deque`. Its block allocation is not "count × sizeof".
  - **Held:** the provenance array's capacity is **measured**. `reserve()` guarantees at least the
    requested count, not exactly it, so no universal "`max_packets` × `sizeof`" allocation bound is
    claimed.
- **Measured, not assumed:** the actual sizes are reported by the test
  `StreamRecorderProvenanceTests.ProvenanceStaysWithinThePacketCapAndRecordsCompiledSizes` as GTest
  XML properties:
  - the size of each type (`sizeofPacketProvenance`, `sizeofProvenanceRecord`,
    `sizeofLiveReplayEntry`, `sizeofSnapshotPayloadRef`);
  - a same-build mirror of the previous entry layout (`sizeofPreProvenanceEntryMirror`,
    `liveEntryDeltaVsMirror`);
  - logical counts read after admission (`liveEntries`, `heldEntries`, `metadataEntries`,
    `maxMetadataEntries`, `packetCap`);
  - measured held capacities (`heldProvenanceCapacity`, `heldPayloadCapacity`,
    `heldProvenanceCapacityBytes`);
  - the logical live delta (`liveEntryDeltaLogicalBytes`, which is count × the per-entry delta, not
    an allocation).

  These are `sizeof`, logical-count and measured-capacity facts for one compiler and build. They
  do not cover allocator overhead, deque block allocation, heap totals or process RSS.

## Linked FFmpeg codec registration inventory (#361)

`RecorderMediaCapabilitiesTests.LinkedFfmpegRegistrationInventory`, in `test_polaris_video` (CTest
`polaris_recorder_media_capabilities`), records what the linked FFmpeg **registers**. It records:
- `av_version_info()` and `avcodec_configuration()`;
- the libavcodec and libavutil versions, both from the headers compiled against and from the
  runtime;
- for each of the `libx264`, `libx265` and `libsvtav1` encoders (looked up by name), and the H.264,
  HEVC and AV1 decoders (looked up by id): present or not, and its name, codec id and media type.

An absent registration is recorded as present `0`, an empty name, id `0` and an empty type. Every
value is written both as a GTest XML property and in one console line,
`POLARIS_RECORDER_MEDIA_CAPABILITIES_JSON {...}`, with identical strings. The test asserts only that
any registration found is the codec it was looked up as, and that the recorded values are coherent.
It assumes no availability set.

**Registration is not media capability.** No codec is opened, allocated, encoded or decoded. A
registered codec may still fail to open, and a registered decoder proves nothing about any
recorded file. These are runtime facts of the linked library as this binary sees them. Which
prepared archive supplied that library is recorded separately, by CI.

## CPU encoder fixtures (#363)

`RecorderEncoderFixtureTests` (2 cases, Linux only, in `test_polaris_stream`, CTest
`polaris_recorder_encoder_fixture`) opens the named `libx264` and `libx265` encoders from the linked
FFmpeg. Each case then:
- feeds 8 deterministic 64×64 YUV420P frames, with explicit single-threaded operation, no B-frames,
  a key frame every 4 frames and scene cut disabled;
- drains the encoder with bounded send, receive and flush calls;
- checks a positive baseline on the first run: 8 non-empty, size-capped packets in input order,
  with key frames exactly at 0 and 4;
- only then repeats the identical run, and requires byte-identical packets.

For both runs, it records the options and every dictionary-set, open, send, receive and flush result,
plus each packet's size, flags, DTS and SHA-256, as GTest XML
properties and in one console line, `POLARIS_RECORDER_ENCODER_FIXTURE_JSON {...}`. Each packet's PTS
is recorded too. It is the fixture's own synthetic frame counter: not producer provenance and not a
presentation time.

A missing encoder or a failed open is a test **failure**: a dependency finding, never a skip. The
fixture proves only that these encoders open and produce repeatable packets in this build. It does
not decode or parse bitstream structure, does not touch the recorder, and makes no claim about
playable output.

## CBS reads and raw replay-save readback of real fixture packets (#365)

`RecorderBitstreamTests` (4 cases, Linux only, in `test_polaris_stream`, CTest
`polaris_recorder_bitstream`) reuses a test-only copy of the #363 encoder support
(`tests/unit/recorder_encoder_fixture_support.h`); the #363 file itself is unchanged. Each case first
requires the frozen baseline: 8 packets, key flags at 0 and 4.

**CBS stream observations (H.264 and HEVC).** The 8 packets are read in order through **one**
coded-bitstream (CBS) context per codec and run, so parameter sets read from earlier packets remain
available to later ones. The context decomposes the same units the product's own reads do
(parameter sets and slice headers). For each packet the test records:
- the read's return, unit count, unit types and unit byte sizes;
- whether the reader holds an active SPS afterwards;
- the packet's leading bytes;
- the size and SHA-256 of the original packet and of a copy passed through the product's filler
  strip.

It asserts only that every read succeeded with at least one unit. Unit sequences, IDR and
parameter-set placement, Annex B framing, filler absence and what a later key frame carries are
recorded, not asserted, until an actual run freezes them. `cbs::validate_sps` is recorded for the
first packet under its real meaning, active-SPS VUI presence; it is not a read-success test. CBS
reading is structure reading only. It is not an `AVCodecParser`, a decoder or playback.

**Raw replay-save readback (H.264 and HEVC).** The test registers one eligible session whose codec is
known, then pushes the 8 packets with the default caps, at a controlled arrival clock (16 ms apart).
Each push carries an explicit synthetic producer counter as its provenance frame index; this is
deliberately not the packet PTS, and neither is a product clock or presentation time. It then saves
into a directory the test created. It requires that:
- the encoder packets equal the retained replay payloads, one for one;
- the counters are kept as supplied;
- the published file equals the packets concatenated in order, byte for byte, and its size equals
  their total;
- the save reports 8 packets, 2 key frames, the session and its codec.

The saved file is raw elementary video: no container, timestamps or playability claim.

Evidence for every case is recorded as GTest XML properties and in one console line,
`POLARIS_RECORDER_BITSTREAM_JSON {...}`, including the process UID and byte hashes. A missing
encoder or a failed open, read or save is a test **failure**, never a skip. Malformed input, fresh
readers for later key frames, and seek or start structure are separate, later slices.

## Not covered

This slice proves none of the following:
- decoding, or which decoders and libraries are present;
- the bitstream framing of any live codec lane (AV1 and PyroWave included); #365 observes only the
  CPU fixture's H.264 and HEVC packets;
- presentation timestamps, cadence, ordering or seeking;
- a container, audio, or A/V sync;
- device, live or playback acceptance.

Those are separate, reviewed slices. Recording remains off by default.
