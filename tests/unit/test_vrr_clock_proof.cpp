/**
 * @file tests/unit/test_vrr_clock_proof.cpp
 * @brief Clock proof rules: fail closed, bounded, and never a source-time claim on their own.
 */
#include <src/vrr_clock_proof.h>
#ifdef __linux__
  #include <src/platform/linux/vrr_clock_probe.h>
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vrr_clock::namespace_e;
using vrr_clock::sample_e;
using status_e = vrr_policy::presentation_status_e;

namespace {
  constexpr std::int64_t k_base = 5'000'000'000;  // 5 s of CLOCK_MONOTONIC

  std::vector<vrr_clock::probe_sample_t> steady_samples(std::size_t count, std::int64_t offset = 0, std::int64_t bracket = 200) {
    std::vector<vrr_clock::probe_sample_t> samples;
    for (std::size_t index = 0; index < count; ++index) {
      const auto before = k_base + static_cast<std::int64_t>(index) * 1000;
      samples.push_back({before, before + bracket / 2 + offset, before + bracket});
    }
    return samples;
  }

  vrr_clock::process_facts_t facts(std::int64_t pid, std::uint32_t uid, vrr_clock::ns_id_t ns, std::uint64_t start = 77) {
    return {pid, uid, start, start, ns, ns};
  }

  /// A kernel stand-in whose answers can change between reads.
  class fake_system_t final: public vrr_clock::system_t {
  public:
    std::optional<peer_cred_t> peer {{4242, 1000}};
    std::map<std::int64_t, std::deque<std::optional<vrr_clock::ns_id_t>>> namespaces;
    std::deque<std::optional<std::uint64_t>> starttimes {77, 77};
    std::uint32_t uid = 1000;
    std::int64_t now = k_base;
    std::size_t clock_samples = 0;
    bool fail_clock = false;
    std::function<void(std::size_t)> on_sample;

    std::optional<peer_cred_t> peer_credentials(int) override {
      return peer;
    }

    std::optional<vrr_clock::ns_id_t> time_namespace(std::int64_t pid) override {
      auto &queue = namespaces[pid];
      if (queue.empty()) return std::nullopt;
      auto value = queue.front();
      if (queue.size() > 1) queue.pop_front();
      return value;
    }

    std::optional<std::uint64_t> process_starttime(std::int64_t) override {
      if (starttimes.empty()) return std::nullopt;
      auto value = starttimes.front();
      if (starttimes.size() > 1) starttimes.pop_front();
      return value;
    }

    std::uint32_t current_uid() override {
      return uid;
    }

    vrr_clock::probe_sample_t sample_clocks() override {
      ++clock_samples;
      if (on_sample) on_sample(clock_samples);
      if (fail_clock) return {};
      const auto before = k_base + static_cast<std::int64_t>(clock_samples) * 1000;
      return {before, before + 100, before + 200};
    }

    std::int64_t monotonic_now_ns() override {
      return now;
    }
  };

  /// A retained compositor expectation whose currency the test controls, counting every check.
  class fake_lease_t final: public vrr_clock::compositor_lease_t {
  public:
    explicit fake_lease_t(vrr_clock::compositor_process_t expected = {4242, 1000, 77}):
        compositor_lease_t(expected) {}

    mutable bool is_current = true;
    mutable std::size_t checks = 0;

    bool current() const override {
      ++checks;
      return is_current;
    }
  };

  vrr_clock::proof_t verified_proof(std::int64_t epoch_start = k_base) {
    const auto mapping = vrr_clock::evaluate_mapping(steady_samples(8), {});
    return vrr_clock::make_proof(9, {namespace_e::same, "same_dev_and_inode"}, true, "test_authority", mapping, epoch_start);
  }

  std::chrono::steady_clock::time_point steady_at(std::int64_t ns) {
    return std::chrono::steady_clock::time_point {std::chrono::nanoseconds {ns}};
  }
}  // namespace

