# Historical packet diagnostics

The optional `packet_dequeue` event carries the immutable encoder setting and
separate PyroWave work receipt already attached to that packet. It observes a
historical API setting or successful wrapper call with nonempty output. It does
not establish current controller permission, GPU completion, measured bitrate,
quality, network delivery or physical playback. Native initialization without an
attributable accepted operation remains unavailable. A queued R1 stays R1 after
R2 and after its encoder retires.

The existing common sender seam emits this event after packet attachments, under
its original `vrr_trace::enabled()` predicate, with one `record()` call. There are
no broker getters, current-owner lookups, backend changes or new sender I/O. The
field builder copies fixed scalar values and static strings into the existing
fixed event; it adds 19 fields to the original eight, using 27 of 40 slots. The
writer remains the only serialization path. This is off by default under the
existing private `POLARIS_VRR_TRACE_V2` collection mechanism; it introduces no
stream configuration, setting output, pacing, wire, UI or policy change.

## Raw contract 2.3

The header is `schema=polaris.vrr-host-trace`, integer `major=2`, integer `minor=3`.
The minor intentionally changes from 2 to 3. Existing v1 and older raw readers keep
their own refusal predicates. The new standalone standard-library inspector
accepts exactly 2.3 and does not adapt old traces by guessing from fields.

Each packet row has exactly 30 unique JSON keys: the writer's `type`, `order`,
`observed_steady_ns`, the original eight fields below, and the 19 added fields.
No raw host, process or peer identities are included in the inspector summary.

| Fields | Meaning |
| --- | --- |
| `packet`, `encoder`, `submission` | Existing trace identities; zero means unavailable. They do not supply missing record identity. |
| `frame_index`, `frame_timestamp_ns`, `encode_done_ns`, `source_clock_timestamp`, `source_time_paced` | Original output PTS, optional times and numeric 0/1 flags, unchanged. |
| `record_stage` | Static `sender_packet_dequeue`. |
| `frame01_available` | Numeric 0/1. When zero, all eight setting payload fields are explicitly null. |
| `frame01_encoder_lifetime`, `frame01_stream_generation` | Historical record scope, separate from trace IDs. |
| `frame01_application_sequence`, `frame01_origin_revision`, `frame01_submission_sequence` | Original positive record identifiers; origin revision is not a currentness check. |
| `frame01_configured_encoder_kbps`, `frame01_input_pts` | Positive configured API setting and exact packet output PTS, not a measured rate. |
| `frame01_receipt_origin` | Static `successful_open` or `backend_reconfiguration`. |
| `pyrowork_available` | Numeric 0/1. When zero, all eight work payload fields are explicitly null. |
| `pyrowork_authority`, `pyrowork_provenance` | Static `unknown`, `wrapper_call_success_nonempty`. |
| `pyrowork_kind` | Static `imported`, `blank_primer`, `blank_hdr`, `packed` or `retained`. |
| `pyrowork_sequence`, `pyrowork_handoff_frame_index` | Wrapper-local work sequence and exact packet handoff PTS. |
| `pyrowork_budget_argument_bytes`, `pyrowork_completed_bitstream_bytes`, `pyrowork_packet_copy_bytes` | Positive byte argument, completed output length and packet copy length. The argument is neither a cap nor measured bitrate; output may exceed it. |

Work sequences are local to a wrapper incarnation and may repeat across wrappers.
Only the fields in a single row are associated. The inspector never joins work
records by sequence, substitutes a trace identity for a setting scope, compares a
revision with a latest request, or reconstructs authority.
The original signed `frame_index` may be negative, including `AV_NOPTS_VALUE`.
Such a packet can have both historical records unavailable. Present setting or
work receipts still require their original nonnegative exact matching PTS; the
inspector never guesses or replaces that index.

## Passive FEC selection at `send_constants`

