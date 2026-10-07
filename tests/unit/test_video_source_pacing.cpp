/**
 * @file tests/unit/test_video_source_pacing.cpp
 * @brief Source-time pacing through the real parallel encode loop (Polaris #216).
 *
 * Every Linux case runs the production encode_published_displays()/encode_run() loop with the real
 * software H.264 encoder (libx264, encode in encode() after conversion), real stamped images from
 * the production capture-epoch open/stamp path, and the real pinned compositor lease from a
 * controlled child process. Assertions read the packets the loop actually produced and the runtime
 * pacing counters the loop actually reported, before the test's own stats entry is removed.
 */
#include <src/adaptive_bitrate.h>
#include <src/config.h>
#include <src/globals.h>
#include <src/platform/common.h>
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/stream_stats.h>
#include <src/thread_safe.h>
#include <src/utility.h>
#include <src/video.h>
#include <src/vrr_source_pacing.h>
#include <src/vrr_source_time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

/**
 * The adaptive controller fed the production source-paced inputs: a game below the ceiling is not
 * pressure, but a slow encoder still is. Fixed-rate inputs for the same game cut bitrate.
 */
TEST(SourcePacingPolicyTests, ALowerGameFrameRateIsNotEncoderPressureButASlowEncodeStillIs) {
  const auto saved = config::video.adaptive_bitrate;
  auto restore = util::fail_guard([saved] {
    config::video.adaptive_bitrate = saved;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
  });
  const auto enable = [] {
    config::video.adaptive_bitrate.enabled = true;
    config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
    config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    adaptive_bitrate::set_runtime_update_supported(true);
    adaptive_bitrate::set_base_bitrate(20000);
    // Initialise the controller, then let its adjustment interval elapse so every later sample is judged.
    adaptive_bitrate::update_network_stats(0.0, 8.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  };
  constexpr double ceiling_fps = 120.0;
  constexpr double game_fps = 90.0;
  constexpr double encode_ms = 9.0;  // over the 8.33 ms budget at 120, under the 11 ms threshold

  // The fixed-rate inputs for this game read as a delivery shortfall plus an over-budget encode.
  enable();
  adaptive_bitrate::update_stream_health(game_fps / ceiling_fps, 0.0, 0.0, 3.0, encode_ms, 5.0, ceiling_fps);
  EXPECT_LT(adaptive_bitrate::get_state().target_bitrate_kbps, 20000) << "the fixed-rate reading cuts bitrate";

  // The production source-paced inputs for the same samples: no cut.
  enable();
  for (int sample = 0; sample < 30; ++sample) {
    const auto inputs = vrr_pacing::source_health_inputs(0.0, sample < 2 ? std::nullopt : std::optional {0.3}, encode_ms, 5.0);
    adaptive_bitrate::update_stream_health(inputs.fps_ratio, inputs.dropped_frame_ratio, inputs.duplicate_frame_ratio,
                                           inputs.frame_jitter_ms, inputs.encode_time_ms, inputs.avg_frame_age_ms,
                                           ceiling_fps);
  }
  EXPECT_EQ(adaptive_bitrate::get_state().target_bitrate_kbps, 20000) << "90 fps under a 120 ceiling became pressure";
  EXPECT_NE(adaptive_bitrate::get_state().state, "encoder_pressure");

  // A genuinely slow encoder in the same session still lowers bitrate.
  enable();
  const auto slow = vrr_pacing::source_health_inputs(0.0, 0.3, 12.0, 5.0);
  adaptive_bitrate::update_stream_health(slow.fps_ratio, slow.dropped_frame_ratio, slow.duplicate_frame_ratio,
                                         slow.frame_jitter_ms, slow.encode_time_ms, slow.avg_frame_age_ms, ceiling_fps);
  EXPECT_LT(adaptive_bitrate::get_state().target_bitrate_kbps, 20000);
  EXPECT_EQ(adaptive_bitrate::get_state().state, "encoder_pressure");
}

#ifdef __linux__
  #include "platform/compositor_identity_fixture.h"

  #include <atomic>
  #include <functional>
  #include <memory>
  #include <string>
  #include <thread>
  #include <time.h>
  #include <vector>

using namespace std::chrono_literals;
using vrr_pacing::event_e;

namespace {
  constexpr int k_size = 64;
  constexpr std::uint64_t k_capture_generation = 41;

  std::int64_t monotonic_now_ns() {
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
  }

  std::int64_t steady_ns(std::chrono::steady_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
  }

  struct cpu_image_t: platf::img_t {
    std::vector<std::uint8_t> storage = std::vector<std::uint8_t>(k_size * k_size * 4, 0x40);

    cpu_image_t() {
      data = storage.data();
      width = k_size;
      height = k_size;
      pixel_pitch = 4;
      row_pitch = k_size * 4;
      frame_metadata = {
        .transport = platf::frame_transport_e::internal,
        .residency = platf::frame_residency_e::cpu,
        .format = platf::frame_format_e::bgra8,
      };
    }
  };

  /// The controlled compositor: its listening socket, the production supervisor handoff, its lease.
  struct pinned_compositor_t {
    compositor_identity_fixture::scratch_dir_t scratch;
    std::string socket_path = (scratch.path / "wayland-1").string();
    std::unique_ptr<compositor_identity_fixture::controlled_child_t> child;
    compositor_identity::unique_fd_t supervisor;
    std::shared_ptr<std::atomic<bool>> published = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<const vrr_clock::compositor_lease_t> lease;
    compositor_identity::unique_fd_t peer;

    pinned_compositor_t() {
      compositor_identity_fixture::controlled_child_t::options_t options;
      options.listen_path = socket_path;
      child = std::make_unique<compositor_identity_fixture::controlled_child_t>(options);
      auto received = child->receive();
      if (!received.identity) return;
      supervisor = compositor_identity::unique_fd_t {compositor_identity_fixture::open_pidfd(getpid())};
      auto flag = published;
      lease = compositor_identity::make_pinned_lease(
        std::move(*received.identity), supervisor.get(), socket_path,
        [flag](const vrr_clock::compositor_lease_t &) { return flag->load(); }
      );
      peer = compositor_identity::unique_fd_t {compositor_identity_fixture::connect_to(socket_path)};
    }

    bool ready() const {
      return lease && peer;
    }
  };

  /// A display on the private source-driven ext-image-copy route, connected to that compositor.
  class paced_display_t final: public platf::display_t {
  public:
    paced_display_t(int peer_fd, std::shared_ptr<const vrr_clock::compositor_lease_t> lease, bool source_route):
        peer_fd_ {peer_fd},
        lease_ {std::move(lease)} {
      width = k_size;
      height = k_size;
      env_width = k_size;
      env_height = k_size;
      capture_timing = source_route ?
        platf::capture_timing_route_t {platf::capture_pacing_e::source_driven, platf::presentation_clock_e::ext_image_copy_monotonic} :
        platf::capture_timing_route_t {platf::capture_pacing_e::fixed_interval, platf::presentation_clock_e::none};
    }

    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return std::make_shared<cpu_image_t>();
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

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e) override {
      // No device data: the session converts in software, as a CPU capture does.
      return std::make_unique<platf::avcodec_encode_device_t>();
    }

  private:
    int peer_fd_;
    std::shared_ptr<const vrr_clock::compositor_lease_t> lease_;
  };

  std::shared_ptr<stream::session_t> make_session() {
    stream::config_t config {};
    rtsp_stream::launch_session_t launch {};
    launch.id = 1;
    launch.gcm_key.resize(16);
    launch.iv.resize(16);
    launch.unique_id = "source-pacing-client";
    return stream::session::alloc(config, launch);
  }

  vrr_policy::pacing_intent_t opted_in() {
    return {true, vrr_policy::requested_e::on, false};
  }

  video::config_t stream_config(int fps, vrr_policy::pacing_intent_t intent, std::uint64_t session_generation) {
    video::config_t config {};
    config.width = k_size;
    config.height = k_size;
    config.framerate = fps;
    config.bitrate = 2000;
    // As a Moonlight client negotiates them.
    config.slicesPerFrame = 1;
    config.numRefFrames = 1;
    config.encoderCscMode = 0;
    config.videoFormat = 0;
    config.dynamicRange = 0;
    config.encodingFramerate = fps * 1000;
    config.stream_rate = {fps, 1};
    config.encode_rate = {fps, 1};
    config.session_generation = session_generation;
    config.capture_generation.generation_id = k_capture_generation;
    config.vrr_pacing = intent;
    return config;
  }

  constexpr int k_display_peer = -2;  ///< use the compositor's connected peer

  /**
   * The real parallel encode loop on its own thread, fed by the test, read through its packets and its
   * runtime pacing counters. Owns its test stats generation and removes it only after the loop joined.
   * The encoder selection is the caller's: one outer with_h264() for every loop of a case.
   */
  class paced_stream_t {
  public:
    paced_stream_t(const pinned_compositor_t &compositor, video::config_t config, bool source_route = true,
                   std::function<void()> after_convert = {}, int peer_fd = k_display_peer):
        display_ {std::make_shared<paced_display_t>(peer_fd == k_display_peer ? compositor.peer.get() : peer_fd,
                                                    compositor.lease, source_route)},
        session_ {make_session()},
        generation_ {config.session_generation},
        client_ip_ {"10.0.9." + std::to_string(config.session_generation % 250)},
        after_convert_ {std::move(after_convert)} {
      stream::session::set_state_for_tests(*session_, stream::session::state_e::RUNNING);
      mail_ = stream::session::mail_for_tests(*session_);
      // The diagnostics accept pacing events only for this registered, frozen generation.
      stream_stats::add_client(client_ip_, "source-pacing", generation_);
      auto record = vrr_policy::make_record(generation_, config.vrr_pacing.requested, vrr_policy::admission_e::launch,
                                            config.vrr_pacing.watch_or_input_only, false);
      record.host_source_pacing = config.vrr_pacing.host_opt_in;
      (void) stream_stats::record_vrr_session(record);
      const auto destination = stream::session::packet_destination_for_tests(*session_);
      loop_ = std::thread([this, config, destination] {
        const std::function<void()> observer = [this] {
          if (after_convert_) after_convert_();
        };
        video::with_after_convert_observer_for_tests(observer, [&] {
          video::encode_published_display_for_tests(mail_, config, display_, destination, images_);
        });
      });
    }

    ~paced_stream_t() {
      stop();
      stream_stats::remove_client(client_ip_, generation_);
    }

    paced_stream_t(const paced_stream_t &) = delete;
    paced_stream_t &operator=(const paced_stream_t &) = delete;

    void stop() {
      if (!loop_.joinable()) return;
      images_->stop();
      stream::session::stop(*session_);
      loop_.join();
    }

    const std::shared_ptr<paced_display_t> &display() const {
      return display_;
    }

    void raise(const std::shared_ptr<platf::img_t> &img) {
      images_->raise(img);
    }

    void request_idr() {
      mail_->event<bool>(mail::idr)->raise(true);
    }

    video::packet_t next_packet(std::chrono::milliseconds timeout) {
      return mail_->queue<video::packet_t>(mail::video_packets)->pop(timeout);
    }

    std::vector<video::packet_t> drain(std::chrono::milliseconds window) {
      std::vector<video::packet_t> packets;
      const auto deadline = std::chrono::steady_clock::now() + window;
      while (std::chrono::steady_clock::now() < deadline) {
        if (auto packet = next_packet(10ms)) packets.push_back(std::move(packet));
      }
      return packets;
    }

    /// This generation's runtime pacing counter as the loop reported it (read before cleanup).
    std::uint64_t count(event_e event) const {
      const auto stats = stream_stats::get_current();
      for (const auto &client : stats.clients) {
        if (client.session_generation == generation_ && client.vrr_pacing) {
          return client.vrr_pacing->event_counts[static_cast<std::size_t>(event)];
        }
      }
      return 0;
    }

    /// The loop publishes a submission's packet before it records that submission's event, and records a
    /// refusal with no packet at all: wait, bounded, for the counter to reach `expected`, then return it so
    /// the caller asserts the exact value. An excess is never waited away.
    std::uint64_t count_reaching(event_e event, std::uint64_t expected, std::chrono::milliseconds timeout = 2s) const {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      auto value = count(event);
      while (value < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        value = count(event);
      }
      return value;
    }

    nlohmann::json vrr() const {
      return stream_stats::vrr_json(stream_stats::get_current(), generation_);
    }

  private:
    std::shared_ptr<paced_display_t> display_;
    std::shared_ptr<stream::session_t> session_;
    std::uint64_t generation_;
    std::string client_ip_;
    safe::mail_t mail_;
    video::img_event_t images_ = std::make_shared<video::img_event_t::element_type>();
    std::function<void()> after_convert_;
    std::thread loop_;
  };

  /// A newly captured frame presented at present_mono_ns, stamped by the production epoch path.
  std::shared_ptr<platf::img_t> stamped_frame(video::capture_epoch_stepper_for_tests_t &epoch,
                                              std::int64_t present_mono_ns) {
    auto img = std::make_shared<cpu_image_t>();
    img->frame_timestamp = std::chrono::steady_clock::now();
    img->provenance.presentation = {present_mono_ns, vrr_policy::presentation_status_e::valid, 1};
    img->provenance.ready_at = std::chrono::steady_clock::now();
    epoch.push(*img, true);
    return img;
  }

  std::int64_t mapped_ns(const platf::img_t &img) {
    return img.provenance.source_time.source_steady_ns.value_or(-1);
  }

  /// Run body with the real software H.264 encoder selected once for every loop in it, then restored.
  void with_h264(const std::function<void()> &body) {
    ASSERT_TRUE(video::with_software_h264_for_tests(body)) << "this build has no software H.264 encoder";
  }

  #define REQUIRE_PRODUCTION_FIXTURE(compositor) \
    ASSERT_TRUE((compositor).ready()) << "the controlled compositor fixture could not start"
}  // namespace

