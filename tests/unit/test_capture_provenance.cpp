/**
 * @file tests/unit/test_capture_provenance.cpp
 * @brief Producer provenance: minted per display epoch, stamped once, carried whole, recorded per consumer.
 */
#include <src/platform/common.h>
#include <src/stream_stats.h>
#include <src/utility.h>
#include <src/video.h>
#include <src/vrr_clock_proof.h>
#ifdef __linux__
  #include <src/platform/linux/graphics.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
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
