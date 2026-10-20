/**
 * @file tests/unit/test_capture_provenance.cpp
 * @brief Producer provenance: minted per display epoch, stamped once, carried whole, recorded per consumer.
 */
#include <src/platform/common.h>
#include <src/stream_stats.h>
#include <src/utility.h>
#include <src/video.h>
#include <src/vrr_clock_proof.h>
#include <src/vrr_source_time.h>
#ifdef __linux__
  #include "platform/compositor_identity_fixture.h"

  #include <src/platform/linux/graphics.h>

  #include <thread>
  #include <time.h>
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
  /// A display whose only behaviour is the timing route and peer descriptor a backend reports.
  class timing_display_t final: public platf::display_t {
  public:
    explicit timing_display_t(platf::capture_timing_route_t timing) {
      capture_timing = timing;
    }

    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return std::make_shared<platf::img_t>();
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }

    int presentation_peer_fd() const override {
      return -1;  // no connected compositor socket in a unit test
    }
  };

  platf::image_provenance_t full_provenance() {
    platf::image_provenance_t provenance;
    provenance.capture_generation_id = 3;
    provenance.capture_epoch_id = 40;
    provenance.source_sequence = 7;
    provenance.route = {platf::capture_pacing_e::source_driven, platf::presentation_clock_e::ext_image_copy_monotonic};
    provenance.ready_at = std::chrono::steady_clock::time_point {123ms};
    provenance.presentation = {120'000'000, vrr_policy::presentation_status_e::valid};
    provenance.prefetched_init = true;
    provenance.clock_proof = std::make_shared<const vrr_clock::proof_t>();
    provenance.source_time.result = vrr_clock::source_time_e::mapped;
    provenance.source_time.presentation_baseline = 3;
    provenance.source_time.source_steady_ns = 120'000'500;
    provenance.source_time.uncertainty_ns = 1'100;
    provenance.source_time.observed_steady_ns = 123'000'000;
    provenance.host_adaptive_sync = std::make_shared<game_mode_adaptive_sync::store_t>();
    return provenance;
  }

  void expect_same(const platf::image_provenance_t &actual, const platf::image_provenance_t &expected) {
    EXPECT_EQ(actual.capture_generation_id, expected.capture_generation_id);
    EXPECT_EQ(actual.capture_epoch_id, expected.capture_epoch_id);
    EXPECT_EQ(actual.source_sequence, expected.source_sequence);
    EXPECT_EQ(actual.route, expected.route);
    EXPECT_EQ(actual.ready_at, expected.ready_at);
    EXPECT_EQ(actual.presentation, expected.presentation);
    EXPECT_EQ(actual.prefetched_init, expected.prefetched_init);
    EXPECT_EQ(actual.clock_proof, expected.clock_proof) << "the same immutable proof, not a copy";
    EXPECT_EQ(actual.source_time, expected.source_time) << "the per-frame receipt travels whole";
    EXPECT_EQ(actual.host_adaptive_sync, expected.host_adaptive_sync) << "the same capture observer, never a different session's";
  }
}  // namespace

TEST(CaptureProvenanceTests, AFrameViewKeepsTheSnapshotItWasMadeFrom) {
  auto image = std::make_shared<platf::img_t>();
  image->provenance = full_provenance();
  const auto accepted = image->provenance;
  const video::frame_t frame {image};
  expect_same(frame.provenance, accepted);
  // The pool reuses the source image later; the frame's snapshot must not follow it.
  video::reset_pooled_image_for_tests(*image);
  expect_same(frame.provenance, accepted);
}

TEST(CaptureProvenanceTests, PoolResetClearsEveryFieldBeforeReuse) {
  platf::img_t image;
  image.frame_timestamp = std::chrono::steady_clock::now();
  image.provenance = full_provenance();
  video::reset_pooled_image_for_tests(image);
  EXPECT_FALSE(image.frame_timestamp);
  expect_same(image.provenance, platf::image_provenance_t {});
}