TEST(SourcePacingProductionTests, UnevenSourceFramesAreEncodedAtExactlyTheirMappedSourceTimes) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9101)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    ASSERT_TRUE(epoch.opened().proof && epoch.opened().proof->clock_domain_verified);
    for (const auto &packet : stream.drain(150ms)) EXPECT_FALSE(packet->source_time_paced) << "readiness never claims authority";
    std::uint64_t paced = 0;
    for (const auto gap : {11ms, 8ms, 14ms, 10ms, 16ms, 9ms}) {
      std::this_thread::sleep_for(gap);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::mapped);
      const auto original = frame->frame_timestamp;
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet) << "no packet for the frame";
      ASSERT_TRUE(packet->frame_timestamp);
      // Causal marker (M2): exact mapped source time, never the grid or the capture handoff time.
      EXPECT_EQ(steady_ns(*packet->frame_timestamp), mapped_ns(*frame)) << "gap " << gap.count() << " ms";
      EXPECT_TRUE(packet->source_time_paced);
      EXPECT_EQ(frame->frame_timestamp, original) << "the image's own timestamp is never rewritten";
      ++paced;
    }
    EXPECT_EQ(stream.count_reaching(event_e::accepted_source, paced), paced);
    EXPECT_EQ(stream.vrr()["effective"], true);
  });
}

