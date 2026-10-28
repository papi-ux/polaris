# Opt-in parser fuzzers

These standalone developer targets exercise the production config/app JSON validators,
the selective-FEC feedback payload parser, the raw GameStream query/key-material
decoders, the adapted RTSP message parser with its request Content-length
decoder, and the typed control-stream message decoders, all described below. They
build only when explicitly enabled; ordinary source CI does not compile or run them.
A bounded campaign can find a failure, but a clean run is not an exhaustive security
guarantee, qualification of a different toolchain, or package or hardware acceptance.

## Source and behavior

`POLARIS_BUILD_PARSER_FUZZERS` defaults to `OFF`. This directory is not included by
the product or test CMake projects. A standalone OFF configure declares no languages,
requests no compiler or JSON package, and creates no fuzz executable. Normal builds
have no new target, instrumentation, dependencies or behavior.

ON compiles `fuzz_confighttp_json.cpp` together with the **unchanged production**
`src/confighttp_validation.cpp`. The root is resolved from `tests/fuzz/../..`; its
real `confighttp_validation.h` and `app_launch_as.h` are used directly. Their source
dependencies are the C++ standard library and an already installed
`nlohmann_json::nlohmann_json` package, version 3.11 or newer. There is no download
or FetchContent fallback, product-library link, server instance or configuration write.