TEST(VrrClockProofTests, BoundedMappingIsValidAndSumsDeltaAndBracket) {
  auto samples = steady_samples(8, 0, 200);
  samples[3].steady_ns += 50;  // one sample 50 ns off the others
  const auto mapping = vrr_clock::evaluate_mapping(samples, {});
  ASSERT_TRUE(mapping.valid) << mapping.reason;
  EXPECT_EQ(mapping.max_delta_ns, 50u);
  EXPECT_EQ(mapping.max_bracket_ns, 200u);
  EXPECT_EQ(mapping.uncertainty_ns, 250u);
  EXPECT_EQ(mapping.steady_to_mono_offset_ns, 0);
}

TEST(VrrClockProofTests, FailedReadsAndReversedBracketsNeverBecomeValid) {
  {
    std::vector<vrr_clock::probe_sample_t> failed(8);  // every field is the failed-read sentinel
    EXPECT_FALSE(vrr_clock::evaluate_mapping(failed, {}).valid);
  }
  {
    auto samples = steady_samples(8);
    samples[5].steady_ns = vrr_clock::k_failed_read;
    EXPECT_EQ(vrr_clock::evaluate_mapping(samples, {}).reason, "clock_read_failed");
  }
  {
    auto samples = steady_samples(8);
    std::swap(samples[2].mono_before_ns, samples[2].mono_after_ns);
    EXPECT_EQ(vrr_clock::evaluate_mapping(samples, {}).reason, "bracket_reversed");
  }
  EXPECT_EQ(vrr_clock::evaluate_mapping(steady_samples(3), {}).reason, "too_few_samples");
}

TEST(VrrClockProofTests, OffsetBoundAppliesToEverySampleNotOnlyTheFirst) {
  vrr_clock::mapping_limits_t limits;
  limits.max_abs_offset_ns = 1000;
  limits.max_uncertainty_ns = std::numeric_limits<std::uint64_t>::max();
  auto samples = steady_samples(8);
  samples.back().steady_ns += 5000;  // only the last sample is far off
  const auto mapping = vrr_clock::evaluate_mapping(samples, limits);
  EXPECT_FALSE(mapping.valid);
  EXPECT_EQ(mapping.reason, "offset_out_of_bounds");
}

TEST(VrrClockProofTests, UncertaintyBeyondTheLimitIsInvalid) {
  vrr_clock::mapping_limits_t limits;
  limits.max_uncertainty_ns = 100;
  EXPECT_EQ(vrr_clock::evaluate_mapping(steady_samples(8, 0, 200), limits).reason, "uncertainty_out_of_bounds");
}

TEST(VrrClockProofTests, ExtremeValuesCannotOverflowIntoAValidMapping) {
  const auto max = std::numeric_limits<std::int64_t>::max();
  std::vector<vrr_clock::probe_sample_t> samples(8, {0, max, max});
  vrr_clock::mapping_limits_t limits;
  limits.max_abs_offset_ns = std::numeric_limits<std::uint64_t>::max();
  limits.max_uncertainty_ns = std::numeric_limits<std::uint64_t>::max();
  const auto mapping = vrr_clock::evaluate_mapping(samples, limits);
  // Bracket max, midpoint max/2: valid arithmetic, but it must not wrap.
  EXPECT_EQ(mapping.max_bracket_ns, static_cast<std::uint64_t>(max));
  EXPECT_EQ(vrr_clock::magnitude(std::numeric_limits<std::int64_t>::min()),
            static_cast<std::uint64_t>(max) + 1);
  std::int64_t out = 0;
  EXPECT_FALSE(vrr_clock::checked_add(max, 1, out));
  EXPECT_FALSE(vrr_clock::checked_sub(std::numeric_limits<std::int64_t>::min(), 1, out));
  std::uint64_t unsigned_out = 0;
  EXPECT_FALSE(vrr_clock::checked_add_unsigned(std::numeric_limits<std::uint64_t>::max(), 1, unsigned_out));
}