TEST(SourcePacingProductionTests, AStaticSourceGetsOnlyBoundedLivenessAndALabelledRecovery) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9102)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    auto frame = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(frame);
    ASSERT_TRUE(stream.next_packet(400ms));
    // 1.2 s with no new source frame. The fixed-rate loop re-encodes ~24 times a second here.
    const auto packets = stream.drain(1200ms);
    // Causal marker (M3): routine timeout repeats suppressed, only the bounded liveness floor.
    EXPECT_LE(packets.size(), 3u) << "routine timeout repeats were not suppressed";
    EXPECT_GE(packets.size(), 1u) << "the bounded liveness floor (500 ms) produced nothing";
    std::int64_t last = mapped_ns(*frame);
    for (const auto &packet : packets) {
      ASSERT_TRUE(packet->frame_timestamp);
      const auto at = steady_ns(*packet->frame_timestamp);
      EXPECT_GE(at - last, 400'000'000) << "liveness comes from the current time, not last + tick";
      EXPECT_TRUE(packet->source_time_paced) << "a liveness re-encode under current authority";
      last = at;
    }
    // The loop keeps running: one more floor emission may land between the drain's end and this read.
    const auto liveness = stream.count_reaching(event_e::liveness, packets.size());
    EXPECT_GE(liveness, packets.size());
    EXPECT_LE(liveness, packets.size() + 1) << "liveness emissions beyond the floor";
    EXPECT_GE(stream.count_reaching(event_e::routine_repeat_suppressed, 10u), 10u);
    stream.request_idr();
    // At most that one racing floor emission precedes the answer; it is still ordered and current.
    auto recovery = stream.next_packet(200ms);
    if (recovery && !recovery->is_idr()) {
      ASSERT_TRUE(recovery->frame_timestamp);
      EXPECT_GE(steady_ns(*recovery->frame_timestamp) - last, 400'000'000);
      last = steady_ns(*recovery->frame_timestamp);
      recovery = stream.next_packet(200ms);
    }
    ASSERT_TRUE(recovery) << "the IDR request was not answered";
    EXPECT_TRUE(recovery->is_idr());
    EXPECT_GT(steady_ns(*recovery->frame_timestamp), last);
    EXPECT_EQ(stream.count_reaching(event_e::accepted_source, 1u), 1u);
    EXPECT_EQ(stream.count_reaching(event_e::idr_recovery, 1u), 1u);
  });
}