TEST(CaptureProvenanceTests, EachOpenMintsANewNonzeroEpochAndStampsEachNewFrameOnce) {
  const timing_display_t display {{platf::capture_pacing_e::fixed_interval, platf::presentation_clock_e::none}};
  auto first = std::make_shared<platf::img_t>();
  auto repeat = std::make_shared<platf::img_t>();
  auto second = std::make_shared<platf::img_t>();
  const auto epoch = video::open_capture_epoch_for_tests(display, 5, {
    {first, true},
    {repeat, false},  // a timeout push: the retained frame, not a new source frame
    {first, true},  // pushed again before its pool reset: never restamped
    {second, true},
  });
  ASSERT_NE(epoch.epoch_id, 0u);
  EXPECT_EQ(epoch.route, display.capture_timing);
  EXPECT_FALSE(epoch.proof) << "a route without a presentation clock has nothing to prove";

  EXPECT_EQ(first->provenance.capture_epoch_id, epoch.epoch_id);
  EXPECT_EQ(first->provenance.capture_generation_id, 5u);
  EXPECT_EQ(first->provenance.source_sequence, 1u);
  EXPECT_EQ(first->provenance.route, display.capture_timing);
  EXPECT_EQ(repeat->provenance.capture_epoch_id, 0u);
  EXPECT_EQ(repeat->provenance.source_sequence, 0u);
  EXPECT_EQ(second->provenance.source_sequence, 2u);

  // A reopen is a new epoch, and its frames never share an identity with the previous one's.
  auto after_reopen = std::make_shared<platf::img_t>();
  const auto reopened = video::open_capture_epoch_for_tests(display, 5, {{after_reopen, true}});
  EXPECT_GT(reopened.epoch_id, epoch.epoch_id);
  EXPECT_EQ(after_reopen->provenance.capture_epoch_id, reopened.epoch_id);
  EXPECT_EQ(after_reopen->provenance.source_sequence, 1u);
}

TEST(CaptureProvenanceTests, AnOrdinaryDesktopMayHaveLogicalGenerationZeroButNeverEpochZero) {
  const timing_display_t display {{}};
  auto image = std::make_shared<platf::img_t>();
  const auto epoch = video::open_capture_epoch_for_tests(display, 0, {{image, true}});
  EXPECT_NE(epoch.epoch_id, 0u);
  EXPECT_EQ(image->provenance.capture_generation_id, 0u);
  EXPECT_EQ(image->provenance.capture_epoch_id, epoch.epoch_id);
  EXPECT_EQ(image->provenance.route.pacing, platf::capture_pacing_e::unknown);
  EXPECT_EQ(image->provenance.route.presentation, platf::presentation_clock_e::none);
  EXPECT_EQ(image->provenance.presentation.status, vrr_policy::presentation_status_e::missing)
    << "a route with no presentation clock never invents one";
}

TEST(CaptureProvenanceTests, StampingKeepsTheBackendsRawPresentationFields) {
  const timing_display_t display {{platf::capture_pacing_e::source_driven, platf::presentation_clock_e::none}};
  auto image = std::make_shared<platf::img_t>();
  image->provenance.ready_at = std::chrono::steady_clock::time_point {5ms};
  image->provenance.presentation = {4'000'000, vrr_policy::presentation_status_e::nonmonotonic};
  image->provenance.prefetched_init = true;
  (void) video::open_capture_epoch_for_tests(display, 1, {{image, true}});
  EXPECT_EQ(image->provenance.ready_at, std::chrono::steady_clock::time_point {5ms});
  EXPECT_EQ(image->provenance.presentation.ns, 4'000'000);
  EXPECT_EQ(image->provenance.presentation.status, vrr_policy::presentation_status_e::nonmonotonic);
  EXPECT_TRUE(image->provenance.prefetched_init);
}

#ifdef __linux__
TEST(CaptureProvenanceTests, AnExtImageCopyEpochCarriesAnHonestlyUnverifiedProof) {
  const timing_display_t display {{platf::capture_pacing_e::source_driven, platf::presentation_clock_e::ext_image_copy_monotonic}};
  auto image = std::make_shared<platf::img_t>();
  const auto epoch = video::open_capture_epoch_for_tests(display, 2, {{image, true}});
  ASSERT_TRUE(epoch.proof);
  EXPECT_EQ(epoch.proof->capture_epoch_id, epoch.epoch_id);
  EXPECT_EQ(epoch.proof->time_namespace, vrr_clock::namespace_e::unverifiable);
  EXPECT_EQ(epoch.proof->namespace_reason, "no_peer_socket");
  EXPECT_FALSE(epoch.proof->producer_identity_verified);
  EXPECT_EQ(epoch.proof->producer_identity_reason, "no_authoritative_compositor_identity");
  EXPECT_FALSE(epoch.proof->clock_domain_verified);
  EXPECT_EQ(image->provenance.clock_proof, epoch.proof) << "every image of the epoch shares one immutable proof";
}

namespace {
  /// A display connected to a real compositor socket, retaining that generation's lease as wlgrab does.
  class leased_display_t final: public platf::display_t {
  public:
    leased_display_t(int peer_fd, std::shared_ptr<const vrr_clock::compositor_lease_t> lease):
        peer_fd_ {peer_fd}, lease_ {std::move(lease)} {
      capture_timing = {platf::capture_pacing_e::source_driven, platf::presentation_clock_e::ext_image_copy_monotonic};
    }

    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return std::make_shared<platf::img_t>();
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }

