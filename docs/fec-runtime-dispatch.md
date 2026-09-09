# FEC runtime dispatch

Polaris pins nanors to `b50735aedef780e533631704a8ed8cad67faf323` and compiles its
`rs.c`, `oblas_common.c`, and `oblas_lite.c` directly. The former local ISA wrapper
is removed. The later upstream revision includes the complete AVX-512 F/BW/DQ/VL
admission check absent from the original Sunshine #5369 revision. No global
`-march=native` or AVX build target is introduced.

The library selects its implementation when a codec is created. Its public init
function is a no-op and its first table population is not synchronized, so host
startup creates and releases one codec before HTTP/RTSP or stream threads start.
Audio and video continue to own separate codecs. Codecs must not be shared
concurrently based on upstream header comments alone.

Packetization, FEC percentages, oversized-frame policy, the 255-shard boundary,
and the special eight-byte NVIDIA audio parity matrix are unchanged. Fixtures
compare exact parity hashes against protected Polaris `7cdc9c90` and nanors
`19f07b5`. Tests cover unaligned buffer tails, data recovery with mixed losses,
independent concurrent streams, supported size rejection, and the actual pinned
selector with partial CPU capability sets. They do not assume that the decoder
reconstructs missing parity or that arbitrary null shard entries are accepted.

A standalone benchmark is provided in `tools/benchmarks/fec.cpp`. Compile the
baseline wrapper at `7cdc9c90` with its original release flags
`-O3 -ftree-vectorize -funroll-loops`, using its pinned nanors dependency. For the
baseline C++ benchmark, force-include a compatibility header containing
`<stddef.h>` and both baseline `rs.h` and `rswrapper.h` inside `extern "C"`.
This selects the released wrapper function pointers, including its runtime ISA
selection. Compile the new three C sources with `-O3` and compile the identical
benchmark with `-std=c++20 -O3` for both versions. Do not link the raw baseline
scalar implementation in place of the released wrapper.

On an Intel Core i9-14900K with GCC 16.2, seven interleaved runs pinned to one CPU
measured the following median encode CPU times. Each run encoded 20,000 blocks;
all before/after parity digests matched. Sizes are bytes per data shard.

| Data + parity shards | Size | Before | After | Reduction |
| --- | ---: | ---: | ---: | ---: |
| 4 + 2, audio matrix | 256 | 48.5 ns | 32.5 ns | 33.0% |
| 20 + 1 | 1408 | 513.4 ns | 360.7 ns | 29.7% |
| 200 + 10 | 1408 | 60.05 µs | 46.27 µs | 22.9% |
| 200 + 55 | 1408 | 329.48 µs | 254.42 µs | 22.8% |

Codec creation took 9–519 ns longer in these workloads (median wall time).
These are standalone FEC measurements, not end-to-end streaming improvements.
Raw samples, executable/source hashes, compiler identity, and baseline shim are
retained with candidate evidence.

The parity vectors also match on native Apple ARM64/NEON and under QEMU scalar,
SSSE3, and AVX2 CPU models. The available emulator cannot execute AVX-512; the
synthetic capability test validates its admission rules only. Native Linux tests
and the affected FEC tests under ASan/UBSan and TSan pass. Supported package builds
remain part of combined candidate acceptance.
