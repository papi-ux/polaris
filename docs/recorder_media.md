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

## Negative and later-key inputs from real fixture packets (#369)

`RecorderBitstreamNegativeTests` (25 cases, Linux only, in `test_polaris_stream`, CTest
`polaris_recorder_bitstream_negative`) starts from the same unchanged 64x64 encoder support and its
frozen baseline. Each case builds a closed input list from the real packets with a named,
deterministic `recipe-v1` edit, or uses them unmodified, and labels every pushed packet `real`,
`constructed-from-real` or `synthetic`:
- `trunc-half` and `trunc-in-sps` of key packet 0 (N1, with its replay save as N2);
- `strip-startcodes` of key packet 0 (N3) and seeded `garbage` of its length (N4);
- a null-payload or zero-size owner frame between real packets (N5);
- `drop-param-sets` on the later key packet 4 (L1) or the first key packet 0 (L3);
- a real dependent packet pushed as a key (`flag-lie`, L4);
- a never-registered generation interleaved with the selected run (F2);
- a second registered session overlapping the selected one (F5): both owners are refused while two
  sessions are registered, the run is cleared with cause `selection`, a dependent frame after the
  overlap ends is refused as awaiting a key, and only the first session's packets from a fresh real
  key are saved. The case asserts named intermediate snapshots of the registered sessions,
  selection, run, refusal counters and cause, and that no discontinuity is counted;
- a codec change inside one session (F1), a session restart (F3) and a save admitted before a
  codec change, through the existing admit/complete seam (F4).

A resolution or SPS change (the design's L2) is not part of this slice.

**What is asserted.** The existing replay admission counters, the window-limit cause where a case is
about it, the save status, packets, key frames, codec and generation, and byte transparency: the
published file equals the admitted packets concatenated in push order. The recorder does not parse,
repair or refuse content; a malformed packet that passes admission is saved as supplied.

**What is only recorded.** CBS cases read every pushed packet in order through one persistent CBS
context, call `cbs::validate_sps` on the first input and `cbs::strip_filler_data` on a copy of each.
Their returns, unit lists and the persistent context's active-SPS state, observed after every
call whatever the read returned, are observations, not a parser validity or refusal contract. The only CBS assertions are completion and test-owned integrity: each input sits in a
buffer followed by FFmpeg's zeroed input padding (which FFmpeg may read) and a 64-byte canary, and
the input, padding and canary must be unchanged after every call; the stripped copy must not grow.
That catches stray writes into those test-owned bytes only. It is not a bounds, read-safety or
memory-safety result: the test binary does not instrument the pinned FFmpeg archive, and no
instrumented-library gate has run.

**Evidence.** Each case records one closed property set as GTest XML properties and one identical
console line, `POLARIS_RECORDER_NEGATIVE_JSON {...}`, with the FFmpeg and compiler identity, the
real source packet hashes, each push's provenance, recipe, size and hash, every CBS observation, the
counters, any named intermediate replay snapshots and the save. CTest sets `POLARIS_RECORDER_NEGATIVE_EVIDENCE_DIR` to
`build/tests/recorder-negative-evidence`. Each case creates its own directory there exclusively and
writes the real source packets, every pushed packet, the admitted concatenation, the published file
and a manifest, so a separate readback can rebuild each recipe and compare the saved bytes with the
raw inputs. A missing directory, an existing case directory or any write error fails the case, so a
rerun needs a fresh build or an emptied evidence directory. Raw evidence is never committed.

## Not covered

This slice proves none of the following:
- decoding, or which decoders and libraries are present;
- the bitstream framing of any live codec lane (AV1 and PyroWave included); #365 and #369 observe
  only the CPU fixture's H.264 and HEVC packets and edits of them;
- parser validity or refusal of malformed input, or FFmpeg read bounds and memory safety;
- presentation timestamps, cadence, ordering or seeking;
- a container, audio, or A/V sync;
- device, live or playback acceptance.

Those are separate, reviewed slices. Recording remains off by default.

## Replay cap trimming of real fixture packets (#371)

`tests/unit/test_recorder_bitstream_window.cpp` (`RecorderBitstreamWindowTests`, Linux only, CTest
`polaris_recorder_bitstream_window`) pushes the same CPU fixture's 8 real H.264 and HEVC packets
(keys at 0 and 4) through test-only replay caps for one eligible session, at a controlled arrival
clock, and saves into a directory the fixture created. Six cases, three per codec:

- **Entry cap 6.** Room for packet 6 removes packet 0 and keyframe realignment removes 1–3. The live
  run, the retained provenance and the saved file must be exactly packets 4–7, with
  `last_window_limit` `entry_cap`.
- **Payload cap of the run's total minus one byte.** Room for packet 7 removes packet 0 and
  realignment removes 1–3; the same save, with `payload_cap`.
- **Payload cap at the largest dependent packet, checked to be below both keys.** Both keys are
  refused as oversized, the six dependent packets are refused while awaiting a keyframe, the cause
  is `discontinuity`, and the save is empty with no file created.

The saved file's first access unit must carry its parameter sets before an IDR slice (H.264 SPS and
PPS; HEVC VPS, SPS and PPS); the exact NAL type lists are recorded. Each case isolates one cause:
cap precedence when both caps apply and duration expiry are not covered. The cap values come from
the run's own packet sizes after the fixture's key positions and size ordering are checked.