Each value accepted by the JSON parser reaches both `validate_config_payload` and
`validate_app_payload`. Only a config value that passes validation reaches
`normalize_write_only_secret_payload`; app validation sees the original value.
Parsing uses `allow_exceptions=false` and returns on `is_discarded()`, so expected
parse rejection includes the documented numeric-overflow case `1E400`, not only
syntax errors. See upstream [number limits](https://json.nlohmann.me/features/types/number_handling/#number-limits),
the [parse return contract](https://json.nlohmann.me/api/basic_json/parse/#return-value),
and the [v3.11.3 parser implementation](https://raw.githubusercontent.com/nlohmann/json/v3.11.3/include/nlohmann/detail/input/parser.hpp).
No catch surrounds the validators or normalization: unexpected validator exceptions
and sanitizer failures remain visible. Command/path strings in seeds are only values
being validated; the harness never executes them.

This omits route authentication/content-type/admission, response-only config-key
preprocessing in `read_config_payload`, persistence, and other JSON route decoders.
RTSP framing, dispatch and handlers outside the message slice below and nvhttp
query/launch validation outside the key-material slice below still need separately
owned targets.
The FEC target below covers only its named payload parser; the typed control-stream
decoder target below covers the other typed client->host messages' first byte-level
decode, but neither covers decryption, the control dispatch loop, session side effects
or the inner re-dispatch of a decrypted message. Generic JSON syntax
coverage or Nova XML cannot substitute for those areas. Keep campaigns separate
from ordinary per-push builds, using a manual or nightly lane with an explicit
source, toolchain, time and resource budget.

## Prerequisites for a separately authorized Linux run

The source currently targets **native Linux x86_64**, LLVM Clang 18 or newer with
its matching compiler-rt **libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer**
runtimes, CMake 3.22+, and a C++20 standard library. Use an installed toolchain
with those runtimes. Select the C++ compiler
explicitly with `CMAKE_CXX_COMPILER` or `CXX`. ON configuration performs a small
compile/link prerequisite check; it does not execute the result or qualify a runtime.
Use a fresh build directory so a cached prerequisite result cannot stand in for
current toolchain proof. All actual target sources receive target-local
`-fsanitize=fuzzer-no-link,address,undefined`, `-O1 -g`, frame pointers and disabled
sanitizer recovery; the executable links `-fsanitize=fuzzer,address,undefined` with
IPO disabled. Other platforms and compiler families fail explicitly.

Before configuring, building or running, record and review the toolchain: retain
compiler/build and local JSON-package identity, compiler resource-directory runtime
hashes, installed package metadata and immutable image ID/digest. A familiar image
tag or successful ordinary source CI proves none of these prerequisites. Do not
install or fetch missing tools/dependencies as part of this target's invocation.

The following recipe runs one bounded local campaign after those prerequisites
have been checked. Run from the product root, setting `fuzz_evidence`
to a new absolute directory outside the checkout. Set `nlohmann_json_DIR` or
`CMAKE_PREFIX_PATH` to the reviewed local package if it is not found in system paths.

```sh
set -euo pipefail
: "${fuzz_evidence:?Set a new absolute evidence directory outside the checkout}"
test ! -e "$fuzz_evidence"
mkdir -p "$fuzz_evidence/corpus" "$fuzz_evidence/crashes"
cp tests/fuzz/corpus/confighttp_json/* "$fuzz_evidence/corpus/"
cmake -S tests/fuzz -B "$fuzz_evidence/build" \
  -DPOLARIS_BUILD_PARSER_FUZZERS=ON \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$fuzz_evidence/configure.log" 2>&1
cmake --build "$fuzz_evidence/build" --target fuzz_confighttp_json --parallel 2 \
  >"$fuzz_evidence/build.log" 2>&1
"$fuzz_evidence/build/fuzz_confighttp_json" "$fuzz_evidence/corpus" \
  -max_total_time=60 -max_len=65536 -rss_limit_mb=1024 -timeout=5 \
  -artifact_prefix="$fuzz_evidence/crashes/" -print_final_stats=1 \
  >"$fuzz_evidence/run.log" 2>&1
```

The driver also refuses inputs over 65,536 bytes; this is a harness resource bound,
not a new HTTP limit. The recipe sets one 60-second libFuzzer budget, a 5-second
per-input timeout and a 1024 MiB RSS limit. It runs one process with no retry or
parallel fuzz workers; these settings do not guarantee exact wall-clock or host
resource usage. Keep the first nonzero exit, configure/build/run logs, exact source
and binary/toolchain/package identities, original corpus hashes, resulting corpus,
and every crash/timeout/OOM artifact. Stop on a failure, file a separate issue
and keep the reproducer as a regression case. Keep parser behavior fixes separate
from the harness.

## Small synthetic seed corpus

Eleven JSON files serialize existing test payloads from base
`7c51ddc42236abcfa5278c7d2ed88cc8eee96411`. All values are synthetic test inputs;
there are no private captures, credentials or user data. The three parser boundary
files below are explicit additions, including one from the upstream number-limit
documentation. Keep the original seed bytes separate from the corpus a campaign
creates, so a later run can distinguish its starting inputs from discovered ones.

Unless noted otherwise, cases are in
[`tests/unit/test_confighttp_validation.cpp`](../unit/test_confighttp_validation.cpp).
`app-mode-*` selects one value per case from the existing
[`app-launch-as-v1.json`](../fixtures/app-launch-as-v1.json) fixture, as that test does.

| File | Existing test/case origin |
| --- | --- |
| `config-bad-key.json` | `ConfigValidationTests.RejectsConfigKeysThatCanBreakSerialization` |
| `config-line-break.json` | `ConfigValidationTests.RejectsConfigValuesWithEmbeddedLineBreaks` |
| `config-unknown-key.json` | `ConfigValidationTests.RejectsUnsupportedConfigKeys` |
| `config-clear.json` | `ConfigValidationTests.AcceptsAiApiKeyClearForPersistence` (empty placeholder only) |
| `config-bad-clear.json` | `ConfigValidationTests.RejectsNonBooleanAiApiKeyClearFlag` |
| `app-valid.json` | `AppValidationTests.AcceptsAWellFormedAppPayload` |
| `app-bad-uuid.json` | `AppValidationTests.RejectsMalformedUuidValues` |
| `app-bad-env.json` | `AppValidationTests.RejectsInvalidEnvironmentVariables` |
| `app-bad-command.json` | `AppValidationTests.RejectsUnexpectedCommandShapes` |
| `app-mode-valid.json` | `AppLaunchAsValidationTests.AcceptsOnlyExactSharedModesAndLegacyBasis`, `values[].id = headless_stream` |
| `app-mode-invalid.json` | Same case, `rejected[] = Host_Default` |
| `empty.json` | Added parser boundary: zero bytes |
| `truncated.json` | Added parser boundary: truncated object/value |
| `numeric-overflow.json` | Upstream [Number limits](https://json.nlohmann.me/features/types/number_handling/#number-limits): literal `1E400`, documented parsing `out_of_range.406`; [v3.11.3 parser](https://raw.githubusercontent.com/nlohmann/json/v3.11.3/include/nlohmann/detail/input/parser.hpp), non-finite `value_float` rejection |

## Selective-FEC feedback payloads

`fuzz_control_fec` includes the unchanged production `src/stream_fec_status.h`
directly and calls `stream::fec_status::parse`. Each input is the raw decrypted
21-byte payload, excluding the little-endian control header; there is no harness
prefix. The driver calls the parser with capability flags `0`, `1`, `3` and
`0xffffffff` to cover the advertised-feature check and unrelated feature bits.
A volatile result consumes the real refusal and decoded fields, including the
two derived shard counts, so optimization cannot discard the decoding. No
parser checks are copied into the driver and expected refusals are not failures.
The target uses the same target-local instrumentation as the JSON target.

The driver's 64-byte cap and the recipe's `-max_len=64` are fuzz resource bounds.
They do not change the production parser's exact-size requirement. This target
has no networking, decryption, session, configuration or persistence. It does not
exercise `tracker_t`, loss statistics, IDX invalidation/input dispatch, FEC policy,
actual shard repair, packet delivery or Live Tuning. It is one slice of the
control-stream area in #505; that area remains incomplete.

The existing standalone ON configure also checks the JSON target's installed
package prerequisite. After that same reviewed-toolchain configure, build only
`fuzz_control_fec` for a separate FEC campaign. Use a new external absolute
`fuzz_evidence` directory with `corpus` and `crashes` subdirectories, and copy only
`tests/fuzz/corpus/control_fec/*` into its corpus. The build/run part is:

```sh
cmake --build "$fuzz_evidence/build" --target fuzz_control_fec --parallel 2 \
  >"$fuzz_evidence/build.log" 2>&1
"$fuzz_evidence/build/fuzz_control_fec" "$fuzz_evidence/corpus" \
  -max_total_time=60 -max_len=64 -rss_limit_mb=1024 -timeout=5 \
  -artifact_prefix="$fuzz_evidence/crashes/" -print_final_stats=1 \
  >"$fuzz_evidence/run.log" 2>&1
```

Use the same first-failure retention and source/toolchain/binary/corpus identity
rules above. A previous JSON campaign does not qualify this target, and adding
this source does not claim an actual configure, build or fuzz result. Do not rerun
an unchanged JSON campaign as a substitute.

### FEC seed origins

All twenty binary seeds serialize literal inputs from `tests/unit/test_stream_fec.cpp`
at `371845d0b53f9ece149981eb61e6a411690e575b`. They contain no private captures.
`valid-block.bin` is the exact `fec_wire` literal; the others apply the existing
test mutations or lengths to that literal, except `extra-byte.bin`, which is the
test's zero-filled 22-byte array. The empty file is retained even though libFuzzer
normally counts only nonempty starting files.

| Files | Existing `StreamFecStatusParserTests` origin |
| --- | --- |
| `valid-block.bin` | `DecodesUnalignedBigEndianPayloadAndReportedBlockFacts` / `fec_wire` |
| `effective-200-percent.bin`, `effective-66-percent.bin` | `AcceptsEffectivePercentageAbovePolicyCapAndClientRounding` |
| `parity-free-wide.bin` | `AcceptsParityFreeOversizedDataAndWideCounters` |
| `zero-data.bin`, `too-many-data.bin`, `too-many-received-data.bin`, `too-many-received-parity.bin`, `bad-block-index.bin`, `zero-block-count.bin`, `too-many-blocks.bin` | `RefusesMalformedDataReceiveCountsAndBlockIdentity` |
| `parity-mismatch.bin`, `zero-percentage.bin`, `protected-too-large.bin`, `protected-255.bin`, `missing-overflow.bin` | `ChecksClientParityRelationProtectedLimitAndMissingBounds` |
| `empty.bin`, `legacy-16.bin`, `truncated-20.bin`, `extra-byte.bin` | `RequiresAdvertisedCapabilityAndExactCurrentLength` |

## Raw GameStream launch/resume key-material queries

`fuzz_nvhttp_launch_query` calls the pinned SimpleWeb `QueryString::parse` used by
`Request::parse_query_string`, then the real production `nvhttp_query.cpp` helpers.
The input is only the raw query text, without a request line, `?` prefix or
fuzzer-specific framing. It does not create a server or a session.

The extraction keeps `get_arg`'s lookup, copied value, absent-field default and
`std::out_of_range(name)` behavior. The three pure field decoders use the existing
`util::from_hex_vec`, `util::from_view` and 32-bit endian conversion unchanged.
Product calls remain at their original statements: malformed key length still
logs/refuses before numeric decoding; corever is decoded after key assignment;
rikeyid is decoded after cipher/scheme setup, both random draws and IV allocation.
The shared 16-byte driver bound is asserted against the product's AES key size.

The driver consumes every decoded key byte, corever and the IV prefix through a
volatile result. It returns before numeric decoding when the key is not 16 bytes.
Each missing-required-field catch is scoped to that field's helper and confirms
that field is absent; unexpected exceptions and sanitizer failures are not hidden.
Absent corever still defaults to `"0"`; empty numeric values still mean zero and
short nonnumeric values keep the old arithmetic, rather than becoming validation
errors. `rikeyid` is narrowed to uint32 before its endian conversion, producing
the object bytes the existing product copy uses.

Parsing semantics come from the pinned SimpleWeb headers: names stay raw, values
are percent-decoded and `+` becomes space, keys are case-insensitive, empty names
are omitted, and bare names have empty values. Duplicate selection remains the
real multimap's `find`; no particular duplicate winner is promised. The hex
decoder remains lenient: all ASCII letters count as convertible, other characters
are skipped, and an odd count drops the leftmost convertible character because
pairs are consumed from the right. For example `abc` yields one byte `bc`.
These are compatibility facts, not new admission rules.

### Build, resource and coverage limits

The default-OFF gate, native Linux x86_64/Clang 18+/C++20 checks, target-local
ASan/UBSan/libFuzzer flags and disabled IPO are shared with the existing JSON
harness. This target additionally needs the already materialized pinned
`third-party/Simple-Web-Server/{utility.hpp,status_code.hpp}` and the existing
installed nlohmann_json package used by `utility.h`; nothing is fetched or
provisioned by this target. Its build compiles the real helper cpp with the driver,
without `nvhttp.cpp`, OpenSSL, asio, a product library, a server or a live session.
The new pure helper is also in the ordinary production source list; only the
fuzzer executable/instrumentation stays outside ordinary builds.

The driver refuses more than 65,536 bytes as a harness resource bound. A future
separately admitted campaign would copy only `corpus/nvhttp_launch_query/*` into a
fresh external corpus, build only `fuzz_nvhttp_launch_query`, and use the existing
60-second campaign, `-max_len=65536`, 5-second per-input timeout and 1024 MiB RSS
limits, preserving the first failure and all exact source/tool/binary/corpus
identities. These settings do not authorize a run or waive host admission.

The manual campaign lane can select this target, and ordinary source CI builds and
tests the extraction; it still does not compile or run the fuzzer. The first
60-second campaign on this target stopped on an undefined-behaviour signed overflow
in `util::from_chars`, reached through `rikeyid`. That decoder is fixed separately
by checked decoding, which this branch now includes, so a new campaign on this
source is its own run with its own first outcome. Do not replay an unchanged JSON
campaign as a substitute.

This covers only raw query parsing and the three named client-key field decoders.
It does not exercise route/auth/presence admission, crypto or RNG, session/worker
state, other launch fields, pre-auth appid conversion, pairing, request-line or
header parsing, RTSP, typed control/FEC, or a real launch/resume. It supplies no
full #505, security, package, hardware, device or release acceptance.

### Exactly eight synthetic query seeds

The Polaris literal origins below are at
`371845d0b53f9ece149981eb61e6a411690e575b`; SimpleWeb is pinned to
`5b29c7004cc53252a8eff06d62899c8847ada971`. These ASCII files have no trailing
newline and contain no private captures. Field-subset serializations are labelled
as such; added boundaries are not presented as existing full test requests.

| File | Origin or explicitly added boundary |
| --- | --- |
| `empty.query` | Existing exact empty literal, SimpleWeb `tests/parse_test.cpp:180` |
| `valid-key.query` | Serialize `launch_args_for_key_bytes(16)` literals from `tests/unit/test_launch_session_key.cpp:29-34`: 32 zero characters, rikeyid=1, mode=1920x1080x60 |
| `encrypted-rtsp.query` | Serialize only the rikey/rikeyid/corever literals in `MultiseatProfileHttp::args`, `tests/unit/platform/test_multiseat_launch_service.cpp:1267` |
| `short-key.query` | Serialize only the rikey=00112233/rikeyid=1/corever=1 fields in `LaunchDesktopSteamTest.LaunchesRefusedBeforeTheSteamStepLeaveSteamAlone`, `tests/unit/test_launch_desktop_steam.cpp:376` |
| `missing-rikeyid.query` | Added boundary: remove rikeyid from valid-key |
| `empty-fields.query` | Added boundary: valid key with explicit empty corever/rikeyid |
| `odd-key.query` | Added boundary: prepend convertible f to the 32-zero key; its leftmost character is ignored |
| `percent-and-duplicates.query` | Added boundary: percent-encoded zero key and equivalent duplicate/case key, raw percent in an unrelated name, unrelated truncated percent value; no duplicate winner assumption |

The empty file remains part of the original corpus identity even when libFuzzer
counts only nonempty initial files.

## Raw RTSP request messages

`fuzz_rtsp_message` passes the raw input bytes to the production `parseRtspMessage`
and releases a parsed message with the production `freeMessage`. It compiles the
**same adapted build-tree copy** of moonlight-common-c's `RtspParser.c` as the product:
the standalone configure includes `cmake/dependencies/rtsp_parser.cmake` and calls
`polaris_prepare_rtsp_parser`, so the four pinned input hashes, the duplicate-node
ownership adaptation and the copied header closure are the product's own. The
unadapted third-party file is never compiled. The input is the message exactly as
`src/rtsp.cpp` passes it, plaintext or already decrypted, without a fuzzer prefix or
the encrypted-RTSP header.

After a successful parse, the driver reads what `print_msg` and the request path read
for that message type: protocol, message buffer, request command and target or
response status, every header name and value, and each payload byte. It then calls
`rtsp_stream::find_content_length`, the plaintext Content-length lookup extracted from
`handle_plaintext_payload`. A volatile result consumes those reads. Parse rejection
is an expected outcome; no parser rule is copied into the driver.

The extraction keeps the loop, the exact case-sensitive `Content-length` name, the
first matching node, the skip to the first digit, `util::from_chars` and its narrowing
to `int`. The product still logs the found value and then compares the received payload
with the declared length at its original statement. `RtspContentLength` pins the
decoding, and a source contract pins the call between the successful parse and the
payload completeness check. These are compatibility facts, not admission rules:
`Content-Length` and other spellings are ignored, `-5` reads as 5, bytes after the
first digit keep the from_chars arithmetic (`12abc` is 17451), a value from_chars
cannot represent is 0, and a value above `INT_MAX` wraps.

### Build, resource and coverage limits

The default-OFF gate, native Linux x86_64/Clang 18+/C++20 checks, target-local
ASan/UBSan/libFuzzer flags and disabled IPO are shared with the other targets; the
same flags also compile the C parser copy. This target requires the pinned
`third-party/moonlight-common-c` gitlink to be materialized before configure. Without
it, the other targets' configure is unchanged, C is not enabled, and building
`fuzz_rtsp_message` fails with a message. With it, configure enables C using the C
driver installed beside the selected Clang C++ compiler, or the explicitly selected
`CMAKE_C_COMPILER`, requires the same Clang version, and fails on any pinned input hash
difference, as the product build does. `utility.h` also needs the installed
nlohmann_json package. Nothing is fetched or provisioned; there is no product library,
socket, session, crypto or server. The new helper is also in the ordinary production
source list and the fast test build; only the fuzzer stays outside ordinary builds.

The driver refuses more than 2,048 bytes and the recipe uses `-max_len=2048`. This is
a harness resource bound. It equals the 2,048-byte RTSP receive buffer that bounds
every plaintext or decrypted message the product parses, and adds no RTSP limit. After
the reviewed-toolchain ON configure, copy only `tests/fuzz/corpus/rtsp_message/*` into
a new external absolute `fuzz_evidence` corpus and use the existing 60-second,
5-second per-input timeout and 1024 MiB RSS limits:

```sh
cmake --build "$fuzz_evidence/build" --target fuzz_rtsp_message --parallel 2 \
  >"$fuzz_evidence/build.log" 2>&1
"$fuzz_evidence/build/fuzz_rtsp_message" "$fuzz_evidence/corpus" \
  -max_total_time=60 -max_len=2048 -rss_limit_mb=1024 -timeout=5 \
  -artifact_prefix="$fuzz_evidence/crashes/" -print_final_stats=1 \
  >"$fuzz_evidence/run.log" 2>&1
```

Use the same first-failure retention and source/toolchain/binary/corpus identity
rules, and also keep the gitlink commit and the four pinned input hashes. Adding this
source claims no configure, build or campaign result, and these settings do not
authorize a run or waive host admission.

This covers the message parser and the plaintext Content-length decoder only. It does
not exercise socket reads, the plaintext header-terminator search and buffering across
reads, encrypted-RTSP header and length checks, AES-GCM decryption, launch-session
admission, the command lookup in `handle_msg`, or any handler: `OPTIONS`, `DESCRIBE`,
`SETUP` target parsing, `ANNOUNCE` SDP attribute parsing and conversion, and `PLAY`.
Those need a server, socket or launch session, or a larger extraction.
`serializeRtspMessage` formats host responses rather than received bytes. This is one
slice of the RTSP area in #505; it supplies no full #505, security, package, hardware,
device or release acceptance.

### Exactly eight synthetic RTSP seeds

The fixture origins are literals in
[`tests/fixtures/rtsp_option_ownership.c`](../fixtures/rtsp_option_ownership.c) at
`61349e606322c48b28f1e3cc8b6df4bb399490ad`, run by `RtspOptionOwnershipTests`. No other
RTSP message literal exists under `tests/unit`, and the pinned moonlight-common-c has
no parser tests. Files are exact wire bytes with CRLF line ends and no added trailing
newline; `.gitattributes` keeps them out of text conversion. They contain no private
captures. Added boundaries are not presented as existing test requests.

| File | Origin or explicitly added boundary |
| --- | --- |
| `fixture-normal.rtsp` | Exact `normal` literal, `NormalHeadersReleaseTheirParserAllocations` |
| `fixture-duplicate.rtsp` | Exact `duplicate` literal, `DuplicatesKeepFirstOrderLastValueAndCaseSensitiveOwnership` |
| `fixture-malformed.rtsp` | Exact `malformed` literal, `MalformedTailReleasesHeadersIncludingDuplicates` |
| `empty.rtsp` | Added boundary: zero bytes |
| `single-crlf-end.rtsp` | Added boundary: `fixture-normal` without its final CRLF, the parser's single-CRLF end form |
| `content-length-payload.rtsp` | Added boundary: `fixture-normal`'s request line and CSeq, then `Content-length: 4` and the synthetic payload `abcd` |
| `rfc-spelling.rtsp` | Added boundary: the same request spelled `Content-Length`, which the exact-name lookup ignores |
| `response.rtsp` | Added boundary: response first line `RTSP/1.0 200 OK` with `fixture-normal`'s CSeq |

The empty file remains part of the original corpus identity even when libFuzzer
counts only nonempty initial files.

## Typed control-stream messages

`fuzz_control_stream` compiles the **unchanged production** `src/control_messages.cpp`
with the driver and calls the real control-message decoders through `control_messages.h`.
That source is the single owner of the first byte-level decode each control-thread handler
in `src/stream.cpp` performs; the handlers were rewritten to call it with no behaviour
change, and `tests/unit/test_source_safety_contracts.cpp` pins that stream.cpp keeps no
second copy. There is no networking, ENet peer, session, decryption, input injection,
thread, configuration or persistence; `utility.h`'s endian helpers (which include
`nlohmann/json.hpp`, the only dependency) are used unchanged.

The input's first byte selects the message type the dispatcher would route to, and the
rest is the payload the handler receives after the 2-byte type field is stripped. The
selector is taken `% 5` across the five decoders; an empty input has no selector and does
nothing. The driver first calls `control_packet_carries_type` on the reconstructed full
packet length, as the dispatcher does, then drives the selected decoder and consumes every
decoded field and cipher view through a volatile result so the decode cannot be optimized
away. Expected refusals, absent-length drops and bounded empty views are not failures; no
decoder rule is copied into the driver.

The five decoders and the production handler each reaches:

| Selector | Message | Handler and decoder |
| --- | --- | --- |
| `0` | `IDX_LOSS_STATS` (`0x0201`) | `decode_loss_stats`: the fixed int32 record at offsets 0, 1, 3 |
| `1` | `IDX_INVALIDATE_REF_FRAMES` (`0x0301`) | `decode_invalidate_ref_frames`: the two int64 frame fields |
| `2` | `IDX_INPUT_DATA` (`0x0206`) | `decode_input_cipher`: `input_control_cipher_fits` and the big-endian declared cipher length |
| `3` | `IDX_ENCRYPTED` (`0x0001`) | `decode_encrypted_control`: `encrypted_control_header_present`, the LE wrapper length/seq, the runt floor and `encrypted_control_cipher_fits` |
| `4` | `IDX_EXEC_SERVER_CMD` (`0x3000`) | `decode_server_cmd_index`: the one-byte command index |

The loss-stats and invalidate decoders reproduce the handlers' fixed-size reads exactly,
including that the handler reads the whole record without first checking the payload is
long enough; a payload shorter than the record reads past its end just as the handler
does. The input-data and encrypted decoders reproduce the handlers' own length guards, so
their cipher views stay inside the bytes that arrived. `decode_server_cmd_index` reads one
byte, as the handler does after its permission gate. Decoding uses `std::memcpy` into
host-order scalars so the fuzz build's alignment check does not flag what the production
packed-struct and `reinterpret_cast` reads did without a diagnostic; the decoded values and
the bytes touched are identical. Periodic ping, request IDR and the Start A/B messages
carry no parsed payload and so have no decoder; set-clipboard and file-transfer-nonce only
check a permission and log, so they are not decoders either. The selective-FEC status
report (`0x5502`) is the remaining typed client->host message and has its own
`fuzz_control_fec` target above; it is not duplicated here.

### Build, resource and coverage limits

The default-OFF gate, native Linux x86_64/Clang 18+/C++20 checks, target-local
ASan/UBSan/libFuzzer flags and disabled IPO are shared with the other targets. This target
needs only `src/control_messages.cpp`, `src/control_messages.h` and `src/utility.h`, plus
the already installed `nlohmann_json` package `utility.h` includes; nothing is fetched or
provisioned, and no product library, socket, session, crypto or server is built. The new
seam is also in the ordinary production source list and the fast test build
(`ControlMessageDecoders` and the `ControlStreamDecodersStayAtTheirExtractedSeam`
contract); only the fuzzer executable and its instrumentation stay outside ordinary builds.

The driver refuses more than 65,536 bytes as a harness resource bound; it is not a new
control-channel limit, and the wrapper length is a uint16, so a declared cipher length can
still both fit and overrun within it. After the reviewed-toolchain ON configure, copy only
`tests/fuzz/corpus/control_stream/*` into a new external absolute `fuzz_evidence` corpus and
use the existing 60-second, 5-second per-input timeout and 1024 MiB RSS limits:

```sh
cmake --build "$fuzz_evidence/build" --target fuzz_control_stream --parallel 2 \
  >"$fuzz_evidence/build.log" 2>&1
"$fuzz_evidence/build/fuzz_control_stream" "$fuzz_evidence/corpus" \
  -max_total_time=60 -max_len=65536 -rss_limit_mb=1024 -timeout=5 \
  -artifact_prefix="$fuzz_evidence/crashes/" -print_final_stats=1 \
  >"$fuzz_evidence/run.log" 2>&1
```

Use the same first-failure retention and source/toolchain/binary/corpus identity rules as
the other targets. Adding this source claims no configure, build or campaign result, and
these settings do not authorize a run or waive host admission. A crash this target finds is
filed and fixed as its own issue with the reproducer kept as a regression case, separately
from this harness; parser behaviour fixes stay out of the harness.

### Nine synthetic control-stream seeds

The byte values below mirror the literals in
[`tests/unit/test_control_messages.cpp`](../unit/test_control_messages.cpp); each is a
selector byte followed by a representative valid payload, or a documented boundary. They
contain no private captures. Every seed's payload is at least as long as its decoder reads,
so the seed run itself touches only in-bounds bytes.

| File | Selector / payload |
| --- | --- |
| `loss-stats.bin` | `0` + int32 LE record `{-7, 123456, 9999, -2024}` |
| `invalidate-ref-frames.bin` | `1` + int64 LE pair `{-1, INT64_MAX}` |
| `input-data.bin` | `2` + big-endian length `20` + 20 cipher bytes `0xA0..0xB3` |
| `input-no-length.bin` | `2` + 3 bytes: boundary too short for the 4-byte length prefix |
| `encrypted-wrapper.bin` | `3` + LE length `24`, LE seq `0x04030201`, 20 body bytes `0x10..0x23` |
| `encrypted-runt.bin` | `3` + LE length `23`: boundary below the `16+4+4` floor |
| `encrypted-short-body.bin` | `3` + LE length `24` with only 2 body bytes: boundary that does not fit |
| `server-cmd.bin` | `4` + one command-index byte `0x07` |
| `empty.bin` | Added boundary: zero bytes, no selector |

The empty file remains part of the original corpus identity even when libFuzzer counts only
nonempty initial files.
