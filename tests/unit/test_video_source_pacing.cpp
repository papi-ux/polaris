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
#include <src/vrr_epoch_authority.h>
#include <src/vrr_source_pacing.h>
#include <src/vrr_source_time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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

  #include <src/platform/linux/vrr_clock_probe.h>

  #include <algorithm>
  #include <atomic>
  #include <functional>
  #include <memory>
  #include <stdexcept>
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

  /// Diagnostic for a stamped frame's per-frame receipt and its epoch proof: names the first failing
  /// gate (the receipt result) and the proof's own mapping evidence. Read-only; asserts nothing.
  std::string receipt_diagnostic(const char *frame, const platf::img_t &img) {
    const auto &receipt = img.provenance.source_time;
    std::string out = std::string {frame} + ": receipt=" + std::string {vrr_clock::name(receipt.result)} +
                      " receipt_uncertainty_ns=" + std::to_string(receipt.uncertainty_ns) +
                      " observed_steady_ns=" + std::to_string(receipt.observed_steady_ns) +
                      " epoch=" + std::to_string(img.provenance.capture_epoch_id) +
                      " presentation_ns=" + (img.provenance.presentation.ns ? std::to_string(*img.provenance.presentation.ns) : std::string {"none"});
    const auto &proof = img.provenance.clock_proof;
    if (!proof) return out + " proof=none";
    return out + " proof.epoch=" + std::to_string(proof->capture_epoch_id) +
           " proof.mapping.valid=" + std::to_string(proof->mapping.valid) +
           " proof.mapping.reason=" + std::string {proof->mapping.reason} +
           " proof.mapping.samples=" + std::to_string(proof->mapping.samples) +
           " proof.mapping.max_bracket_ns=" + std::to_string(proof->mapping.max_bracket_ns) +
           " proof.mapping.max_delta_ns=" + std::to_string(proof->mapping.max_delta_ns) +
           " proof.mapping.uncertainty_ns=" + std::to_string(proof->mapping.uncertainty_ns) +
           " proof.clock_domain_verified=" + std::to_string(proof->clock_domain_verified) +
           " proof.namespace_reason=" + std::string {proof->namespace_reason} +
           " proof.producer_identity_verified=" + std::to_string(proof->producer_identity_verified) +
           " proof.producer_identity_reason=" + std::string {proof->producer_identity_reason} +
           " proof.epoch_start_mono_ns=" + std::to_string(proof->epoch_start_mono_ns);
  }

  /// The fixed, in-bound bracket of every stable clock sample (well inside the 20 us per-frame limit).
  constexpr std::int64_t k_stable_bracket_ns = 1'000;

  /**
   * The clocks of a positive mapped-source fixture epoch. Every kernel fact collect() reads (socket peer,
   * uid, start times, time namespaces) and the epoch start are the real kernel's, unchanged. Only the
   * bracketed clock samples are stable: one real CLOCK_MONOTONIC read m becomes
   * {before = m - floor(b / 2), steady = m + d, after = before + b}, so the clock advances with real
   * elapsed time (liveness, source age, the rate ceiling and the one-second health window stay real)
   * while scheduling jitter between two reads cannot widen a bracket. The steady coordinate is m itself
   * because steady_clock is CLOCK_MONOTONIC here, checked by stable_clock() before any use. Production
   * limits, lease checks and pacing are untouched; negative cases arm out-of-bound values through the
   * same production collect() and per-frame paths. Armed only from the test thread, between pushes.
   */
  class stable_real_time_clock_t final: public vrr_clock::system_t {
  public:
    std::optional<peer_cred_t> peer_credentials(int fd) override {
      return real().peer_credentials(fd);
    }

    std::optional<vrr_clock::ns_id_t> time_namespace(std::int64_t pid) override {
      return real().time_namespace(pid);
    }

    std::optional<std::uint64_t> process_starttime(std::int64_t pid) override {
      return real().process_starttime(pid);
    }

    std::uint32_t current_uid() override {
      return real().current_uid();
    }

    std::int64_t monotonic_now_ns() override {
      return unusable_ ? vrr_clock::k_failed_read : real().monotonic_now_ns();
    }

    vrr_clock::probe_sample_t sample_clocks() override {
      vrr_clock::probe_sample_t sample;  // every field k_failed_read
      const auto m = unusable_ ? vrr_clock::k_failed_read : real().monotonic_now_ns();
      if (m < 0) return sample;
      std::int64_t bracket = live_bracket_ns_;
      std::int64_t drift = live_drift_ns_;
      if (const auto pending = proof_samples_pending_.load(); pending > 0) {
        proof_samples_pending_ = pending - 1;
        bracket = proof_bracket_ns_;
        drift = 0;
      }
      sample.mono_before_ns = m - bracket / 2;  // floor: both operands are nonnegative
      sample.mono_after_ns = sample.mono_before_ns + bracket;  // exactly bracket wide, odd widths included
      sample.steady_ns = m + drift;
      return sample;
    }

    /// The next collect(), which takes exactly the default proof sample count, reads this bracket.
    void arm_proof(std::int64_t bracket_ns) {
      proof_bracket_ns_ = bracket_ns;
      proof_samples_pending_ = static_cast<int>(std::clamp(
        vrr_clock::mapping_limits_t {}.min_samples, vrr_clock::k_min_probe_samples, vrr_clock::k_max_probe_samples
      ));
    }

    int proof_samples_pending() const {
      return proof_samples_pending_;
    }

    /// Every later per-frame sample reads this bracket and steady drift, until disarmed.
    void arm(std::int64_t bracket_ns, std::int64_t drift_ns) {
      live_bracket_ns_ = bracket_ns;
      live_drift_ns_ = drift_ns;
    }

    void disarm() {
      arm(k_stable_bracket_ns, 0);
    }

    /// A host whose steady clock is not CLOCK_MONOTONIC: every read fails, so no proof can be built.
    void make_unusable() {
      unusable_ = true;
    }

  private:
    static vrr_clock::system_t &real() {
      return vrr_clock::linux_probe::real_system();
    }

    std::atomic<bool> unusable_ {false};
    std::atomic<int> proof_samples_pending_ {0};
    std::atomic<std::int64_t> proof_bracket_ns_ {k_stable_bracket_ns};
    std::atomic<std::int64_t> live_bracket_ns_ {k_stable_bracket_ns};
    std::atomic<std::int64_t> live_drift_ns_ {0};
  };

  /**
   * Stable clocks for one positive fixture epoch, after confirming on this host that a real steady
   * read lies inside a real CLOCK_MONOTONIC bracket (the steady coordinate offset is then 0). A host
   * where it does not is a test failure, never a skip: the clocks then read nothing, so the fixture's
   * mapped-receipt preconditions fail with their named receipt.
   */
  std::shared_ptr<stable_real_time_clock_t> stable_clock() {
    const auto real = vrr_clock::linux_probe::real_system().sample_clocks();
    const bool inside = real.mono_before_ns >= 0 && real.steady_ns >= 0 && real.mono_after_ns >= 0 &&
                        real.mono_before_ns <= real.steady_ns && real.steady_ns <= real.mono_after_ns;
    EXPECT_TRUE(inside) << "steady_clock is not CLOCK_MONOTONIC on this host: before=" << real.mono_before_ns
                        << " steady=" << real.steady_ns << " after=" << real.mono_after_ns;
    auto clocks = std::make_shared<stable_real_time_clock_t>();
    if (!inside) clocks->make_unusable();
    return clocks;
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
    ASSERT_TRUE(epoch.opened().proof && epoch.opened().proof->clock_domain_verified);
    const auto readiness = stream.drain(150ms);
    ASSERT_FALSE(readiness.empty()) << "no initial readiness";
    for (const auto &packet : readiness) {
      EXPECT_FALSE(packet->source_time_paced) << "readiness never claims authority";
      // Its timestamp is still in the source-submission coordinates the source frames use.
      EXPECT_TRUE(packet->source_clock_timestamp) << "readiness left the source-clock timestamp coordinates";
    }
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
      EXPECT_TRUE(packet->source_clock_timestamp);
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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
      EXPECT_TRUE(packet->source_clock_timestamp) << "a latched fallback left the source-clock coordinates";
    }
    // Republished and reopened (a new epoch): the generation stays in fallback.
    published->store(true);
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation, stable_clock()};
    for (int index = 0; index < 4; ++index) {
      std::this_thread::sleep_for(20ms);
      auto later = stamped_frame(reopened, monotonic_now_ns());
      ASSERT_EQ(later->provenance.source_time.result, vrr_clock::source_time_e::mapped);
      stream.raise(later);
      if (auto packet = stream.next_packet(200ms)) {
        EXPECT_FALSE(packet->source_time_paced) << "a latched fallback was promoted again";
        EXPECT_TRUE(packet->source_clock_timestamp) << "a latched fallback left the source-clock coordinates";
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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
      EXPECT_TRUE(packet->source_clock_timestamp) << "the latched recovery left the source-clock coordinates";
    }
    EXPECT_EQ(stream.count_reaching(event_e::authority_lost, 1u), 1u);
    // A valid new epoch after republish stays in fallback.
    compositor.published->store(true);
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation, stable_clock()};
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
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
    (void) stream.drain(150ms);
    // Every frame this test relies on must carry a mapped receipt from the production stamp path; a
    // refusal is a fatal, named precondition failure (never skipped, never an ignored frame).
    auto first = stamped_frame(epoch, monotonic_now_ns());
    ASSERT_EQ(first->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("first", *first);
    stream.raise(first);
    auto first_packet = stream.next_packet(400ms);
    ASSERT_TRUE(first_packet);
    ASSERT_TRUE(first_packet->frame_timestamp) << "first packet carries no frame timestamp";
    std::int64_t last = steady_ns(*first_packet->frame_timestamp);
    stream.raise(first);
    EXPECT_FALSE(stream.next_packet(150ms)) << "a replayed source frame was emitted again";
    std::this_thread::sleep_for(10ms);
    auto anchor = stamped_frame(epoch, monotonic_now_ns());
    ASSERT_EQ(anchor->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("anchor", *anchor);
    stream.raise(anchor);
    auto anchor_packet = stream.next_packet(400ms);
    ASSERT_TRUE(anchor_packet);
    ASSERT_TRUE(anchor_packet->frame_timestamp) << "anchor packet carries no frame timestamp";
    last = steady_ns(*anchor_packet->frame_timestamp);
    auto collision = stamped_frame(epoch, *anchor->provenance.presentation.ns + 5'000);
    ASSERT_EQ(collision->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("collision", *collision);
    stream.raise(collision);
    // Causal marker (M4): one 90 kHz tick of order between emissions.
    EXPECT_FALSE(stream.next_packet(150ms)) << "two emissions in one RTP tick";
    auto older = stamped_frame(epoch, monotonic_now_ns());
    ASSERT_EQ(older->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("older", *older);
    std::this_thread::sleep_for(2ms);
    stream.request_idr();
    auto recovery = stream.next_packet(200ms);
    ASSERT_TRUE(recovery);
    ASSERT_TRUE(recovery->frame_timestamp) << "recovery packet carries no frame timestamp";
    ASSERT_GT(steady_ns(*recovery->frame_timestamp), last);
    last = steady_ns(*recovery->frame_timestamp);
    stream.raise(older);
    EXPECT_FALSE(stream.next_packet(150ms)) << "a source older than the recovery was emitted";
    video::capture_epoch_stepper_for_tests_t reopened {*stream.display(), k_capture_generation, stable_clock()};
    std::this_thread::sleep_for(10ms);
    auto after = stamped_frame(reopened, monotonic_now_ns());
    ASSERT_EQ(after->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("after", *after);
    stream.raise(after);
    auto after_packet = stream.next_packet(400ms);
    ASSERT_TRUE(after_packet);
    ASSERT_TRUE(after_packet->frame_timestamp) << "after packet carries no frame timestamp";
    ASSERT_TRUE(after_packet->source_time_paced) << "the packet compared to after's mapped time was not a source-time emission; "
                                                 << receipt_diagnostic("after", *after);
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
      // None of these sessions ever records a source submission, so no packet carries either flag.
      for (const auto &packet : stream.drain(100ms)) {
        EXPECT_FALSE(packet->source_time_paced);
        EXPECT_FALSE(packet->source_clock_timestamp);
      }
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      const auto original = *frame->frame_timestamp;
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet);
      EXPECT_FALSE(packet->source_time_paced);
      EXPECT_FALSE(packet->source_clock_timestamp);
      // Exactly the fixed-rate loop: its pacer starts at the image's own capture timestamp.
      EXPECT_EQ(*packet->frame_timestamp, original);
      const auto repeats = stream.drain(600ms);
      EXPECT_GE(repeats.size(), 5u) << "legacy repeats changed";
      for (const auto &repeat : repeats) EXPECT_FALSE(repeat->source_clock_timestamp);
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
    video::capture_epoch_stepper_for_tests_t epoch {*eligible.display(), k_capture_generation, stable_clock()};
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
    EXPECT_TRUE(paced->source_clock_timestamp);
    EXPECT_EQ(steady_ns(*paced->frame_timestamp), mapped_ns(*frame));
    EXPECT_FALSE(legacy->source_time_paced);
    EXPECT_FALSE(legacy->source_clock_timestamp) << "the concurrent legacy consumer took source-clock coordinates";
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
      for (const auto &packet : readiness) {
        EXPECT_FALSE(packet->source_time_paced);
        EXPECT_TRUE(packet->source_clock_timestamp) << "readiness was submitted through the source-submission record";
      }
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_NE(frame->provenance.source_time.result, vrr_clock::source_time_e::mapped);
      const auto original = *frame->frame_timestamp;
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet);
      EXPECT_FALSE(packet->source_time_paced);
      EXPECT_FALSE(packet->source_clock_timestamp) << "the pure legacy loop kept source-clock coordinates";
      EXPECT_EQ(*packet->frame_timestamp, original) << "readiness did not keep the session out of the legacy loop";
      EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
    }
    // After an effective source frame, a reopen whose epoch cannot prove anything stays fail-closed.
    {
      paced_stream_t stream {compositor, stream_config(120, opted_in(), 9116)};
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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
      for (const auto &packet : stream.drain(200ms)) {
        EXPECT_FALSE(packet->source_time_paced);
        EXPECT_TRUE(packet->source_clock_timestamp) << "the latched fallback left the source-clock coordinates";
      }
      EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
    }
  });
}