TEST(SourcePacingProductionTests, TheRateCeilingBoundsActualEncodeWorkButAllowsACatchUpPair) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(60, opted_in(), 9103)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    // 240 fps of valid source frames for 250 ms against a 60 fps stream.
    int raised = 0;
    const auto burst_end = std::chrono::steady_clock::now() + 250ms;
    std::vector<video::packet_t> packets;
    while (std::chrono::steady_clock::now() < burst_end) {
      stream.raise(stamped_frame(epoch, monotonic_now_ns()));
      ++raised;
      std::this_thread::sleep_for(4ms);
      while (auto packet = stream.next_packet(0ms)) packets.push_back(std::move(packet));
    }
    for (auto &packet : stream.drain(100ms)) packets.push_back(std::move(packet));
    std::vector<std::int64_t> completions;
    for (const auto &packet : packets) {
      if (packet->source_time_paced && packet->encode_done_timestamp) completions.push_back(steady_ns(*packet->encode_done_timestamp));
    }
    EXPECT_GE(raised, 40);
    EXPECT_GE(completions.size(), 10u);
    // Causal marker: the ceiling bounds ACTUAL encode work. Within any span of actual encode
    // completions there are at most rate × span + the burst (+1 for the span's own endpoints).
    for (std::size_t first = 0; first < completions.size(); ++first) {
      for (std::size_t last = first + 1; last < completions.size(); ++last) {
        const double span_s = static_cast<double>(completions[last] - completions[first]) / 1e9;
        EXPECT_LE(static_cast<double>(last - first + 1), 60.0 * span_s + 2.0 + 1.0)
          << "encodes " << first << ".." << last << " exceeded the ceiling";
      }
    }
    EXPECT_GE(stream.count_reaching(event_e::rate_ceiling, 10u), 10u) << "intentional ceiling refusals are counted";
    // Refill, then a catch-up pair 8 ms apart: both are emitted at their own times.
    std::this_thread::sleep_for(250ms);
    auto first = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(first);
    auto first_packet = stream.next_packet(300ms);
    std::this_thread::sleep_for(8ms);
    auto second = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(second);
    auto second_packet = stream.next_packet(300ms);
    ASSERT_TRUE(first_packet && second_packet) << "the catch-up pair was not emitted";
    EXPECT_EQ(steady_ns(*first_packet->frame_timestamp), mapped_ns(*first));
    EXPECT_EQ(steady_ns(*second_packet->frame_timestamp), mapped_ns(*second));
  });
}

