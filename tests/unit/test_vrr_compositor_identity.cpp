/** @file tests/unit/test_vrr_compositor_identity.cpp */
#include <src/vrr_compositor_identity.h>

#include <gtest/gtest.h>

#include <deque>
#include <memory>

namespace {
  class fake_lease_t final: public vrr_clock::compositor_lease_t {
  public:
    explicit fake_lease_t(vrr_clock::compositor_process_t process = {4242, 1000, 77}):
        compositor_lease_t(process) {}

    mutable std::deque<bool> observations {true, true};
    mutable unsigned reads = 0;
    bool current() const override {
      ++reads;
      if (observations.empty()) return false;
      const auto value = observations.front();
      if (observations.size() > 1) observations.pop_front();
      return value;
    }
  };
}

TEST(VrrCompositorIdentityTests, ExactPeerNeedsLiveGenerationBeforeAndAfterReads) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  EXPECT_EQ(lease->reads, 1u);
  const auto match = check.verify(4242, 1000, 77, 77);
  EXPECT_TRUE(match.verified);
  EXPECT_EQ(match.reason, "authoritative_compositor_peer_matched");
  EXPECT_EQ(lease->reads, 2u);
}

TEST(VrrCompositorIdentityTests, NamespaceAgreementCannotReplaceExpectedIdentity) {
  const vrr_clock::compositor_identity_check_t check {nullptr};
  const auto match = check.verify(4242, 1000, 77, 77);
  EXPECT_FALSE(match.verified);
  EXPECT_EQ(match.reason, "no_authoritative_compositor_identity");
}

TEST(VrrCompositorIdentityTests, AlreadyRetiredLeaseCannotRecoverDuringCollection) {
  const auto lease = std::make_shared<fake_lease_t>();
  lease->observations = {false, true};
  const vrr_clock::compositor_identity_check_t check {lease};
  const auto match = check.verify(4242, 1000, 77, 77);
  EXPECT_FALSE(match.verified);
  EXPECT_EQ(match.reason, "compositor_generation_unavailable");
}

TEST(VrrCompositorIdentityTests, RetainedLeaseCannotOutliveGenerationAuthority) {
  auto lease = std::make_shared<fake_lease_t>();
  const auto retained = std::weak_ptr<fake_lease_t>(lease);
  const vrr_clock::compositor_identity_check_t check {lease};
  lease->observations = {false};
  lease.reset();
  EXPECT_FALSE(retained.expired()) << "collection pins descriptors until it finishes";
  const auto match = check.verify(4242, 1000, 77, 77);
  EXPECT_FALSE(match.verified);
  EXPECT_EQ(match.reason, "compositor_generation_changed");
}

TEST(VrrCompositorIdentityTests, SupervisorOrUnrelatedPeerCannotQualifyCompositorTimes) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  for (const auto pid : {-1, 0, 1, 4241, 4243}) {
    const auto match = check.verify(pid, 1000, 77, 77);
    EXPECT_FALSE(match.verified) << pid;
    EXPECT_EQ(match.reason, "compositor_peer_pid_mismatch");
  }
}

TEST(VrrCompositorIdentityTests, PeerUidMustBeReadAndMatchOwnedProcess) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  for (const auto uid : {std::optional<std::uint32_t> {}, std::optional<std::uint32_t> {1001}}) {
    const auto match = check.verify(4242, uid, 77, 77);
    EXPECT_FALSE(match.verified);
    EXPECT_EQ(match.reason, "compositor_peer_uid_mismatch");
  }
}

TEST(VrrCompositorIdentityTests, BothStarttimeReadsAreRequired) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  EXPECT_EQ(check.verify(4242, 1000, std::nullopt, 77).reason, "compositor_peer_starttime_unavailable");
  EXPECT_EQ(check.verify(4242, 1000, 77, std::nullopt).reason, "compositor_peer_starttime_unavailable");
  EXPECT_EQ(check.verify(4242, 1000, std::nullopt, std::nullopt).reason, "compositor_peer_starttime_unavailable");
}

TEST(VrrCompositorIdentityTests, StableReusedPidIsStillADifferentProcess) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  const auto match = check.verify(4242, 1000, 99, 99);
  EXPECT_FALSE(match.verified);
  EXPECT_EQ(match.reason, "compositor_peer_starttime_mismatch");
}

TEST(VrrCompositorIdentityTests, ReplacementBetweenReadsCannotQualify) {
  const auto lease = std::make_shared<fake_lease_t>();
  const vrr_clock::compositor_identity_check_t check {lease};
  for (const auto &pair : {std::pair {77u, 99u}, std::pair {99u, 77u}, std::pair {0u, 77u}}) {
    const auto match = check.verify(4242, 1000, pair.first, pair.second);
    EXPECT_FALSE(match.verified);
    EXPECT_EQ(match.reason, "compositor_peer_starttime_mismatch");
  }
}

TEST(VrrCompositorIdentityTests, InvalidOwnedIdentityFailsClosed) {
  for (const auto process : {vrr_clock::compositor_process_t {-1, 1000, 77},
                             vrr_clock::compositor_process_t {0, 1000, 77},
                             vrr_clock::compositor_process_t {1, 1000, 77},
                             vrr_clock::compositor_process_t {4242, 1000, 0}}) {
    const auto lease = std::make_shared<fake_lease_t>(process);
    const vrr_clock::compositor_identity_check_t check {lease};
    const auto match = check.verify(process.pid, process.uid, process.starttime, process.starttime);
    EXPECT_FALSE(match.verified);
    EXPECT_EQ(match.reason, "compositor_identity_invalid");
  }
}