The separate existing `send_constants` event adds six fixed fields at block 0,
under its original opt-in `vrr_trace::enabled()` predicate. Its existing `packet`
trace ID joins it to the frame's packet history; it does not supply missing FEC
identity. The event uses 14 of the existing 40 field slots. There is no additional
event, observer, ring, writer or packet-record field, and no raw-version change.
The packet inspector above does not validate or interpret these separate fields.

| Fields | Meaning |
| --- | --- |
| `fec_selection_basis` | Static `encoder_confirmed_trial`, `configured_baseline` or `unavailable`. |
| `fec_selection_generation` | Matching configured publication scope, or null when unavailable. It is not a unique baseline publication epoch. |
| `fec_selection_authority_revision` | Exact pending authority revision checked by the successful encoder confirmation that published the selected trial; otherwise null. It is separate from encoder/operator revisions and trace IDs. |
| `fec_selection_from_frame` | Positive accepted next-frame boundary of that same selected trial; otherwise null. |
| `fec_configured_percentage` | Nominal configured F0 from the selected publication; null when unavailable. |
| `fec_nominal_percentage` | Nominal allocation selected for this frame before FEC fitting or oversized fallback; null when unavailable. |

Allocation and identity come from one atomic read of the same immutable shared
publication, once per frame. Thin allocation/percentage wrappers preserve the
existing values. The sender carries that scalar selection through packetization;
it does not read a newer publication to label an older allocation. A subsequent
restore or publication cannot relabel the already selected tuple. This observes
selection at the packetizer, not the publication in effect at encoder submission
or permission current at event emission. The atomic shared-pointer read is not
claimed to be lock-free; no new controller lock, wait, poll, SDK call or sender
allocation is added.

Only a confirmed raise at a positive boundary, in the matching generation and
at or after that boundary, has trial identity. Pending, refused, stale-authority,
same-percentage ABA and wrong-generation requests cannot provide it. A configured
baseline, a restore and a queued frame before the raised boundary report
`configured_baseline` with configured scope and value only: revision and boundary
are null. A restore reuses the preallocated baseline without allocation, and even
a later encoder acknowledgement of the restore does not give baseline a trial
identity. No baseline epoch is manufactured; zero, the latest request and a
future trial cannot supply a missing confirmed revision.

Absent/mismatched publications and nonpositive frame arguments report
`unavailable`, with all five payload fields null. Their existing allocation values
remain unchanged, including fallback and invalid/zero-frame behavior. They never
borrow another generation's identity. Nominal allocation can differ from the
existing `fec_percentage`, which keeps its fitted/constructed packetizer meaning
after fit and oversized fallback. Neither field measures actual sent bytes,
achieved bitrate, client recovery, causal loss or FEC benefit; the checked revision
does not confer policy authority or change percentage-only `fec_applied` behavior.
Wire/audio/warp accounting and pacing behavior are unchanged.

New cases in the existing `LiveTuningFecActuator` group exercise exact checked
revision and boundary, pending/refused/stale/ABA identity, restore/reconfiguration,
foreign generations, invalid frame allocation compatibility and concurrent tuple
coherence. They do not exercise the real sender or an encoder SDK. Compilation,
the actual native roster, event serialization and owned-stream behavior require
separately selected exact-head execution; static source checks cannot supply them.

## Inspect private bytes

After an independently authorized collection has closed, run from the source
checkout (the two arguments name private local files):

```sh
python3 -B scripts/validation/live-tuning/inspect_frame_records.py RAW_JSONL CLOSE_JSON
```

The inspector reads regular files with leaf symlinks refused. It bounds raw bytes
to 256 MiB, events to 2,000,000 and close bytes to 64 KiB, and respects smaller
advertised writer budgets and the writer's 64 KiB footer reserve. Offline memory
use is bounded by those limits; no inspector work runs on the sender path.
Duplicate keys at any depth, malformed/nonfinite JSON, unsupported versions,
incorrect scalar types/ranges, missing/extra packet keys, inconsistent nulls,
unknown record enums, wrong PTS/copy lengths and invalid event order are refused.
The actual writer uses integers for flags; JSON booleans cannot replace them.