TEST(SourcePacingProductionTests, ALeaseRetiredDuringConversionIsNeverEncodedAndTheFallbackPersists) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    std::atomic<int> converts {0};
    std::atomic<int> retire_on {-1};
    const auto published = compositor.published;
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9104), true, [&] {
      // Stands in for a conversion that blocks while the generation is retired.
      if (++converts == retire_on.load()) published->store(false);
    }};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    auto before = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(before);
    auto paced = stream.next_packet(400ms);
    ASSERT_TRUE(paced);
    EXPECT_TRUE(paced->source_time_paced);
    EXPECT_EQ(steady_ns(*paced->frame_timestamp), mapped_ns(*before));
    std::this_thread::sleep_for(12ms);
    retire_on = converts.load() + 1;
    auto retired = stamped_frame(epoch, monotonic_now_ns());
    ASSERT_EQ(retired->provenance.source_time.result, vrr_clock::source_time_e::mapped);
    stream.raise(retired);
    for (const auto &packet : stream.drain(150ms)) {
      // Causal marker (M1): the post-conversion authority gate. Without it this frame goes at its time.
      EXPECT_NE(steady_ns(*packet->frame_timestamp), mapped_ns(*retired));
      EXPECT_FALSE(packet->source_time_paced);
    }
    // Republished and reopened (a new epoch): the generation stays in fallback.
    published->store(true);
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation};
    for (int index = 0; index < 4; ++index) {
      std::this_thread::sleep_for(20ms);
      auto later = stamped_frame(reopened, monotonic_now_ns());
      ASSERT_EQ(later->provenance.source_time.result, vrr_clock::source_time_e::mapped);
      stream.raise(later);
      if (auto packet = stream.next_packet(200ms)) {
        EXPECT_FALSE(packet->source_time_paced) << "a latched fallback was promoted again";
        EXPECT_NE(steady_ns(*packet->frame_timestamp), mapped_ns(*later));
      }
    }
    EXPECT_EQ(stream.count_reaching(event_e::authority_lost, 1u), 1u);
    EXPECT_EQ(stream.count_reaching(event_e::accepted_source, 1u), 1u);
    EXPECT_EQ(stream.vrr()["effective"], false);
    EXPECT_EQ(stream.vrr()["fallback_reason"], "authority_lost");
  });
}