    int presentation_peer_fd() const override {
      return peer_fd_;
    }

    std::shared_ptr<const vrr_clock::compositor_lease_t> compositor_lease() const override {
      return lease_;
    }

  private:
    int peer_fd_;
    std::shared_ptr<const vrr_clock::compositor_lease_t> lease_;
  };
}  // namespace

TEST(CaptureProvenanceTests, AnEpochVerifiesItsRealPeerOnlyAgainstTheDisplaysCurrentLease) {
  // A controlled compositor: a direct child listening on a real socket, handed over through the
  // production supervisor protocol and pinned exactly as the runtime pins its generation.
  compositor_identity_fixture::scratch_dir_t scratch;
  const auto socket_path = (scratch.path / "wayland-1").string();
  compositor_identity_fixture::controlled_child_t::options_t options;
  options.listen_path = socket_path;
  compositor_identity_fixture::controlled_child_t child {options};
  auto received = child.receive();
  ASSERT_TRUE(received.identity) << received.reason;
  compositor_identity::unique_fd_t supervisor {compositor_identity_fixture::open_pidfd(getpid())};
  auto published = std::make_shared<std::atomic<bool>>(true);
  const auto lease = compositor_identity::make_pinned_lease(
    std::move(*received.identity), supervisor.get(), socket_path,
    [published](const vrr_clock::compositor_lease_t &) { return published->load(); }
  );
  ASSERT_TRUE(lease);
  const compositor_identity::unique_fd_t peer {compositor_identity_fixture::connect_to(socket_path)};
  ASSERT_TRUE(peer);

  const leased_display_t display {peer.get(), lease};
  const auto epoch = video::open_capture_epoch_for_tests(display, 2, {});
  ASSERT_TRUE(epoch.proof);
  EXPECT_EQ(epoch.proof->time_namespace, vrr_clock::namespace_e::same) << epoch.proof->namespace_reason;
  EXPECT_TRUE(epoch.proof->producer_identity_verified) << epoch.proof->producer_identity_reason;
  EXPECT_EQ(epoch.proof->producer_identity_reason, "authoritative_compositor_peer_matched");
  EXPECT_TRUE(epoch.proof->mapping.valid) << epoch.proof->mapping.reason;
  EXPECT_TRUE(epoch.proof->clock_domain_verified);

  // The same peer without the display's lease (the previous always-unverified wiring) proves nothing.
  const leased_display_t unleased {peer.get(), nullptr};
  const auto without = video::open_capture_epoch_for_tests(unleased, 2, {});
  ASSERT_TRUE(without.proof);
  EXPECT_FALSE(without.proof->producer_identity_verified);
  EXPECT_EQ(without.proof->producer_identity_reason, "no_authoritative_compositor_identity");
  EXPECT_FALSE(without.proof->clock_domain_verified);

  // A lease for a different process never vouches for this peer, even while current.
  compositor_identity_fixture::controlled_child_t other;
  auto other_received = other.receive();
  ASSERT_TRUE(other_received.identity) << other_received.reason;
  const auto foreign = compositor_identity::make_pinned_lease(
    std::move(*other_received.identity), supervisor.get(), socket_path,
    [](const vrr_clock::compositor_lease_t &) { return true; }
  );
  ASSERT_TRUE(foreign);
  const leased_display_t mismatched {peer.get(), foreign};
  const auto wrong = video::open_capture_epoch_for_tests(mismatched, 2, {});
  ASSERT_TRUE(wrong.proof);
  EXPECT_FALSE(wrong.proof->producer_identity_verified);
  EXPECT_EQ(wrong.proof->producer_identity_reason, "compositor_peer_pid_mismatch");

  // Retirement: the display still holds the old lease, and a new epoch gets no authority from it.
  published->store(false);
  const auto retired = video::open_capture_epoch_for_tests(display, 2, {});
  ASSERT_TRUE(retired.proof);
  EXPECT_GT(retired.epoch_id, epoch.epoch_id);
  EXPECT_FALSE(retired.proof->producer_identity_verified);
  EXPECT_EQ(retired.proof->producer_identity_reason, "compositor_generation_unavailable");
  EXPECT_FALSE(retired.proof->clock_domain_verified);
  EXPECT_TRUE(epoch.proof->clock_domain_verified) << "an earlier epoch's immutable proof is unchanged";
}

namespace {
  std::int64_t monotonic_now_ns() {
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
  }