TEST(VrrClockProofTests, NamespaceComparisonRequiresDevAndInodeAndFailsClosed) {
  const vrr_clock::ns_id_t ours {3, 4026531834};
  const auto self = facts(0, 1000, ours);
  EXPECT_EQ(vrr_clock::compare_time_namespaces(self, facts(4242, 1000, ours), 1000).result, namespace_e::same);
  EXPECT_EQ(vrr_clock::compare_time_namespaces(self, facts(4242, 1000, {9, ours.ino}), 1000).result, namespace_e::different)
    << "an equal inode on another device is a different namespace";
  EXPECT_EQ(vrr_clock::compare_time_namespaces(self, facts(4242, 1000, {ours.dev, 1}), 1000).result, namespace_e::different);

  struct case_t {
    vrr_clock::process_facts_t self;
    vrr_clock::process_facts_t peer;
    std::string_view reason;
  };
  auto unstable_self = self;
  unstable_self.ns_second = vrr_clock::ns_id_t {3, 1};
  auto missing_self = self;
  missing_self.ns_second.reset();
  auto replaced = facts(4242, 1000, ours, 77);
  replaced.starttime_after = 78;
  auto no_start = facts(4242, 1000, ours);
  no_start.starttime_before.reset();
  auto unstable_peer = facts(4242, 1000, ours);
  unstable_peer.ns_second = vrr_clock::ns_id_t {3, 1};
  auto denied_peer = facts(4242, 1000, ours);
  denied_peer.ns_first.reset();
  auto no_uid = facts(4242, 1000, ours);
  no_uid.uid.reset();
  const std::vector<case_t> cases {
    {self, facts(1, 1000, ours), "peer_pid_invalid"},
    {self, facts(0, 1000, ours), "peer_pid_invalid"},
    {self, facts(-5, 1000, ours), "peer_pid_invalid"},
    {self, facts(4242, 1001, ours), "peer_uid_mismatch"},
    {self, no_uid, "peer_uid_mismatch"},
    {missing_self, facts(4242, 1000, ours), "self_namespace_unavailable"},
    {unstable_self, facts(4242, 1000, ours), "self_namespace_unstable"},
    {self, no_start, "peer_starttime_unavailable"},
    {self, replaced, "peer_process_replaced"},
    {self, denied_peer, "peer_namespace_unavailable"},
    {self, unstable_peer, "peer_namespace_unstable"},
  };
  for (const auto &item : cases) {
    const auto result = vrr_clock::compare_time_namespaces(item.self, item.peer, 1000);
    EXPECT_EQ(result.result, namespace_e::unverifiable) << item.reason;
    EXPECT_EQ(result.reason, item.reason);
  }
}

TEST(VrrClockProofTests, ClockDomainNeedsSameNamespaceAuthoritativeIdentityValidMappingAndEpoch) {
  const auto mapping = vrr_clock::evaluate_mapping(steady_samples(8), {});
  const vrr_clock::namespace_result_t same {namespace_e::same, "same_dev_and_inode"};
  EXPECT_TRUE(vrr_clock::make_proof(1, same, true, "authority", mapping, k_base).clock_domain_verified);
  // The same namespace alone proves nothing about who the peer is.
  EXPECT_FALSE(vrr_clock::make_proof(1, same, false, "no_authoritative_compositor_identity", mapping, k_base).clock_domain_verified);
  EXPECT_FALSE(vrr_clock::make_proof(1, {namespace_e::unverifiable, "x"}, true, "authority", mapping, k_base).clock_domain_verified);
  EXPECT_FALSE(vrr_clock::make_proof(1, {namespace_e::different, "x"}, true, "authority", mapping, k_base).clock_domain_verified);
  EXPECT_FALSE(vrr_clock::make_proof(1, same, true, "authority", vrr_clock::mapping_t {}, k_base).clock_domain_verified);
  EXPECT_FALSE(vrr_clock::make_proof(0, same, true, "authority", mapping, k_base).clock_domain_verified);
  EXPECT_FALSE(vrr_clock::make_proof(1, same, true, "authority", mapping, vrr_clock::k_failed_read).clock_domain_verified);
}