TEST(SourcePacingProductionTests, ALeaseRetiredWhileIdleBlocksAuthorisedRecoveryAndTheFallbackPersists) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9112)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    auto frame = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(frame);
    auto paced = stream.next_packet(400ms);
    ASSERT_TRUE(paced);
    ASSERT_TRUE(paced->source_time_paced);
    // Retired while idle: no new capture, then an IDR re-encode of that source frame's surface.
    compositor.published->store(false);
    stream.request_idr();
    for (const auto &packet : stream.drain(700ms)) {
      // Causal marker: the idle at-use authority check on the retained surface's own epoch.
      EXPECT_FALSE(packet->source_time_paced) << "a recovery or liveness frame claimed lost authority";
    }
    EXPECT_EQ(stream.count_reaching(event_e::authority_lost, 1u), 1u);
    // A valid new epoch after republish stays in fallback.
    compositor.published->store(true);
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation};
    std::this_thread::sleep_for(15ms);
    auto later = stamped_frame(reopened, monotonic_now_ns());
    ASSERT_EQ(later->provenance.source_time.result, vrr_clock::source_time_e::mapped);
    stream.raise(later);
    if (auto packet = stream.next_packet(300ms)) EXPECT_FALSE(packet->source_time_paced);
    EXPECT_EQ(stream.count_reaching(event_e::authority_lost, 1u), 1u);
    EXPECT_EQ(stream.vrr()["effective"], false);
  });
}