**Evidence.** Each case records one closed property set as GTest XML properties and one identical
console line, `POLARIS_RECORDER_WINDOW_JSON {...}`. CTest sets `POLARIS_RECORDER_WINDOW_EVIDENCE_DIR`
to `build/tests/recorder-window-evidence`. Each case creates its own directory there exclusively and
writes its 8 source packets, its published save (when one exists) and a manifest, each created
exclusively, fully written, synced, closed and read back, before any assertion. A missing, relative,
non-directory or symlinked root, an existing case directory or any write error fails the case.
Raw evidence is never committed or published with shared preview artifacts.

These are in-run byte and counter properties only. No packet hash is frozen in the test; whether a
run's bytes match earlier runs, or what a decoder showed about them, is a separate readback's
question. Nothing here is decode, playback, container, timing or memory-safety evidence.

## Replay duration expiry of real fixture packets (#375)

`tests/unit/test_recorder_bitstream_expiry.cpp` (`RecorderBitstreamExpiryTests`, Linux only, CTest
`polaris_recorder_bitstream_expiry`) pushes the same CPU fixture's 8 real H.264 and HEVC packets
(keys at 0 and 4) through the replay buffer's duration expiry for one eligible session, and saves
into a directory the fixture created. The duration is one minute (D = 60 s) and the default hard
caps stay in force, far above 8 packets, so no cap can remove anything. Packets 0–3 arrive at
t0 + i × 16 ms and packets 4–7 at t0 + 30 s + (i − 4) × 16 ms on a controlled steady arrival clock.
A packet expires when the read, append or save time minus its **arrival** is strictly greater than D.
Every expectation follows from those arrival offsets and D; the packet PTS is recorded and never
compared. Six cases, three per codec:

- **Read-triggered expiry.** A read at exactly t0 + 60000 ms keeps all 8 packets: packet 0 is exactly
  D old. A read at t0 + 60033 ms expires packets 0–2, and keyframe realignment removes packet 3,
  which is still fresh: 4 packets, `trimmed` 4, cause `duration`. A later read stamped t0 + 60000 ms
  restores nothing. The save is exactly packets 4–7.
- **Append-triggered expiry.** Packets 0–6 arrive with no status read; packet 7 arrives at
  t0 + 60033 ms. A read stamped earlier, at t0 + 30048 ms, when nothing could expire, already shows
  packets 4–7 with cause `duration`, so the append performed the expiry. The save is exactly
  packets 4–7.
- **A fully stale run.** At t0 + 90049 ms every packet has expired. The save is `empty`, admits
  nothing and creates no file; replay awaits a keyframe, the last accepted keyframe's age is
  reported from packet 4's arrival, and a dependent packet is refused. Packet 4 then restarts the run.

Each read records its packets, bytes, `trimmed`, keyframes, arrival span, front age, last accepted
keyframe age, cause, `awaiting_keyframe` and refused dependent packets, and is compared exactly.
Before the first trim the cause is `selection`, from the session start; no cap cause ever appears.
The saved file must open with parameter sets before an IDR slice (H.264 SPS and PPS; HEVC VPS, SPS
and PPS). The final live run's provenance must be the expected pushes' packets, producer counters
and sequences. Each case isolates expiry as the only removal cause.

**Evidence.** Each case records one closed property set as GTest XML properties and one identical
console line, `POLARIS_RECORDER_EXPIRY_JSON {...}`. CTest sets `POLARIS_RECORDER_EXPIRY_EVIDENCE_DIR`
to `build/tests/recorder-expiry-evidence`. Each case creates its own directory there exclusively and
writes its 8 source packets, its published save (when one exists) and a manifest, each created
exclusively, fully written, synced, closed and read back, before any assertion, and under 1 MiB in
total. A missing, relative, non-directory or symlinked root, an existing case directory or any write
error fails the case. Raw evidence is never committed or published with shared preview artifacts.

These are in-run byte, counter and arrival-clock properties only. No packet hash is frozen in the
test. A saved suffix may inherit earlier decoder readback only when its bytes are identical to the
accepted inputs; that is a separate readback's question, with no decoder rerun here. Nothing here
is decode, playback, container, presentation timing, cap precedence, production clock or
memory-safety evidence. Recording remains off by default.