TEST(VrrClockProofTests, CollectReadsBothSidesTwiceAndReportsSocketPeerFactsOnly) {
  fake_system_t system;
  const vrr_clock::ns_id_t ours {3, 11};
  system.namespaces[0] = {ours};
  system.namespaces[4242] = {ours};
  const auto proof = vrr_clock::collect(7, 21, system, {}, nullptr);
  ASSERT_TRUE(proof);
  EXPECT_EQ(proof->capture_epoch_id, 21u);
  EXPECT_EQ(proof->time_namespace, namespace_e::same);
  EXPECT_TRUE(proof->mapping.valid) << proof->mapping.reason;
  EXPECT_FALSE(proof->producer_identity_verified);
  EXPECT_EQ(proof->producer_identity_reason, "no_authoritative_compositor_identity");
  EXPECT_FALSE(proof->clock_domain_verified) << "same namespace without an authoritative compositor identity";
  EXPECT_EQ(proof->epoch_start_mono_ns, k_base);
}

TEST(VrrClockProofTests, CollectVerifiesTheActualPeerAgainstACurrentRetainedLease) {
  fake_system_t system;
  const vrr_clock::ns_id_t ours {3, 11};
  system.namespaces[0] = {ours};
  system.namespaces[4242] = {ours};
  auto lease = std::make_shared<fake_lease_t>();
  const auto proof = vrr_clock::collect(7, 21, system, {}, lease);
  ASSERT_TRUE(proof);
  EXPECT_TRUE(proof->producer_identity_verified) << proof->producer_identity_reason;
  EXPECT_EQ(proof->producer_identity_reason, "authoritative_compositor_peer_matched");
  EXPECT_TRUE(proof->clock_domain_verified);
  EXPECT_EQ(lease->checks, 2u) << "current() brackets the peer and mapping reads, once before and once after";
}

TEST(VrrClockProofTests, CollectRefusesALeaseThatDoesNotDescribeThePeerItRead) {
  const vrr_clock::ns_id_t ours {3, 11};
  const auto run = [&](vrr_clock::compositor_process_t expected, auto &&prepare) {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    prepare(system);
    const auto proof = vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>(expected));
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_FALSE(proof->clock_domain_verified);
    return std::string(proof->producer_identity_reason);
  };
  const auto unchanged = [](fake_system_t &) {};
  // The socket's peer is some other process, e.g. a launcher that created the listening socket.
  EXPECT_EQ(run({4243, 1000, 77}, unchanged), "compositor_peer_pid_mismatch");
  EXPECT_EQ(run({4242, 1001, 77}, unchanged), "compositor_peer_uid_mismatch");
  // Same pid, different process: the lease's start time is not the one read.
  EXPECT_EQ(run({4242, 1000, 78}, unchanged), "compositor_peer_starttime_mismatch");
  EXPECT_EQ(run({4242, 1000, 77}, [](fake_system_t &system) { system.starttimes = {std::nullopt}; }),
            "compositor_peer_starttime_unavailable");
  EXPECT_EQ(run({4242, 1000, 77}, [](fake_system_t &system) { system.starttimes = {77, 78}; }),
            "compositor_peer_starttime_mismatch");
  EXPECT_EQ(run({0, 1000, 77}, unchanged), "compositor_identity_invalid");
}