TEST(SourcePacingProductionTests, EmissionStaysOrderedAcrossReplayEqualTickRecoveryAndANewEpoch) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9105)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    auto first = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(first);
    auto first_packet = stream.next_packet(400ms);
    ASSERT_TRUE(first_packet);
    std::int64_t last = steady_ns(*first_packet->frame_timestamp);
    stream.raise(first);
    EXPECT_FALSE(stream.next_packet(150ms)) << "a replayed source frame was emitted again";
    std::this_thread::sleep_for(10ms);
    auto anchor = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(anchor);
    auto anchor_packet = stream.next_packet(400ms);
    ASSERT_TRUE(anchor_packet);
    last = steady_ns(*anchor_packet->frame_timestamp);
    auto collision = stamped_frame(epoch, *anchor->provenance.presentation.ns + 5'000);
    ASSERT_EQ(collision->provenance.source_time.result, vrr_clock::source_time_e::mapped);
    stream.raise(collision);
    // Causal marker (M4): one 90 kHz tick of order between emissions.
    EXPECT_FALSE(stream.next_packet(150ms)) << "two emissions in one RTP tick";
    auto older = stamped_frame(epoch, monotonic_now_ns());
    std::this_thread::sleep_for(2ms);
    stream.request_idr();
    auto recovery = stream.next_packet(200ms);
    ASSERT_TRUE(recovery);
    ASSERT_GT(steady_ns(*recovery->frame_timestamp), last);
    last = steady_ns(*recovery->frame_timestamp);
    stream.raise(older);
    EXPECT_FALSE(stream.next_packet(150ms)) << "a source older than the recovery was emitted";
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation};
    std::this_thread::sleep_for(10ms);
    auto after = stamped_frame(reopened, monotonic_now_ns());
    stream.raise(after);
    auto after_packet = stream.next_packet(400ms);
    ASSERT_TRUE(after_packet);
    EXPECT_EQ(steady_ns(*after_packet->frame_timestamp), mapped_ns(*after));
    EXPECT_GE(steady_ns(*after_packet->frame_timestamp) - last, vrr_pacing::k_tick_ns);
    EXPECT_EQ(stream.count_reaching(event_e::stale_or_replayed, 1u), 1u);
    EXPECT_EQ(stream.count_reaching(event_e::tick_collision, 1u), 1u);
    EXPECT_EQ(stream.count_reaching(event_e::regressed_vs_emitted, 1u), 1u);
  });
}

TEST(SourcePacingProductionTests, IneligibleSessionsAndUnsupportedRoutesKeepTheFixedRateLoop) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    const std::vector<std::pair<vrr_policy::pacing_intent_t, bool>> cases {
      {{false, vrr_policy::requested_e::on, false}, true},  // host opt-in off (the default)
      {{true, vrr_policy::requested_e::malformed, false}, true},
      {{true, vrr_policy::requested_e::off, false}, true},
      {{true, vrr_policy::requested_e::on, true}, true},  // Watch / input-only
      {opted_in(), false},  // eligible, but a fixed-interval route without a presentation clock
    };
    std::uint64_t generation = 9106;
    for (const auto &[intent, source_route] : cases) {
      SCOPED_TRACE(generation);
      paced_stream_t stream {compositor, stream_config(120, intent, generation++), source_route};
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
      (void) stream.drain(100ms);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      const auto original = *frame->frame_timestamp;
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet);
      EXPECT_FALSE(packet->source_time_paced);
      // Exactly the fixed-rate loop: its pacer starts at the image's own capture timestamp.
      EXPECT_EQ(*packet->frame_timestamp, original);
      EXPECT_GE(stream.drain(600ms).size(), 5u) << "legacy repeats changed";
      EXPECT_EQ(stream.count(event_e::accepted_source), 0u);
      if (intent.eligible()) EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
    }
  });
}