A complete result requires one header, ordered event rows, one terminal footer,
zero loss/cap/refusal/overflow, matching counts and the exact original-byte
`content_sha256`/`bytes_before_footer`. It then requires the separate close receipt
with successful write, footer and main-close facts and matching final main-file
bytes/SHA256. Footer and close receipt have major 2 and no minor field, as emitted
by the production writer. Missing, truncated, capped, lost, refused, unclosed or
mismatched input yields `INCONCLUSIVE`, `records=null` and exit status 1. It never
turns missing observations into measured zero or emits surviving rows as complete.
A complete trace with no packet rows is also unavailable for this inspector.

`COMPLETE_HISTORICAL_DIAGNOSTIC` (exit 0) means only complete packet history under
this raw-byte contract. Other event semantics, clocks, host identities and
current permission remain unassessed. The build commit string is informational;
null or empty means absent. Header/footer consistency does not bind a running
binary, and even a full commit string leaves `source_binary_binding=unassessed`.
A separately owned host/package/binary receipt and reviewed producer/reader hashes
are needed before making any external source association. This script does not
accept or manufacture that receipt. It retains `no_fsync_claim` and
`receipt_own_close=not_observable_in_this_file`.

Raw traces contain private host/process/peer facts. Keep raw files and close
receipts private. Summary output omits those facts, unrelated event fields, input
paths and exception text; publish neither raw files nor identifying command
arguments. Hashes are integrity bindings, not authenticity or device acceptance.

## Guards and execution boundary

The new fast C++ test TU is linked into the existing selected `test_polaris` route.
It guards queued R1 after R2 and retirement, missing records despite trace IDs,
queued PyroWave byte arguments with unknown authority, usable exact FFmpeg-open
metadata versus unattributed native open, all five work kind strings and exact
27-slot uniqueness. A Linux test passes the shared fields through the production
recorder and reads actual JSON, minor 3, explicit nulls, both setting origins,
positive work fields, footer hashes/counts and the separate close receipt. It also
checks that a disabled recorder scope adds no event. These public operation
observations are metadata tests; they do not execute an FFmpeg/NVENC SDK call or
the real sender loop. An owned-session trace would supply that later witness.
A separate Linux guard passes actual production-recorder raw and close bytes to
this inspector through a single argument-list child invocation. Since the producer
moved to 2.4, the guard requires the inspector's exit status 1 and its typed
`unsupported_version` refusal, never a complete diagnostic. Hand-authored fixtures
alone cannot provide that producer/reader interoperability witness; the unchanged
fixtures keep the positive 2.3 coverage.

One Unix GTest invokes the exact source-relative Python fixture file once with
`posix_spawnp`, argument list `python3 -B FILE -v`, inherited stdout/stderr and no
shell, retry or fallback. It asserts child exit status. Verbose unittest names,
counts, skips and failures remain visible in CI/CTest logs; a wrapper PASS cannot
replace independent qualification of the inner roster. Outside Unix it explicitly
skips; the recorder round-trip separately skips outside Linux. No workflow,
selector, required-status or production dependency changes are needed.

For direct fixture diagnosis after a selected execution gate:

```sh
python3 -B scripts/validation/live-tuning/test_inspect_frame_records.py -v
```

Fixtures independently declare a producer-shaped envelope and packet fields.
They cover positive history, both origins/all kinds, stale R1, missing records,
versions, duplicates/malformed JSON, null/type/range relationships, byte/hash/order/
count integrity, losses/caps/refusals, truncated/footer/close mismatch and private
input failures. A separately selected first execution must retain real product
`stream.cpp`/`vrr_trace.cpp` compile and link witnesses, test TU/support compile
and link witnesses, the exact native/Python roster and actual cases/skips/logs.
Static preparation or earlier CI for a different source cannot establish any of
those results, package behavior, media, hardware or full Live Tuning acceptance.