  /// A newly captured ext-image-copy frame as the backend fills it: a protocol time, then readiness.
  std::shared_ptr<platf::img_t> presented_frame(std::int64_t presentation_mono_ns, std::uint64_t baseline = 1,
                                                vrr_policy::presentation_status_e status = vrr_policy::presentation_status_e::valid,
                                                bool prefetched = false) {
    auto img = std::make_shared<platf::img_t>();
    img->provenance.presentation = {presentation_mono_ns, status, baseline};
    img->provenance.ready_at = std::chrono::steady_clock::now();
    img->provenance.prefetched_init = prefetched;
    return img;
  }
}  // namespace

TEST(CaptureProvenanceTests, EachNewFrameIsMappedOnlyUnderTheEpochsLeaseCurrentAtObservation) {
  // The real controlled compositor: its own listening socket, the production supervisor handoff and
  // the pinned lease, connected to through a real peer descriptor.
  compositor_identity_fixture::scratch_dir_t scratch;
  const auto socket_path = (scratch.path / "wayland-1").string();
  compositor_identity_fixture::controlled_child_t::options_t options;
  options.listen_path = socket_path;
  compositor_identity_fixture::controlled_child_t child {options};
  auto received = child.receive();
  ASSERT_TRUE(received.identity) << received.reason;
  compositor_identity::unique_fd_t supervisor {compositor_identity_fixture::open_pidfd(getpid())};
  auto published = std::make_shared<std::atomic<bool>>(true);
  const std::shared_ptr<const vrr_clock::compositor_lease_t> lease = compositor_identity::make_pinned_lease(
    std::move(*received.identity), supervisor.get(), socket_path,
    [published](const vrr_clock::compositor_lease_t &) { return published->load(); }
  );
  ASSERT_TRUE(lease);
  const compositor_identity::unique_fd_t peer {compositor_identity_fixture::connect_to(socket_path)};
  ASSERT_TRUE(peer);
  const leased_display_t display {peer.get(), lease};
  const auto held_before_epoch = lease.use_count();

  auto stepper = std::make_unique<video::capture_epoch_stepper_for_tests_t>(display, 2);
  const auto opened = stepper->opened();
  ASSERT_TRUE(opened.proof);
  ASSERT_TRUE(opened.proof->clock_domain_verified) << opened.proof->producer_identity_reason;
  EXPECT_EQ(lease.use_count(), held_before_epoch + 1) << "the epoch snapshots the display's lease once";

  // Presentation times are read after the epoch opened, as a live compositor's would be.
  const auto first_present = monotonic_now_ns();
  auto first = presented_frame(first_present);
  const auto pacing_timestamp = std::chrono::steady_clock::time_point {std::chrono::milliseconds {42}};
  first->frame_timestamp = pacing_timestamp;
  stepper->push(*first, true);
  const auto &mapped = first->provenance.source_time;
  ASSERT_EQ(mapped.result, vrr_clock::source_time_e::mapped) << vrr_clock::name(mapped.result);
  ASSERT_TRUE(mapped.source_steady_ns);
  EXPECT_EQ(*mapped.source_steady_ns, first_present + opened.proof->mapping.steady_to_mono_offset_ns);
  EXPECT_GT(mapped.uncertainty_ns, 0u);
  EXPECT_LE(mapped.uncertainty_ns, vrr_clock::source_time_limits_t {}.max_total_uncertainty_ns);
  EXPECT_EQ(mapped.presentation_baseline, 1u);
  EXPECT_EQ(first->frame_timestamp, pacing_timestamp) << "the pacing timestamp is never replaced";
  EXPECT_EQ(first->provenance.clock_proof, opened.proof) << "the historical proof pointer is stable";

  // A repeat push (no new source frame) is never stamped and gets no receipt.
  auto repeat = presented_frame(monotonic_now_ns());
  stepper->push(*repeat, false);
  EXPECT_EQ(repeat->provenance.capture_epoch_id, 0u);
  EXPECT_EQ(repeat->provenance.source_time.result, vrr_clock::source_time_e::not_observed);

  auto prefetched = presented_frame(monotonic_now_ns(), 1, vrr_policy::presentation_status_e::valid, true);
  stepper->push(*prefetched, true);
  EXPECT_EQ(prefetched->provenance.source_time.result, vrr_clock::source_time_e::prefetched_init);
  auto missing = presented_frame(0, 1, vrr_policy::presentation_status_e::missing);
  missing->provenance.presentation.ns.reset();
  stepper->push(*missing, true);
  EXPECT_EQ(missing->provenance.source_time.result, vrr_clock::source_time_e::missing);

  std::this_thread::sleep_for(std::chrono::milliseconds {2});
  const auto second_present = monotonic_now_ns();
  auto second = presented_frame(second_present);
  stepper->push(*second, true);
  ASSERT_EQ(second->provenance.source_time.result, vrr_clock::source_time_e::mapped)
    << vrr_clock::name(second->provenance.source_time.result);

  // The capture session was re-initialized inside this epoch (new tracker session), and its tracker
  // reports a time earlier than one already mapped as valid. The epoch refuses it; the previous path,
  // judging from the historical proof alone, did not.
  auto across_reset = presented_frame(second_present - 1'000, 2);
  stepper->push(*across_reset, true);
  EXPECT_EQ(across_reset->provenance.source_time.result, vrr_clock::source_time_e::regressed_in_epoch);
  EXPECT_FALSE(across_reset->provenance.source_time.source_steady_ns);
  EXPECT_EQ(vrr_clock::classify(vrr_policy::presentation_status_e::valid, second_present - 1'000, false,
                                across_reset->provenance.ready_at, opened.proof.get(), {}),
            vrr_clock::sample_e::valid);

  auto future = presented_frame(monotonic_now_ns() + 10'000'000);
  stepper->push(*future, true);
  EXPECT_EQ(future->provenance.source_time.result, vrr_clock::source_time_e::future);

  // Retire the generation mid-epoch. Same display, same epoch, same immutable proof.
  published->store(false);
  const auto retired_present = monotonic_now_ns();
  auto retired = presented_frame(retired_present);
  stepper->push(*retired, true);
  EXPECT_EQ(retired->provenance.source_time.result, vrr_clock::source_time_e::lease_not_current);
  EXPECT_FALSE(retired->provenance.source_time.source_steady_ns);
  EXPECT_EQ(retired->provenance.clock_proof, opened.proof);
  EXPECT_TRUE(vrr_clock::authorises_clock_comparison(*opened.proof)) << "the history itself is unchanged";
  EXPECT_EQ(vrr_clock::classify(vrr_policy::presentation_status_e::valid, retired_present, false,
                                retired->provenance.ready_at, opened.proof.get(), {}),
            vrr_clock::sample_e::valid)
    << "the previous path treated the historical proof as live authority";

  // Receipts and images hold no lease; only the epoch did.
  EXPECT_EQ(lease.use_count(), held_before_epoch + 1);
  stepper.reset();
  EXPECT_EQ(lease.use_count(), held_before_epoch);
}

TEST(CaptureProvenanceTests, DisplaysWithoutAnOwnedLeaseOrPresentationClockNeverMap) {
  compositor_identity_fixture::scratch_dir_t scratch;
  const auto socket_path = (scratch.path / "wayland-1").string();
  compositor_identity_fixture::controlled_child_t::options_t options;
  options.listen_path = socket_path;
  compositor_identity_fixture::controlled_child_t child {options};
  ASSERT_TRUE(child.receive().identity);
  const compositor_identity::unique_fd_t peer {compositor_identity_fixture::connect_to(socket_path)};
  ASSERT_TRUE(peer);

  const leased_display_t unleased {peer.get(), nullptr};
  video::capture_epoch_stepper_for_tests_t unowned {unleased, 2};
  auto frame = presented_frame(monotonic_now_ns());
  unowned.push(*frame, true);
  EXPECT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::identity_unverified);
  EXPECT_FALSE(frame->provenance.source_time.source_steady_ns);

  const timing_display_t fixed {{platf::capture_pacing_e::fixed_interval, platf::presentation_clock_e::none}};
  video::capture_epoch_stepper_for_tests_t no_clock {fixed, 2};
  auto plain = presented_frame(monotonic_now_ns());
  no_clock.push(*plain, true);
  EXPECT_EQ(plain->provenance.source_time.result, vrr_clock::source_time_e::no_presentation_clock);

  // A pooled image's reset clears its receipt with the rest of its provenance.
  video::reset_pooled_image_for_tests(*plain);
  EXPECT_EQ(plain->provenance.source_time, vrr_clock::source_time_receipt_t {});
}

