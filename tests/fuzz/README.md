# Opt-in config/app validator fuzzer

This standalone developer target exercises the production config/app JSON validators.
It builds only when explicitly enabled; ordinary source CI does not compile or run it.
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
RTSP primitives and handlers, the full typed control IDX/FEC boundary and nvhttp
query/launch validation still need separately owned targets. Generic JSON syntax
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
`CMAKE_PREFIX_PATH` to the admitted local package if it is not found in system paths.

```sh
set -euo pipefail
: "${fuzz_evidence:?Set a new approved absolute evidence directory outside the checkout}"
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
not a new HTTP limit. The recipe proposes one 60-second libFuzzer budget, a 5-second
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