TEST(VrrClockProofTests, CollectRefusesAbsentRetiredOrReplacedGenerations) {
  const vrr_clock::ns_id_t ours {3, 11};
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    const auto proof = vrr_clock::collect(7, 1, system, {}, nullptr);
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_EQ(proof->producer_identity_reason, "no_authoritative_compositor_identity");
  }
  {
    // A retained lease of a generation already retired grants nothing to a new epoch.
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    auto lease = std::make_shared<fake_lease_t>();
    lease->is_current = false;
    const auto proof = vrr_clock::collect(7, 1, system, {}, lease);
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_EQ(proof->producer_identity_reason, "compositor_generation_unavailable");
    EXPECT_FALSE(proof->clock_domain_verified);
  }
  {
    // Retired or replaced while the peer and clocks were being read.
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    auto lease = std::make_shared<fake_lease_t>();
    system.on_sample = [&](std::size_t sample) {
      if (sample == 1) lease->is_current = false;
    };
    const auto proof = vrr_clock::collect(7, 1, system, {}, lease);
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_EQ(proof->producer_identity_reason, "compositor_generation_changed");
    EXPECT_FALSE(proof->clock_domain_verified);
  }
  {
    // No connected peer: nothing was read, so nothing is verified even with a current lease.
    fake_system_t system;
    system.namespaces[0] = {ours};
    const auto proof = vrr_clock::collect(-1, 1, system, {}, std::make_shared<fake_lease_t>());
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_EQ(proof->producer_identity_reason, "no_authoritative_compositor_identity");
  }
  {
    // SO_PEERCRED failed: no actual peer facts to match.
    fake_system_t system;
    system.peer.reset();
    system.namespaces[0] = {ours};
    const auto proof = vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>());
    EXPECT_FALSE(proof->producer_identity_verified);
    EXPECT_EQ(proof->producer_identity_reason, "compositor_peer_pid_mismatch");
  }
}

TEST(VrrClockProofTests, CollectFailsClosedOnReplacementMissingPeerAndFailedClocks) {
  const vrr_clock::ns_id_t ours {3, 11};
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    system.starttimes = {77, 99};  // pid reused between the namespace reads
    EXPECT_EQ(vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>())->namespace_reason, "peer_process_replaced");
  }
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours, vrr_clock::ns_id_t {3, 12}};  // peer changed namespace mid-read
    EXPECT_EQ(vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>())->namespace_reason, "peer_namespace_unstable");
  }
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    EXPECT_EQ(vrr_clock::collect(-1, 1, system, {}, std::make_shared<fake_lease_t>())->namespace_reason, "no_peer_socket");
  }
  {
    fake_system_t system;
    system.peer.reset();
    system.namespaces[0] = {ours};
    EXPECT_EQ(vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>())->time_namespace, namespace_e::unverifiable);
  }
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    system.fail_clock = true;
    const auto proof = vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>());
    EXPECT_FALSE(proof->mapping.valid);
    EXPECT_FALSE(proof->clock_domain_verified) << "all-sentinel samples must never be a valid mapping";
  }
  {
    fake_system_t system;
    system.namespaces[0] = {ours};
    system.namespaces[4242] = {ours};
    system.now = vrr_clock::k_failed_read;
    const auto proof = vrr_clock::collect(7, 1, system, {}, std::make_shared<fake_lease_t>());
    EXPECT_EQ(proof->mapping.reason, "epoch_start_read_failed");
    EXPECT_FALSE(proof->clock_domain_verified);
  }
}

TEST(VrrClockProofTests, CollectClampsTheSampleCount) {
  for (const auto &[requested, taken] : {std::pair<std::uint64_t, std::size_t> {0, 2}, {1, 2}, {8, 8}, {1'000'000, 64}}) {
    fake_system_t system;
    vrr_clock::mapping_limits_t limits;
    limits.min_samples = requested;
    (void) vrr_clock::collect(-1, 1, system, limits, nullptr);
    EXPECT_EQ(system.clock_samples, taken) << requested;
  }
}

