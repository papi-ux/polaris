/**
 * @file tests/unit/test_vrr_epoch_authority.cpp
 * @brief The epoch authority registry: only the owning epoch's exact lease and proof, only while it
 *        is open, and never after it is replaced or destroyed. Supplements the encode-loop cases.
 */
#include <src/vrr_epoch_authority.h>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using vrr_clock::authority_e;

namespace {
  class switchable_lease_t final: public vrr_clock::compositor_lease_t {
  public:
    switchable_lease_t(): compositor_lease_t({4242, 1000, 77}) {}

    mutable std::atomic<bool> is_current {true};
    std::function<void()> on_check;

    bool current() const override {
      if (on_check) on_check();
      return is_current.load();
    }
  };

  struct epoch_fixture_t {
    std::uint64_t epoch_id;
    std::uint64_t generation;
    std::shared_ptr<const vrr_clock::proof_t> proof = std::make_shared<const vrr_clock::proof_t>();
    std::shared_ptr<switchable_lease_t> lease = std::make_shared<switchable_lease_t>();

    vrr_clock::authority_publication_t publish() const {
      return vrr_clock::authority_publication_t {
        std::make_shared<vrr_clock::epoch_authority_t>(epoch_id, generation, proof, lease)
      };
    }
  };

  std::uint64_t unique_epoch() {
    static std::atomic<std::uint64_t> next {9'000'000};
    return next.fetch_add(1);
  }
}  // namespace

TEST(VrrEpochAuthorityTests, OnlyTheOpenEpochsOwnProofAndCurrentLeaseAreAuthority) {
  const epoch_fixture_t epoch {unique_epoch(), 5};
  auto publication = epoch.publish();
  ASSERT_TRUE(publication.published());
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::current);
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 6, epoch.proof.get()), authority_e::generation_mismatch);
  // An equal copy of the proof is a different object: identity, not equality.
  const auto copy = std::make_shared<const vrr_clock::proof_t>(*epoch.proof);
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, copy.get()), authority_e::proof_mismatch);
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, nullptr), authority_e::proof_mismatch);
  EXPECT_EQ(vrr_clock::check_epoch_authority(unique_epoch(), 5, epoch.proof.get()), authority_e::no_epoch);
  epoch.lease->is_current = false;
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::lease_not_current);
}

TEST(VrrEpochAuthorityTests, AReplacedOrDestroyedEpochNeverAnswersCurrentAgain) {
  const epoch_fixture_t first {unique_epoch(), 5};
  const epoch_fixture_t second {unique_epoch(), 5};
  auto publication = first.publish();
  // A reopen: the new epoch's publication is assigned over the old one, which is withdrawn.
  publication = second.publish();
  EXPECT_EQ(vrr_clock::check_epoch_authority(first.epoch_id, 5, first.proof.get()), authority_e::no_epoch);
  EXPECT_EQ(vrr_clock::check_epoch_authority(second.epoch_id, 5, second.proof.get()), authority_e::current);
  // The old epoch's lease is still current: authority is the epoch's, not the lease's alone.
  EXPECT_TRUE(first.lease->current());
  {
    auto moved = std::move(publication);
    EXPECT_EQ(vrr_clock::check_epoch_authority(second.epoch_id, 5, second.proof.get()), authority_e::current)
      << "a move transfers ownership without withdrawing";
  }
  EXPECT_EQ(vrr_clock::check_epoch_authority(second.epoch_id, 5, second.proof.get()), authority_e::no_epoch)
    << "destroying the owner withdraws it";
}

TEST(VrrEpochAuthorityTests, OnlyTheEpochHoldsTheLease) {
  const epoch_fixture_t epoch {unique_epoch(), 5};
  const auto before = epoch.lease.use_count();
  {
    auto publication = epoch.publish();
    EXPECT_EQ(epoch.lease.use_count(), before) << "the capture epoch's existing snapshot is the only owner";
    for (int check = 0; check < 3; ++check) {
      (void) vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get());
    }
    EXPECT_EQ(epoch.lease.use_count(), before) << "a check holds it only while checking";
  }
  EXPECT_EQ(epoch.lease.use_count(), before);
}

TEST(VrrEpochAuthorityTests, AnAuthorityCannotRetainOrReplaceAnExpiredEpochLease) {
  epoch_fixture_t epoch {unique_epoch(), 5};
  auto publication = epoch.publish();
  const std::weak_ptr<switchable_lease_t> original = epoch.lease;
  epoch.lease.reset();
  EXPECT_TRUE(original.expired()) << "publication must not prolong the original snapshot";
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::lease_not_current);
  epoch.lease = std::make_shared<switchable_lease_t>();
  EXPECT_TRUE(epoch.lease->current());
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::lease_not_current)
    << "a later current lease cannot replace the original one";
}

TEST(VrrEpochAuthorityTests, WithdrawalDuringTheLeaseCheckStillRefusesTheFrame) {
  const epoch_fixture_t epoch {unique_epoch(), 5};
  auto publication = epoch.publish();
  epoch.lease->on_check = [&] { publication.reset(); };
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::closed);
  EXPECT_EQ(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::no_epoch);
  EXPECT_EQ(epoch.lease.use_count(), 1);
}

TEST(VrrEpochAuthorityTests, ChecksRacingWithdrawalNeverSeeAClosedEpochAsCurrent) {
  for (int round = 0; round < 50; ++round) {
    const epoch_fixture_t epoch {unique_epoch(), 5};
    auto publication = std::make_unique<vrr_clock::authority_publication_t>(epoch.publish());
    std::atomic<bool> withdrawn {false};
    std::atomic<int> current_after_withdrawal {0};
    std::vector<std::thread> checkers;
    for (int thread = 0; thread < 4; ++thread) {
      checkers.emplace_back([&] {
        for (int check = 0; check < 200; ++check) {
          const bool was_withdrawn = withdrawn.load();
          const auto result = vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get());
          if (was_withdrawn && result == authority_e::current) current_after_withdrawal++;
        }
      });
    }
    publication.reset();
    withdrawn = true;
    for (auto &checker : checkers) checker.join();
    EXPECT_EQ(current_after_withdrawal.load(), 0) << "round " << round;
    EXPECT_NE(vrr_clock::check_epoch_authority(epoch.epoch_id, 5, epoch.proof.get()), authority_e::current);
  }
}