TEST(CaptureProvenanceTests, ObserverCostCohortRecordsPerFrameMappingElapsedWithoutAThreshold) {
  // A separate measurement cohort: the real pinned compositor and its current lease, the production
  // epoch open and stamp, and many newly generated source frames. It records how long each push
  // (stamp + receipt) took as GTest properties. Diagnostic only: no timing threshold, and a busy CI
  // scheduler may legitimately refuse some frames (e.g. a wide clock bracket).
  constexpr std::size_t k_frames = 512;
  compositor_identity_fixture::scratch_dir_t scratch;
  const auto socket_path = (scratch.path / "wayland-1").string();
  compositor_identity_fixture::controlled_child_t::options_t options;
  options.listen_path = socket_path;
  compositor_identity_fixture::controlled_child_t child {options};
  auto received = child.receive();
  ASSERT_TRUE(received.identity) << received.reason;
  compositor_identity::unique_fd_t supervisor {compositor_identity_fixture::open_pidfd(getpid())};
  const std::shared_ptr<const vrr_clock::compositor_lease_t> lease = compositor_identity::make_pinned_lease(
    std::move(*received.identity), supervisor.get(), socket_path,
    [](const vrr_clock::compositor_lease_t &) { return true; }
  );
  ASSERT_TRUE(lease);
  const compositor_identity::unique_fd_t peer {compositor_identity_fixture::connect_to(socket_path)};
  ASSERT_TRUE(peer);
  const leased_display_t display {peer.get(), lease};
  video::capture_epoch_stepper_for_tests_t stepper {display, 2};
  const auto opened = stepper.opened();
  ASSERT_TRUE(opened.proof);
  ASSERT_TRUE(opened.proof->clock_domain_verified) << opened.proof->producer_identity_reason;

  // Allowed refusals under scheduling noise; anything else would be a gate failing for no reason.
  const std::array<vrr_clock::source_time_e, 5> scheduling_refusals {
    vrr_clock::source_time_e::mapping_drifted,
    vrr_clock::source_time_e::live_clock_unavailable,
    vrr_clock::source_time_e::regressed_in_epoch,  // two equal monotonic reads
    vrr_clock::source_time_e::stale,  // a preemption longer than the staleness bound
    vrr_clock::source_time_e::future,
  };
  std::map<std::string, std::size_t> refused_by;
  std::size_t mapped = 0;
  std::size_t refused = 0;
  std::int64_t total_ns = 0;
  std::int64_t max_ns = 0;
  for (std::size_t index = 0; index < k_frames; ++index) {
    const auto present = monotonic_now_ns();
    auto frame = presented_frame(present);
    const auto begin = std::chrono::steady_clock::now();
    stepper.push(*frame, true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    total_ns += elapsed;
    max_ns = std::max(max_ns, static_cast<std::int64_t>(elapsed));

    const auto &receipt = frame->provenance.source_time;
    ASSERT_NE(frame->provenance.capture_epoch_id, 0u) << "every generated frame is a new source frame";
    if (receipt.result == vrr_clock::source_time_e::mapped) {
      ++mapped;
      ASSERT_TRUE(receipt.source_steady_ns);
      EXPECT_EQ(*receipt.source_steady_ns, present + opened.proof->mapping.steady_to_mono_offset_ns);
      EXPECT_LE(receipt.uncertainty_ns, vrr_clock::source_time_limits_t {}.max_total_uncertainty_ns);
    } else {
      ++refused;
      ++refused_by[std::string(vrr_clock::name(receipt.result))];
      EXPECT_NE(std::find(scheduling_refusals.begin(), scheduling_refusals.end(), receipt.result), scheduling_refusals.end())
        << "unexpected refusal " << vrr_clock::name(receipt.result) << " under a current lease";
      EXPECT_FALSE(receipt.source_steady_ns) << "a refusal keeps no mapped value";
      EXPECT_EQ(receipt.uncertainty_ns, 0u);
    }
  }

  EXPECT_EQ(mapped + refused, k_frames);
  EXPECT_GE(mapped, 1u) << "a current lease and a quiet bracket must map at least once";
  RecordProperty("observer_cohort_samples", static_cast<int>(k_frames));
  RecordProperty("observer_cohort_mapped", static_cast<int>(mapped));
  RecordProperty("observer_cohort_refused", static_cast<int>(refused));
  RecordProperty("observer_cohort_mean_push_ns", std::to_string(total_ns / static_cast<std::int64_t>(k_frames)));
  RecordProperty("observer_cohort_max_push_ns", std::to_string(max_ns));
  for (const auto &[name, count] : refused_by) {
    RecordProperty("observer_cohort_refused_" + name, static_cast<int>(count));
  }
}

TEST(CaptureProvenanceTests, AConversionCloneCopiesTheAcceptedSnapshotNotTheSourceImage) {
  auto image = std::make_shared<egl::img_descriptor_t>();
  image->width = image->height = 4;
  image->frame_metadata = {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };
  std::fill_n(image->sd.fds, 4, -1);
  image->provenance = full_provenance();
  const auto accepted = image->provenance;
  const video::frame_t frame {image};

  // The source image changes after the consumer accepted the frame: first cleared, then restamped.
  video::reset_pooled_image_for_tests(*image);
  image->provenance.capture_epoch_id = 41;
  image->provenance.source_sequence = 1;
  const auto clone = video::clone_compat_image_for_conversion_for_tests(frame);
  ASSERT_TRUE(clone);
  expect_same(clone->provenance, accepted);
}
#endif

TEST(CaptureProvenanceTests, AConsumerAcceptsOnlyItsOwnLogicalCaptureGeneration) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  auto image_of = [](std::uint64_t logical_generation, std::uint64_t epoch, std::uint64_t sequence) {
    platf::image_provenance_t provenance;
    provenance.capture_generation_id = logical_generation;
    provenance.capture_epoch_id = epoch;
    provenance.source_sequence = sequence;
    return provenance;
  };

  stream_stats::add_client("10.0.0.21", "Private", 9201);
  video::config_t private_config {};
  private_config.session_generation = 9201;
  private_config.capture_generation.generation_id = 7;
  // A stale image of logical generation 6 never enters generation 7's consumer.
  EXPECT_EQ(video::record_accepted_captures_for_tests(private_config, {image_of(6, 910, 1)}), 0u);
  EXPECT_EQ(video::record_accepted_captures_for_tests(private_config, {image_of(0, 910, 1)}), 0u);
  EXPECT_EQ(video::record_accepted_captures_for_tests(private_config, {image_of(7, 910, 1)}), 1u);

  // Logical generation 0 is the ordinary desktop's valid generation, not a wildcard.
  stream_stats::add_client("10.0.0.22", "Desktop", 9202);
  video::config_t desktop_config {};
  desktop_config.session_generation = 9202;
  EXPECT_EQ(video::record_accepted_captures_for_tests(desktop_config, {image_of(7, 911, 1)}), 0u);
  EXPECT_EQ(video::record_accepted_captures_for_tests(desktop_config, {image_of(0, 911, 1)}), 1u);
}