TEST(VrrClockProofTests, ClassifyOrdersPrefetchedAndProtocolStatusFirst) {
  const auto proof = verified_proof();
  const auto ready = steady_at(k_base + 10'000'000);
  // A representable, otherwise valid time captured during initialization is never eligible.
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base + 9'000'000, true, ready, &proof, {}), sample_e::prefetched_init);
  EXPECT_EQ(vrr_clock::classify(status_e::missing, std::nullopt, false, ready, &proof, {}), sample_e::missing);
  EXPECT_EQ(vrr_clock::classify(status_e::invalid_nsec, std::nullopt, false, ready, &proof, {}), sample_e::invalid);
  EXPECT_EQ(vrr_clock::classify(status_e::invalid_overflow, std::nullopt, false, ready, &proof, {}), sample_e::invalid);
  EXPECT_EQ(vrr_clock::classify(status_e::nonmonotonic, k_base + 9'000'000, false, ready, &proof, {}), sample_e::nonmonotonic);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, -1, false, ready, &proof, {}), sample_e::invalid);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, std::nullopt, false, ready, &proof, {}), sample_e::invalid);
}

TEST(VrrClockProofTests, ClassifyFindsBeforeEpochFutureAndStaleAgainstTheMappedReadyTime) {
  const auto proof = verified_proof(k_base);
  const auto uncertainty = static_cast<std::int64_t>(proof.mapping.uncertainty_ns);
  const auto ready_ns = k_base + 50'000'000;
  const auto ready = steady_at(ready_ns);  // offset 0 in verified_proof
  vrr_clock::plausibility_limits_t limits;
  limits.staleness_ns = 20'000'000;
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base - 1, false, ready, &proof, limits), sample_e::before_epoch);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, ready_ns + uncertainty + 1, false, ready, &proof, limits), sample_e::future);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, ready_ns + uncertainty, false, ready, &proof, limits), sample_e::valid);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, ready_ns - 20'000'000 - uncertainty - 1, false, ready, &proof, limits), sample_e::stale);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, ready_ns - 20'000'000 - uncertainty, false, ready, &proof, limits), sample_e::valid);
}

TEST(VrrClockProofTests, ClassifyNeverReportsValidWithoutAVerifiedClockDomain) {
  const auto mapping = vrr_clock::evaluate_mapping(steady_samples(8), {});
  const auto unverified = vrr_clock::make_proof(
    9, {namespace_e::same, "same_dev_and_inode"}, false, "no_authoritative_compositor_identity", mapping, k_base
  );
  const auto ready = steady_at(k_base + 10'000'000);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base + 9'000'000, false, ready, &unverified, {}), sample_e::clock_unverified);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base + 9'000'000, false, ready, nullptr, {}), sample_e::clock_unverified);
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base + 9'000'000, false, std::nullopt, &unverified, {}), sample_e::clock_unverified);
  // An unidentified producer's raw time is never compared with our clock, even one that looks future.
  EXPECT_EQ(vrr_clock::classify(status_e::valid, k_base + 900'000'000, false, ready, &unverified, {}), sample_e::clock_unverified);
}

TEST(VrrClockProofTests, NoCrossClockComparisonWithoutAnAuthoritativeSameNamespaceProof) {
  const auto mapping = vrr_clock::evaluate_mapping(steady_samples(8), {});
  ASSERT_TRUE(mapping.valid);
  const auto ready_ns = k_base + 50'000'000;
  const auto ready = steady_at(ready_ns);
  vrr_clock::plausibility_limits_t limits;
  limits.staleness_ns = 20'000'000;
  // Raw values that would be future, stale and before-epoch if they were in our clock.
  const std::vector<std::int64_t> adversarial {ready_ns + 900'000'000, ready_ns - 900'000'000, k_base - 1, 0,
                                               std::numeric_limits<std::int64_t>::max()};

  const std::vector<vrr_clock::proof_t> refused {
    vrr_clock::make_proof(9, {namespace_e::different, "different_dev_or_inode"}, true, "authority", mapping, k_base),
    vrr_clock::make_proof(9, {namespace_e::different, "different_dev_or_inode"}, false, "unknown", mapping, k_base),
    vrr_clock::make_proof(9, {namespace_e::unverifiable, "peer_namespace_unavailable"}, true, "authority", mapping, k_base),
    vrr_clock::make_proof(9, {namespace_e::unverifiable, "no_peer_socket"}, false, "unknown", mapping, k_base),
    vrr_clock::make_proof(9, {namespace_e::same, "same_dev_and_inode"}, false, "unknown", mapping, k_base),
  };
  for (const auto &proof : refused) {
    for (const auto raw : adversarial) {
      EXPECT_EQ(vrr_clock::classify(status_e::valid, raw, false, ready, &proof, limits), sample_e::clock_unverified)
        << vrr_clock::name(proof.time_namespace) << " identity=" << proof.producer_identity_verified << " raw=" << raw;
    }
  }
}

