/** @file tools/benchmarks/fec.cpp
 * Compare the same FEC workload against two independently compiled libraries.
 * Build each library at its pinned revision with the release compiler flags.
 */
#include <rs.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <vector>

int main() {
  reed_solomon_init();
  auto warmup = reed_solomon_new(1, 1);
  if (!warmup) return 1;
  reed_solomon_release(warmup);
  std::puts("data_shards,parity_shards,bytes,audio,iterations,wall_ns_per_encode,cpu_ns_per_encode,wall_ns_per_create,parity_digest");
  for (auto shape : {std::array {4, 2, 256, 1}, std::array {20, 1, 1408, 0}, std::array {200, 10, 1408, 0}, std::array {200, 55, 1408, 0}}) {
    const auto [ds, ps, bytes, audio] = shape;
    auto rs = reed_solomon_new(ds, ps);
    if (!rs) return 1;
    if (audio) {
      const unsigned char parity[] = {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
      std::memcpy(rs->p, parity, sizeof(parity));
    }
    std::vector<std::vector<uint8_t>> storage(ds + ps, std::vector<uint8_t>(bytes + 1));
    std::vector<uint8_t *> pointers;
    for (int i = 0; i < ds + ps; ++i) {
      pointers.push_back(storage[i].data() + 1);
      for (int j = 0; i < ds && j < bytes; ++j) pointers.back()[j] = (i * 29 + j * 17 + (j >> 3)) & 255;
    }
    for (int i = 0; i < 100; ++i) if (reed_solomon_encode(rs, pointers.data(), ds + ps, bytes)) return 2;
    constexpr int iterations = 20000;
    const auto start = std::chrono::steady_clock::now();
    const auto cpu_start = std::clock();
    for (int i = 0; i < iterations; ++i) if (reed_solomon_encode(rs, pointers.data(), ds + ps, bytes)) return 2;
    const auto cpu_end = std::clock();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    uint64_t digest = 14695981039346656037ULL;
    for (int i = ds; i < ds + ps; ++i) for (int j = 0; j < bytes; ++j) digest = (digest ^ pointers[i][j]) * 1099511628211ULL;
    reed_solomon_release(rs);
    const auto create_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
      rs = reed_solomon_new(ds, ps);
      if (!rs) return 1;
      reed_solomon_release(rs);
    }
    const auto create_elapsed = std::chrono::steady_clock::now() - create_start;
    std::printf("%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%016llx\n", ds, ps, bytes, audio, iterations,
      std::chrono::duration<double, std::nano>(elapsed).count() / iterations,
      double(cpu_end - cpu_start) * 1e9 / CLOCKS_PER_SEC / iterations,
      std::chrono::duration<double, std::nano>(create_elapsed).count() / 10000,
      static_cast<unsigned long long>(digest));
  }
}