TEST(SourcePacingProductionTests, OneStampedImageIsSourcePacedForAnEligibleConsumerAndLegacyForAnother) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    // Two concurrent real loops under one encoder selection, as two consumers of one producer.
    paced_stream_t eligible {compositor, stream_config(120, opted_in(), 9113)};
    paced_stream_t host_off {compositor, stream_config(120, {false, vrr_policy::requested_e::on, false}, 9114)};
    video::capture_epoch_stepper_for_tests_t epoch {*eligible.display(), k_capture_generation};
    (void) eligible.drain(150ms);
    (void) host_off.drain(150ms);
    auto frame = stamped_frame(epoch, monotonic_now_ns());
    const auto original = *frame->frame_timestamp;
    eligible.raise(frame);
    host_off.raise(frame);
    auto paced = eligible.next_packet(400ms);
    auto legacy = host_off.next_packet(400ms);
    ASSERT_TRUE(paced && legacy);
    EXPECT_TRUE(paced->source_time_paced);
    EXPECT_EQ(steady_ns(*paced->frame_timestamp), mapped_ns(*frame));
    EXPECT_FALSE(legacy->source_time_paced);
    EXPECT_EQ(*legacy->frame_timestamp, original);
    EXPECT_EQ(frame->frame_timestamp, original) << "the shared image was not rewritten by either consumer";
  });
}

TEST(SourcePacingProductionTests, ReadinessThenAnUnprovenEpochKeepsTheLegacyLoopAndAReopenAfterSourceFailsClosed) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    // A display with a lease but no presentation peer: its epochs can never prove the clock domain.
    {
      paced_stream_t stream {compositor, stream_config(120, opted_in(), 9115), true, {}, -1};
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
      const auto readiness = stream.drain(150ms);
      ASSERT_FALSE(readiness.empty()) << "no initial readiness";
      for (const auto &packet : readiness) EXPECT_FALSE(packet->source_time_paced);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_NE(frame->provenance.source_time.result, vrr_clock::source_time_e::mapped);
      const auto original = *frame->frame_timestamp;
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet);
      EXPECT_FALSE(packet->source_time_paced);
      EXPECT_EQ(*packet->frame_timestamp, original) << "readiness did not keep the session out of the legacy loop";
      EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
    }
    // After an effective source frame, a reopen whose epoch cannot prove anything stays fail-closed.
    {
      paced_stream_t stream {compositor, stream_config(120, opted_in(), 9116)};
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
      (void) stream.drain(150ms);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      stream.raise(frame);
      auto paced = stream.next_packet(400ms);
      ASSERT_TRUE(paced && paced->source_time_paced);
      const auto unproven_display = std::make_shared<paced_display_t>(-1, compositor.lease, true);
      video::capture_epoch_stepper_for_tests_t unproven {*unproven_display, k_capture_generation};
      std::this_thread::sleep_for(15ms);
      auto later = stamped_frame(unproven, monotonic_now_ns());
      stream.raise(later);
      for (const auto &packet : stream.drain(200ms)) EXPECT_FALSE(packet->source_time_paced);
      EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
    }
  });
}

TEST(SourcePacingProductionTests, AFrameCarryingAnotherProofObjectIsNeverSourcePaced) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9111)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation};
    (void) stream.drain(150ms);
    auto genuine = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(genuine);
    auto genuine_packet = stream.next_packet(400ms);
    ASSERT_TRUE(genuine_packet);
    EXPECT_TRUE(genuine_packet->source_time_paced);
    std::this_thread::sleep_for(12ms);
    auto forged = stamped_frame(epoch, monotonic_now_ns());
    ASSERT_EQ(forged->provenance.source_time.result, vrr_clock::source_time_e::mapped);
    forged->provenance.clock_proof = std::make_shared<const vrr_clock::proof_t>(*forged->provenance.clock_proof);
    stream.raise(forged);
    for (const auto &packet : stream.drain(150ms)) {
      EXPECT_NE(steady_ns(*packet->frame_timestamp), mapped_ns(*forged));
      EXPECT_FALSE(packet->source_time_paced);
    }
    std::this_thread::sleep_for(12ms);
    auto later = stamped_frame(epoch, monotonic_now_ns());
    stream.raise(later);
    if (auto packet = stream.next_packet(300ms)) EXPECT_FALSE(packet->source_time_paced);
    EXPECT_EQ(stream.count_reaching(event_e::authority_lost, 1u), 1u);
  });
}

#else
TEST(SourcePacingProductionTests, LinuxOnly) {
  GTEST_SKIP() << "the source-driven ext-image-copy route and its compositor fixture are Linux-only";
}
#endif