namespace {
  std::uint64_t accepted_frames_of(std::uint64_t session_generation) {
    const auto stats = stream_stats::get_current();
    for (const auto &client : stats.clients) {
      if (client.session_generation == session_generation) {
        return client.capture_timing ? client.capture_timing->accepted_frames : 0;
      }
    }
    return 0;
  }

  platf::image_provenance_t accepted_image(std::uint64_t epoch, std::uint64_t sequence) {
    platf::image_provenance_t provenance;
    provenance.capture_epoch_id = epoch;
    provenance.source_sequence = sequence;
    return provenance;
  }
}  // namespace

TEST(CaptureProvenanceTests, AStaticSourcesFirstImageLandsOnceAfterLateRegistration) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  constexpr std::uint64_t generation = 9301;
  video::config_t config {};
  config.session_generation = generation;
  video::accepted_capture_consumer_for_tests_t consumer;

  // The video thread accepts its first image before the session registers its stats entry.
  EXPECT_FALSE(consumer.record(config, accepted_image(930, 1)));
  EXPECT_TRUE(consumer.has_pending());
  EXPECT_FALSE(consumer.retry(config)) << "still unregistered";
  EXPECT_TRUE(consumer.has_pending());

  // A static source: every later iteration only repeats that image, so only retries can land it.
  stream_stats::add_client("10.0.0.31", "Static", generation);
  EXPECT_TRUE(consumer.retry(config));
  EXPECT_FALSE(consumer.has_pending());
  EXPECT_EQ(accepted_frames_of(generation), 1u);

  // Further repeats never count it twice.
  EXPECT_FALSE(consumer.retry(config));
  EXPECT_FALSE(consumer.record(config, accepted_image(930, 1)));
  EXPECT_EQ(accepted_frames_of(generation), 1u);

  EXPECT_TRUE(consumer.record(config, accepted_image(930, 2)));
  EXPECT_EQ(accepted_frames_of(generation), 2u);

  // After teardown retires the entry, nothing lands, and a pending retry stays refused.
  stream_stats::remove_client("10.0.0.31", generation);
  EXPECT_FALSE(consumer.record(config, accepted_image(930, 3)));
  EXPECT_FALSE(consumer.retry(config));
  EXPECT_EQ(accepted_frames_of(generation), 0u);
}

