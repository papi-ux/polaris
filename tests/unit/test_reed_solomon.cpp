/** @file tests/unit/test_reed_solomon.cpp
 * Wire parity and runtime-dispatched FEC regression coverage.
 */
#include <gtest/gtest.h>
#include <rs.h>
#include <openssl/sha.h>
#include <array>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace {
  using codec = std::unique_ptr<reed_solomon, decltype(&reed_solomon_release)>;
  class ReedSolomonTests : public testing::Test {
  protected:
    static void SetUpTestSuite() {
      // Match the host startup warmup before stream threads create codecs.
      codec warmup {reed_solomon_new(1, 1), reed_solomon_release};
      ASSERT_TRUE(warmup);
    }
  };
  struct shards_t {
    std::vector<std::vector<uint8_t>> storage;
    std::vector<uint8_t *> pointers;
    shards_t(int ds, int ps, int bytes): storage(ds + ps, std::vector<uint8_t>(bytes + 2, 0xa5)) {
      for (int i = 0; i < ds + ps; ++i) {
        // Deliberately unaligned, exact sized tails with adjacent canaries.
        pointers.push_back(storage[i].data() + 1);
        if (i < ds) for (int j = 0; j < bytes; ++j) pointers.back()[j] = (i * 29 + j * 17 + (j >> 3)) & 255;
      }
    }
    void check_guards() const {
      for (const auto &shard : storage) { EXPECT_EQ(shard.front(), 0xa5); EXPECT_EQ(shard.back(), 0xa5); }
    }
    std::string parity_hash(int ds, int bytes) const {
      std::vector<uint8_t> parity;
      for (size_t i = ds; i < pointers.size(); ++i) parity.insert(parity.end(), pointers[i], pointers[i] + bytes);
      std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
      SHA256(parity.data(), parity.size(), digest.data());
      constexpr char hex[] = "0123456789abcdef";
      std::string result;
      for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
      return result;
    }
  };
}

TEST_F(ReedSolomonTests, ParityMatchesTheProtectedBaselineIncludingAudioMatrix) {
  // Generated with Polaris 7cdc9c90's wrapper and nanors 19f07b5, same input
  // formula and no shard padding. These are compatibility fixtures, not values
  // derived from the library under test.
  struct vector_t { int ds, ps, bytes; const char *sha256; };
  for (const auto &[ds, ps, bytes, expected] : {
    vector_t {4, 2, 17, "035c666842bbedf8cf517a0cd3d0b76ef2e832e5b476a1f8df5b480099a97f07"},
    vector_t {10, 3, 1024, "84f60b4d274b7fb4315246efab7b1a90f69453e4f9726fd595ecf55621985f5d"},
    vector_t {200, 55, 1408, "76e4c23e96914cdc4683424966d247c1222dd91d64dc0827d3353ec27e6fb9b6"}
  }) {
    codec rs {reed_solomon_new(ds, ps), reed_solomon_release};
    ASSERT_TRUE(rs);
    shards_t shards {ds, ps, bytes};
    ASSERT_EQ(reed_solomon_encode(rs.get(), shards.pointers.data(), ds + ps, bytes), 0);
    EXPECT_EQ(shards.parity_hash(ds, bytes), expected);
    if (ds == 4) {
      const uint8_t audio[] {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
      std::memcpy(rs->p, audio, sizeof(audio));
      ASSERT_EQ(reed_solomon_encode(rs.get(), shards.pointers.data(), ds + ps, bytes), 0);
      EXPECT_EQ(shards.parity_hash(ds, bytes), "50cf87a08ec549d4f761ab0c6b83e59ff37165ab6575e638930d33a53f13db06");
    }
    shards.check_guards();
  }
}

TEST_F(ReedSolomonTests, UnalignedTailsAndMixedLossesRecoverExactData) {
  for (int bytes : {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 1392, 1408, 1500}) {
    constexpr int ds = 10, ps = 4;
    codec rs {reed_solomon_new(ds, ps), reed_solomon_release};
    ASSERT_TRUE(rs);
    shards_t shards {ds, ps, bytes};
    ASSERT_EQ(reed_solomon_encode(rs.get(), shards.pointers.data(), ds + ps, bytes), 0);
    const auto expected = shards.storage;
    std::array<uint8_t, ds + ps> missing {};
    for (int index : {0, 3, 9, 12}) {
      missing[index] = 1;
      std::memset(shards.pointers[index], 0, bytes);
    }
    ASSERT_EQ(reed_solomon_decode(rs.get(), shards.pointers.data(), missing.data(), ds + ps, bytes), 0);
    // The host does not decode; this receiver-style check requires every data
    // shard to recover even when a parity shard is also unavailable.
    for (int i = 0; i < ds; ++i) EXPECT_EQ(shards.storage[i], expected[i]) << "bytes=" << bytes << " shard=" << i;
    shards.check_guards();
  }
}

TEST_F(ReedSolomonTests, IndependentConcurrentStreamsKeepIdenticalParity) {
  std::vector<std::future<void>> streams;
  for (int i = 0; i < 16; ++i) streams.push_back(std::async(std::launch::async, [] {
    for (int j = 0; j < 40; ++j) {
      codec rs {reed_solomon_new(10, 3), reed_solomon_release};
      ASSERT_TRUE(rs);
      shards_t shards {10, 3, 1024};
      ASSERT_EQ(reed_solomon_encode(rs.get(), shards.pointers.data(), 13, 1024), 0);
      EXPECT_EQ(shards.parity_hash(10, 1024), "84f60b4d274b7fb4315246efab7b1a90f69453e4f9726fd595ecf55621985f5d");
    }
  }));
  for (auto &stream : streams) stream.get();
}

TEST_F(ReedSolomonTests, InvalidSizesFailWithoutAccessingShardPayload) {
  for (auto dimensions : {std::array {0, 1}, std::array {1, 0}, std::array {255, 1}, std::array {-1, 1}}) {
    EXPECT_EQ(reed_solomon_new(dimensions[0], dimensions[1]), nullptr);
  }
  codec rs {reed_solomon_new(1, 1), reed_solomon_release};
  ASSERT_TRUE(rs);
  std::array<uint8_t *, 2> empty {nullptr, nullptr};
  EXPECT_EQ(reed_solomon_encode(rs.get(), empty.data(), 1, 1), -1);
  EXPECT_EQ(reed_solomon_encode(rs.get(), empty.data(), 2, 0), -1);
}

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
extern "C" size_t nanors_test_alignment(unsigned features);
TEST_F(ReedSolomonTests, RuntimeDispatchRequiresEveryAvx512InstructionSubset) {
  constexpr unsigned ssse3 = 1, avx2 = 2, avx512 = 4 | 8 | 16 | 32, gfni = 64;
  EXPECT_EQ(nanors_test_alignment(0), sizeof(void *));
  EXPECT_EQ(nanors_test_alignment(ssse3), 16u);
  EXPECT_EQ(nanors_test_alignment(ssse3 | avx2), 32u);
  for (unsigned missing : {4u, 8u, 16u, 32u}) {
    EXPECT_EQ(nanors_test_alignment(ssse3 | avx2 | (avx512 & ~missing)), 32u);
    EXPECT_EQ(nanors_test_alignment(ssse3 | avx2 | (avx512 & ~missing) | gfni), 32u);
  }
  EXPECT_EQ(nanors_test_alignment(ssse3 | avx2 | avx512), 64u);
  EXPECT_EQ(nanors_test_alignment(ssse3 | avx2 | avx512 | gfni), 64u);
}
#endif