TEST(SourcePacingProductionTests, AFrameCarryingAnotherProofObjectIsNeverSourcePaced) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9111)};
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
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

TEST(SourcePacingProductionTests, ThePeriodicPolicySampleIsSourceRelativeForItsOwnGenerationAndAFixedTargetMissIsNotAWarning) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  constexpr std::uint64_t generation = 9117;
  constexpr double ceiling_fps = 120.0;
  const bool host_flag = config::video.vrr_source_pacing;
  with_h264([&] {
    // Declared before the stream, so it runs after the stream's destructor joined the loop and removed
    // its generation: this case's own stats and policy sample are reset on an ASSERT early return too.
    auto reset_own_stats = util::fail_guard([] { stream_stats::update_stream_active(false); });
    {
      paced_stream_t stream {compositor, stream_config(static_cast<int>(ceiling_fps), opted_in(), generation)};
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, stable_clock()};
      ASSERT_TRUE(epoch.opened().proof && epoch.opened().proof->clock_domain_verified);
      (void) stream.drain(150ms);
      // The loop's own periodic branch, read through the published policy sample: this generation's
      // source label, its source-handoff cadence, and a delivered rate measured over a full one-second
      // window, so the fixed 120 FPS target is genuinely missed (~77 FPS of uneven source frames).
      const auto sampled = [&](const stream_stats::stats_t &stats) {
        return stats.video_policy_session_generation == generation && stats.video_policy_source_time &&
               stats.video_policy_cadence_available && stats.fps > 0.0 && stats.fps < ceiling_fps * 0.88;
      };
      std::uint64_t paced = 0;
      bool observed = false;
      for (int index = 0; index < 220 && !(observed && paced >= 35); ++index) {
        // The source feed itself: uneven 12/14 ms intervals, each longer than the ceiling's 8.33 ms, so
        // the feed rate stays below the 120 FPS ceiling.
        std::this_thread::sleep_for(index % 2 ? 14ms : 12ms);
        auto frame = stamped_frame(epoch, monotonic_now_ns());
        ASSERT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic("periodic", *frame);
        stream.raise(frame);
        while (auto packet = stream.next_packet(0ms)) {
          if (packet->source_time_paced) ++paced;
        }
        observed = observed || sampled(stream_stats::get_current());
      }
      // Packets publish before their events; the sample may follow the last packet. Bounded.
      for (auto deadline = std::chrono::steady_clock::now() + 2s; !observed && std::chrono::steady_clock::now() < deadline;) {
        while (auto packet = stream.next_packet(10ms)) {
          if (packet->source_time_paced) ++paced;
        }
        observed = sampled(stream_stats::get_current());
      }
      ASSERT_TRUE(observed) << "the periodic branch never published this generation's source-relative sample";
      // Collect what the last raised frames still had in flight (well inside the 500 ms liveness floor).
      for (const auto &packet : stream.drain(100ms)) {
        if (packet->source_time_paced) ++paced;
      }
      EXPECT_GE(paced, 35u) << "fewer than 35 mapped source frames were actually emitted";
      EXPECT_EQ(stream.count_reaching(event_e::accepted_source, paced), paced);

      const auto stats = stream_stats::get_current();
      EXPECT_EQ(stats.video_policy_session_generation, generation);
      EXPECT_TRUE(stats.video_policy_source_time);
      EXPECT_TRUE(stats.video_policy_cadence_available);
      EXPECT_GE(stats.video_policy_sample_count, 1u);
      // The delivered rate misses the fixed target by far more than the legacy shortfall threshold, yet a
      // source-relative sample warns only on its own measured handoff jitter or involuntary drops.
      EXPECT_LT(stats.fps, ceiling_fps * 0.88);
      const bool measured_warning = stats.frame_jitter_ms >= 2.2 || stats.dropped_frame_ratio >= 0.04;
      EXPECT_EQ(stats.pacing_warning_streak > 0, measured_warning)
        << "fps=" << stats.fps << " jitter=" << stats.frame_jitter_ms << " dropped=" << stats.dropped_frame_ratio;
      EXPECT_EQ(stream.vrr()["effective"], true);
    }
    // The owned thread joined and its generation was removed: end the test stream so nothing leaks.
    stream_stats::update_stream_active(false);
    const auto after = stream_stats::get_current();
    EXPECT_EQ(after.video_policy_session_generation, 0u);
    EXPECT_FALSE(after.video_policy_source_time);
    EXPECT_EQ(after.video_policy_sample_count, 0u);
    for (const auto &client : after.clients) EXPECT_NE(client.session_generation, generation);
    EXPECT_TRUE(stream_stats::vrr_json(after, generation).is_null());
    EXPECT_EQ(config::video.vrr_source_pacing, host_flag) << "the case changed the host opt-in";
  });
}