TEST(CaptureProvenanceTests, ANewerAcceptedImageReplacesThePendingOne) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  constexpr std::uint64_t generation = 9302;
  video::config_t config {};
  config.session_generation = generation;
  video::accepted_capture_consumer_for_tests_t consumer;
  EXPECT_FALSE(consumer.record(config, accepted_image(931, 1)));
  EXPECT_FALSE(consumer.record(config, accepted_image(931, 2)));
  stream_stats::add_client("10.0.0.32", "Replaced", generation);
  EXPECT_TRUE(consumer.retry(config));
  EXPECT_EQ(accepted_frames_of(generation), 1u) << "only the newest pending observation lands";
  EXPECT_FALSE(consumer.retry(config));

  // Input-only and generation-0 consumers never publish, not even by retry.
  video::accepted_capture_consumer_for_tests_t input_only;
  auto input_config = config;
  input_config.input_only = true;
  EXPECT_FALSE(input_only.record(input_config, accepted_image(932, 1)));
  EXPECT_FALSE(input_only.has_pending());
  EXPECT_FALSE(input_only.retry(input_config));
}

TEST(CaptureProvenanceTests, APendingObservationIsNeverTransferredToAnotherLogicalGeneration) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  constexpr std::uint64_t generation = 9303;
  video::config_t config {};
  config.session_generation = generation;
  config.capture_generation.generation_id = 4;
  video::accepted_capture_consumer_for_tests_t consumer;
  auto image = accepted_image(933, 1);
  image.capture_generation_id = 4;
  EXPECT_FALSE(consumer.record(config, image));  // refused before registration
  ASSERT_TRUE(consumer.has_pending());

  stream_stats::add_client("10.0.0.33", "Moved", generation);
  auto moved = config;
  moved.capture_generation.generation_id = 5;
  EXPECT_FALSE(consumer.retry(moved));
  EXPECT_FALSE(consumer.has_pending()) << "dropped, not kept for a later consumer";
  EXPECT_EQ(accepted_frames_of(generation), 0u);
  EXPECT_FALSE(consumer.retry(config)) << "nothing left to transfer";
}