TEST(VrrClockProofTests, ContradictoryProofFlagsAreNeverTrusted) {
  const auto mapping = vrr_clock::evaluate_mapping(steady_samples(8), {});
  const auto ready = steady_at(k_base + 50'000'000);
  const auto future = k_base + 900'000'000;
  auto proof = verified_proof();
  ASSERT_TRUE(vrr_clock::authorises_clock_comparison(proof));
  ASSERT_EQ(vrr_clock::classify(status_e::valid, future, false, ready, &proof, {}), sample_e::future);

  // Each forged combination claims clock_domain_verified while a required fact says otherwise.
  std::vector<vrr_clock::proof_t> forged(6, proof);
  forged[0].time_namespace = namespace_e::different;
  forged[1].time_namespace = namespace_e::unverifiable;
  forged[2].producer_identity_verified = false;
  forged[3].capture_epoch_id = 0;
  forged[4].mapping.valid = false;
  forged[5].epoch_start_mono_ns = vrr_clock::k_failed_read;
  for (const auto &item : forged) {
    ASSERT_TRUE(item.clock_domain_verified);
    EXPECT_FALSE(vrr_clock::authorises_clock_comparison(item));
    EXPECT_EQ(vrr_clock::classify(status_e::valid, future, false, ready, &item, {}), sample_e::clock_unverified);
  }
  // And the reverse: every fact present but the verdict withheld.
  auto withheld = proof;
  withheld.clock_domain_verified = false;
  EXPECT_EQ(vrr_clock::classify(status_e::valid, future, false, ready, &withheld, {}), sample_e::clock_unverified);
  (void) mapping;
}

TEST(VrrClockProofTests, AnUnknownPresentationStatusFailsClosed) {
  const auto proof = verified_proof();
  const auto ready = steady_at(k_base + 10'000'000);
  for (const int raw : {5, 42, -1, 255}) {
    EXPECT_EQ(vrr_clock::classify(static_cast<status_e>(raw), k_base + 9'000'000, false, ready, &proof, {}), sample_e::invalid)
      << raw;
  }
  // Initialization frames stay ineligible whatever their status, without any clock comparison.
  EXPECT_EQ(vrr_clock::classify(static_cast<status_e>(42), k_base + 900'000'000, true, ready, nullptr, {}), sample_e::prefetched_init);
}

#ifdef __linux__
TEST(VrrClockProofTests, ParsesProcessStartTimeAfterTheLastParenthesis) {
  // comm may contain spaces and parentheses; starttime is field 22.
  const std::string stat =
    "4242 (lab (wc) x) S 1 4242 4242 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 3 0 987654 123456 789\n";
  EXPECT_EQ(vrr_clock::linux_probe::parse_starttime(stat), 987654u);
  EXPECT_FALSE(vrr_clock::linux_probe::parse_starttime("4242 (labwc S 1"));
  EXPECT_FALSE(vrr_clock::linux_probe::parse_starttime("4242 (labwc) S 1 2 3"));
  EXPECT_FALSE(vrr_clock::linux_probe::parse_starttime(
    "4242 (labwc) S 1 4242 4242 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 3 0 98x7 1\n"));
}
#endif