TEST(SourcePacingProductionTests, AnOverBracketOrDriftedLiveClockReadIsRefusedPerFrameAndNeverPaced) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    paced_stream_t stream {compositor, stream_config(120, opted_in(), 9118)};
    // A null injection is refused before any epoch opens; the default constructor stays the real path.
    EXPECT_THROW((void) std::make_unique<video::capture_epoch_stepper_for_tests_t>(*stream.display(), k_capture_generation, nullptr),
                 std::invalid_argument);
    const auto clocks = stable_clock();
    video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, clocks};
    const auto opened = epoch.opened();
    ASSERT_TRUE(opened.proof);
    ASSERT_TRUE(opened.proof->clock_domain_verified) << opened.proof->mapping.reason;
    EXPECT_EQ(opened.proof->mapping.max_bracket_ns, static_cast<std::uint64_t>(k_stable_bracket_ns));
    EXPECT_EQ(vrr_clock::check_epoch_authority(opened.epoch_id, k_capture_generation, opened.proof.get()),
              vrr_clock::authority_e::current);
    (void) stream.drain(150ms);
    std::uint64_t accepted = 0;
    std::uint64_t refused = 0;
    // An in-bound live read: mapped through the production stamp, emitted at exactly its source time.
    const auto expect_paced = [&](const char *label) {
      std::this_thread::sleep_for(12ms);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::mapped) << receipt_diagnostic(label, *frame);
      stream.raise(frame);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet) << label;
      ASSERT_TRUE(packet->frame_timestamp) << label;
      EXPECT_TRUE(packet->source_time_paced) << label;
      EXPECT_EQ(steady_ns(*packet->frame_timestamp), mapped_ns(*frame)) << label;
      ++accepted;
      EXPECT_EQ(stream.count_reaching(event_e::accepted_source, accepted), accepted) << label;
    };
    // An out-of-bound live read: refused at stamping, never paced, and named by the real loop.
    const auto expect_refused = [&](const char *label) {
      std::this_thread::sleep_for(12ms);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::mapping_drifted) << receipt_diagnostic(label, *frame);
      EXPECT_EQ(mapped_ns(*frame), -1) << label;
      stream.raise(frame);
      for (const auto &packet : stream.drain(150ms)) {
        EXPECT_FALSE(packet->source_time_paced) << label;
      }
      ++refused;
      EXPECT_EQ(stream.count_reaching(event_e::receipt_not_mapped, refused), refused) << label;
    };
    ASSERT_NO_FATAL_FAILURE(expect_paced("stable"));
    // Causal: each refusal below is the injected live sample's own, at one unit past the production bound.
    clocks->arm(20'001, 0);
    ASSERT_NO_FATAL_FAILURE(expect_refused("live bracket 20001 ns"));
    clocks->arm(20'000, 0);
    ASSERT_NO_FATAL_FAILURE(expect_paced("live bracket exactly 20000 ns"));
    clocks->arm(k_stable_bracket_ns, 50'001);
    ASSERT_NO_FATAL_FAILURE(expect_refused("live drift 50001 ns"));
    clocks->arm(k_stable_bracket_ns, 50'000);
    ASSERT_NO_FATAL_FAILURE(expect_paced("live drift exactly 50000 ns"));
    clocks->disarm();
    ASSERT_NO_FATAL_FAILURE(expect_paced("disarmed"));
    // A per-frame refusal neither latched nor unsupported the session.
    EXPECT_EQ(stream.count(event_e::authority_lost), 0u);
    EXPECT_EQ(stream.count(event_e::unsupported_route), 0u);
    EXPECT_EQ(stream.vrr()["effective"], true);
  });
}

TEST(SourcePacingProductionTests, AnEpochWhoseOwnClockProofIsOutOfBoundsNeverMapsOrPaces) {
  pinned_compositor_t compositor;
  REQUIRE_PRODUCTION_FIXTURE(compositor);
  with_h264([&] {
    {
      paced_stream_t stream {compositor, stream_config(120, opted_in(), 9119)};
      const auto clocks = stable_clock();
      clocks->arm_proof(50'001);
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, clocks};
      EXPECT_EQ(clocks->proof_samples_pending(), 0) << "collect() did not read exactly the armed proof samples";
      const auto opened = epoch.opened();
      ASSERT_TRUE(opened.proof);
      // The production bound refused the proof while the real identity, namespace and live lease held.
      EXPECT_FALSE(opened.proof->mapping.valid);
      EXPECT_EQ(opened.proof->mapping.reason, "uncertainty_out_of_bounds");
      EXPECT_EQ(opened.proof->mapping.max_bracket_ns, 50'001u);
      EXPECT_EQ(opened.proof->mapping.uncertainty_ns, 50'001u);
      EXPECT_TRUE(opened.proof->producer_identity_verified) << opened.proof->producer_identity_reason;
      EXPECT_EQ(opened.proof->time_namespace, vrr_clock::namespace_e::same) << opened.proof->namespace_reason;
      EXPECT_FALSE(opened.proof->clock_domain_verified);
      EXPECT_FALSE(vrr_clock::authorises_clock_comparison(*opened.proof));
      EXPECT_EQ(vrr_clock::check_epoch_authority(opened.epoch_id, k_capture_generation, opened.proof.get()),
                vrr_clock::authority_e::no_epoch)
        << "an unproven epoch published authority";
      (void) stream.drain(150ms);
      auto frame = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_EQ(frame->provenance.source_time.result, vrr_clock::source_time_e::mapping_invalid)
        << receipt_diagnostic("proof bracket 50001 ns", *frame);
      stream.raise(frame);
      for (const auto &packet : stream.drain(300ms)) {
        EXPECT_FALSE(packet->source_time_paced);
      }
      EXPECT_EQ(stream.count_reaching(event_e::unsupported_route, 1u), 1u);
      EXPECT_EQ(stream.count(event_e::accepted_source), 0u);
    }
    {
      paced_stream_t stream {compositor, stream_config(120, opted_in(), 9120)};
      const auto clocks = stable_clock();
      clocks->arm_proof(50'000);
      video::capture_epoch_stepper_for_tests_t epoch {*stream.display(), k_capture_generation, clocks};
      EXPECT_EQ(clocks->proof_samples_pending(), 0) << "collect() did not read exactly the armed proof samples";
      const auto opened = epoch.opened();
      ASSERT_TRUE(opened.proof);
      ASSERT_TRUE(opened.proof->clock_domain_verified) << opened.proof->mapping.reason;
      EXPECT_EQ(opened.proof->mapping.uncertainty_ns, 50'000u);
      EXPECT_EQ(vrr_clock::check_epoch_authority(opened.epoch_id, k_capture_generation, opened.proof.get()),
                vrr_clock::authority_e::current);
      (void) stream.drain(150ms);
      auto first = stamped_frame(epoch, monotonic_now_ns());
      ASSERT_EQ(first->provenance.source_time.result, vrr_clock::source_time_e::mapped)
        << receipt_diagnostic("first under a 50000 ns proof", *first);
      stream.raise(first);
      auto packet = stream.next_packet(400ms);
      ASSERT_TRUE(packet);
      ASSERT_TRUE(packet->frame_timestamp);
      EXPECT_TRUE(packet->source_time_paced);
      EXPECT_EQ(steady_ns(*packet->frame_timestamp), mapped_ns(*first));
      EXPECT_EQ(stream.count_reaching(event_e::accepted_source, 1u), 1u);
    }
  });
}

#else
TEST(SourcePacingProductionTests, LinuxOnly) {
  GTEST_SKIP() << "the source-driven ext-image-copy route and its compositor fixture are Linux-only";
}
#endif