TEST(CaptureProvenanceTests, TheEpochAllocatorFailsClosedAtExhaustionAndNeverReissues) {
  // A caller-owned allocator seeded at the boundary: the process-wide one is never touched.
  std::atomic<std::uint64_t> allocator {std::numeric_limits<std::uint64_t>::max() - 1};
  EXPECT_EQ(video::allocate_capture_identity_for_tests(allocator), std::numeric_limits<std::uint64_t>::max() - 1);
  EXPECT_EQ(video::allocate_capture_identity_for_tests(allocator), std::numeric_limits<std::uint64_t>::max());
  for (int attempt = 0; attempt < 3; ++attempt) {
    EXPECT_EQ(video::allocate_capture_identity_for_tests(allocator), 0u) << "exhausted stays exhausted";
  }
  EXPECT_EQ(allocator.load(), 0u);

  std::atomic<std::uint64_t> fresh {1};
  EXPECT_EQ(video::allocate_capture_identity_for_tests(fresh), 1u);
  EXPECT_EQ(video::allocate_capture_identity_for_tests(fresh), 2u);
}

TEST(CaptureProvenanceTests, AnExhaustedSequenceLeavesImagesUnstampedAndUnrecordable) {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  std::vector<std::shared_ptr<platf::img_t>> images;
  for (int index = 0; index < 4; ++index) images.push_back(std::make_shared<platf::img_t>());
  video::stamp_seeded_epoch_for_tests(77, 0, max - 1, {
    {images[0], true}, {images[1], true}, {images[2], true}, {images[3], true},
  });
  EXPECT_EQ(images[0]->provenance.source_sequence, max - 1);
  EXPECT_EQ(images[1]->provenance.source_sequence, max);
  for (const auto index : {2, 3}) {
    EXPECT_EQ(images[index]->provenance.capture_epoch_id, 0u) << "no identity is reissued after exhaustion";
    EXPECT_EQ(images[index]->provenance.source_sequence, 0u);
  }

  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.23", "Exhausted", 9203);
  video::config_t config {};
  config.session_generation = 9203;
  EXPECT_EQ(video::record_accepted_captures_for_tests(config, {images[2]->provenance, images[3]->provenance}), 0u);
}

TEST(CaptureProvenanceTests, AConsumerRecordsOnlyNewStampedFramesAndNeverAsInputOnly) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  constexpr std::uint64_t generation = 9101;
  stream_stats::add_client("10.0.0.9", "Consumer", generation);

  video::config_t config {};
  config.session_generation = generation;
  auto stamped = [](std::uint64_t sequence) {
    platf::image_provenance_t provenance;
    provenance.capture_epoch_id = 900;
    provenance.source_sequence = sequence;
    return provenance;
  };
  EXPECT_EQ(video::record_accepted_captures_for_tests(config, {
    platf::image_provenance_t {},  // unstamped: a probe's or a timeout's image
    stamped(1),
    stamped(1),  // the same retained frame accepted again
    stamped(2),
  }), 2u);

  config.input_only = true;
  EXPECT_EQ(video::record_accepted_captures_for_tests(config, {stamped(3)}), 0u);
  config.input_only = false;
  config.session_generation = 0;
  EXPECT_EQ(video::record_accepted_captures_for_tests(config, {stamped(4)}), 0u);
}