## Supplied tap times beside real fixture packets (#377)

`tests/unit/test_recorder_bitstream_timing.cpp` (`RecorderBitstreamTimingTests`, Linux only, CTest
`polaris_recorder_bitstream_timing`) pushes the same CPU fixture's 8 real H.264 and HEVC packets
(keys at 0 and 4) for one eligible session with three coordinates supplied and kept apart: the
arrival clock passed to the push (t0 + i × 20 ms), a supplied tap time as `frame_timestamp`
(t0 − 3 ms + i × 16 667 µs, before each packet's arrival; `tapToArrivalUs` = 3000 + 3333 × i) and
explicit producer counters 37700 + i as `frame_index`. The encoder's PTS/DTS are recorded as
unrelated fixture coordinates and never compared. The configured 5-minute duration and the default
caps hold the whole 140 ms run, so nothing expires, trims or realigns. Four cases, two per codec:

- **Steady tap times.** All 8 pushes are admitted. The live provenance read before the save and the
  held snapshot provenance of the admitted save (`snapshot_provenance_for_tests`) both carry every
  supplied field exactly, version 1, with no mark. The save is packets 0–7 byte for byte, 2
  keyframes, arrival span exactly 140 ms, and the live run is unchanged afterwards. From the observed
  held records alone the test derives `tapOffsetsUs`, normalized to the first held entry's tap time:
  exactly 0, 16667, 33334, 50001, 66668, 83335, 100002, 116669, strictly increasing, `tapSpanUs`
  116669. `ArrivalSpanIsNotTapSpan`: the observed arrival span (140 000 µs, from the save and the
  run) differs from that tap span. The two coordinates are compared as complete vectors and final
  spans; both are 0 at the first frame, so no offset-by-offset inequality is asserted.
- **Missing, duplicate and regressed tap times.** Packet 2 is untimed, packet 5 repeats packet 4's
  tap time and packet 6 is 1 µs earlier than packet 5. The run, the save and the bytes are those of
  the steady case. The observed marks, live and held, are exactly none, none, `time_missing`, none,
  none, `time_duplicate`, `time_regressed`, none (packet 3 is not compared because its neighbour is
  untimed); the derived `tapOffsetsUs` are 0, 16667, null, 50001, 66668, 66668, 66667, 116669, not
  strictly increasing, first violation at 5, untimed [2], with nothing substituted from the arrival
  clock. A supplied tap-time regression never cuts the run; the recorder's own arrival-clock
  regression rule is a different, untouched thing.

The marks are also recomputed by the test from the observed supplied values with the adjacent-entry
rule above and must equal the observed marks (valid here because nothing trims, so each entry's
adjacent retained entry is the previous one). The saved file must open with parameter sets before
an IDR slice; its first access unit's NAL types are recorded.

**What this is not.** The supplied tap series is an injected test value standing where the production
tap copies `packet->frame_timestamp`; it is not a presentation timestamp, a measured presentation
duration, cadence, ordering, seeking, a container, audio or A/V sync, and the real cadence or jitter
of production tap times is unmeasured. The before state of this slice is missing real-packet timing
coverage, not a demonstrated product failure.

**Evidence.** Each case records one closed property set as GTest XML properties and one identical
console line, `POLARIS_RECORDER_TIMING_JSON {...}`, all durations in whole microseconds. CTest sets
`POLARIS_RECORDER_TIMING_EVIDENCE_DIR` to `build/tests/recorder-timing-evidence`. Each case creates
its own directory there exclusively and writes its 8 source packets, its published save and a
`manifest.json` (schema version 1) whose `retained` and `held` sections serialize the provenance the
recorder actually returned (sequence, payload size and hash, version, `frameIndex`, `tapOffsetUs`
relative to t0, source flags, marks; a held entry's sequence is the snapshot API's sentinel 0 and
its payload comes from the copied snapshot payload) and whose `derived` section is recomputed from
those held records, separately from the declared inputs in `pushes` and the declared `expected`
values. `tapToArrivalUs` is recomputed per entry from the observed held tap time, so the duplicate
and regressed entries of the marks case carry their own values and the untimed entry is null. Each
file is created exclusively, fully written, synced, closed and read back, before any assertion, and a
case stays under 1 MiB. A missing, relative, non-directory or symlinked root, an existing case
directory or any write error fails the case. Raw evidence is never committed or published with
shared preview artifacts. No packet hash is frozen in the test; whether a save's bytes equal the
inputs an earlier decoder readback accepted is a separate readback's question, with no decoder rerun
here. Recording remains off by default.
