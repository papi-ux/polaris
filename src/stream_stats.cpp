/**
 * @file src/stream_stats.cpp
 * @brief Thread-safe stream statistics collector for real-time monitoring.
 *
 * Supports tracking multiple simultaneous streaming clients.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <random>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "adaptive_bitrate.h"
#include "config.h"
#include "configuration_store.h"
#include "crypto.h"
#include "logging.h"
#include "network.h"
#include "stream_start_outcome.h"
#include "stream_stats.h"
#include "utility.h"
#include "verified_action.h"

namespace video {
  std::string active_encoder_name();
  bool active_encoder_withholds_hdr();
  bool active_encoder_withholds_hdr_for_explicit_vulkan();
}
#ifdef __linux__
  #include "platform/linux/kms_capture_readiness.h"
  #include "platform/linux/misc.h"
  #include "platform/linux/stream_runtime.h"
  #include "platform/linux/user_unit_override.h"
  #include "platform/linux/wayland.h"
  #include "platform/linux/stream_display_policy.h"
  #include "platform/linux/virtual_display.h"
#endif

namespace stream_stats {

  bool is_meaningful_fps_shortfall(double target_fps, double delivered_fps) {
    if (!std::isfinite(target_fps) || !std::isfinite(delivered_fps) ||
        target_fps < 24.0 || delivered_fps <= 0.0 || delivered_fps >= target_fps) {
      return false;
    }

    const double fps_gap = target_fps - delivered_fps;
    const double fps_ratio = delivered_fps / target_fps;
    return fps_gap >= 2.0 && fps_ratio < 0.95;
  }

  double encoder_pressure_frame_budget_ms(double target_fps) {
    return std::isfinite(target_fps) && target_fps > 0.0 ?
             1000.0 / target_fps :
             std::numeric_limits<double>::infinity();
  }

  namespace {
    double encoder_pressure_fail_threshold_ms(double target_fps) {
      return std::min(12.0, encoder_pressure_frame_budget_ms(target_fps));
    }

    double encoder_pressure_watch_threshold_ms(double target_fps) {
      return std::min(8.0, encoder_pressure_frame_budget_ms(target_fps) * 0.90);
    }
  }

  bool encoder_time_fails_budget(double encode_time_ms, double target_fps) {
    return std::isfinite(encode_time_ms) && encode_time_ms > 0.0 &&
           encode_time_ms >= encoder_pressure_fail_threshold_ms(target_fps);
  }

  bool encoder_time_nears_budget(double encode_time_ms, double target_fps) {
    return std::isfinite(encode_time_ms) && encode_time_ms > 0.0 &&
           encode_time_ms >= encoder_pressure_watch_threshold_ms(target_fps);
  }

  static std::mutex stats_mutex;
  static stats_t current_stats;
  // The most recently ended session, guarded by stats_mutex. It lives outside current_stats
  // because update_stream_active(false) resets that wholesale when the last stream ends, which is
  // when a person comes to read it. remove_client() is its only writer.
  static std::optional<ended_session_t> last_ended_session;

  // measurement-spec-v1.md 6.1: increments on every add_client()/
  // remove_client() call. Deliberately its own atomic, outside stats_t -
  // update_stream_active(false) resets current_stats wholesale when all
  // sessions end, and this counter must survive that (a full stream
  // restart between a benchmark run's arm and freeze must never be
  // masked by the revision coincidentally returning to its starting
  // value). Starts at 1 so 0 stays available as an obviously-invalid/
  // unset sentinel.
  static std::atomic<std::uint64_t> client_population_revision_counter {1};

  namespace {
    // Hot-path telemetry: written and read without stats_mutex so the
    // encode-loop and network-report call sites - and their get_current()
    // readers - can never be stalled by a slow or cold-path stats_mutex
    // holder (HDR renegotiation, GPU-native probing, client add/remove).
    // Each field is independent last-value telemetry, not a transaction
    // across fields, so relaxed ordering is sufficient - this matches the
    // snapshot semantics get_current() already had under the old single
    // mutex, which never guaranteed cross-field consistency either.
    std::atomic<double> hot_fps {0.0};
    std::atomic<int> hot_bitrate_kbps {0};
    std::atomic<double> hot_encode_time_ms {0.0};
    std::atomic<int> hot_codec_id {-1};  // -1=unset, 0=h264, 1=hevc, 2=av1, 3=pyrowave
    std::atomic<int> hot_width {0};
    std::atomic<int> hot_height {0};
    std::atomic<double> hot_duplicate_frame_ratio {0.0};
    std::atomic<double> hot_dropped_frame_ratio {0.0};
    std::atomic<double> hot_avg_frame_age_ms {0.0};
    std::atomic<double> hot_frame_jitter_ms {0.0};
    std::atomic<double> hot_capture_source_fps {0.0};
    std::atomic<double> hot_latency_ms {0.0};
    std::atomic<double> hot_packet_loss {0.0};
    std::atomic<bool> hot_packet_loss_available {false};
    std::atomic<double> hot_control_channel_packet_loss {0.0};
    std::atomic<uint64_t> hot_control_channel_samples {0};
    // Process-lifetime monotonic revision used by read-only pacing Recheck.
    std::atomic<uint64_t> hot_video_sample_revision {0};
    // Process-lifetime monotonic revision. Doctor uses this to prove that a
    // verification decision includes measurements received after its change.
    std::atomic<uint64_t> hot_network_sample_revision {0};
    std::atomic<bool> hot_network_risk {false};
    std::atomic<uint64_t> hot_bytes_sent {0};
    std::atomic<bool> hot_doctor_live_action_scope_available {false};

    struct doctor_video_policy_state_t {
      double target_fps = 0.0;
      double delivered_fps = 0.0;
      double capture_source_fps = 0.0;
      double duplicate_frame_ratio = 0.0;
      double dropped_frame_ratio = 0.0;
      double avg_frame_age_ms = 0.0;
      double frame_jitter_ms = 0.0;
      double encode_time_ms = 0.0;
      platf::frame_transport_e capture_transport =
        platf::frame_transport_e::unknown;
      platf::frame_residency_e capture_residency =
        platf::frame_residency_e::unknown;
      platf::frame_residency_e encode_target_residency =
        platf::frame_residency_e::unknown;
      std::uint64_t sample_count = 0;
      std::uint64_t pacing_warning_streak = 0;
    };
    doctor_video_policy_state_t doctor_video_policy_state;
    std::mutex doctor_video_policy_mutex;

    bool video_policy_has_pacing_warning(
        const doctor_video_policy_state_t &sample) {
      const bool source_cadence_available =
        sample.capture_source_fps > 0.0 && sample.target_fps > 0.0;
      const bool source_cadence_confirms_motion =
        source_cadence_available &&
        sample.capture_source_fps >= sample.target_fps * 0.85;
      const bool static_or_duplicate_content =
        sample.duplicate_frame_ratio >= 0.50 ||
        (source_cadence_available &&
         sample.capture_source_fps < sample.target_fps * 0.50 &&
         sample.duplicate_frame_ratio >= 0.10);
      return
        sample.dropped_frame_ratio >= 0.04 ||
        (!static_or_duplicate_content &&
         (sample.frame_jitter_ms >= 2.2 ||
          (is_meaningful_fps_shortfall(
             sample.target_fps,
             sample.delivered_fps
           ) && source_cadence_confirms_motion)));
    }

    bool video_policy_pacing_warning_is_confirmed(
        const doctor_video_policy_state_t &sample) {
      return sample.sample_count >= DOCTOR_PACING_WARMUP_SAMPLES &&
        sample.pacing_warning_streak >= DOCTOR_PACING_CONFIRMATION_SAMPLES &&
        video_policy_has_pacing_warning(sample);
    }

    bool video_policy_suppresses_quality_restore(
        const doctor_video_policy_state_t &sample) {
      const bool encoder_watch =
        encoder_time_nears_budget(sample.encode_time_ms, sample.target_fps) ||
        sample.avg_frame_age_ms >= 18.0;
      // SHM is a supported compatibility path. It is capability context, not
      // pressure by itself; only measured encode/capture latency or confirmed
      // pacing trouble should block a guarded quality restore.
      return encoder_watch || video_policy_pacing_warning_is_confirmed(sample);
    }

    void publish_doctor_video_policy_locked() {
      const bool capture_cpu_copy =
        doctor_video_policy_state.capture_transport == platf::frame_transport_e::shm ||
        doctor_video_policy_state.capture_residency == platf::frame_residency_e::cpu ||
        doctor_video_policy_state.encode_target_residency == platf::frame_residency_e::cpu;
      adaptive_bitrate::note_doctor_video_policy_evidence(
        video_policy_suppresses_quality_restore(doctor_video_policy_state),
        capture_cpu_copy || video_policy_has_pacing_warning(doctor_video_policy_state)
      );
    }

    // Guarded by its own lock so the lock-free update_network_stats
    // overload stays clear of stats_mutex.
    network_risk_tracker_t network_risk_tracker;
    network_judge_t network_judge;
    std::mutex network_risk_mutex;

    // The stream generation whose first counted client media report, and whose first report that
    // restarted its baseline after a gap, were logged, so each is said once a stream.
    std::atomic<std::uint64_t> logged_media_report_generation {0};
    std::atomic<std::uint64_t> logged_media_gap_generation {0};

    constexpr auto CLIENT_MEDIA_COUNTER_MAX_GAP = std::chrono::seconds(5);
    struct client_media_counter_epoch_t {
      client_media_counters_t sample;
      std::chrono::steady_clock::time_point received_at {};
    };
    // Serializes one authenticated counter ingest (including evidence
    // publication) with the stream-generation reset. It must always be taken
    // before stats_mutex; the narrower counter mutex is never held while
    // acquiring stats or network-risk state.
    std::mutex client_media_ingest_mutex;
    std::mutex client_media_counter_mutex;
    std::optional<client_media_counter_epoch_t> last_client_media_counter_epoch;

    struct primary_network_observation_t {
      std::uint64_t revision = 0;
      std::chrono::steady_clock::time_point received_at {};
      std::uint64_t media_loss_revision = 0;
      std::chrono::steady_clock::time_point media_loss_received_at {};
      bool media_sample = false;
      double latency_ms = 0.0;
      double packet_loss = 0.0;
      bool packet_loss_available = false;
      double control_channel_packet_loss = 0.0;
      std::uint64_t control_channel_samples = 0;
      bool network_risk = false;
      std::uint64_t bytes_sent = 0;
    };

    constexpr std::size_t NETWORK_OBSERVATION_CAPACITY = 256;
    constexpr std::int64_t DOCTOR_CURRENT_NETWORK_MAX_AGE_MS = 2000;
    std::deque<primary_network_observation_t> primary_network_observations;
    primary_network_observation_t primary_network_state;

    // Recovery counters: monotonic for the life of the process, matching
    // the roadmap's P0-2 ask; a per-session view is a get_current() delta.
    std::atomic<uint64_t> hot_idr_requests_total {0};
    std::atomic<uint64_t> hot_invalidate_ref_frames_requests_total {0};

    // Store the codec (h264/hevc/av1/pyrowave) as
    // the same small int id callers already derive it from (see
    // stream_recorder.cpp's identical convention) keeps the hot write path
    // lock-free without inventing a new scheme.
    int codec_to_id(const std::string &codec) {
      if (codec == "pyrowave") return 3;
      if (codec == "av1") return 2;
      if (codec == "hevc") return 1;
      if (codec == "h264") return 0;
      return -1;
    }

    std::string codec_from_id(int id) {
      switch (id) {
        case 0: return "h264";
        case 1: return "hevc";
        case 2: return "av1";
        case 3: return "pyrowave";
        default: return {};
      }
    }

    std::string doctor_codec_family(std::string codec) {
      std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
      });
      if (codec.find("av1") != std::string::npos) return "av1";
      if (codec.find("hevc") != std::string::npos || codec.find("h265") != std::string::npos ||
          codec.find("h.265") != std::string::npos) return "hevc";
      if (codec.find("h264") != std::string::npos || codec.find("h.264") != std::string::npos ||
          codec.find("avc") != std::string::npos) return "h264";
      return {};
    }

    std::string recovery_evidence_result_id(std::string base,
                                            const stats_t &stats,
                                            const nlohmann::json &health,
                                            std::string_view app_uuid,
                                            double target_fps,
                                            int live_bitrate_kbps) {
      if (!health.value("relaunch_recommended", false)) return base;

      auto codec = health.value("safe_codec", doctor_codec_family(stats.codec));
      if (codec.empty()) codec = "h264";
      const nlohmann::json evidence = {
        {"app_uuid", app_uuid},
        {"stream_display_mode", health.value("safe_display_mode", std::string {})},
        {"width", stats.width},
        {"height", stats.height},
        {"target_fps", health.value("safe_target_fps", static_cast<int>(std::round(target_fps)))},
        {"target_bitrate_kbps", health.value("safe_bitrate_kbps", live_bitrate_kbps)},
        {"preferred_codec", codec},
        {"hdr", health.value("safe_hdr", false)}
      };
      const auto fingerprint = util::hex(crypto::hash(evidence.dump())).to_string();
      return base + "-" + fingerprint.substr(0, 32);
    }

    void reset_hot_fields() {
      hot_bitrate_kbps.store(0, std::memory_order_relaxed);
      hot_codec_id.store(-1, std::memory_order_relaxed);
      hot_width.store(0, std::memory_order_relaxed);
      hot_height.store(0, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
        hot_fps.store(0.0, std::memory_order_relaxed);
        hot_encode_time_ms.store(0.0, std::memory_order_relaxed);
        hot_duplicate_frame_ratio.store(0.0, std::memory_order_relaxed);
        hot_dropped_frame_ratio.store(0.0, std::memory_order_relaxed);
        hot_avg_frame_age_ms.store(0.0, std::memory_order_relaxed);
        hot_frame_jitter_ms.store(0.0, std::memory_order_relaxed);
        hot_capture_source_fps.store(0.0, std::memory_order_relaxed);
        doctor_video_policy_state = {};
      }
      hot_latency_ms.store(0.0, std::memory_order_relaxed);
      hot_packet_loss.store(0.0, std::memory_order_relaxed);
      hot_packet_loss_available.store(false, std::memory_order_relaxed);
      hot_control_channel_packet_loss.store(0.0, std::memory_order_relaxed);
      hot_control_channel_samples.store(0, std::memory_order_relaxed);
      // Video/network sample revisions intentionally survive stream resets so an
      // old receipt cannot mistake a new generation's first samples for its
      // own baseline. Authenticated session generation still fences actions.
      {
        std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
        network_risk_tracker.reset();
        network_judge.reset();
        primary_network_observations.clear();
        primary_network_state = primary_network_observation_t {};
      }
      {
        std::lock_guard<std::mutex> media_lock(client_media_counter_mutex);
        last_client_media_counter_epoch.reset();
      }
      hot_network_risk.store(false, std::memory_order_relaxed);
      hot_bytes_sent.store(0, std::memory_order_relaxed);
      hot_doctor_live_action_scope_available.store(false, std::memory_order_relaxed);
      // idr_requests_total / invalidate_ref_frames_requests_total are
      // intentionally NOT reset here: they are process-lifetime recovery
      // counters (see the header doc comment), not per-session state.
    }
  }  // namespace

  std::optional<bool> device_nodes_match(const std::string &lhs, const std::string &rhs) {
    if (lhs.empty() || rhs.empty()) {
      return std::nullopt;
    }
    std::error_code ec;
    const bool equivalent = std::filesystem::equivalent(lhs, rhs, ec);
    if (ec) {
      return std::nullopt;
    }
    return equivalent;
  }

  namespace {
    constexpr std::size_t CAPTURE_PROFILE_SUMMARY_FRAMES = 600;
    constexpr std::size_t CAPTURE_PROFILE_BUCKET_COUNT =
      static_cast<std::size_t>(platf::frame_transport_e::unknown) + 1;

    struct capture_profile_bucket_t {
      std::vector<long long> dispatch_us;
      std::vector<long long> ingest_us;
      std::vector<long long> total_us;
      std::vector<long long> source_interval_us;
      std::vector<long long> ready_to_handoff_us;
    };

    std::array<capture_profile_bucket_t, CAPTURE_PROFILE_BUCKET_COUNT> capture_profile_buckets;

    capture_profile_bucket_t &capture_profile_bucket(platf::frame_transport_e transport) {
      auto index = static_cast<std::size_t>(transport);
      if (index >= capture_profile_buckets.size()) {
        index = static_cast<std::size_t>(platf::frame_transport_e::unknown);
      }
      return capture_profile_buckets[index];
    }

    void clear_capture_profile_buckets() {
      for (auto &bucket : capture_profile_buckets) {
        bucket.dispatch_us.clear();
        bucket.ingest_us.clear();
        bucket.total_us.clear();
        bucket.source_interval_us.clear();
        bucket.ready_to_handoff_us.clear();
      }
    }

#ifdef __linux__
    stream_display_policy::resolved_t current_stream_policy() {
      // One resolve snapshot: override flag comes from live labwc state only.
      return stream_display_policy::resolve_current(
        false,
        stream_runtime::labwc::runtime_state().gpu_native_override_active
      );
    }
#endif

    long long percentile_value(std::vector<long long> values, double percentile) {
      if (values.empty()) {
        return 0;
      }

      std::sort(values.begin(), values.end());
      auto pos = static_cast<std::size_t>(percentile * static_cast<double>(values.size() - 1));
      return values[pos];
    }

    // P0-3/P0-3A T0-T2 stage timing: unlike capture_profile_buckets above
    // (which fills, logs, and clears - fine for periodic logging), this
    // backs an HTTP-queryable endpoint, so a true ring buffer is used
    // instead of a tumbling window - there is no "just cleared, empty"
    // moment a query can land on. Sized to comfortably hold a full 120 s
    // bench run at up to 120 fps (14400 samples) with headroom, so a
    // P0-5-style run doesn't wrap mid-collection. One dedicated mutex,
    // separate from stats_mutex: writes happen at up to ~120 Hz per active
    // session, reads are occasional HTTP handlers, and coupling this to the
    // already-de-contended stats_mutex would reintroduce exactly the kind
    // of cross-concern contention P0-2 removed.
    constexpr std::size_t FRAME_TIMING_RING_CAPACITY = 16384;

    struct timing_ring_t {
      std::array<double, FRAME_TIMING_RING_CAPACITY> samples_ms {};
      std::size_t next_index = 0;
      std::size_t filled = 0;

      // Rejected (negative/non-monotonic) samples for this stage - see
      // record_frame_timing()'s validation. Counted here rather than in
      // session_timing_state_t so each of the three stages tracks its own
      // rejection count independently.
      int invalid_count = 0;

      void push(double value_ms) {
        samples_ms[next_index] = value_ms;
        next_index = (next_index + 1) % FRAME_TIMING_RING_CAPACITY;
        filled = std::min(filled + 1, FRAME_TIMING_RING_CAPACITY);
      }

      frame_timing_percentiles_t percentiles() const {
        if (filled == 0) {
          return {0, 0, 0, invalid_count};
        }
        std::vector<double> sorted(samples_ms.begin(), samples_ms.begin() + static_cast<long>(filled));
        std::sort(sorted.begin(), sorted.end());
        auto pick = [&](double p) {
          auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
          return sorted[idx];
        };
        return {pick(0.50), pick(0.99), static_cast<int>(filled), invalid_count};
      }
    };

    // P0-3A: per-session T0-T2 state, found by linear scan the same way
    // client_stats_t's own `clients` vector (above) is - config::stream.max_sessions
    // is clamped to [0,8] and defaults to 2, so a scan is simpler than a
    // hashed lookup without being any slower at this size.
    struct session_timing_state_t {
      std::string device_uuid;
      std::string session_token;

      // Matches the owning session_t::session_generation. A reconnecting
      // device reuses device_uuid but gets a new generation, which is what
      // lets record_frame_timing()/stop_session_timing() tell a stale
      // write/stop (from a session that already lost its uuid slot) apart
      // from a legitimate one.
      std::uint64_t session_generation = 0;

      timing_ring_t capture_to_encode_ring;
      timing_ring_t encode_to_send_ring;
      timing_ring_t capture_to_send_ring;
    };

    std::mutex frame_timing_mutex;
    std::vector<session_timing_state_t> session_timings;

    // Caller must hold frame_timing_mutex.
    session_timing_state_t *find_session_timing_locked(const std::string &device_uuid) {
      auto it = std::find_if(session_timings.begin(), session_timings.end(),
        [&device_uuid](const session_timing_state_t &s) { return s.device_uuid == device_uuid; });
      return it == session_timings.end() ? nullptr : &*it;
    }

    nlohmann::json capture_source_json(const capture_source_t &source) {
      if (source.width <= 0 || source.height <= 0 || source.stream_width <= 0 || source.stream_height <= 0) {
        return nullptr;
      }
      return {
        {"width", source.width}, {"height", source.height},
        {"stream_width", source.stream_width}, {"stream_height", source.stream_height},
        {"transport", platf::from_frame_transport(source.transport)},
        {"residency", platf::from_frame_residency(source.residency)},
        {"pixel_ratio", (static_cast<double>(source.width) * source.height) /
                         (static_cast<double>(source.stream_width) * source.stream_height)}
      };
    }

    /// Drawn once per process, so a stream_instance_id never repeats across a restart, where
    /// session generations begin again at one.
    const std::string &process_instance_nonce() {
      static const std::string nonce = [] {
        std::random_device device;
        const auto value = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
        char text[17] {};
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return std::string {text};
      }();
      return nonce;
    }

    /// The frames a client accepted from the display its capture names. Only frames that display
    /// delivered count, so until it delivers one, transport, residency and format read unknown,
    /// and a display opened again starts over.
    capture_source_t frames_of_published_display(const client_stats_t &client) {
      return client.capture_frame_since_publication ? client.capture_source : capture_source_t {};
    }

    /// A session's capture: what it asked for and opened, and what its accepted frames carried.
    nlohmann::json capture_backend_json(const capture_backend_t &backend, const capture_source_t &frames) {
      nlohmann::json capture {
        {"preference", backend.preference},
        {"requested", backend.requested},
        {"opened", backend.opened},
        {"route", backend.route},
        {"transport", platf::from_frame_transport(frames.transport)},
        {"residency", platf::from_frame_residency(frames.residency)},
        {"format", platf::from_frame_format(frames.format)},
      };
      if (!backend.mode_override_reason.empty()) {
        capture["mode_override_reason"] = backend.mode_override_reason;
      }
      if (!backend.route_fallback_reason.empty()) {
        capture["route_fallback_reason"] = backend.route_fallback_reason;
      }
      return capture;
    }

    /// A lifecycle time in UTC ISO 8601, to the second. Empty for a time that was never set.
    std::string iso8601_utc(std::chrono::system_clock::time_point at) {
      if (at.time_since_epoch().count() == 0) {
        return {};
      }
      const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
      std::tm utc {};
#ifdef _WIN32
      gmtime_s(&utc, &seconds);
#else
      gmtime_r(&seconds, &utc);
#endif
      char buffer[32];
      if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
      }
      return buffer;
    }

    /// The last session as the stats report it. Every value was frozen from the session's own
    /// generation, and one it never reported is absent rather than given a default.
    nlohmann::json ended_session_json(const ended_session_t &ended) {
      nlohmann::json session {
        {"state", "ended"},
        {"client_name", ended.client_name},
      };
      if (const auto id = stream_instance_id(ended.session_generation); !id.empty()) {
        session["stream_instance_id"] = id;
      }
      if (const auto at = iso8601_utc(ended.started_at); !at.empty()) {
        session["started_at"] = at;
      }
      if (const auto at = iso8601_utc(ended.ended_at); !at.empty()) {
        session["ended_at"] = at;
      }
      if (!ended.capture_backend.opened.empty()) {
        session["capture"] = capture_backend_json(ended.capture_backend, ended.capture_frames);
      }
      if (!ended.codec.empty()) {
        session["codec"] = ended.codec;
      }
      if (!ended.encoder_backend.empty()) {
        session["encoder_backend"] = ended.encoder_backend;
      }
      if (!ended.pyrowave_route.empty()) {
        session["pyrowave_route"] = ended.pyrowave_route;
      }
      if (const auto &stop = ended.app_stop; !stop.path.empty()) {
        nlohmann::json app_stop {
          {"path", stop.path},
          {"windows_asked", stop.windows_asked},
          {"waited_ms", stop.waited.count()},
        };
        if (!stop.target.empty()) {
          app_stop["target"] = stop.target;
        }
        if (stop.launcher) {
          app_stop["launcher"] = {
            {"app_id", stop.launcher->app_id},
            {"path", stop.launcher->path},
            {"waited_ms", stop.launcher->waited.count()},
          };
        }
        if (stop.flatpak_instances) {
          app_stop["flatpak_instances"] = {
            {"launcher", stop.flatpak_instances->launcher},
            {"game", stop.flatpak_instances->game},
            {"helper", stop.flatpak_instances->helper},
            {"left_alone", stop.flatpak_instances->left_alone},
          };
        }
        if (!stop.capture.empty()) {
          app_stop["capture"] = stop.capture;
          app_stop["unattributed"] = stop.unattributed;
        }
        session["app_stop"] = std::move(app_stop);
      }
      if (!ended.start_outcome.empty()) {
        session["start"] = {{"outcome", ended.start_outcome}};
        if (ended.start_client_left_after_ms >= 0) {
          session["start"]["client_left_after_ms"] = ended.start_client_left_after_ms;
        }
      }
      return session;
    }

    nlohmann::json fec_protection_json(const fec_protection_stats_t &stats) {
      return {
        {"oversized_frames_total", stats.oversized_frames_total},
        {"oversized_idr_frames_total", stats.oversized_idr_frames_total},
        {"oversized_reference_invalidation_frames_total", stats.oversized_reference_invalidation_frames_total},
        {"largest_encoded_frame_bytes", stats.largest_encoded_frame_bytes},
        {"largest_packetized_frame_bytes", stats.largest_packetized_frame_bytes},
        {"protected_payload_limit_bytes", stats.protected_payload_limit_bytes},
        {"max_required_blocks", stats.max_required_blocks},
        {"protocol_max_blocks", stats.protocol_max_blocks},
        {"fec_percentage", stats.fec_percentage},
        {"packet_size", stats.packet_size},
        {"largest_frame_type", stats.largest_frame_type}
      };
    }

    void merge_oversized_fec_frame(fec_protection_stats_t &stats,
                                    std::size_t encoded_frame_bytes,
                                    std::size_t packetized_frame_bytes,
                                    std::size_t protected_payload_limit_bytes,
                                    std::size_t required_blocks,
                                    int fec_percentage,
                                    int packet_size,
                                    std::string_view frame_type) {
      ++stats.oversized_frames_total;
      if (frame_type == "idr") {
        ++stats.oversized_idr_frames_total;
      } else if (frame_type == "reference_invalidation") {
        ++stats.oversized_reference_invalidation_frames_total;
      }

      const bool new_largest =
        required_blocks > stats.max_required_blocks ||
        (required_blocks == stats.max_required_blocks &&
         packetized_frame_bytes > stats.largest_packetized_frame_bytes);
      if (new_largest) {
        stats.largest_encoded_frame_bytes = encoded_frame_bytes;
        stats.largest_packetized_frame_bytes = packetized_frame_bytes;
        stats.max_required_blocks = required_blocks;
        stats.largest_frame_type = frame_type;
      }

      stats.protected_payload_limit_bytes = protected_payload_limit_bytes;
      stats.fec_percentage = fec_percentage;
      stats.packet_size = packet_size;
    }

    /**
     * The kind of client the top-level client_name belongs to, from the session entry with that name
     * and address. The top-level name is the session that started last, not always the first entry,
     * so the kind is looked up rather than copied. Empty when no entry matches or none was given one.
     */
    std::string streaming_client_family(const stats_t &stats) {
      for (const auto &client : stats.clients) {
        if (client.name == stats.client_name && client.ip == stats.client_ip) {
          return client.client_family;
        }
      }
      return {};
    }
  }  // namespace

  std::string stats_t::to_json() const {
    return to_json(nlohmann::json::object());
  }

  std::string stats_t::to_json(const nlohmann::json &doctor_health) const {
    nlohmann::json j;

    j["streaming"] = streaming;
    j["client_name"] = client_name;
    j["client_ip"] = client_ip;
    // "nova" or "moonlight" for the stream client_name names; empty when nothing says which.
    j["client_family"] = streaming_client_family(*this);
    j["client_network_path"] = client_network_path;
    // Named apart from the session status display_mode object, which describes the virtual display.
    j["display_mode_decision"] = {
      {"requested", display_mode_requested},
      {"applied", display_mode_applied},
      {"pinned_by_host", display_mode_pinned_by_host},
    };
#ifdef __linux__
    {
      const auto policy = current_stream_policy();
      const auto backend = !runtime_backend.empty() ? runtime_backend : policy.backend_name;
      j["runtime_backend"] = backend.empty() ? "none" : backend;
      // path id is SoT; UI falls back stream_display_mode_id → stream_path_id
      j["stream_path_id"] = policy.selection;
      j["stream_display_mode"] = policy.label.empty() ? "Mirror Desktop" : policy.label;
    }
#else
    j["runtime_backend"] = runtime_backend.empty() ? "none" : runtime_backend;
    j["stream_display_mode"] = "Mirror Desktop";
#endif
    j["runtime_requested_headless"] = runtime_requested_headless;
    j["runtime_effective_headless"] = runtime_effective_headless;
    j["runtime_gpu_native_override_active"] = runtime_gpu_native_override_active;
    j["runtime_reported_refresh_hz"] = runtime_reported_refresh_hz;
    j["runtime_display_warning"] = runtime_display_warning;
    j["capture_transport"] = platf::from_frame_transport(capture_transport);
    j["capture_residency"] = platf::from_frame_residency(capture_residency);
    j["capture_format"] = platf::from_frame_format(capture_format);
    j["capture_device"] = capture_device;
    j["wayland_main_device"] = wayland_main_device;
    j["gpu_native_probe"] = gpu_native_probe_json(*this);
    const auto capture_path = capture_path_summary(*this);
    const auto capture_reason = capture_path_reason(*this);
    const auto capture_reason_message = capture_path_reason_message(capture_reason);
    const bool capture_cpu_copy = capture_path_uses_cpu_copy(*this);
    const bool capture_gpu_native = capture_path_is_gpu_native(*this);
    // Flat capture_* fields only (UI + diagnostics); nested capture_decision was a pure duplicate.
    j["capture_path"] = capture_path;
    j["capture_path_reason"] = capture_reason;
    j["capture_path_reason_message"] = capture_reason_message;
    j["capture_cpu_copy"] = capture_cpu_copy;
    j["capture_gpu_native"] = capture_gpu_native;
    j["capture_cross_gpu_dmabuf_risk"] = capture_path_has_cross_gpu_dmabuf_risk(*this);
    j["linux_gpu_profile"] = linux_gpu_profile_json(*this);
    j["encode_target_device"] = encode_target_device;
    j["encode_target_residency"] = platf::from_frame_residency(encode_target_residency);
    j["encode_target_format"] = platf::from_frame_format(encode_target_format);
    j["convert_path"] = encode_target_device.empty() ? "unknown" : encode_target_device;
    j["dynamic_range"] = dynamic_range;
    j["display_hdr"] = display_hdr;
    j["hdr_policy_reason"] = hdr_policy_reason;
    j["hdr_policy_hdr"] = hdr_policy_hdr;
    j["hdr_metadata_available"] = hdr_metadata_available;
    j["stream_hdr_enabled"] = stream_hdr_enabled;
    j["color_coding"] = color_coding;
    j["hdr_effective_mode"] = hdr_effective_mode(*this);
    j["hdr_downgrade_reason"] = hdr_downgrade_reason(*this);
    j["hdr_downgrade_message"] = hdr_downgrade_message(*this);
    j["fps"] = fps;
    j["requested_client_fps"] = requested_client_fps;
    j["session_target_fps"] = session_target_fps;
    j["encode_target_fps"] = encode_target_fps;
    j["bitrate_kbps"] = bitrate_kbps;
    j["encode_time_ms"] = encode_time_ms;
    j["duplicate_frame_ratio"] = duplicate_frame_ratio;
    j["dropped_frame_ratio"] = dropped_frame_ratio;
    j["avg_frame_age_ms"] = avg_frame_age_ms;
    // Compatibility: frame_jitter_ms predates the metric's precise name. The
    // value is mean absolute error from the requested frame interval, not the
    // variance of otherwise on-target delivery intervals.
    j["frame_interval_error_ms"] = frame_jitter_ms;
    j["frame_jitter_ms"] = frame_jitter_ms;
    j["capture_source_fps"] = capture_source_fps;
    j["capture_pacing"] = capture_pacing;
    j["codec"] = codec;
    j["pacing_policy"] = pacing_policy;
    j["optimization_source"] = optimization_source;
    j["optimization_confidence"] = optimization_confidence;
    j["optimization_cache_status"] = optimization_cache_status;
    j["optimization_reasoning"] = optimization_reasoning;
    j["optimization_normalization_reason"] = optimization_normalization_reason;
    j["recommendation_version"] = recommendation_version;
    j["paired_target_bitrate_kbps"] = paired_target_bitrate_kbps;
    j["effective_launch_bitrate_kbps"] = effective_launch_bitrate_kbps;
    j["stream_chroma"] = stream_chroma;
    if (auto pyrowave = pyrowave_bitrate_json(*this); !pyrowave.is_null()) {
      j["pyrowave_bitrate"] = std::move(pyrowave);
    }
    j["width"] = width;
    j["height"] = height;
    j["fec_protection"] = fec_protection_json(fec_protection);
    j["latency_ms"] = latency_ms;
    j["packet_loss"] = packet_loss;
    j["packet_loss_available"] = packet_loss_available;
    j["packet_loss_source"] = packet_loss_source;
    j["control_channel_packet_loss"] = control_channel_packet_loss;
    j["control_channel_samples"] = control_channel_samples;
    j["video_sample_revision"] = video_sample_revision;
    j["video_policy_sample_count"] = video_policy_sample_count;
    j["pacing_warning_streak"] = pacing_warning_streak;
    j["network_sample_revision"] = network_sample_revision;
    j["network_last_received_age_ms"] = network_last_received_age_ms;
    j["media_loss_sample_revision"] = media_loss_sample_revision;
    j["media_loss_last_received_age_ms"] = media_loss_last_received_age_ms;
    // As served: a figure whose readings stopped reads as stale, as Doctor treats it.
    j["network_verdict"] = network_verdict_json(served_network_verdict(*this));
    j["bytes_sent"] = bytes_sent;
    j["gpu_usage"] = gpu_usage;
    j["adaptive_target_bitrate_kbps"] = adaptive_target_bitrate_kbps;
    j["adaptive_bitrate_enabled"] = adaptive_bitrate_enabled;
    j["adaptive_bitrate_active"] = adaptive_bitrate_active;
    j["adaptive_bitrate_state"] = adaptive_bitrate_state;
    j["adaptive_runtime_update_supported"] = adaptive_runtime_update_supported;
    j["doctor_live_action_scope_available"] = doctor_live_action_scope_available;
    j["idr_requests_total"] = idr_requests_total;
    j["invalidate_ref_frames_requests_total"] = invalidate_ref_frames_requests_total;
    j["headless_mode"] = config::video.linux_display.headless_mode;
    j["ai_enabled"] = config::video.ai_optimizer.enabled;
    auto by_creation = input_virtual_pads;
    std::sort(by_creation.begin(), by_creation.end(), [](const virtual_pad_t &a, const virtual_pad_t &b) {
      return a.created < b.created;
    });
    nlohmann::json pads = nlohmann::json::array();
    for (std::size_t i = 0; i < by_creation.size(); ++i) {
      pads.push_back({{"player", static_cast<int>(i) + 1}, {"kind", by_creation[i].kind}});
    }
    j["controller_input"] = {
      {"pads", std::move(pads)},
      {"virtual_controller_created", input_virtual_controller_created},
      {"virtual_controller_number", input_virtual_controller_number},
      {"virtual_controller_kind", input_virtual_controller_kind},
      {"virtual_controller_error", input_virtual_controller_error},
      {"host_controller_isolation", input_host_controller_isolation},
      {"host_controller_isolation_detail", input_host_controller_isolation_detail},
      {"steam_input_status", input_steam_input_status},
      {"steam_profiles_checked", input_steam_profiles_checked},
      {"steam_profiles_with_xbox_support", input_steam_profiles_with_xbox_support},
      {"steam_forced_app_count", input_steam_forced_app_count},
      {"steam_input_detail", input_steam_input_detail},
      {"haptics_supported", input_haptics_supported},
      {"haptics_detail", input_haptics_detail}
    };

    // Multi-client data
    nlohmann::json clients_json = nlohmann::json::array();
    for (const auto &c : clients) {
      nlohmann::json cj;
      cj["name"] = c.name;
      cj["ip"] = c.ip;
      // Absent when the session was given no kind, because missing is unknown.
      if (!c.client_family.empty()) {
        cj["client_family"] = c.client_family;
      }
      cj["fps"] = c.fps;
      cj["bitrate_kbps"] = c.bitrate_kbps;
      cj["encode_time_ms"] = c.encode_time_ms;
      cj["codec"] = c.codec;
      // The encoder this client's own stream sampled, which last_session keeps when it ends.
      // Absent before its first sample, because missing is unknown.
      if (!c.encoder_backend.empty()) {
        cj["encoder_backend"] = c.encoder_backend;
      }
      cj["width"] = c.width;
      cj["height"] = c.height;
      cj["capture_source"] = capture_source_json(c.capture_source);
      if (c.session_generation != 0) {
        cj["stream_instance_id"] = stream_instance_id(c.session_generation);
      }
      if (!c.capture_backend.opened.empty()) {
        cj["capture"] = capture_backend_json(c.capture_backend, frames_of_published_display(c));
      }
      if (!c.pyrowave_route.empty()) {
        cj["pyrowave_route"] = c.pyrowave_route;
      }
      if (!c.start_outcome.empty()) {
        cj["start_outcome"] = c.start_outcome;
      }
      cj["latency_ms"] = c.latency_ms;
      cj["packet_loss"] = c.packet_loss;
      cj["packet_loss_available"] = c.packet_loss_available;
      cj["packet_loss_source"] = c.packet_loss_source;
      cj["control_channel_packet_loss"] = c.control_channel_packet_loss;
      cj["bytes_sent"] = c.bytes_sent;
      cj["fec_protection"] = fec_protection_json(c.fec_protection);
      cj["adaptive_target_bitrate_kbps"] = c.adaptive_target_bitrate_kbps;
      clients_json.push_back(cj);
    }
    j["clients"] = clients_json;
    j["capture_source"] = clients.empty() ? nlohmann::json(nullptr) : capture_source_json(clients.front().capture_source);
    // The one client's capture at the top level, for a reader that expects a single stream. Two
    // clients have two answers, and the first one's is no answer for both, so then there is none.
    if (clients.size() == 1 && !clients.front().capture_backend.opened.empty()) {
      const auto &backend = clients.front().capture_backend;
      j["capture_backend_preference"] = backend.preference;
      j["capture_backend_requested"] = backend.requested;
      j["capture_backend_opened"] = backend.opened;
      j["capture_backend_route"] = backend.route;
      if (!backend.mode_override_reason.empty()) {
        j["capture_mode_override_reason"] = backend.mode_override_reason;
      }
      if (!backend.route_fallback_reason.empty()) {
        j["capture_route_fallback_reason"] = backend.route_fallback_reason;
      }
    }
    // The one client's encoder at the top level, for the same reader. The process-wide
    // encoder_backend is whichever encode loop sampled last, Browser Stream's included, so it is
    // never the top-level encoder_backend: with two clients there is no one answer. The capture
    // forecast in linux_gpu_profile still names it, as the encoder the next stream is likely to get.
    if (clients.size() == 1 && !clients.front().encoder_backend.empty()) {
      j["encoder_backend"] = clients.front().encoder_backend;
    }
    // The most recently ended session on this host, which may not be any client listed above: one
    // viewer can end while another streams. A report about one session matches its
    // stream_instance_id.
    if (last_session) {
      j["last_session"] = ended_session_json(*last_session);
    }
    j["active_sessions"] = static_cast<int>(clients.size());
    j["doctor"] = build_doctor_json(*this, doctor_health);
    if (const auto identity = get_single_active_session_identity()) {
      const auto controller = adaptive_bitrate::get_doctor_state();
      bind_doctor_action_scope(
        j["doctor"], identity->session_token, identity->session_generation,
        controller.action_authority_revision,
        network_sample_revision,
        video_sample_revision
      );
    }

    return j.dump();
  }

  bool capture_path_uses_cpu_copy(const stats_t &stats) {
    return
      stats.capture_transport == platf::frame_transport_e::shm ||
      stats.capture_residency == platf::frame_residency_e::cpu ||
      stats.encode_target_residency == platf::frame_residency_e::cpu;
  }

  bool capture_path_is_gpu_native(const stats_t &stats) {
    return
      stats.capture_transport == platf::frame_transport_e::dmabuf &&
      stats.capture_residency == platf::frame_residency_e::gpu &&
      stats.encode_target_residency == platf::frame_residency_e::gpu;
  }

  bool capture_path_has_cross_gpu_dmabuf_risk(const stats_t &stats) {
#ifdef __linux__
    if (!stats.runtime_effective_headless ||
        !config::video.linux_display.use_cage_compositor ||
        stats.capture_transport != platf::frame_transport_e::dmabuf ||
        stats.capture_residency != platf::frame_residency_e::gpu) {
      return false;
    }
    const auto pairing = device_nodes_match(stats.capture_device, config::video.adapter_name);
    return pairing.has_value() && !*pairing;
#else
    return false;
#endif
  }


  namespace {
    std::optional<bool> build_has_cuda_override;
  }

  bool build_has_cuda() {
    if (build_has_cuda_override) {
      return *build_has_cuda_override;
    }
#ifdef POLARIS_BUILD_CUDA
    return true;
#else
    return false;
#endif
  }

#ifdef POLARIS_TESTS
  void set_build_has_cuda_for_tests(std::optional<bool> has_cuda) {
    build_has_cuda_override = has_cuda;
  }
#endif

  capture_forecast_t forecast_capture_path(const capture_forecast_inputs_t &in) {
    capture_forecast_t out;
    out.residency = "unknown";

    const auto system_memory = [&out](std::string cause, std::string severity, std::string message, std::string action) {
      out.residency = "system_memory";
      out.cause = std::move(cause);
      out.severity = std::move(severity);
      out.message = std::move(message);
      out.action = std::move(action);
    };

    const auto &backend = in.capture_backend;
    const bool nvidia = in.encoder == "nvenc";
    const bool vaapi = in.encoder == "vaapi";
    const bool vulkan = in.encoder == "vulkan";

    // Nothing has been looked at yet, or nothing captures at all; both have their own findings.
    if (backend.empty() || backend == "none") {
      return out;
    }
    // A software encoder reads system memory whatever the capture did; that is the encoder's
    // own story, not a capture one.
    if (in.encoder == "software") {
      out.residency = "system_memory";
      return out;
    }
    if (backend == "nvfbc") {
      out.residency = "gpu";
      return out;
    }
    if (backend == "x11") {
      system_memory(
        "x11_capture",
        "warning",
        "Capture is running through X11 (x11grab), which copies every frame through system "
        "memory before the encoder. That is the X11 path itself, not a fault in the driver or "
        "the GPU.",
        "Stream from a Wayland session, or use a Private Stream mode, which captures Polaris' "
        "own compositor and can keep frames on the GPU. capture = nvfbc keeps X11 capture on "
        "the GPU on NVIDIA cards that expose NvFBC."
      );
      return out;
    }
    if (nvidia && !in.build_has_cuda) {
      system_memory(
        "build_without_cuda",
        "warning",
        "This Polaris binary was built without CUDA, so on NVIDIA every capture path copies "
        "each frame through system memory before NVENC sees it, whatever stream mode or "
        "GPU-native setting is chosen. The startup log says Build features: cuda=disabled and "
        "each session logs Attempting to use NVENC without CUDA support. Reverting back to "
        "GPU -> RAM -> GPU.",
        "Install a Polaris package built with CUDA: the official Fedora, Arch and Ubuntu "
        "packages all are (polaris --version reports cuda=enabled), or build from source with "
        "-DPOLARIS_ENABLE_CUDA=ON. Only the NVIDIA driver is needed at run time, not the CUDA "
        "toolkit."
      );
      return out;
    }
    if (backend == "kms") {
      out.residency = (nvidia || vaapi || vulkan) ? "gpu" : "unknown";
      return out;
    }
    if (vaapi && (backend == "portal" || backend == "wlr")) {
      // Every VA-API capture path stays on the copy route until the DMA-BUF import boundary has
      // proof from affected hosts (#367). Say so before the first stream, because a fresh AMD
      // host reads SHM in Mission Control and assumes something is broken.
      if (backend == "portal" && in.portal_vaapi_dmabuf_opted_in) {
        return out;
      }
      const bool private_stream = backend == "wlr" && in.use_cage_compositor;
      const std::string path =
        backend == "portal" ? "Desktop capture through the desktop portal (Mirror Desktop, Host Virtual Display)" :
        private_stream ? "Private Stream capture" :
                         "wlroots capture";
      system_memory(
        "vaapi_system_memory_by_design",
        "info",
        path + " on VA-API keeps frames in system memory by design: the DMA-BUF import into the "
        "encoder has crashed or stalled on AMD hosts, so every VA-API capture path takes one copy "
        "per frame until affected hosts prove it safe. This is the expected path on AMD and "
        "Intel, not a fault.",
        std::string {"Nothing to change for a stable stream. If throughput falls short at high "
                     "resolution or refresh, lower resolution, frame rate or bitrate first."} +
          (backend == "portal" ?
             " POLARIS_PORTAL_DMABUF=1 in Polaris' environment opts into the unvalidated DMA-BUF "
             "path, with no automatic fallback if it stalls." :
             "")
      );
      return out;
    }
    if (vulkan && backend == "portal") {
      // The portal never offers Vulkan Video a DMA-BUF: its encode device there is the RAM
      // uploader whatever PipeWire could negotiate, until the portal can retire a failed DMA-BUF
      // frame to that uploader the way the private compositor can. POLARIS_PORTAL_DMABUF=1 is
      // VA-API's opt-in and does not reach it. Say so before the first stream, as VA-API does,
      // rather than wait for a stream to show a path that cannot change (#635).
      system_memory(
        "vulkan_portal_system_memory_by_design",
        "info",
        "Capture through the portal (Mirror Desktop, Host Virtual Display, Gamescope Stream) on "
        "Vulkan Video keeps frames in system memory by design: the portal hands Vulkan Video every "
        "frame in shared memory and Vulkan Video uploads it to the GPU itself, because the portal "
        "has no way yet to fall back when a DMA-BUF frame fails to import. This is the expected "
        "path for Vulkan Video on the portal, not a fault.",
        std::string {"Nothing to change for a stable stream. If throughput falls short at high "
                     "resolution or refresh, lower resolution, frame rate or bitrate first. "
                     "Private Stream can keep Vulkan Video frames on the GPU."} +
          (in.portal_vaapi_dmabuf_opted_in ?
             " POLARIS_PORTAL_DMABUF=1 applies to VA-API only and does not change this." :
             "")
      );
      return out;
    }
    if (backend == "portal") {
      // With CUDA the portal is asked for DMA-BUF and the compositor decides; KDE handed over
      // system memory in the lab. Nothing to say until a stream shows which.
      return out;
    }
    if (backend == "wlr" && vulkan) {
      out.residency = "gpu";
      return out;
    }
    if (backend == "wlr" && nvidia) {
      if (!in.use_cage_compositor) {
        // A wlroots desktop captured directly: GPU-native when it offers wlr-export-dmabuf.
        return out;
      }
      const bool hidden_headless = in.headless_mode && !in.prefer_gpu_native_capture;
      if (hidden_headless) {
        if (in.headless_extcopy_dmabuf_probe == std::optional<bool> {false}) {
          system_memory(
            "headless_dmabuf_unavailable",
            "warning",
            "Private Stream runs the hidden headless compositor, and the last time this host "
            "tried, that compositor could not hand frames over as DMA-BUF, so capture fell back "
            "to system memory (SHM) and each frame is copied before NVENC.",
            "Use the Private Stream (GPU-native) launch mode, which runs the private compositor "
            "windowed, where DMA-BUF capture works. Nova can choose it for one launch. For every "
            "client, choose it under Settings, Audio/Video, Where games run, or set "
            "linux_prefer_gpu_native_capture = enabled, and restart Polaris."
          );
        } else if (in.headless_extcopy_dmabuf_probe == std::optional<bool> {true}) {
          out.residency = "gpu";
        }
        return out;
      }
      if (in.windowed_gpu_native_probe == std::optional<bool> {false}) {
        system_memory(
          "windowed_dmabuf_unavailable",
          "warning",
          "The private compositor runs windowed so capture can stay on the GPU, but the last "
          "DMA-BUF probe on this host failed, so frames are copied through system memory (SHM) "
          "before NVENC.",
          "Check the NVIDIA driver and the compositor under it: this path needs wlr-export-dmabuf "
          "from labwc and a driver that can import the buffer. A support bundle from one stream "
          "shows the import error."
        );
      } else if (in.windowed_gpu_native_probe == std::optional<bool> {true}) {
        out.residency = "gpu";
      }
      return out;
    }
    return out;
  }

  nlohmann::json capture_forecast_json(const capture_forecast_inputs_t &in, const capture_forecast_t &forecast) {
    return {
      {"backend", in.capture_backend.empty() ? "unevaluated" : in.capture_backend},
      {"encoder", in.encoder.empty() ? "unknown" : in.encoder},
      {"build_has_cuda", in.build_has_cuda},
      {"residency", forecast.residency},
      {"cause", forecast.cause}
    };
  }

#ifdef __linux__
  namespace {
    std::string backend_setting_label(std::string_view preference) {
      if (preference == "evdi") {
        return "EVDI";
      }
      if (preference == "kwin") {
        return "KWin";
      }
      if (preference == "wlr") {
        return "Hyprland";
      }
      if (preference == "kscreen") {
        return "kscreen-doctor";
      }
      return "Automatic";
    }

    /**
     * Host Virtual Display on KDE Plasma: the three ways a stream screen there went wrong in
     * testing, each silent until now. The game opened on the desk's monitor because the screen
     * came from EVDI, a tap landed on the wrong monitor because KWin spread the touch screen
     * over all of them, and an EVDI screen kept a 1.35 scale KWin had stored for it.
     */
    void append_host_virtual_display_warnings(nlohmann::json &warnings, const virtual_display::doctor_notes_t &notes) {
      if (!notes.plasma) {
        return;
      }

      for (const auto &route : notes.input_routes) {
        if (route.routed || route.output.empty()) {
          continue;
        }
        const auto device = route.device.empty() ? std::string {"an input device"} : route.device;
        warnings.push_back({
          {"id", "hvd_input_not_mapped"},
          {"severity", "warning"},
          {"message", "Polaris could not point " + device + " at the stream screen [" + route.output +
                        "], so a tap or a pen stroke from the client can land on another monitor. KWin said: " +
                        route.error},
          {"action", "Run Polaris inside the Plasma session it streams, as the packaged polaris.service does, "
                     "so it can reach KWin on that session's bus. A support bundle from one stream carries the "
                     "whole KWin answer."}
        });
        break;  // One is enough: the cause is the same for every device.
      }

      if (!notes.scaled_screen.empty()) {
        const auto percent = std::to_string(static_cast<int>(notes.scaled_screen_scale * 100.0 + 0.5));
        const auto asked = std::to_string(static_cast<int>(notes.scaled_screen_expected * 100.0 + 0.5));
        warnings.push_back({
          {"id", "hvd_screen_scaled"},
          {"severity", "warning"},
          {"message", "KWin runs the stream screen [" + notes.scaled_screen + "] at " + percent +
                        "% scale rather than the " + asked + "% this device asked for, from a layout it stored "
                        "for that screen. Its desktop is not the size the client expects, so everything on it is "
                        "drawn at the wrong size."},
          {"action", "While a stream is running, set that screen to " + asked + "% in System Settings, under "
                     "Display & Monitor; KWin keeps the choice for next time. Or set Host Virtual Display Backend "
                     "to Automatic, so Plasma gets Polaris's own KWin screen, which Polaris scales itself."}
        });
      }

      if (notes.last_backend &&
          *notes.last_backend != virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT) {
        const auto used = std::string {virtual_display::backend_name(*notes.last_backend)};
        const bool chosen = notes.preference != "auto" && notes.preference != "kwin";
        std::string message;
        std::string action;
        if (chosen) {
          message = "Host Virtual Display used " + used + " because Host Virtual Display Backend is set to " +
                    backend_setting_label(notes.preference) +
                    ". On Plasma that screen is a monitor like any other, so a game opens on your primary one "
                    "rather than on the stream.";
          action = "Set Host Virtual Display Backend to Automatic, unless you chose " +
                   backend_setting_label(notes.preference) + " for a reason.";
        } else {
          message = "Host Virtual Display used " + used + " because Polaris could not create a KWin screen: " +
                    (notes.kwin_reason.empty() ? std::string {"no reason was recorded."} : notes.kwin_reason) +
                    " A game may open on your primary monitor rather than on the stream.";
          action = "Fix what the reason names and restart Polaris. Troubleshooting, under Host Virtual Display "
                   "on KDE, covers each one.";
        }
        warnings.push_back({
          {"id", "hvd_kwin_screen_unused"},
          {"severity", "info"},
          {"message", std::move(message)},
          {"action", std::move(action)}
        });
      }
    }
  }  // namespace
#endif

  nlohmann::json linux_gpu_profile_json(const stats_t &stats) {
    const auto &linux_display = config::video.linux_display;
    const bool gpu_native_requested =
      stats.gpu_native_probe.requested ||
      stats.runtime_gpu_native_override_active ||
      linux_display.prefer_gpu_native_capture;
    // The encoder node that is actually in use: adapter_name when set, otherwise the node
    // Polaris chose. The old comparison only knew the configured name, so a hybrid laptop
    // whose desktop renders on the iGPU while Polaris auto-picked the NVIDIA card looked
    // "unknown" to the Doctor, the exact case where the split matters.
    std::string effective_adapter = config::video.adapter_name;
    const std::string encoder_adapter_source = effective_adapter.empty() ? "auto" : "configured";
    std::string compositor_device = stats.wayland_main_device;
#ifdef __linux__
    if (effective_adapter.empty()) {
      effective_adapter = platf::effective_encoder_render_device();
    }
    if (compositor_device.empty()) {
      compositor_device = wl::last_compositor_main_device();
    }
#endif
    const auto capture_device_pairing = device_nodes_match(stats.capture_device, effective_adapter);
    const auto wayland_device_pairing = device_nodes_match(compositor_device, effective_adapter);
    nlohmann::json adapter_matches_capture_device = nullptr;
    nlohmann::json adapter_matches_wayland_main_device = nullptr;
    if (capture_device_pairing.has_value()) {
      adapter_matches_capture_device = *capture_device_pairing;
    }
    if (wayland_device_pairing.has_value()) {
      adapter_matches_wayland_main_device = *wayland_device_pairing;
    }

    std::string adapter_pairing_device;
    std::string adapter_pairing_device_source = "none";
    std::optional<bool> adapter_pairing;
    if (!stats.capture_device.empty()) {
      adapter_pairing_device = stats.capture_device;
      adapter_pairing_device_source = "capture_device";
      adapter_pairing = capture_device_pairing;
    } else if (!compositor_device.empty()) {
      adapter_pairing_device = compositor_device;
      adapter_pairing_device_source = "wayland_main_device";
      adapter_pairing = wayland_device_pairing;
    }

    std::string adapter_pairing_status = "unknown";
    if (adapter_pairing.has_value()) {
      adapter_pairing_status = *adapter_pairing ? "matched" : "mismatched";
    }
    const bool capture_metadata_reported =
      stats.capture_transport != platf::frame_transport_e::unknown ||
      stats.capture_residency != platf::frame_residency_e::unknown ||
      stats.encode_target_residency != platf::frame_residency_e::unknown;

    nlohmann::json configuration_warnings = nlohmann::json::array();
    nlohmann::json capture_forecast = nullptr;
#ifdef __linux__
    // No capture at all. Polaris logs this fatally at startup and then carries on serving, so a
    // host in this state pairs normally, accepts launches and advertises H.264 as the only codec
    // it has, while Doctor reports nothing. The log line is the only trace and it scrolls past
    // once, at boot, which is the worst possible place for it.
    if (platf::capture_sources_missing()) {
      configuration_warnings.push_back({
        {"id", "no_capture_backend"},
        {"severity", "fail"},
        {"message", "This host has no working capture backend, so no encoder could be probed and "
                    "it is advertising H.264 as the only codec it has. Streams will connect and "
                    "look far worse than this hardware can manage, or fail outright."},
        {"action", "Check the capture setting against the stream mode. Capture backends are not "
                   "interchangeable across compositors: wlr needs the wlroots capture protocols, "
                   "which KDE and GNOME do not have. Leaving capture unset lets Polaris pick one "
                   "that works. The startup log names the protocol that was missing."}
      });
    }

#ifdef POLARIS_BUILD_PYROWAVE
    // A scanout PyroWave cannot read. KWin composites HDR as sixteen bit float, so a KDE host in HDR
    // with capture = kms hands the codec a buffer it has to refuse. The refusal comes before the
    // stream now, by name to a client that said it would ask for PyroWave and as a bare status at
    // the handshake to one that did not, and nothing else in the report calls the format unusual, so
    // this is where a user finds out why.
    if (stats.capture_format == platf::frame_format_e::rgba16f) {
      configuration_warnings.push_back({
        {"id", "capture_format_unreadable_by_pyrowave"},
        {"severity", "warning"},
        {"message", "Capture on this host is handing over sixteen bit float frames, which PyroWave "
                    "cannot read. A KDE desktop in HDR composites in that format, so with "
                    "capture = kms a PyroWave stream of the desktop is refused before it starts. "
                    "Every other codec is unaffected, and so is a host scanning out packed ten bit "
                    "HDR."},
        {"action", "Turn HDR off on the host display to use PyroWave there, or capture by another "
                   "route. This is the compositor's float buffer rather than HDR itself, so it does "
                   "not change while the display keeps its mode."}
      });
    }
#endif

    // The configured capture backend could not capture anything and Polaris used another one.
    // Without this the only trace is a warning in the middle of startup, while the host goes on
    // serving with a backend nobody chose.
    const bool kms_refused = platf::kms_capture_refused_for_capability();
    constexpr auto enable_kms_command = "sudo -H polaris --setup-host --enable-kms";
    if (const auto substitution = platf::capture_backend_substitution_note(); !substitution.empty()) {
      // A substituted kms is a different story from a substituted wlr. wlr fails
      // because the compositor lacks a protocol; kms fails because the binary
      // lacks a capability that one command grants. Telling the kms case to
      // "set capture to the substituted backend" walks a user away from the only
      // path that carries HDR.
      const bool kms_for_capability = kms_refused && substitution.rfind("kms -> ", 0) == 0;
      configuration_warnings.push_back({
        {"id", "capture_backend_substituted"},
        {"severity", "warning"},
        {"message", kms_for_capability ?
           "This host is configured for KMS capture, but the Polaris binary does not hold "
           "CAP_SYS_ADMIN, so it could not read a framebuffer and Polaris substituted another "
           "backend (" + substitution + "). Nothing is wrong with the display or the GPU." :
           "The capture backend this host is configured to use cannot capture anything "
           "in the current stream mode, so Polaris substituted another one (" +
           substitution + "). Streams that capture the host desktop use the substitute; "
           "a Gamescope session or a virtual output keeps the configured backend. Capture "
           "backends are not interchangeable across compositors: wlr needs the wlroots "
           "capture protocols, which KDE and GNOME do not have, so only the "
           "private-compositor modes can use it there."},
        {"action", kms_for_capability ?
           std::string {"Run "} + enable_kms_command + " once and do what it prints, since it may ask for a new "
           "login first, then restart Polaris; KMS capture is what carries HDR, so keep it if HDR is the goal." :
           "Either set capture to the substituted backend so the configuration matches "
           "what is running, or go back to a private-compositor stream mode if you want "
           "the configured one."}
      });
    }

    // The refusal on its own, whatever happened next. With capture = kms and nothing to
    // substitute, the host serves with no capture at all and this is the one line that says
    // why; with a substitute, it is why the stream cannot carry HDR. It speaks only where KMS is
    // what a launch into the live mode asks for, the question a launch refusal asks: kms, drm,
    // which dispatch reads as kms, or auto, whose search reached KMS. A private compositor mode
    // captures through wlroots whatever capture says, and a launch into Gamescope Stream or the
    // dongle fills an unset capture with the portal before it asks, so a refusal the idle search
    // met there changes nothing about the stream.
    const auto mode_capture = stream_display_policy::canonical_capture_backend(
      stream_display_policy::capture_for_launch_into_current_mode()
    );
    // Autodetect in Mirror Desktop, Desktop Takeover, Gamescope Stream or the dongle, or beside KWin
    // screens, starts Polaris without capabilities so the portal and KWin accept it. Its search
    // passes over KMS on purpose, and granting the capability again changes nothing about the stream.
    const bool kms_set_aside = mode_capture.empty() && platf::kms_readiness::capability_set_aside();
    if (kms_refused && !kms_set_aside && (mode_capture == "kms" || mode_capture.empty())) {
      const bool nothing_else = platf::capture_sources_missing();
      configuration_warnings.push_back({
        {"id", "kms_capture_needs_capability"},
        {"severity", nothing_else ? "fail" : "warning"},
        {"message", "KMS capture found the display but could not read a DRM framebuffer handle, "
                    "because the Polaris binary does not hold CAP_SYS_ADMIN. That capability is "
                    "opt-in: the polaris-kms package carries it on a helper of its own, which updates "
                    "keep, and host setup points the polaris user service at that helper."},
        {"action", std::string {"Run "} + enable_kms_command + " once and do what it prints, since it may ask for a new "
                   "login first, then restart Polaris."}
      });
    }

    // Without CUDA the GPU-native advice below is a dead end; the capture forecast names the
    // build instead.
    if (
      linux_display.headless_mode &&
      linux_display.use_cage_compositor &&
      !linux_display.prefer_gpu_native_capture &&
      config::video.encoder == "nvenc" &&
      build_has_cuda()
    ) {
      configuration_warnings.push_back({
        {"id", "nvidia_headless_gpu_native_disabled"},
        {"severity", "warning"},
        {"message", "NVIDIA true-headless labwc is configured with GPU-native capture disabled; cold or missing encoder cache can fail launch with 503 encoder initialization even when NVENC is healthy."},
        {"action", "Set linux_prefer_gpu_native_capture = enabled, restart Polaris, and retry Private Stream (GPU-native) before chasing CUDA/NVENC driver issues."}
      });
    }

    // Where capture will land for the configured mode on this host, before any stream. The
    // capture reasons carried by a session say what happened after it happened, which is
    // exactly when a first-time host gives up. The forecast reads configuration, the build and
    // the last capture-source evaluation, and stays silent until the host has looked.
    {
      std::string encoder = stats.encoder_backend.empty() ? config::video.encoder : stats.encoder_backend;
      if (encoder.empty() || encoder == "auto") {
        encoder = video::active_encoder_name();
      }
      const char *portal_dmabuf_env = std::getenv("POLARIS_PORTAL_DMABUF");
      const capture_forecast_inputs_t inputs {
        .capture_backend = platf::selected_capture_backend(),
        .encoder = encoder,
        .build_has_cuda = build_has_cuda(),
        .use_cage_compositor = linux_display.use_cage_compositor,
        .headless_mode = linux_display.headless_mode,
        .prefer_gpu_native_capture = linux_display.prefer_gpu_native_capture,
        .headless_extcopy_dmabuf_probe = stream_runtime::labwc::cached_headless_extcopy_dmabuf_probe_result(),
        .windowed_gpu_native_probe = stream_runtime::labwc::cached_windowed_gpu_native_probe_result(),
        .portal_vaapi_dmabuf_opted_in = portal_dmabuf_env != nullptr && std::string_view {portal_dmabuf_env} == "1",
      };
      const auto forecast = forecast_capture_path(inputs);
      capture_forecast = capture_forecast_json(inputs, forecast);
      if (!forecast.cause.empty()) {
        configuration_warnings.push_back({
          {"id", "capture_copies_through_system_memory"},
          {"cause", forecast.cause},
          {"severity", forecast.severity},
          {"message", forecast.message},
          {"action", forecast.action}
        });
      }
    }
  #ifdef POLARIS_BUILD_VULKAN
    auto vaapi_vendor = stats.vaapi_vendor;
    std::transform(vaapi_vendor.begin(), vaapi_vendor.end(), vaapi_vendor.begin(), [](unsigned char byte) {
      return static_cast<char>(std::tolower(byte));
    });
    const bool amd_vaapi = stats.encode_target_device == "vaapi" &&
                           (vaapi_vendor.find("amd") != std::string::npos ||
                            vaapi_vendor.find("radeon") != std::string::npos ||
                            vaapi_vendor.find("radeonsi") != std::string::npos);
    const bool private_compositor_session =
      linux_display.use_cage_compositor &&
      (stats.runtime_requested_headless || stats.runtime_effective_headless ||
       stats.runtime_gpu_native_override_active);
    if (private_compositor_session && capture_path_uses_cpu_copy(stats) &&
        amd_vaapi && config::video.encoder == "vaapi") {
      configuration_warnings.push_back({
        {"id", "amd_private_explicit_vaapi_shm"},
        {"severity", "info"},
        {"message", "AMD Private Stream is using the explicitly selected VA-API compatibility path with SHM/system-memory capture. This can limit throughput at high resolution or refresh rate, but is not itself a configuration error."},
        {"action", "Keep VA-API selected if it provides stable streaming. If throughput is insufficient, first reduce resolution, frame rate, or bitrate. Change encoder only after checking a current build on this GPU and driver; a successful fallback cannot be assumed if a hardware probe crashes."}
      });
    }
  #endif
#endif
    // A client asked for HDR and did not get it. hdr_downgrade_message already says why, but
    // only over the session status route, and only while a stream is live. Put it where someone
    // looking for a reason will actually stand. The wlroots capture classes never override
    // is_hdr(), so on that path the answer is permanent and worth saying out loud rather than
    // leaving the user to re-toggle a switch that cannot work.
    // HDR turned off by something the user saved, rather than by what the hardware can do.
    // This one has to speak without a client ever asking for HDR, because that is the whole
    // shape of it: the saved setting stops the request being made at all, so dynamic_range stays
    // zero and every capability-based check below stays quiet. Name the file, because nothing in
    // a normal log does.
    if (!stats.hdr_policy_hdr && !stats.hdr_policy_reason.empty()) {
      const auto &reason = stats.hdr_policy_reason;
      const auto device = stats.hdr_policy_device.empty() ? std::string {"this device"} : stats.hdr_policy_device;
      std::string id = "hdr_disabled_by_saved_setting";
      std::string message;
      std::string action;
      if (reason == "paired_device_hdr_unsupported") {
        message = "HDR is switched off for " + device + " in device_db.json, where hdr_capable is "
                  "false. Polaris will not ask for HDR for that device however the client is set.";
        action = "If the device's display really does support HDR, set hdr_capable for it in "
                 "device_db.json, restart Polaris, and check the HDR row again.";
      } else if (reason == "client_profile_hdr_lock") {
        message = "HDR is switched off for " + device + " in its saved client profile, which "
                  "overrides what the client asks for.";
        action = "Turn Enable HDR back on for that device on the Devices page, or delete its "
                 "saved profile, then start a stream and check the HDR row again.";
      } else if (reason == "host_encoder_hdr_unsupported" && video::active_encoder_withholds_hdr_for_explicit_vulkan()) {
        // Stream stats carry no flag for an encoder the launch chose, so this names both sources.
        message = "HDR was refused because this stream encodes with Vulkan Video, set by encoder = vulkan "
                  "or chosen for the launch, which reads each frame on Gamescope Stream through system "
                  "memory as 8-bit and offers no HDR there. Nothing fell back.";
        action = "Stream without HDR on Gamescope Stream, or with another encoder there. VA-API takes "
                 "frames through the same 8-bit system memory upload unless POLARIS_PORTAL_DMABUF=1 is "
                 "set, and HDR through that unvalidated DMA-BUF route is not proven.";
      } else if (reason == "host_encoder_hdr_unsupported" && video::active_encoder_withholds_hdr()) {
        // The usual source of this reason on AMD Gamescope Stream, and nothing fell back: the encoder
        // row reads pass, so the advice below led nowhere.
        message = "HDR was refused because Auto encodes Gamescope Stream on this AMD host with Vulkan "
                  "Video, which reads each frame through system memory as 8-bit and offers no HDR "
                  "there. Nothing fell back; that is the route's policy.";
        // VA-API takes the same 8-bit upload on the portal unless the DMA-BUF opt-in is set, so
        // keeping it is no promise of HDR.
        action = "Stream without HDR on Gamescope Stream. hevc_mode = 3 or encoder = vaapi keeps VA-API "
                 "there, but VA-API takes frames through the same 8-bit system memory upload unless "
                 "POLARIS_PORTAL_DMABUF=1 is set, and HDR through that unvalidated DMA-BUF route is not "
                 "proven.";
      } else if (reason == "host_encoder_hdr_unsupported") {
        message = "HDR was refused because this host's encoder did not advertise a 10-bit "
                  "profile when the stream was resolved.";
        action = "Check the encoder row. If NVENC or VA-API fell back, fix that first; HDR "
                 "follows the encoder.";
      } else if (reason == "kwin_virtual_output_sdr") {
        // Not a saved setting: the screen Host Virtual Display got cannot carry HDR at all.
        id = "hdr_unavailable_on_kwin_virtual_screen";
        message = "HDR was asked for, but this stream runs on a screen KWin created for Host Virtual "
                  "Display, and KWin virtual screens carry no HDR. The stream is SDR.";
        action = "For HDR on KDE, stream Mirror Desktop from an HDR monitor with capture = kms, as "
                 "Linux HDR and Main10 in the configuration docs describes.";
      }
      if (!message.empty()) {
        configuration_warnings.push_back({
          {"id", id},
          {"severity", "info"},
          {"message", message},
          {"action", action}
        });
      }
    }

    if (const auto hdr_reason = hdr_downgrade_reason(stats);
        hdr_reason == "headless_hdr_unavailable" || hdr_reason == "display_not_hdr") {
      const bool wlroots_capture =
        config::video.capture == "wlr" ||
        (config::video.capture.empty() && linux_display.use_cage_compositor);
      configuration_warnings.push_back({
        {"id", "hdr_capture_path_cannot_report_hdr"},
        {"severity", "info"},
        {"message", wlroots_capture ?
           "A client asked for HDR and Polaris streamed 10-bit SDR instead. The wlroots capture "
           "path this host is using does not report display HDR at all, so HDR cannot engage on "
           "it whatever the monitor, GPU or client can do." :
           "A client asked for HDR and Polaris streamed 10-bit SDR instead, because the active "
           "capture display did not report HDR."},
        {"action", std::string {
           "True HDR needs a capture path that reads the display's HDR metadata, which today "
           "means the KMS/DRM path: capture = kms with Mirror Desktop as the host's stream mode, "
           "streaming the HDR monitor itself. A launch into Mirror Desktop from another mode keeps "
           "kms too, except on a host whose own mode is Host Virtual Display or Desktop Takeover: "
           "loading that mode put the portal or wlroots in place of kms, and that lasts until "
           "Polaris restarts. Those two modes capture their display through the portal or "
           "wlroots. Gamescope Stream and the dongle keep kms only as the host's own mode, and a "
           "launch into either from another mode captures through the portal. Private Stream on "
           "headless labwc captures through wlroots and is always SDR. See docs/runtime.md."} +
           (kms_refused ?
              std::string {" On this host KMS capture was refused for a missing capability; run "} +
                enable_kms_command + " first." :
              std::string {})}
      });
    }

    if (adapter_pairing_status == "mismatched") {
      const auto driver_of = [](const std::string &node) {
        std::string driver;
#ifdef __linux__
        driver = platf::render_device_driver(node);
#endif
        return driver.empty() ? std::string {"driver unknown"} : driver;
      };
      if (encoder_adapter_source == "auto") {
        // Nobody chose this split; Polaris did, by preferring the discrete card. On a
        // laptop that means the desktop renders on the iGPU, frames cross to the NVIDIA
        // card through system memory, and the card is woken for every stream, which is
        // where whole-machine freezes have come from.
        configuration_warnings.push_back({
          {"id", "linux_gpu_adapter_mismatch"},
          {"severity", "warning"},
          {"message", "Polaris chose " + effective_adapter + " (" + driver_of(effective_adapter) +
                        ") for encoding, while the compositor renders on " + adapter_pairing_device +
                        " (" + driver_of(adapter_pairing_device) + "). Frames cross between the two "
                        "through system memory, and the discrete card is woken for every stream; on a "
                        "laptop with NVIDIA runtime power management that split is where whole-machine "
                        "freezes have come from."},
          {"action", "To keep everything on the compositor's GPU, set adapter_name = " + adapter_pairing_device +
                       " and encoder = vaapi. To keep NVENC, turn off NVIDIA runtime power management "
                       "(options nvidia NVreg_DynamicPowerManagement=0x00 in /etc/modprobe.d, then rebuild "
                       "the initramfs and reboot). A split you chose on purpose can stay; set adapter_name "
                       "to make it explicit."}
        });
      } else {
        configuration_warnings.push_back({
          {"id", "linux_gpu_adapter_mismatch"},
          {"severity", "warning"},
          {"message", "The configured encoder adapter " + config::video.adapter_name + " differs from the " + adapter_pairing_device_source + " render node " + adapter_pairing_device + "; cross-GPU DMA-BUF import can fail or fall back to system memory."},
          {"action", "Verify the render-node mapping under /dev/dri/by-path, then select the adapter used by the compositor or keep the conservative SHM fallback."}
        });
      }
    }

#ifdef __linux__
    // Capture, encode and audio threads running at ordinary priority. Polaris logs this once, at
    // the first stream, and carries on; a support bundle exported later carried it only in raw log
    // text, beside a microstutter report it may well have explained. It stays a warning rather
    // than a failure because many hosts stream well without it.
    if (const auto limits = platf::thread_priority_unavailable_note(); !limits.empty()) {
      configuration_warnings.push_back({
        {"id", "thread_priority_unavailable"},
        {"severity", "warning"},
        {"message", "Polaris could not raise the priority of its capture, encode and audio threads (" + limits +
                      "), so they share the ordinary scheduler with the game. When the game is loading the CPU "
                      "hard, that can show up as stutter in the stream."},
        {"action", "Run Polaris as the packaged polaris.service, which asks for realtime priority where the user "
                   "manager allows it, rather than starting it some other way, or install RealtimeKit. "
                   "Troubleshooting, under Thread priority warning during a stream, has the commands that show "
                   "which limit applies."}
      });
    }

    append_host_virtual_display_warnings(configuration_warnings, virtual_display::doctor_notes());
#endif

    // A settings file the store refused (#782). Polaris keeps running on the settings it loaded,
    // but Settings, every settings save and Live Tuning go through the store, and until now only
    // the console's Settings page said so. Apps, pairing and the console password have files of
    // their own and still save. This is the refusal the last read or save met, so it
    // clears the moment the file reads again. Paired clients read this profile in session status
    // and support bundles carry it, so it names the kind of refusal and leaves the file's path,
    // which carries the user's name, to the console and the log.
    if (const auto refused = configuration_store::last_refusal(config::sunshine.config_file)) {
      configuration_warnings.push_back({
        {"id", "settings_file_unreadable"},
        {"severity", "warning"},
        {"refusal", private_state_file::refusal_name(refused->kind)},
        {"message", "Polaris refused to read its settings file, so Settings cannot load and no settings change "
                    "can be saved, whether it comes from the console, Live Tuning or a paired client. Apps, "
                    "pairing and the console password are kept in other files and still save. Polaris keeps "
                    "running on the settings it has already loaded."},
        {"action", "The banner at the top of the console names the file, the reason and the command that fixes "
                   "it, and so does the Polaris log. Once the file is fixed, Try again in the banner reads it "
                   "again, and Polaris does not need a restart."}
      });
    }

    nlohmann::json profile = {
      {"encoder_api", stats.encode_target_device},
      {"encoder_adapter", config::video.adapter_name},
      {"encoder_adapter_effective", effective_adapter},
      {"encoder_adapter_source", encoder_adapter_source},
      {"capture_device", stats.capture_device},
      {"wayland_main_device", stats.wayland_main_device},
      {"compositor_render_device", compositor_device},
      {"adapter_matches_capture_device", adapter_matches_capture_device},
      {"adapter_matches_wayland_main_device", adapter_matches_wayland_main_device},
      {"adapter_pairing_status", adapter_pairing_status},
      {"adapter_pairing_device", adapter_pairing_device},
      {"adapter_pairing_device_source", adapter_pairing_device_source},
      {"vaapi_vendor", stats.vaapi_vendor},
      {"capture_forecast", std::move(capture_forecast)},
      {"cross_gpu_dmabuf_risk", capture_path_has_cross_gpu_dmabuf_risk(stats)},
      {"gpu_native_requested", gpu_native_requested},
      {"gpu_native_attempted", stats.gpu_native_probe.headless_extcopy.attempted || stats.gpu_native_probe.windowed.attempted || (gpu_native_requested && capture_metadata_reported)},
      {"gpu_native_succeeded", capture_path_is_gpu_native(stats)},
      {"configuration_warnings", std::move(configuration_warnings)}
    };

    return profile;
  }

  std::string capture_path_summary(const stats_t &stats) {
    const bool capture_unknown =
      stats.capture_transport == platf::frame_transport_e::unknown &&
      stats.capture_residency == platf::frame_residency_e::unknown &&
      stats.encode_target_residency == platf::frame_residency_e::unknown;
    if (capture_unknown) {
      return "unknown";
    }
    if (stats.capture_transport == platf::frame_transport_e::shm) {
      return "shm_cpu_capture";
    }
    if (stats.capture_residency == platf::frame_residency_e::cpu) {
      return "cpu_capture";
    }
    if (stats.encode_target_residency == platf::frame_residency_e::cpu) {
      return "cpu_encode_upload";
    }
    if (capture_path_is_gpu_native(stats)) {
      return "gpu_native";
    }
    if (stats.capture_transport == platf::frame_transport_e::dmabuf &&
        stats.capture_residency == platf::frame_residency_e::gpu) {
      return "gpu_capture";
    }
    return "mixed_or_unknown";
  }

  std::string capture_path_reason(const stats_t &stats) {
    const bool capture_unknown =
      stats.capture_transport == platf::frame_transport_e::unknown &&
      stats.capture_residency == platf::frame_residency_e::unknown &&
      stats.encode_target_residency == platf::frame_residency_e::unknown;
    if (capture_unknown) {
      return "no_capture_metadata";
    }

    if (capture_path_is_gpu_native(stats)) {
#ifdef __linux__
      const auto &linux_display = config::video.linux_display;
      if (stats.runtime_effective_headless && linux_display.use_cage_compositor) {
        if (capture_path_has_cross_gpu_dmabuf_risk(stats)) {
          return "headless_extcopy_dmabuf_cross_gpu_risk";
        }
        return "headless_extcopy_dmabuf";
      }
      if (stats.runtime_gpu_native_override_active) {
        return "windowed_dmabuf_override";
      }
#endif
      return "gpu_native";
    }

    const auto &linux_display = config::video.linux_display;
    const bool gpu_native_requested =
      stats.runtime_gpu_native_override_active ||
      linux_display.prefer_gpu_native_capture;

    if (stats.capture_transport == platf::frame_transport_e::shm) {
      if (gpu_native_requested) {
        return "gpu_native_requested_shm_fallback";
      }
      if (stats.runtime_effective_headless && linux_display.use_cage_compositor) {
        return "headless_shm_fallback";
      }
      return "shm_capture";
    }

    if (stats.capture_residency == platf::frame_residency_e::cpu) {
      return gpu_native_requested ? "gpu_native_requested_cpu_capture" : "cpu_capture";
    }

    if (stats.encode_target_residency == platf::frame_residency_e::cpu) {
      return gpu_native_requested ? "gpu_native_requested_cpu_encode_upload" : "encoder_upload_cpu";
    }

    if (stats.capture_transport == platf::frame_transport_e::dmabuf &&
        stats.capture_residency == platf::frame_residency_e::gpu) {
      return "dmabuf_gpu_capture";
    }

    return "mixed_or_unknown";
  }

  nlohmann::json gpu_native_probe_json(const stats_t &stats) {
    auto attempt_json = [](const gpu_native_probe_attempt_t &attempt) {
      return nlohmann::json {
        {"attempted", attempt.attempted},
        {"cached", attempt.cached},
        {"result", attempt.result},
        {"failure_stage", attempt.failure_stage},
        {"failure_reason", attempt.failure_reason}
      };
    };

    return {
      {"requested", stats.gpu_native_probe.requested},
      {"attempted", stats.gpu_native_probe.headless_extcopy.attempted || stats.gpu_native_probe.windowed.attempted},
      {"headless_extcopy", attempt_json(stats.gpu_native_probe.headless_extcopy)},
      {"windowed", attempt_json(stats.gpu_native_probe.windowed)},
      {"selected_strategy", stats.gpu_native_probe.selected_strategy},
      {"fallback", stats.gpu_native_probe.fallback}
    };
  }

  std::string capture_path_reason_message(const std::string &reason) {
    if (reason == "gpu_native") {
      return "Capture and encoder conversion are GPU-resident.";
    }
    if (reason == "headless_extcopy_dmabuf") {
      return "True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.";
    }
    if (reason == "headless_extcopy_dmabuf_cross_gpu_risk") {
      return "True-headless DMA-BUF capture is using a different DRM render node than the configured encoder adapter; Polaris should fall back to SHM/system memory to avoid known cross-GPU black video.";
    }
    if (reason == "windowed_dmabuf_override") {
      return "Polaris is using a windowed private compositor so GPU-native capture can stay GPU-resident.";
    }
    if (reason == "gpu_native_requested_shm_fallback") {
      return "GPU-native capture was requested, but the active Wayland capture fell back to SHM/system-memory frames.";
    }
    if (reason == "gpu_native_requested_cpu_capture") {
      return "GPU-native capture was requested, but the active capture frames are CPU-resident.";
    }
    if (reason == "gpu_native_requested_cpu_encode_upload") {
      return "GPU-native capture was requested, but encoder upload/conversion is still CPU-resident.";
    }
    if (reason == "headless_shm_fallback" || reason == "headless_shm_default") {
      return "Private Stream is using the conservative SHM/system-memory path; the stream can be healthy, but capable high-FPS hosts should use a GPU-native path when available.";
    }
    if (reason == "encoder_upload_cpu") {
      return "Encoder upload/conversion uses CPU-resident frames.";
    }
    if (reason == "cpu_capture" || reason == "shm_capture") {
      return "The active capture path is CPU-resident.";
    }
    if (reason == "dmabuf_gpu_capture") {
      return "Capture is using DMA-BUF/GPU frames, but the encoder path is not fully GPU-native.";
    }
    if (reason == "no_capture_metadata") {
      return "No capture metadata has been reported yet.";

    }
    return "The active capture and encoder path is mixed or not fully classified.";
  }

  std::string hdr_effective_mode(const stats_t &stats) {
    if (stats.dynamic_range > 0 && stats.stream_hdr_enabled) {
      return "hdr10";
    }
    if (stats.dynamic_range > 0) {
      return "sdr_10bit";
    }
    return "sdr_8bit";
  }

  std::string hdr_downgrade_reason(const stats_t &stats) {
    if (stats.dynamic_range <= 0 || stats.stream_hdr_enabled) {
      return "none";
    }
    if (!stats.display_hdr) {
      if (stats.runtime_effective_headless) {
        return "headless_hdr_unavailable";
      }
      return "display_not_hdr";
    }
    if (!stats.hdr_metadata_available) {
      return "hdr_metadata_missing";
    }
    return "stream_hdr_disabled";
  }

  std::string hdr_downgrade_message(const stats_t &stats) {
    const auto reason = hdr_downgrade_reason(stats);
    if (reason == "none") {
      return {};
    }
    if (reason == "headless_hdr_unavailable") {
      return "The client requested HDR, but Private Stream is using a compositor output that does not report HDR. Polaris is streaming 10-bit SDR, not HDR; use a physical or virtual HDR-capable display path for true HDR.";
    }
    if (reason == "display_not_hdr") {
      return "The client requested HDR, but the active capture display is not reporting HDR. Polaris is streaming 10-bit SDR, not HDR.";
    }
    if (reason == "hdr_metadata_missing") {
      return "The client requested HDR, but the active capture display did not expose HDR10 metadata. Polaris is streaming 10-bit SDR, not HDR.";
    }
    return "The client requested HDR, but Polaris did not advertise HDR for this stream. Polaris is streaming 10-bit SDR, not HDR.";
  }

  namespace {
    bool doctor_has_capture_metadata(const stats_t &stats) {
      return stats.capture_transport != platf::frame_transport_e::unknown ||
        stats.capture_residency != platf::frame_residency_e::unknown ||
        stats.capture_format != platf::frame_format_e::unknown ||
        !stats.capture_device.empty();
    }

    double doctor_target_fps(const stats_t &stats) {
      if (stats.encode_target_fps > 0.0) return stats.encode_target_fps;
      if (stats.session_target_fps > 0.0) return stats.session_target_fps;
      if (stats.requested_client_fps > 0.0) return stats.requested_client_fps;
      return stats.fps;
    }

    void append_doctor_evidence(nlohmann::json &evidence,
                                const std::string &id,
                                const std::string &label,
                                const nlohmann::json &value,
                                const std::string &unit,
                                const std::string &status,
                                const std::string &source,
                                const std::string &detail) {
      evidence.push_back({
        {"id", id},
        {"label", label},
        {"value", value},
        {"unit", unit},
        {"status", status},
        {"source", source},
        {"redacted", false},
        {"detail", detail}
      });
    }

    /// How long a failed start stays the thing Doctor leads with once nothing is streaming.
    constexpr auto DOCTOR_FAILED_START_WINDOW = std::chrono::minutes {15};

    /// Whether an ended session ended within the window before now. A clock set back reads as recent.
    bool ended_within(const ended_session_t &ended, std::chrono::system_clock::time_point now,
                      std::chrono::system_clock::duration window) {
      if (ended.ended_at == std::chrono::system_clock::time_point {}) {
        return false;
      }
      return ended.ended_at >= now || now - ended.ended_at <= window;
    }

    /**
     * The last session, when it was a start the client gave up on during its own video setup, nothing
     * streams now, and it ended recently enough to be what someone opening Doctor is asking about.
     */
    const ended_session_t *recent_failed_start(const stats_t &stats, std::chrono::system_clock::time_point now) {
      if (stats.streaming || !stats.last_session) {
        return nullptr;
      }
      const auto &last = *stats.last_session;
      if (last.start_outcome != stream_start::k_client_left_during_setup ||
          !ended_within(last, now, DOCTOR_FAILED_START_WINDOW)) {
        return nullptr;
      }
      return &last;
    }

    /// "The last stream, to <client>" or "The last stream", for the sentences below.
    std::string last_stream_subject(const ended_session_t &ended) {
      return ended.client_name.empty() ? std::string {"The last stream"} :
                                         "The last stream, to " + ended.client_name + ",";
    }

    /// What Doctor says about a start the client left during video setup.
    std::string failed_start_summary(const ended_session_t &ended) {
      std::string summary = last_stream_subject(ended) + " failed to start: the client left during video setup";
      if (ended.start_client_left_after_ms >= 0) {
        summary += " " + std::to_string(ended.start_client_left_after_ms) + " ms after connecting";
      }
      summary += ", before any video arrived.";
      if (const auto codec = stream_start::codec_label(ended.codec); !codec.empty()) {
        summary += " It had negotiated " + std::string {codec} + ".";
      }
      return summary;
    }

    nlohmann::json failed_start_recommendation(const ended_session_t &ended, const std::string &summary) {
      const bool pyrowave = ended.codec == "pyrowave";
      return {
        {"title", "Try this first"},
        {"body", stream_start::failed_start_next_step(ended.codec)},
        {"why", summary},
        {"next_step_label", pyrowave ? "Choose HEVC or H.264" : "Read the client's error"},
        {"expected_effect", pyrowave ?
           "The next stream starts on a codec that device can decode." :
           "The client's own message says what stopped it, such as a decoder it could not create."}
      };
    }

    /// What Doctor's recommendation and action need to know about a PyroWave stream.
    struct pyrowave_doctor_t {
      /// The stream is PyroWave and its shape gives advice.
      bool active = false;
      /// Doctor can raise the stream toward raise_goal_kbps.
      bool raise_available = false;
      /// The raise goal as a request, and which of advice, cap or max_bitrate set it.
      int raise_goal_kbps = 0;
      std::string_view limited_by;
      /// The same goal at the encoder, which is where a live bitrate applies.
      int raise_goal_encoder_kbps = 0;
      /// Live Tuning's PyroWave floor is reached, and the floor as a request.
      bool at_floor = false;
      int floor_kbps = 0;
      /// The most a player can set by hand on this host, as a request: stream_bitrate::k_max_request_kbps,
      /// or max_bitrate where that is lower.
      int manual_max_kbps = 0;
    };

    /// A bitrate as a player sets it, in whole Mbps, rounded up so setting it satisfies it.
    std::string whole_mbps(int kbps) {
      return std::to_string((std::max(kbps, 0) + 999) / 1000) + " Mbps";
    }

    std::string pyrowave_limit_phrase(std::string_view limited_by) {
      if (limited_by == "max_bitrate") return "the host's max_bitrate";
      if (limited_by == "cap") return "the most Doctor raises PyroWave to";
      return "what PyroWave's model advises";
    }

    /// What to set as a live bitrate to reach the raise goal. A live bitrate applies at the encoder, with
    /// no FEC or audio taken off it, so it is the goal at the encoder, and the text says how that relates
    /// to the request Doctor quotes everywhere else.
    std::string pyrowave_live_goal_guidance(const pyrowave_doctor_t &pyrowave) {
      return "set about " + whole_mbps(pyrowave.raise_goal_encoder_kbps) + " as the live bitrate in your client, which "
             "turns Live Tuning off for this stream only. A live bitrate applies at the encoder, so that is the request "
             "of about " + whole_mbps(pyrowave.raise_goal_kbps) + " Polaris advises without its FEC and audio.";
    }

    std::string pyrowave_floor_guidance(const pyrowave_doctor_t &pyrowave) {
      return "PyroWave is at its floor, a request of about " + whole_mbps(pyrowave.floor_kbps) +
             ", where Live Tuning and Doctor stop cutting because below it the picture falls apart. Switch to "
             "HEVC, or lower the resolution or frame rate.";
    }

    /// Whether a player can set more by hand than the raise goal, both in the whole Mbps Doctor quotes.
    /// True where the cap holds the goal and max_bitrate, if set, is above it; where max_bitrate holds the
    /// goal, the host takes no more by hand either.
    bool pyrowave_more_by_hand(const pyrowave_doctor_t &pyrowave) {
      return pyrowave.manual_max_kbps / 1000 > (pyrowave.raise_goal_kbps + 999) / 1000;
    }

    /// The limit that holds the raise goal below the far figure, with the goal. Where a player can still
    /// set more by hand it is only how far Doctor raises, and otherwise it is what this host allows.
    std::string pyrowave_limit_figure(const pyrowave_doctor_t &pyrowave) {
      return "the " + whole_mbps(pyrowave.raise_goal_kbps) +
             (pyrowave_more_by_hand(pyrowave) ? " Doctor raises it to" : " this host's max_bitrate allows");
    }

    /// A smaller or slower picture, or HEVC, and the most a player can set by hand where that is more than
    /// Doctor raises to. Doctor changes no bitrate itself: no raise it offers reaches what the model asks,
    /// and a cut would only soften the picture further.
    std::string pyrowave_host_limit_guidance(const pyrowave_doctor_t &pyrowave) {
      std::string guidance = "Lower the resolution or frame rate, or use HEVC, for a sharper picture.";
      if (pyrowave_more_by_hand(pyrowave)) {
        // Rounded down, so a player who sets it stays within what the host takes.
        guidance += " You can also set up to " + std::to_string(pyrowave.manual_max_kbps / 1000) + " Mbps by hand.";
      }
      return guidance;
    }

    std::string two_decimals(double value) {
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f", value);
      return text;
    }

    /// What the loss row says: the frames behind the figure, which loss it is, and the band.
    std::string video_frame_loss_detail(const network_verdict_t &verdict) {
      return std::to_string(verdict.frames_lost) + " of " + std::to_string(verdict.frames_expected) +
             " video frames in the client's last " + std::to_string(verdict.media_samples) +
             " reports never reached it whole after FEC recovery, " + two_decimals(verdict.loss_pct) +
             "%. Doctor calls loss network pressure at " + two_decimals(network_judge_t::k_loss_enter_pct) +
             "% over the last " + std::to_string(network_judge_t::k_window.count()) +
             " seconds and clears it only below " + two_decimals(network_judge_t::k_loss_exit_pct) +
             "%. Frames the host dropped before sending are counted separately.";
    }

    /// What the latency row says: the median behind the figure and the band.
    std::string round_trip_detail(const network_verdict_t &verdict) {
      return "Median of the host's last " + std::to_string(verdict.rtt_samples) + " round trip readings over " +
             std::to_string(network_judge_t::k_window.count()) + " seconds. Doctor watches RTT from " +
             std::to_string(static_cast<int>(network_judge_t::k_rtt_enter_ms)) + " ms, clears it below " +
             std::to_string(static_cast<int>(network_judge_t::k_rtt_exit_ms)) + " ms and fails the stream at " +
             std::to_string(static_cast<int>(network_judge_t::k_rtt_fail_ms)) + " ms.";
    }

    nlohmann::json doctor_recommendation(const std::string &primary_issue,
                                         const std::string &summary,
                                         const nlohmann::json &health,
                                         bool live_bitrate_tunable,
                                         bool single_session_scope,
                                         bool auto_safe_managing,
                                         const pyrowave_doctor_t &pyrowave,
                                         const judged_network_t &network) {
      std::string title = "Try this first";
      std::string body = "Start a stream, reproduce the issue, then export diagnostics with this Doctor result attached.";
      std::string next_step = "Export diagnostics";
      std::string expected = "Support gets deterministic telemetry instead of guesswork.";

      if (primary_issue == "none") {
        title = "Keep playing";
        body = "Streaming telemetry looks ready. Keep this page open if you are trying to catch an intermittent problem.";
        next_step = "Keep monitoring";
        expected = "No recovery action should be needed right now.";
      } else if (primary_issue == "network_jitter") {
        if (pyrowave.at_floor) {
          body = "Confirmed network pressure is affecting this stream. " + pyrowave_floor_guidance(pyrowave);
          next_step = "Use HEVC or a lower mode";
          expected = "A codec that needs fewer bits, or a smaller picture, fits the link without the picture falling apart.";
        } else if (auto_safe_managing) {
          body = "Confirmed network pressure is affecting this stream, and Live Tuning already owns the live bitrate correction. Doctor will measure the result without racing the active controller.";
          next_step = "Recheck Live Tuning";
          expected = "Live Tuning should lower the encoder target until loss and latency return to the stable range.";
        } else if (live_bitrate_tunable) {
          body = "Current sustained loss or latency evidence confirms network pressure. Doctor can lower bitrate one guarded step and watch the same telemetry for recovery.";
          next_step = "Fix and verify";
          expected = "Packet loss and latency should return to the stable range without changing encoder or display settings.";
        } else if (!single_session_scope) {
          body = "Doctor requires one fresh stream generation that has not shared the process-global bitrate target. Disconnect additional viewers and reconnect the affected stream before rechecking.";
          next_step = "Reconnect one stream";
          expected = "No other encoder can be changed by this stream's Auto Fix.";
        } else {
          body = "Current sustained loss or latency evidence confirms network pressure, but this encoder cannot change bitrate during a live stream. Lower the paired or client bitrate for the next launch.";
          next_step = "Lower next-stream bitrate";
          expected = "The next stream should start at a bitrate the network can sustain.";
        }
      } else if (primary_issue == "network_observation") {
        body = "Doctor sees a debounced network warning, but current loss and latency do not justify reducing quality. Recheck the live path before changing bitrate.";
        next_step = "Recheck network";
        expected = "Doctor will either clear the warning or gather direct evidence before offering a bitrate change.";
      } else if (primary_issue == "control_channel_observation") {
        title = "Keep monitoring";
        // Only what the window judged: a stream with no media reports, or none yet, has no loss to
        // clear, and ENet's first seconds give no RTT to judge.
        const std::string window = " over the last " + std::to_string(network_judge_t::k_window.count()) + " seconds";
        const std::string judged =
          network.loss_judged && network.rtt_judged ? "video frame loss and round trip time" + window + " stay below network pressure" :
          network.loss_judged ? "video frame loss" + window + " stays below network pressure" :
          network.rtt_judged ? "no video frame loss is measured, and round trip time" + window + " stays below network pressure" :
          std::string {"neither video frame loss nor round trip time is judged yet"};
        body = "The reliable control channel retried packets, but " + judged + ", so Doctor changes nothing for this.";
        if (auto_safe_managing) {
          body += " Live Tuning keeps adjusting the live bitrate on its own.";
        }
        next_step = "Keep monitoring";
        expected = "Sustained video frame loss or RTT pressure must appear before Doctor recommends a network recovery action.";
      } else if (primary_issue == "quality_reduced_live") {
        if (auto_safe_managing) {
          body = "The current network is clean and Live Tuning is already holding or recovering the live target below this stream's launch ceiling. Doctor will verify that recovery without applying a competing bitrate change.";
          next_step = "Recheck Live Tuning";
          expected = "Live Tuning should recover quality gradually while keeping the stream inside the measured network budget.";
        } else if (live_bitrate_tunable) {
          body = "The current network is clean, and the live adaptive target is below this stream's effective launch ceiling. Doctor can retry quality gradually and verify every step.";
          next_step = "Restore and verify";
          expected = "Bitrate should climb toward the capability-validated launch ceiling while Doctor stops immediately if live loss or latency returns.";
        } else if (!single_session_scope) {
          body = "Doctor requires one fresh stream generation that has not shared the process-global bitrate target. Disconnect additional viewers and reconnect the affected stream before rechecking.";
          next_step = "Reconnect one stream";
          expected = "No other encoder can be changed by this stream's Auto Fix.";
        } else {
          body = "The current network is clean, but this encoder cannot restore bitrate during the active stream. Doctor will not change the next launch.";
          next_step = "Keep current settings";
          expected = "Any later launch remains governed only by the user's selected preset and capability validation.";
        }
      } else if (primary_issue == "pyrowave_starved") {
        // A starved stream always has a raise to offer: it is starved only more than a tenth below the raise
        // goal as a request, and a request never falls as the encoder rate rises, so its encoder runs below
        // the goal's. The PyroWave advice tests hold that.
        const auto goal = whole_mbps(pyrowave.raise_goal_kbps);
        if (auto_safe_managing) {
          body = "The network is clean and PyroWave is short of bits. Live Tuning owns the bitrate and never raises it above "
                 "your request, so " + pyrowave_live_goal_guidance(pyrowave);
          next_step = "Raise the bitrate";
          expected = "Fewer frames should hit PyroWave's byte ceiling, and the picture should sharpen.";
        } else if (live_bitrate_tunable) {
          body = "The network is clean and PyroWave is more than a tenth below the request Polaris advises. Doctor can "
                 "raise it to a request of " + goal + " in guarded steps, verifying each one, and Undo puts back the bitrate "
                 "you chose.";
          next_step = "Raise and verify";
          expected = "Fewer frames should hit PyroWave's byte ceiling while loss and latency stay in range.";
        } else if (!single_session_scope) {
          body = "Doctor requires one fresh stream generation that has not shared the process-global bitrate target. Disconnect additional viewers and reconnect the affected stream before rechecking.";
          next_step = "Reconnect one stream";
          expected = "No other encoder can be changed by this stream's Auto Fix.";
        } else {
          body = "PyroWave is more than a tenth below the request Polaris advises, but this stream cannot change bitrate "
                 "live. Set a request of about " + goal + " in your client for the next stream.";
          next_step = "Raise next-stream bitrate";
          expected = "The next stream should start at a bitrate PyroWave can use.";
        }
      } else if (primary_issue == "pyrowave_needs_more_than_allowed") {
        // The same guidance whoever owns the bitrate, because Doctor changes no bitrate here.
        body = pyrowave_host_limit_guidance(pyrowave);
        next_step = "Use a lower mode or HEVC";
        expected = "A smaller or slower picture needs fewer bits, and HEVC needs fewer for the same picture.";
      } else if (primary_issue == "steam_input_conflict") {
        body = "Local Steam Input settings can claim the Polaris Xbox virtual controller while strict isolation prevents Steam from creating its replacement controller. Disable Steam Input for Xbox controllers in Steam Settings, and set any per-game Force On overrides to Default or Disable.";
        next_step = "Adjust Steam Input";
        expected = "Proton games should read the Polaris virtual controller directly without a per-game workaround.";
      } else if (primary_issue == "encoder_load") {
        // PyroWave's time goes to colour conversion and a wavelet transform, which take as long at any
        // bitrate, so a lower bitrate softens the picture and gives the encoder nothing back.
        body = pyrowave.active ?
          "Lower the resolution or FPS to give PyroWave more frame time. Its encode takes as long at any bitrate, so a lower bitrate would only soften the picture." :
          "Trim bitrate, resolution, or FPS to give the active encoder more frame time.";
        next_step = "Lower stream load";
        expected = "Encode time should fall back under the low-latency budget.";
      } else if (primary_issue == "host_render_limited") {
        body = "Lower the game render preset, render resolution, or stream FPS before tuning bitrate.";
        next_step = "Lower game/FPS target";
        expected = "Game frames should arrive on pace with the stream target.";
      } else if (primary_issue == "capture_missing" || primary_issue == "no_active_stream") {
        body = "Start or resume the affected stream so Polaris can classify the real capture and encode path.";
        next_step = "Start stream";
        expected = "Doctor can switch from readiness hints to live telemetry evidence.";
      } else if (primary_issue.find("capture") != std::string::npos ||
                 primary_issue.find("shm") != std::string::npos ||
                 primary_issue.find("dmabuf") != std::string::npos ||
                 primary_issue == "nvenc_cuda_disabled") {
        body = health.value("summary", summary);
        next_step = "Review capture evidence";
        expected = "Advanced evidence will show whether the host is on GPU-native, DMA-BUF, SHM/system-memory, or CPU-copy fallback.";
      } else {
        body = health.value("summary", summary);
        next_step = health.value("relaunch_recommended", false) ? "Plan safe relaunch" : "Open Advanced";
        expected = "The recommended change should target the loudest deterministic signal first.";
      }

      return {
        {"title", title},
        {"body", body},
        {"why", summary},
        {"next_step_label", next_step},
        {"expected_effect", expected}
      };
    }

    nlohmann::json doctor_safe_action(const std::string &primary_issue,
                                      const nlohmann::json &health,
                                      int current_bitrate_kbps,
                                      const doctor_quality_goal_t &quality_goal,
                                      bool live_bitrate_tunable,
                                      bool single_session_scope,
                                      bool auto_safe_managing,
                                      const pyrowave_doctor_t &pyrowave,
                                      const std::string &source_result_id,
                                      std::string_view app_uuid,
                                      const std::string &failed_start_next_step = {}) {
      std::string id = "none";
      std::string label = "No automatic action";
      std::string kind = "none";
      std::string endpoint;
      std::string method;
      nlohmann::json payload = nlohmann::json::object();
      std::string rollback = "No change is applied by Doctor.";
      std::string unavailable_reason;

      nlohmann::json verification = {
        {"mode", "none"},
        {"delay_seconds", 0},
        {"endpoint", ""},
        {"success_when", nlohmann::json::array()}
      };

      const bool auto_safe_network_management = auto_safe_managing &&
        (primary_issue == "network_jitter" || primary_issue == "quality_reduced_live");
      const auto read_only_guidance = [&](std::string reason) {
        id = "none";
        label = "Manual";
        kind = "manual_guidance";
        unavailable_reason = std::move(reason);
        rollback = "Read-only guidance; Doctor does not change the stream.";
      };
      if (primary_issue == "network_jitter" && pyrowave.at_floor) {
        // No cut below PyroWave's floor, from Live Tuning or from Doctor: the answer is another codec
        // or a smaller picture.
        read_only_guidance(pyrowave_floor_guidance(pyrowave));
      } else if (primary_issue == "pyrowave_starved" && !auto_safe_managing &&
                 pyrowave.raise_available && live_bitrate_tunable) {
        // The one place Polaris raises a stream above the player's own request: a single tap, on a
        // clean network, to the far advice and no higher than the cap and max_bitrate, verified in
        // guarded steps with Undo. Live Tuning, which acts on its own, never does.
        id = "restore_quality";
        label = "Auto Fix";
        kind = "live_tuning";
        endpoint = "/api/doctor/action";
        method = "POST";
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        payload["target_bitrate_kbps"] = pyrowave.raise_goal_kbps;
        payload["goal_source"] = "pyrowave_advice";
        rollback = "Undo restores the live bitrate and Auto Quality state that were active before this Doctor run.";
        verification = {
          {"mode", "graduated_live_telemetry"},
          {"delay_seconds", 8},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"network_risk stays clear", "packet_loss_pct <= 2", "latency_ms < 45", "PyroWave's advised bitrate is reached"})}
        };
      } else if (primary_issue == "pyrowave_starved") {
        const auto goal = whole_mbps(pyrowave.raise_goal_kbps);
        read_only_guidance(
          auto_safe_managing ?
            "Live Tuning owns the bitrate and never raises it above your request, so " +
              pyrowave_live_goal_guidance(pyrowave) :
          single_session_scope ?
            "The active encoder does not support runtime bitrate updates. Set a request of about " + goal +
              " in your client for the next stream." :
            "Auto Fix requires a fresh, unshared stream generation to own the process-global bitrate controller."
        );
      } else if (primary_issue == "pyrowave_needs_more_than_allowed") {
        // Read only whoever owns the bitrate: no raise Doctor offers reaches what the model asks, and a
        // cut only softens the picture.
        read_only_guidance(pyrowave_host_limit_guidance(pyrowave));
      } else if (auto_safe_network_management) {
        id = "recheck_network";
        label = "Recheck";
        kind = "verification";
        endpoint = "/api/doctor/action";
        method = "POST";
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        rollback = "This check does not change bitrate or stream settings. Live Tuning remains the only live bitrate controller.";
        verification = {
          {"mode", "live_telemetry"},
          {"delay_seconds", 3},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"Live Tuning remains the live bitrate owner", "current loss and latency are measured again"})}
        };
      } else if (primary_issue == "network_jitter" && live_bitrate_tunable) {
        id = "lower_bitrate";
        label = "Auto Fix";
        kind = "live_tuning";
        endpoint = "/api/doctor/action";
        method = "POST";
        const int derived_bitrate_kbps = current_bitrate_kbps > 0 ?
          std::max(1000, static_cast<int>(std::round(current_bitrate_kbps * 0.80))) : 0;
        const int health_bitrate_kbps = health.value("safe_bitrate_kbps", 0);
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        // A safe bitrate that is no lower than the stream, which is what PyroWave's health reports,
        // names no step, so the payload names one guarded 20% step instead.
        payload["target_bitrate_kbps"] = health_bitrate_kbps > 0 && health_bitrate_kbps < current_bitrate_kbps ?
          health_bitrate_kbps : derived_bitrate_kbps;
        rollback = "Undo restores the live bitrate and Auto Quality state that were active before this Doctor run.";
        verification = {
          {"mode", "live_telemetry"},
          {"delay_seconds", 8},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"network_risk clears", "packet_loss_pct <= 2", "latency_ms < 45"})}
        };
      } else if (primary_issue == "network_observation") {
        id = "recheck_network";
        label = "Recheck";
        kind = "verification";
        endpoint = "/api/doctor/action";
        method = "POST";
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        rollback = "This check does not change bitrate or stream settings.";
        verification = {
          {"mode", "live_telemetry"},
          {"delay_seconds", 3},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"current network evidence remains below the action threshold"})}
        };
      } else if (primary_issue == "quality_reduced_live" && live_bitrate_tunable) {
        id = "restore_quality";
        label = "Auto Fix";
        kind = "live_tuning";
        endpoint = "/api/doctor/action";
        method = "POST";
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        payload["target_bitrate_kbps"] = quality_goal.target_kbps;
        payload["goal_source"] = quality_goal.source;
        rollback = "Undo restores the live bitrate and Auto Quality state that were active before this Doctor run.";
        verification = {
          {"mode", "graduated_live_telemetry"},
          {"delay_seconds", 8},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"network_risk stays clear", "packet_loss_pct <= 2", "latency_ms < 45", "effective launch bitrate ceiling is reached"})}
        };
      } else if ((primary_issue == "network_jitter" || primary_issue == "quality_reduced_live") && !live_bitrate_tunable) {
        unavailable_reason = single_session_scope ?
          "The active encoder does not support runtime bitrate updates." :
          "Auto Fix requires a fresh, unshared stream generation to own the process-global bitrate controller.";
      } else if (primary_issue == "steam_input_conflict") {
        id = "none";
        label = "Manual";
        kind = "manual_guidance";
        unavailable_reason =
          "Automatic Steam profile changes are disabled in this release. Review the host-wide Xbox opt-in and per-game overrides in Steam controller settings.";
        rollback = "Read-only guidance; Doctor does not close Steam or change any profile.";
        verification = {
          {"mode", "manual_steam_config"},
          {"delay_seconds", 0},
          {"endpoint", ""},
          {"success_when", nlohmann::json::array({"Steam Input host opt-in and per-game overrides are reviewed manually"})}
        };
      } else if (primary_issue == "stream_failed_to_start") {
        id = "none";
        label = "Manual";
        kind = "manual_guidance";
        unavailable_reason = failed_start_next_step;
        rollback = "Read-only guidance; Doctor changes nothing.";
        verification = {
          {"mode", "manual_client_change"},
          {"delay_seconds", 0},
          {"endpoint", ""},
          {"success_when", nlohmann::json::array({"the next stream from that client starts"})}
        };
      } else if (primary_issue == "no_active_stream" || primary_issue == "capture_missing") {
        id = "export_support_bundle";
        label = "Manual";
        kind = "export";
        rollback = "Export only; no host settings are changed.";
      } else if (health.value("relaunch_recommended", false)) {
        id = "recheck_pacing";
        label = "Recheck";
        kind = "verification";
        endpoint = "/api/doctor/action";
        method = "POST";
        payload["action_id"] = id;
        payload["source_result_id"] = source_result_id;
        rollback = "This check is read-only and cannot change the next launch.";
        verification = {
          {"mode", "live_telemetry"},
          {"delay_seconds", 3},
          {"endpoint", "/api/doctor/action"},
          {"success_when", nlohmann::json::array({"a fresh complete pacing evidence window is available"})}
        };
      }

      const bool undoable = id == "lower_bitrate" || id == "restore_quality";
      const std::string capability =
        (id == "lower_bitrate" || id == "restore_quality") ? "auto_fix" :
        (id == "recheck_network" || id == "recheck_pacing") ? "recheck" : "manual";

      return {
        {"id", id},
        {"label", label},
        {"capability", capability},
        {"kind", kind},
        {"destructive", false},
        {"requires_confirmation", id != "none" && id != "lower_bitrate" && id != "recheck_network" && id != "recheck_pacing" && id != "restore_quality"},
        {"requires_owner", id != "none"},
        {"allowed_in_viewer_mode", id == "export_support_bundle"},
        {"endpoint", endpoint},
        {"method", method},
        {"unavailable_reason", unavailable_reason},
        {"payload_preview", payload},
        {"rollback", rollback},
        {"verification", verification},
        {"paired_endpoint", ""},
        {"owner_tuning_allowed", false},
        {"undo", {{"supported", undoable}, {"endpoint", undoable ? "/api/doctor/action" : ""},
          {"paired_endpoint", ""}}}
      };
    }
  }  // namespace

  nlohmann::json build_doctor_json(const stats_t &stats,
                                   const nlohmann::json &health,
                                   std::string_view app_uuid) {
    const auto capture_reason = capture_path_reason(stats);
    const auto capture_path = capture_path_summary(stats);
    const bool capture_cpu_copy = capture_path_uses_cpu_copy(stats);
    const bool capture_gpu_native = capture_path_is_gpu_native(stats);
    const bool capture_known = doctor_has_capture_metadata(stats);
    const double target_fps = doctor_target_fps(stats);
    const double target_fps_gap = std::max(0.0, target_fps - stats.fps);
    const bool meaningful_fps_shortfall = is_meaningful_fps_shortfall(target_fps, stats.fps);
    // Doctor grades the network from the windowed verdict (network_judge_t), never from the newest
    // report: a second of burst loss or a Wi-Fi RTT spike moves the window's figure, not the
    // headline. judged_network() says when the verdict is current enough to count. Loss comes only
    // from client media reports; ENet's peer->packetLoss is a reliable control-channel EWMA, context
    // and never video loss.
    const auto verdict = served_network_verdict(stats);
    const auto network = judged_network(stats);
    const bool current_network_observation =
      stats.network_sample_revision > 0 &&
      stats.network_last_received_age_ms >= 0 &&
      stats.network_last_received_age_ms <= DOCTOR_CURRENT_NETWORK_MAX_AGE_MS;
    const bool loss_judged = network.loss_judged;
    const bool rtt_judged = network.rtt_judged;
    const bool confirmed_media_loss = network.loss_pressure;
    const bool rtt_fail = network.rtt_fail;
    const bool network_risk = network.risk;
    const bool network_fail = network.fail;
    const bool network_watch = network_risk && !network_fail;
    // ENet's own estimate over the same window and band, so one retransmission does not bring the
    // finding and the next quiet poll take it away.
    const bool control_channel_observation =
      stats.streaming &&
      current_network_observation &&
      verdict.control_loss_available && verdict.control_loss_elevated;
    const int live_bitrate_kbps = stats.adaptive_runtime_update_supported && stats.adaptive_target_bitrate_kbps > 0 ?
      stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    // The saved paired profile's bitrate, or the rate the stream opened at when there is none, so a
    // stream with no saved profile can climb back after a reduction too.
    const auto launch_quality_goal = doctor_quality_goal(stats, "launch");
    const int effective_quality_target_kbps = launch_quality_goal.encoder_kbps;
    // Enabled Auto Safe remains the sole continuous bitrate owner even while
    // its actuator is momentarily holding or recovering. A clean, reduced
    // target is therefore an informational observation, not a user action.
    const bool auto_safe_managing = stats.adaptive_bitrate_enabled;
    const bool network_evidence_available = loss_judged || rtt_judged;
    // Clean enough to raise quality: the loss and latency limits a Doctor quality restore verifies with.
    const bool network_clean_for_quality =
      stats.streaming && network_evidence_available && !network_risk &&
      (!loss_judged || verdict.loss_pct < network_judge_t::k_loss_enter_pct) &&
      (!rtt_judged || verdict.rtt_ms < network_judge_t::k_rtt_fail_ms);
    const bool quality_reduced_live =
      network_clean_for_quality &&
      stats.adaptive_runtime_update_supported && effective_quality_target_kbps > live_bitrate_kbps;
    // PyroWave more than a tenth below the rate Polaris advises, or held below its model's figure by the
    // cap or max_bitrate with most frames at its byte ceiling, on a clean network. Watch findings that
    // rank below every network, encoder and capture failure.
    // While Live Tuning owns the bitrate its target moves with the network, down for an RTT spike and
    // back a step at a time, and PyroWave's advice judged on it came and went as the target crossed
    // the starved line. Doctor judges the rate the stream is set to, which Live Tuning returns to.
    const auto pyrowave = evaluate_pyrowave_bitrate(
      stats, auto_safe_managing && effective_quality_target_kbps > 0 ? effective_quality_target_kbps : 0
    );
    // A stream cut below a request that already meets the raise goal climbs back to that request, by
    // the ordinary quality restore or by Live Tuning's own recovery when it owns the bitrate. PyroWave's
    // raise would stop short of what the player asked for, and its text would ask for less.
    const bool launch_restore_covers_pyrowave = quality_reduced_live &&
      effective_quality_target_kbps >= pyrowave.advice.raise_goal_encoder_kbps;
    const bool pyrowave_starved = pyrowave.active && pyrowave.starved && network_clean_for_quality &&
      !launch_restore_covers_pyrowave;
    // Never the same stream as a starved one: it runs within a tenth of its goal, and no raise Doctor
    // offers reaches what the model asks. Nor one Doctor's quality restore would bring back: that restore
    // is a step Doctor can take, and this finding must not hide it.
    const bool pyrowave_needs_more = pyrowave.active && pyrowave.needs_more_than_allowed &&
      network_clean_for_quality && !launch_restore_covers_pyrowave && !(quality_reduced_live && !auto_safe_managing);
    pyrowave_doctor_t pyrowave_doctor;
    if (pyrowave.active) {
      pyrowave_doctor.active = true;
      pyrowave_doctor.raise_goal_kbps = pyrowave.advice.raise_goal_kbps;
      pyrowave_doctor.limited_by = pyrowave.advice.raise_goal_limited_by;
      pyrowave_doctor.raise_goal_encoder_kbps = pyrowave.advice.raise_goal_encoder_kbps;
      pyrowave_doctor.raise_available = pyrowave.advice.raise_goal_encoder_kbps > live_bitrate_kbps;
      pyrowave_doctor.at_floor = pyrowave.at_floor;
      pyrowave_doctor.floor_kbps = pyrowave.floor_request_kbps;
      pyrowave_doctor.manual_max_kbps = config::video.max_bitrate > 0 ?
        std::min(stream_bitrate::k_max_request_kbps, config::video.max_bitrate) : stream_bitrate::k_max_request_kbps;
    }
    const bool single_session_scope = stats.clients.size() <= 1 &&
      stats.doctor_live_action_scope_available;
    const bool live_bitrate_tunable =
      stats.adaptive_runtime_update_supported && single_session_scope;
    // Auto Safe ownership is a policy choice, not a momentary actuator state.
    // FFmpeg bitrate changes can briefly recreate the encoder, and explicit or
    // rollback hand-offs can temporarily change the controller-state label.
    // None of those transitions authorizes Doctor to become a second writer.
    // While Auto Safe is enabled, Doctor may observe the live result; only
    // disabling Auto Safe can make a guarded Doctor mutation available.
    // Frame age is capture→encoder latency. On a CPU-copy capture path it is
    // dominated by the SHM copy/convert, so an over-budget age indicts the
    // capture path, not the encoder — the old verdict here sent an SHM-bound
    // user to lower bitrate, which cannot recover capture throughput (issue
    // #367: encode at 5.8 ms of budget while frames arrived 26.5 ms old).
    const bool encoder_time_fail = encoder_time_fails_budget(stats.encode_time_ms, target_fps);
    const bool frame_age_counts_against_encoder = !capture_cpu_copy;
    const bool encoder_fail = encoder_time_fail ||
                              (frame_age_counts_against_encoder && stats.avg_frame_age_ms >= 22.0);
    const bool encoder_watch = !encoder_fail &&
                               (encoder_time_nears_budget(stats.encode_time_ms, target_fps) ||
                                (frame_age_counts_against_encoder && stats.avg_frame_age_ms >= 18.0));
    const bool capture_latency_fail = capture_cpu_copy && !encoder_time_fail && stats.avg_frame_age_ms >= 22.0;
    const bool capture_latency_watch = capture_cpu_copy && !encoder_time_fail &&
                                       !capture_latency_fail && stats.avg_frame_age_ms >= 18.0;
    const bool source_cadence_available = stats.capture_source_fps > 0.0 && target_fps > 0.0;
    const bool source_cadence_confirms_motion =
      source_cadence_available && stats.capture_source_fps >= target_fps * 0.85;
    const bool observed_target_shortfall = meaningful_fps_shortfall && source_cadence_confirms_motion;
    const bool static_or_duplicate_content =
      stats.duplicate_frame_ratio >= 0.50 ||
      (source_cadence_available && stats.capture_source_fps < target_fps * 0.50 &&
       stats.duplicate_frame_ratio >= 0.10);
    const bool raw_pacing_watch =
      stats.dropped_frame_ratio >= 0.04 ||
      (!static_or_duplicate_content &&
       (stats.frame_jitter_ms >= 2.2 || observed_target_shortfall));
    const bool pacing_window_ready =
      stats.video_policy_sample_count >= DOCTOR_PACING_WARMUP_SAMPLES;
    const bool pacing_streak_confirmed =
      stats.pacing_warning_streak >= DOCTOR_PACING_CONFIRMATION_SAMPLES;
    const bool pacing_watch =
      raw_pacing_watch && pacing_window_ready && pacing_streak_confirmed;
    const bool pacing_collecting = raw_pacing_watch && !pacing_watch;
    const bool capture_pacing_watch = capture_cpu_copy && pacing_watch;
    const bool capture_pressure = capture_latency_fail || capture_latency_watch || capture_pacing_watch;
    const bool strict_gamepad_isolation =
      stats.input_host_controller_isolation == "strict_bwrap";
    const bool steam_input_known =
      stats.input_steam_input_status != "unknown" &&
      stats.input_steam_input_status != "not_applicable";
    const bool steam_input_conflict =
      strict_gamepad_isolation &&
      (stats.input_steam_profiles_with_xbox_support > 0 ||
       stats.input_steam_forced_app_count > 0);
    const auto doctor_now = std::chrono::system_clock::now();
    const ended_session_t *const failed_start = recent_failed_start(stats, doctor_now);

    std::string primary_issue = health.value("primary_issue", std::string {});
    if (primary_issue == "steady" || primary_issue == "none") primary_issue.clear();
    const bool health_claims_network_jitter = primary_issue == "network_jitter";
    const bool health_claims_unconfirmed_frame_pacing =
      primary_issue == "frame_pacing" && !pacing_watch;
    const bool health_claims_unconfirmed_capture_pressure =
      !capture_reason.empty() && primary_issue == capture_reason &&
      capture_cpu_copy && !capture_pressure;
    const bool suppressed_stale_network_finding =
      health_claims_network_jitter && !network_fail && !network_watch;
    if (health_claims_network_jitter && !network_fail) {
      // Health and client reports can outlive the network sample that created
      // them. A stale label is useful context, but it cannot authorize another
      // bitrate reduction without current debounced network evidence. A live
      // debounced watch can request another check, never a quality change.
      primary_issue = network_watch ? "network_observation" : std::string {};
    }
    if (health_claims_unconfirmed_frame_pacing) {
      primary_issue.clear();
    }
    if (health_claims_unconfirmed_capture_pressure) {
      primary_issue.clear();
    }
    if (steam_input_conflict && !network_fail && !encoder_fail && !capture_latency_fail) {
      // This is a deterministic host-state conflict, not an inference from a
      // missing controller event. Keep critical live stream failures ahead of
      // it, but do not let a generic pacing or capture watch hide why the
      // controller is structurally dead inside the strict sandbox.
      primary_issue = "steam_input_conflict";
    }
    if (failed_start) {
      // Nothing streams, so no live finding is current, and a start that just failed is what the
      // person opening Doctor is looking at. "No active stream" told them only what they knew.
      primary_issue = "stream_failed_to_start";
    }
    if (primary_issue.empty()) {
      if (!stats.streaming) primary_issue = "no_active_stream";
      else if (network_fail) primary_issue = "network_jitter";
      else if (encoder_fail) primary_issue = "encoder_load";
      // A capture path failing its frame-age budget is the red verdict here;
      // letting a mere network watch outrank it would re-serve the
      // lower-bitrate advice this attribution exists to avoid.
      else if (capture_latency_fail) primary_issue = capture_reason;
      else if (network_watch) primary_issue = "network_observation";
      else if (encoder_watch) primary_issue = "encoder_load";
      else if (capture_latency_watch || capture_pacing_watch) primary_issue = capture_reason;
      else if (!capture_known) primary_issue = "capture_missing";
      else if (pacing_watch) primary_issue = "frame_pacing";
      else if (pyrowave_starved) primary_issue = "pyrowave_starved";
      else if (quality_reduced_live && !auto_safe_managing) primary_issue = "quality_reduced_live";
      else if (pyrowave_needs_more) primary_issue = "pyrowave_needs_more_than_allowed";
      else if (control_channel_observation) primary_issue = "control_channel_observation";
      else primary_issue = "none";
    }

    std::string traffic = "green";
    std::string status = "ok";
    std::string severity = "info";
    std::string simple_state = "Streaming ready";
    const auto health_grade = health.value("grade", std::string {});
    const bool honor_health_grade =
      (!health_claims_network_jitter || network_fail) &&
      !health_claims_unconfirmed_frame_pacing &&
      !health_claims_unconfirmed_capture_pressure;
    if (primary_issue == "stream_failed_to_start") {
      traffic = "amber";
      status = "needs_action";
      severity = "warning";
      simple_state = "Needs attention";
    } else if (primary_issue == "no_active_stream" || primary_issue == "capture_missing") {
      traffic = "amber";
      status = "unknown";
      severity = "warning";
      simple_state = "Needs attention";
    } else if ((honor_health_grade && health_grade == "degraded") || network_fail || encoder_fail || capture_latency_fail) {
      traffic = "red";
      status = "needs_action";
      severity = "critical";
      simple_state = "Needs attention";
    } else if ((primary_issue != "none" && primary_issue != "control_channel_observation") ||
               (honor_health_grade && health_grade == "watch") || capture_pressure || pacing_watch) {
      traffic = "amber";
      status = "needs_action";
      severity = "warning";
      simple_state = "Needs attention";
    }

    // One figure in one unit: what the stream runs at and what Doctor would raise it to, both as the
    // request a player sets, FEC and audio included, so the player compares like with like.
    std::string pyrowave_summary;
    if (pyrowave.active) {
      pyrowave_summary = "PyroWave runs at a request of about " + whole_mbps(pyrowave.request_kbps) +
                         ", where Polaris advises a request of about " + whole_mbps(pyrowave.advice.raise_goal_kbps) +
                         " for " + std::to_string(pyrowave.advice.width) + "x" + std::to_string(pyrowave.advice.height) +
                         " at " + std::to_string(pyrowave.advice.fps) + " fps on a device's own screen";
      if (pyrowave.advice.raise_goal_limited_by != "advice") {
        pyrowave_summary += ", " + pyrowave_limit_phrase(pyrowave.advice.raise_goal_limited_by);
      }
      if (pyrowave.ceiling_frame_share) {
        pyrowave_summary += ", and " + std::to_string(static_cast<int>(std::lround(*pyrowave.ceiling_frame_share * 100.0))) +
                            "% of recent frames hit its byte ceiling";
      }
      pyrowave_summary += ".";
    }
    // The limit finding in one sentence: what the model asks, the limit below it, and how full the byte
    // budget runs. The finding needs a known share, so it always has one.
    std::string pyrowave_limit_summary;
    if (pyrowave.active && pyrowave.ceiling_frame_share) {
      pyrowave_limit_summary =
        "PyroWave at " + std::to_string(pyrowave.advice.width) + "x" + std::to_string(pyrowave.advice.height) + " and " +
        std::to_string(pyrowave.advice.fps) + " fps wants a request of about " + whole_mbps(pyrowave.advice.advice_far_kbps) +
        " on this screen, above " + pyrowave_limit_figure(pyrowave_doctor) + ", and " +
        std::to_string(static_cast<int>(std::lround(*pyrowave.ceiling_frame_share * 100.0))) +
        "% of recent frames fill its byte budget.";
    }
    const std::string summary =
      primary_issue == "none" ? "Streaming telemetry looks ready." :
      primary_issue == "stream_failed_to_start" ? failed_start_summary(*failed_start) :
      primary_issue == "no_active_stream" ? "No active stream is running, so Doctor cannot verify the live path yet." :
      primary_issue == "capture_missing" ? "Capture metadata has not arrived yet; start a stream before tuning advanced settings." :
      primary_issue == "network_jitter" ? "Sustained network pressure is affecting this stream." :
      primary_issue == "network_observation" ? "A network warning needs more live evidence before Doctor changes quality." :
      primary_issue == "control_channel_observation" ?
        (loss_judged ?
          "Control-channel retries were observed, but video frame loss stays below network pressure." :
          "Control-channel retries were observed, but no video frame loss is measured.") :
      primary_issue == "quality_reduced_live" ? "The reversible live bitrate target is below the capability-validated launch ceiling and current network evidence is clean." :
      primary_issue == "pyrowave_starved" ? pyrowave_summary + " The network is clean." :
      primary_issue == "pyrowave_needs_more_than_allowed" ? pyrowave_limit_summary :
      primary_issue == "steam_input_conflict" ? "Local Steam Input settings conflict with strict gamepad isolation for the Polaris Xbox virtual controller." :
      primary_issue == "encoder_load" ? "Encoder load is above the low-latency budget." :
      primary_issue == "frame_pacing" ? "Frame pacing telemetry needs attention." :
      health.contains("summary") && !suppressed_stale_network_finding &&
        !health_claims_unconfirmed_frame_pacing ? health.value("summary", std::string {}) :
      capture_path_reason_message(capture_reason);

    nlohmann::json evidence = nlohmann::json::array();
    append_doctor_evidence(evidence, "streaming", "Active stream", stats.streaming, "", stats.streaming ? "pass" : "unknown", "stream_stats", stats.streaming ? "A stream is active." : "No active stream is reporting live telemetry.");
    // How the last stream's start ended, while nothing streams. Right after "no active stream",
    // because a start that failed is usually why nothing is.
    if (!stats.streaming && stats.last_session && !stats.last_session->start_outcome.empty()) {
      const auto &last = *stats.last_session;
      const bool recent = ended_within(last, doctor_now, DOCTOR_FAILED_START_WINDOW);
      std::string row_status = "info";
      std::string detail;
      if (last.start_outcome == stream_start::k_client_left_during_setup) {
        row_status = recent ? "fail" : "info";
        detail = failed_start_summary(last);
      } else if (last.start_outcome == stream_start::k_no_ping) {
        // Written when a socket's wait for its first ping runs out, even after the other socket heard
        // from the client, so it names the wait rather than claiming neither packet came.
        row_status = recent ? "watch" : "info";
        detail = last_stream_subject(last) +
                 " ended waiting for a first packet from the client on its video or audio port. A firewall "
                 "or a UDP path problem between the client and this host usually does that.";
      } else {
        row_status = "pass";
        detail = last_stream_subject(last) + " started: the client's first video or audio packet arrived.";
      }
      append_doctor_evidence(evidence, "last_stream_start", "Last stream start", last.start_outcome, "", row_status,
                             "stream_session", detail);
    }
#ifdef __linux__
    // Which binary produced this report. The Bazzite DRM/KMS recipe runs a copy
    // outside the package, and that copy stays on the old version across
    // updates while the console says nothing; a support thread needs this on
    // its first line.
    if (const auto running = platf::user_unit::running_executable()) {
      const auto binary = platf::user_unit::describe_running_binary(*running, POLARIS_EXECUTABLE_PATH);
      const bool outside_package = binary.matches_package == std::optional<bool> {false};
      std::string detail = std::string {"Polaris "} + PROJECT_VERSION + " is running from " + binary.path + ".";
      if (const auto note = platf::user_unit::running_binary_note(binary); !note.empty()) {
        detail += " " + note;
      }
      append_doctor_evidence(evidence, "running_binary", "Running binary", binary.path, "", outside_package ? "watch" : "pass", "process", detail);
    }
#endif
    append_doctor_evidence(evidence, "capture_path", "Capture path", capture_path, "", !capture_known ? "unknown" : capture_latency_fail ? "fail" : capture_cpu_copy ? "watch" : capture_gpu_native ? "pass" : "watch", "stream_stats", capture_path_reason_message(capture_reason));
    if (stats.streaming && stats.clients.size() == 1) {
      const auto &source = stats.clients.front().capture_source;
      const auto value = capture_source_json(source);
      if (!value.is_null()) {
        const bool source_cpu_copy = source.transport == platf::frame_transport_e::shm ||
                                     source.residency == platf::frame_residency_e::cpu;
        const bool oversized_cpu_source = source_cpu_copy && value["pixel_ratio"].get<double>() >= 2.0;
        const std::string sizes = std::to_string(source.width) + "x" + std::to_string(source.height) +
                                 " capture for a " + std::to_string(source.stream_width) + "x" + std::to_string(source.stream_height) + " stream. ";
        append_doctor_evidence(evidence, "capture_source_size", "Capture source size", value,
          "", "info", "session_capture_frame",
          sizes + (oversized_cpu_source ?
            "The CPU capture path handles at least twice the stream's pixel count before scaling. "
            "This can add copy work, but is not proof of the frame-rate bottleneck. Check the active "
            "output mode and the adapter's supported modes; lowering the client resolution alone may not shrink the source." :
            "These are the delivered source dimensions before encoder scaling; size alone does not establish a performance problem."));
      }
    }
    append_doctor_evidence(evidence, "encoder", "Encoder", stats.encode_target_device, "", encoder_fail ? "fail" : encoder_watch ? "watch" : "pass", "stream_stats", stats.encode_time_ms > 0.0 ? "Encode timing is reported by stream telemetry." : "Encoder timing has not been reported yet.");
    const auto encoder_selection = health.value("encoder_selection", nlohmann::json::object());
    append_doctor_evidence(
      evidence,
      "encoder_selection",
      "Encoder selection",
      encoder_selection.value("selected_encoder", std::string {"unknown"}),
      "",
      encoder_selection.value("selected_encoder", std::string {"unknown"}) == "unknown" ?
        "unknown" :
        encoder_selection.value("fallback_used", false) ? "watch" : "pass",
      "deterministic_launch_policy",
      encoder_selection.value("reason", std::string {"Encoder selection evidence is unavailable."})
    );
    // The one loss figure: frames the client never received whole, after FEC recovery, over the
    // window. Nova quotes this row, the session status carries the same verdict, and Live Tuning
    // acts on it, so none of them can show a different number.
    append_doctor_evidence(
      evidence,
      "packet_loss",
      "Video frame loss",
      loss_judged ? nlohmann::json(verdict.loss_pct) : nlohmann::json(nullptr),
      "%",
      !loss_judged ? "unknown" : confirmed_media_loss ? "fail" : "pass",
      loss_judged ? "media_transport" : "unavailable",
      loss_judged ?
        video_frame_loss_detail(verdict) :
      verdict.loss_stale ?
        "No client media report has reached the host in the last " +
          std::to_string(judged_network_t::k_media_report_max_age_ms / 1000) +
          " seconds, so video frame loss is not judged." :
        "Fewer than " + std::to_string(network_judge_t::k_min_media_samples) +
          " client media reports arrived in the last " + std::to_string(network_judge_t::k_window.count()) +
          " seconds, so video frame loss is not judged yet."
    );
    append_doctor_evidence(
      evidence,
      "control_channel_packet_loss",
      "Control-channel loss estimate",
      verdict.control_loss_available ? nlohmann::json(verdict.control_loss_pct) : nlohmann::json(nullptr),
      "%",
      !verdict.control_loss_available ? "unknown" : control_channel_observation ? "watch" : "pass",
      "enet_control_channel",
      "ENet reliable-channel EWMA, averaged over the last " + std::to_string(network_judge_t::k_window.count()) +
        " seconds; Doctor notes it from " + two_decimals(network_judge_t::k_loss_enter_pct) + "% and stops below " +
        two_decimals(network_judge_t::k_loss_exit_pct) + "%. Retransmissions can make this read high even when video delivery is healthy; it cannot grade the stream or authorize a bitrate reduction."
    );
    append_doctor_evidence(
      evidence,
      "latency",
      "Network latency",
      rtt_judged ? nlohmann::json(verdict.rtt_ms) : nlohmann::json(nullptr),
      "ms",
      !rtt_judged ? "unknown" : rtt_fail ? "fail" : verdict.rtt_elevated ? "watch" : "pass",
      rtt_judged ? "stream_stats" : "unavailable",
      rtt_judged ?
        round_trip_detail(verdict) :
        "No current round trip readings are available for this stream."
    );
    // Which kind of client is streaming, because what Doctor can see and what the player can change
    // both follow from it. A Moonlight-protocol client reads as every other stream does, so without
    // this row its missing media loss and its host-only controls looked like faults of the stream.
    if (stats.streaming) {
      const auto family = streaming_client_family(stats);
      const std::string who = stats.client_name.empty() ? std::string {"This client"} : stats.client_name;
      if (family == "nova") {
        append_doctor_evidence(evidence, "client_family", "Client", family, "", "info", "pairing_record",
                               who + " is Nova, for Android or for Linux; Polaris cannot tell which from the stream.");
      } else if (family == "moonlight") {
        append_doctor_evidence(evidence, "client_family", "Client", family, "", "info", "pairing_record",
                               who + " speaks only the Moonlight protocol, as Moonlight and Artemis do. Polaris gets no "
                               "media loss from it, so Doctor works from round-trip time and host evidence. PyroWave, "
                               "Live Tuning from the client and choosing the launch mode per launch are Nova only, "
                               "though Artemis can ask for Host Virtual Display; Live Tuning on Mission Control still "
                               "tunes this stream.");
      }
    }
    // Both rows below survive the end of a stream, so they say which one they describe.
    const std::string last_stream_prefix = stats.streaming ? "" : "From the last stream. ";
    const std::string last_launch_prefix = stats.streaming ? "" : "From the last launch. ";
    // The path the client came in by. Informational only: a tailnet stream is fine when its path
    // is direct, so this never grades anything and only says what to check. A support bundle's
    // microstutter came down to one client having moved from the LAN to Tailscale, which nothing
    // else in this report could show.
    if (!stats.client_network_path.empty()) {
      const auto &path = stats.client_network_path;
      constexpr auto relay_hint =
        " That is fine when the connection is direct. When Tailscale relays it instead, expect stutter "
        "and extra latency; running tailscale ping with this host's name on the client says which.";
      std::string detail;
      if (path == "lan") {
        detail = "The client is on the same local network as this host.";
      } else if (path == "cgnat") {
        // Written as 100.64/10: a dotted network address here would be relabelled as a client
        // address by the export's address pseudonymizer.
        detail = std::string {"The client reached this host from the shared 100.64/10 range, which usually "
                              "means Tailscale."} + relay_hint;
      } else if (path == "tailscale") {
        detail = std::string {"The client reached this host over Tailscale."} + relay_hint;
      } else if (path == "public") {
        detail = "The client reached this host over the internet, so the path between them is outside this "
                 "network. Latency and packet loss above are the ones to watch.";
      } else if (path == "link-local") {
        detail = "The client reached this host by a link-local address, which usually means a direct cable or "
                 "a network without an address server.";
      } else if (path == "loopback") {
        detail = "The client is running on this host.";
      } else {
        detail = "The client's address could not be classified.";
      }
      append_doctor_evidence(evidence, "client_network_path", "Client network path", path, "", "info", "stream_stats",
                             last_stream_prefix + detail);
    }
    // How the display mode was chosen. A paired client's Display Mode Override replaces whatever the
    // client asks for without telling it, so "I can't select 1080p" looked like a client problem
    // while the host had the answer. Watch only when it actually replaced a different request.
    if (!stats.display_mode_applied.empty()) {
      const auto &requested = stats.display_mode_requested;
      const auto &applied = stats.display_mode_applied;
      const bool pinned = stats.display_mode_pinned_by_host;
      const bool replaced_request = pinned && !requested.empty() && requested != applied;
      std::string detail;
      if (replaced_request) {
        detail = "This client's pairing has a Display Mode Override of " + applied + ", so Polaris used that "
                 "instead of the " + requested + " the client asked for. Clear Display Mode Override for this "
                 "client on the Devices page to let the client choose.";
      } else if (pinned) {
        detail = "This client's pairing sets a Display Mode Override of " + applied +
                 (requested.empty() ? "." : ", which matches what the client asked for.");
      } else {
        detail = "Polaris used the display mode the client asked for.";
      }
      append_doctor_evidence(evidence, "display_mode_decision", "Display mode", applied, "",
                             replaced_request ? "watch" : "info", "launch", last_launch_prefix + detail);
    }
    {
      std::string detail = stats.adaptive_runtime_update_supported ?
        "Current live encoder target; Doctor changes it only for confirmed pressure or a verified same-stream restore." :
        "Applied encoder bitrate; this encoder does not expose live bitrate updates.";
      if (pyrowave.active) {
        // The television figure is the model's readout, uncapped, so it says so where it passes what
        // Polaris recommends on its own.
        detail += " " + pyrowave_summary + " On a television or monitor (H 2.0) PyroWave's model asks for a request of " +
                  whole_mbps(pyrowave.advice.advice_near_kbps) + ", in " + (pyrowave.advice.chroma444 ? "4:4:4" : "4:2:0");
        if (pyrowave.advice.advice_near_kbps > pyrowave.advice.cap_kbps) {
          detail += ", more than the " + whole_mbps(pyrowave.advice.cap_kbps) + " Polaris recommends on its own";
        }
        detail += ". The kbps value is the rate at the encoder, and every Mbps figure here is a request.";
        if (pyrowave.floor_request_kbps > 0) {
          detail += " Live Tuning cuts it no lower than a request of " + whole_mbps(pyrowave.floor_request_kbps) + ".";
        }
      }
      const bool bitrate_short = quality_reduced_live ||
        (pyrowave.active && (pyrowave.starved || pyrowave.needs_more_than_allowed));
      append_doctor_evidence(
        evidence, "bitrate", "Live bitrate", live_bitrate_kbps, "kbps",
        !stats.streaming ? "unknown" : network_fail ? "fail" : bitrate_short ? "watch" : "pass",
        "stream_stats", detail
      );
    }
    const bool has_oversized_fec_frames =
      stats.fec_protection.oversized_frames_total > 0;
    const std::string fec_protection_detail = has_oversized_fec_frames ?
      "The sender transmitted oversized encoded frames without FEC parity because their packetized payload exceeded the four-block protection envelope. The largest encoded frame was " +
        std::to_string(stats.fec_protection.largest_encoded_frame_bytes) +
        " bytes and required " + std::to_string(stats.fec_protection.max_required_blocks) +
        " blocks; the protected packetized limit was " +
        std::to_string(stats.fec_protection.protected_payload_limit_bytes) +
        " bytes. This is frame-size evidence, not proof of media packet loss. Lower bitrate only if client media-loss or pacing telemetry also shows impact." :
      "No encoded frame has exceeded the active video FEC protection envelope.";
    append_doctor_evidence(
      evidence,
      "fec_protection",
      "Frames outside FEC protection",
      stats.fec_protection.oversized_frames_total,
      "frames",
      has_oversized_fec_frames ? "info" : "pass",
      "sender_packetizer",
      fec_protection_detail
    );
    append_doctor_evidence(
      evidence,
      "live_bitrate_owner",
      "Live bitrate owner",
      auto_safe_managing ? nlohmann::json("auto_safe") :
        live_bitrate_tunable ? nlohmann::json("doctor_available") : nlohmann::json("fixed_session"),
      "",
      auto_safe_managing || live_bitrate_tunable ? "pass" : "watch",
      "deterministic_controller",
      auto_safe_managing ?
        "Live Tuning owns continuous live bitrate adjustment. Doctor measures it and does not offer a competing mutation." :
      live_bitrate_tunable ?
        "Live Tuning is not managing this target. Evidence-supported Doctor Auto Fix may own one reversible, verified bitrate step." :
        "The current stream has no safe, exclusive live bitrate actuator. Doctor remains observational."
    );
    append_doctor_evidence(
      evidence,
      "live_bitrate_control",
      "Live bitrate control",
      stats.adaptive_runtime_update_supported,
      "",
      !stats.streaming ? "unknown" : stats.adaptive_runtime_update_supported ? "pass" : "watch",
      "encoder_capability",
      !stats.streaming ? "Start a stream before checking encoder bitrate controls." :
      stats.adaptive_runtime_update_supported ? "The active encoder accepts runtime bitrate updates." :
                                                "The active encoder does not support runtime bitrate updates; bitrate changes require a new stream."
    );
    append_doctor_evidence(
      evidence,
      "effective_quality_ceiling",
      "Effective quality ceiling",
      effective_quality_target_kbps > 0 ? nlohmann::json(effective_quality_target_kbps) : nlohmann::json(nullptr),
      "kbps",
      quality_reduced_live ? "watch" : effective_quality_target_kbps > 0 ? "pass" : "unknown",
      "launch_policy",
      quality_reduced_live ?
        "The reversible live bitrate target is below the capability-validated launch ceiling." :
        "The launch ceiling is the paired preference after host capability validation, or the bitrate the stream opened at when no paired preference is saved."
    );
    append_doctor_evidence(
      evidence,
      "target_fps_gap",
      "Target FPS gap",
      target_fps_gap,
      "FPS",
      observed_target_shortfall && pacing_watch ? "watch" :
        meaningful_fps_shortfall && (!source_cadence_available || !pacing_window_ready || !pacing_streak_confirmed) ? "unknown" :
        "pass",
      "stream_stats",
      observed_target_shortfall && pacing_watch ?
        "Encoded FPS is below the requested cadence while measured source cadence confirms moving content." :
      observed_target_shortfall && !pacing_window_ready ?
        "Encoded FPS is below the requested cadence, but Doctor is still collecting its startup pacing window." :
      observed_target_shortfall && !pacing_streak_confirmed ?
        "Encoded FPS is below the requested cadence, but the shortfall has not persisted for two consecutive complete windows." :
      meaningful_fps_shortfall && !source_cadence_available ?
        "Encoded FPS is below the requested cadence, but source cadence is unavailable; static content is not a pacing fault." :
        "No source-confirmed target cadence shortfall is present."
    );
    append_doctor_evidence(
      evidence,
      "frame_pacing",
      "Mean target interval error",
      stats.frame_jitter_ms,
      "ms",
      pacing_watch ? "watch" :
        (!pacing_window_ready || pacing_collecting) ? "unknown" : "pass",
      "stream_stats",
      pacing_watch ?
        "The pacing threshold remained exceeded across a complete startup window and consecutive observations." :
      !pacing_window_ready ?
        "Doctor is collecting six complete video telemetry windows before grading startup pacing." :
      pacing_collecting ?
        "The threshold was crossed once; Doctor requires a second consecutive complete window before warning." :
        "Mean absolute distance between actual source-frame intervals and the requested interval; this is not statistical network jitter."
    );
    append_doctor_evidence(
      evidence,
      "steam_input_compatibility",
      "Steam Input compatibility",
      stats.input_steam_input_status,
      "",
      steam_input_conflict ? "fail" : strict_gamepad_isolation && steam_input_known ? "pass" : "unknown",
      "local_steam_config",
      stats.input_steam_input_detail.empty() ?
        (strict_gamepad_isolation ? "Steam Input compatibility has not been inspected yet." : "Strict gamepad isolation is not active.") :
        stats.input_steam_input_detail
    );

    const auto hdr_reason = hdr_downgrade_reason(stats);
    append_doctor_evidence(
      evidence,
      "hdr",
      "HDR",
      hdr_effective_mode(stats),
      "",
      stats.stream_hdr_enabled ? "pass" : stats.dynamic_range > 0 ? "watch" : "info",
      "capture_display",
      hdr_reason == "none" ?
        (stats.stream_hdr_enabled ?
           "Streaming HDR10." :
           "No client has asked for HDR on this host.") :
        hdr_downgrade_message(stats)
    );

    auto advanced = nlohmann::json::object();
    advanced["stream_stats_keys"] = nlohmann::json::array({"capture_path", "capture_path_reason", "capture_transport", "capture_residency", "capture_format", "capture_source", "capture_cpu_copy", "capture_gpu_native", "capture_cross_gpu_dmabuf_risk", "encode_target_device", "encode_target_residency", "fps", "encode_time_ms", "packet_loss", "packet_loss_available", "packet_loss_source", "control_channel_packet_loss", "control_channel_samples", "frame_interval_error_ms", "frame_jitter_ms", "video_policy_sample_count", "pacing_warning_streak", "fec_protection"});
    advanced["linux_gpu_profile"] = linux_gpu_profile_json(stats);
    advanced["gpu_native_probe"] = gpu_native_probe_json(stats);
    advanced["fec_protection"] = fec_protection_json(stats.fec_protection);
    advanced["controller_input"] = {
      {"host_controller_isolation", stats.input_host_controller_isolation},
      {"steam_input_status", stats.input_steam_input_status},
      {"steam_profiles_checked", stats.input_steam_profiles_checked},
      {"steam_profiles_with_xbox_support", stats.input_steam_profiles_with_xbox_support},
      {"steam_forced_app_count", stats.input_steam_forced_app_count},
      {"steam_input_detail", stats.input_steam_input_detail}
    };
    advanced["health"] = health;
    advanced["encoder_selection"] = encoder_selection;
    advanced["recent_issue_codes"] = nlohmann::json::array();
    if (steam_input_conflict) {
      advanced["recent_issue_codes"].push_back("steam_input_conflict");
    }
    advanced["network_verdict"] = network_verdict_json(verdict);
    advanced["pacing_coverage"] = {
      {"sample_count", stats.video_policy_sample_count},
      {"required_sample_count", DOCTOR_PACING_WARMUP_SAMPLES},
      {"warning_streak", stats.pacing_warning_streak},
      {"required_warning_streak", DOCTOR_PACING_CONFIRMATION_SAMPLES},
      {"ready", pacing_window_ready},
      {"confirmed", pacing_watch}
    };
    advanced["raw_fields_redacted"] = true;

    double confidence_score = 0.35;
    std::string confidence_level = "low";
    std::string basis = "insufficient_data";
    if (primary_issue == "steam_input_conflict") {
      confidence_score = 0.98;
      confidence_level = "high";
      basis = "local_steam_config_and_isolation_plan";
    } else if (primary_issue == "stream_failed_to_start") {
      confidence_score = 0.9;
      confidence_level = "high";
      basis = "host_session_record";
    } else if (primary_issue == "quality_reduced_live") {
      confidence_score = 0.96;
      confidence_level = "high";
      basis = "session_policy_and_live_telemetry";
    } else if (suppressed_stale_network_finding) {
      confidence_score = 0.84;
      confidence_level = "medium";
      basis = "live_evidence_overrode_stale_finding";
    } else if (primary_issue == "network_observation") {
      confidence_score = 0.68;
      confidence_level = "medium";
      basis = "live_evidence_recheck";
    } else if (primary_issue == "control_channel_observation") {
      confidence_score = 0.72;
      confidence_level = "medium";
      basis = "control_channel_only";
    } else if (pacing_collecting && primary_issue == "none") {
      confidence_score = 0.45;
      confidence_level = "low";
      basis = "pacing_window_collecting";
    } else if (health_claims_unconfirmed_frame_pacing && primary_issue == "none") {
      confidence_score = 0.84;
      confidence_level = "medium";
      basis = "live_evidence_overrode_unconfirmed_pacing";
    } else if (health.contains("primary_issue")) {
      confidence_score = 0.92;
      confidence_level = "high";
      basis = "direct_telemetry";
    } else if (stats.streaming && capture_known) {
      confidence_score = 0.78;
      confidence_level = "medium";
      basis = "direct_telemetry";
    } else if (!stats.streaming) {
      confidence_score = 0.25;
      confidence_level = "unknown";
      basis = "insufficient_data";
    }

    nlohmann::json doctor;
    doctor["version"] = 2;
    doctor["result_id"] = recovery_evidence_result_id(
      "doctor-v2-" + status + "-" + primary_issue + "-" + capture_reason,
      stats,
      health,
      app_uuid,
      target_fps,
      live_bitrate_kbps
    );
    doctor["scope"] = "stream";
    doctor["status"] = status;
    doctor["traffic_light"] = traffic;
    doctor["status_color"] = traffic;
    doctor["severity"] = severity;
    doctor["simple_state"] = simple_state;
    doctor["primary_issue"] = primary_issue;
    doctor["confidence"] = {
      {"level", confidence_level},
      {"score", confidence_score},
      {"basis", basis},
      {"sample_window", {
        {"samples", stats.control_channel_samples},
        {"seconds", network_evidence_available ? network_judge_t::k_window.count() : 0},
        {"video_samples", stats.video_policy_sample_count},
        {"pacing_warning_streak", stats.pacing_warning_streak}
      }}
    };
    doctor["summary"] = summary;
    doctor["recommendation"] = failed_start ?
      failed_start_recommendation(*failed_start, summary) :
      doctor_recommendation(
        primary_issue, summary, health, live_bitrate_tunable, single_session_scope,
        auto_safe_managing, pyrowave_doctor, network
      );
    doctor["evidence"] = std::move(evidence);
    doctor["advanced_evidence"] = std::move(advanced);
    doctor["safe_recovery_action"] = doctor_safe_action(
      primary_issue,
      health,
      live_bitrate_kbps,
      launch_quality_goal,
      live_bitrate_tunable,
      single_session_scope,
      auto_safe_managing,
      pyrowave_doctor,
      doctor["result_id"].get<std::string>(),
      app_uuid,
      failed_start ? stream_start::failed_start_next_step(failed_start->codec) : std::string {}
    );
    doctor["suppressed_findings"] = nlohmann::json::array();
    if (suppressed_stale_network_finding) {
      doctor["suppressed_findings"].push_back({
        {"id", "stale_network_jitter"},
        {"reason", "Confirmed media loss is unavailable and current RTT evidence is below the action threshold; the older health label cannot trigger a bitrate change."}
      });
    }
    if (pacing_collecting) {
      doctor["suppressed_findings"].push_back({
        {"id", "pacing_window_collecting"},
        {"reason", !pacing_window_ready ?
          "Doctor is collecting six complete video telemetry windows before grading startup pacing." :
          "Pacing evidence has not crossed the warning threshold for two consecutive complete windows."}
      });
    } else if (health_claims_unconfirmed_frame_pacing) {
      doctor["suppressed_findings"].push_back({
        {"id", "unconfirmed_frame_pacing"},
        {"reason", "The older health label is not supported by a current, fully confirmed pacing window."}
      });
    }
    // Actions that reported success and did not land. These are not stream
    // health, so they do not move status or traffic light, but they belong in
    // the same payload: they are the evidence a user cannot see any other way,
    // and the export path already carries this object.
    doctor["silent_failures"] = verified_action::to_json();
    doctor["redaction"] = {{"policy", "polaris-diagnostics-redaction-v1"}, {"applied", true}, {"redacted_fields", nlohmann::json::array()}, {"notice", "Tokens, cookies, credentials, auth headers, client IPs, and sensitive config fields are redacted before export or AI explanation."}};
    doctor["ai_explanation"] = {{"enabled", false}, {"provider", "none"}, {"model", ""}, {"generated_at", nullptr}, {"input_redacted", true}, {"source_result_id", doctor["result_id"]}, {"summary", ""}, {"limits", nlohmann::json::array({"AI can explain only; deterministic Doctor owns status, evidence, and actions."})}, {"error", ""}};
    return doctor;
  }

  void update_stream_active(bool active, const std::string &client_name, const std::string &client_ip) {
    std::lock_guard<std::mutex> ingest_lock(client_media_ingest_mutex);
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.streaming = active;
    if (active) {
      current_stats.client_name = client_name;
      current_stats.client_ip = client_ip;
      current_stats.client_network_path = std::string {net::describe_client_network_path(client_ip)};
    } else {
      // Reset all stats when stream ends, but carry the controller-input facts
      // across: they describe the host, not the stream that just ended, and the
      // Steam Input conflict is only safe to fix while nothing is streaming.
      // Wiping them here made the finding vanish at exactly the moment its own
      // one-click fix became applicable.
      const auto isolation = current_stats.input_host_controller_isolation;
      const auto isolation_detail = current_stats.input_host_controller_isolation_detail;
      const auto steam_status = current_stats.input_steam_input_status;
      const auto steam_profiles = current_stats.input_steam_profiles_checked;
      const auto steam_opt_in = current_stats.input_steam_profiles_with_xbox_support;
      const auto steam_forced = current_stats.input_steam_forced_app_count;
      const auto steam_detail = current_stats.input_steam_input_detail;
      // The HDR facts describe the host's capture path, not the stream that just ended, and the
      // person who wants to know why HDR did not engage goes looking for the answer after
      // disconnecting, not during. Same reasoning as the controller-input fields above.
      const auto hdr_dynamic_range = current_stats.dynamic_range;
      const auto hdr_display = current_stats.display_hdr;
      const auto hdr_metadata = current_stats.hdr_metadata_available;
      const auto hdr_stream_enabled = current_stats.stream_hdr_enabled;
      const auto hdr_effective_headless = current_stats.runtime_effective_headless;
      const auto hdr_policy_reason = current_stats.hdr_policy_reason;
      const auto hdr_policy_hdr = current_stats.hdr_policy_hdr;
      const auto hdr_policy_device = current_stats.hdr_policy_device;
      // How the client connected and how its display mode was chosen. A support bundle came in
      // with "can't select 1080p" and "microstutters", exported after the stream had ended, and
      // both answers (a Display Mode Override pinning 4K, a client on Tailscale) were only in raw
      // log text. The path is kept as its kind, never as the address.
      const auto network_path = current_stats.client_network_path;
      const auto mode_requested = current_stats.display_mode_requested;
      const auto mode_applied = current_stats.display_mode_applied;
      const auto mode_pinned = current_stats.display_mode_pinned_by_host;

      current_stats = stats_t {};
      clear_capture_profile_buckets();
      reset_hot_fields();

      current_stats.input_host_controller_isolation = isolation;
      current_stats.input_host_controller_isolation_detail = isolation_detail;
      current_stats.input_steam_input_status = steam_status;
      current_stats.input_steam_profiles_checked = steam_profiles;
      current_stats.input_steam_profiles_with_xbox_support = steam_opt_in;
      current_stats.input_steam_forced_app_count = steam_forced;
      current_stats.input_steam_input_detail = steam_detail;

      current_stats.dynamic_range = hdr_dynamic_range;
      current_stats.display_hdr = hdr_display;
      current_stats.hdr_metadata_available = hdr_metadata;
      current_stats.stream_hdr_enabled = hdr_stream_enabled;
      current_stats.runtime_effective_headless = hdr_effective_headless;
      current_stats.hdr_policy_reason = hdr_policy_reason;
      current_stats.hdr_policy_hdr = hdr_policy_hdr;
      current_stats.hdr_policy_device = hdr_policy_device;

      current_stats.client_network_path = network_path;
      current_stats.display_mode_requested = mode_requested;
      current_stats.display_mode_applied = mode_applied;
      current_stats.display_mode_pinned_by_host = mode_pinned;
    }
  }

  void record_display_mode_decision(const std::string &requested, const std::string &applied, bool pinned_by_host) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.display_mode_requested = requested;
    current_stats.display_mode_applied = applied;
    current_stats.display_mode_pinned_by_host = pinned_by_host;
  }

  std::string client_family_for_stream(std::string_view pairing_family) {
    return pairing_family == "nova" ? "nova" : "moonlight";
  }

  void add_client(const std::string &client_ip,
                  const std::string &client_name,
                  std::uint64_t session_generation,
                  const std::string &client_family) {
    // Every call is a real attach from rtsp_stream::start() - bump the
    // population revision unconditionally, matching measurement-spec-v1.md
    // 6.1's "increments on every client attach or detach" (the dedup guard
    // just below is only about not duplicating the clients_t bookkeeping
    // entry, not about whether this call represents a real event).
    client_population_revision_counter.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(stats_mutex);

    // Real RTSP sessions carry a process-unique generation, so an old and a
    // replacement session from the same device/IP remain distinct until the
    // old join completes. Legacy/test callers that omit it retain the prior
    // IP-keyed behavior.
    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip, session_generation](const client_stats_t &c) {
        return session_generation > 0 ?
          c.session_generation == session_generation :
          c.session_generation == 0 && c.ip == client_ip;
      });

    if (it == current_stats.clients.end()) {
      client_stats_t client;
      client.name = client_name;
      client.ip = client_ip;
      client.client_family = client_family;
      client.session_generation = session_generation;
      client.started_at = std::chrono::system_clock::now();
      current_stats.clients.push_back(std::move(client));
    }

    // Always mark streaming as active and update primary client info
    current_stats.streaming = true;
    if (current_stats.clients.size() == 1) {
      current_stats.client_name = client_name;
      current_stats.client_ip = client_ip;
      current_stats.fec_protection = current_stats.clients.front().fec_protection;
    }
  }

  void remove_client(const std::string &client_ip,
                     std::uint64_t session_generation) {
    client_population_revision_counter.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(stats_mutex);

    // Freeze the ending session before its entry goes, from what its own generation stored. Only a
    // removal that finds its live generation writes it, so removing the same session twice, one that
    // was never added, or a legacy entry with no generation leaves the last session as it was.
    if (session_generation > 0) {
      const auto ending = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
        [session_generation](const client_stats_t &c) {
          return c.session_generation == session_generation;
        });
      if (ending != current_stats.clients.end()) {
        last_ended_session = ended_session_t {
          .session_generation = ending->session_generation,
          .client_name = ending->name,
          .started_at = ending->started_at,
          .ended_at = std::chrono::system_clock::now(),
          .capture_backend = ending->capture_backend,
          .capture_frames = frames_of_published_display(*ending),
          .codec = ending->codec,
          .encoder_backend = ending->encoder_backend,
          .pyrowave_route = ending->pyrowave_route,
          .start_outcome = ending->start_outcome,
          .start_client_left_after_ms = ending->start_client_left_after_ms,
        };
      }
    }

    current_stats.clients.erase(
      std::remove_if(current_stats.clients.begin(), current_stats.clients.end(),
        [&client_ip, session_generation](const client_stats_t &c) {
          return session_generation > 0 ?
            c.session_generation == session_generation :
            c.session_generation == 0 && c.ip == client_ip;
        }),
      current_stats.clients.end());

    if (current_stats.clients.empty()) {
      current_stats.streaming = false;
      current_stats.client_name.clear();
      current_stats.client_ip.clear();
      current_stats.fec_protection = {};
      current_stats.stream_chroma.clear();
      current_stats.bitrate_request = {};
      current_stats.bitrate_request_recorded = false;
      current_stats.pyrowave_window_frames = 0;
      current_stats.pyrowave_window_ceiling_frames = 0;
    } else {
      // Update primary client info to first remaining client
      const auto &primary = current_stats.clients.front();
      current_stats.client_name = primary.name;
      current_stats.client_ip = primary.ip;
      current_stats.fec_protection = primary.fec_protection;
      current_stats.stream_chroma = primary.stream_chroma;
      current_stats.bitrate_request = primary.bitrate_request;
      current_stats.bitrate_request_recorded = primary.bitrate_request_recorded;
      current_stats.pyrowave_window_frames = primary.pyrowave_window_frames;
      current_stats.pyrowave_window_ceiling_frames = primary.pyrowave_window_ceiling_frames;
    }
  }

  void update_video_stats(double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height, std::string_view encoder_backend, std::uint64_t session_generation) {
    hot_bitrate_kbps.store(bitrate_kbps, std::memory_order_relaxed);
    hot_codec_id.store(codec_to_id(codec), std::memory_order_relaxed);
    hot_width.store(width, std::memory_order_relaxed);
    hot_height.store(height, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
      doctor_video_policy_state.delivered_fps = fps;
      doctor_video_policy_state.encode_time_ms = encode_time_ms;
      publish_doctor_video_policy_locked();
      hot_fps.store(fps, std::memory_order_relaxed);
      hot_encode_time_ms.store(encode_time_ms, std::memory_order_relaxed);
      hot_video_sample_revision.fetch_add(1, std::memory_order_release);
    }

    // Multi-client mirror: bounded by active client count (typically 1),
    // and the only reason this call still needs stats_mutex at all.
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.encoder_backend = encoder_backend;
    // Every session's encode loop writes here, so each writes its own entry. Writing the first
    // client put one session's codec and encoder on another's entry, while the other entries kept
    // what they started with. Generation zero names no session: Browser Stream's encode loop
    // passes it, and nothing keeps a Browser Stream from running beside a Moonlight or Nova
    // session. It writes only an entry registered with no generation either, the way add_client()
    // and remove_client() match one, so it never lands on a session's entry or its last_session.
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client != current_stats.clients.end()) {
      auto &c = *client;
      c.fps = fps;
      c.bitrate_kbps = bitrate_kbps;
      c.encode_time_ms = encode_time_ms;
      c.codec = codec;
      c.encoder_backend = encoder_backend;
      c.width = width;
      c.height = height;
    }
  }

  void update_video_stats(const std::string &client_ip, double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height, std::string_view encoder_backend, std::uint64_t session_generation) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    // Overlapping reconnects share an address, so a session with a generation finds its own entry
    // by it. The address alone found the older session's. A caller with no generation finds only
    // an entry registered with none at that address, as add_client() does, never a session's.
    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip, session_generation](const client_stats_t &c) {
        return session_generation > 0 ?
          c.session_generation == session_generation :
          c.session_generation == 0 && c.ip == client_ip;
      });

    if (it != current_stats.clients.end()) {
      it->fps = fps;
      it->bitrate_kbps = bitrate_kbps;
      it->encode_time_ms = encode_time_ms;
      it->codec = codec;
      it->encoder_backend = encoder_backend;
      it->width = width;
      it->height = height;
    }

    // Also update top-level stats (use first client for backward compat)
    if (!current_stats.clients.empty() && current_stats.clients.front().ip == client_ip) {
      current_stats.encoder_backend = encoder_backend;
      hot_bitrate_kbps.store(bitrate_kbps, std::memory_order_relaxed);
      hot_codec_id.store(codec_to_id(codec), std::memory_order_relaxed);
      hot_width.store(width, std::memory_order_relaxed);
      hot_height.store(height, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
        doctor_video_policy_state.delivered_fps = fps;
        doctor_video_policy_state.encode_time_ms = encode_time_ms;
        publish_doctor_video_policy_locked();
        hot_fps.store(fps, std::memory_order_relaxed);
        hot_encode_time_ms.store(encode_time_ms, std::memory_order_relaxed);
        hot_video_sample_revision.fetch_add(1, std::memory_order_release);
      }
    }
  }

  void update_session_targets(double requested_client_fps,
                              double session_target_fps,
                              double encode_target_fps,
                              const std::string &pacing_policy,
                              const std::string &optimization_source,
                              const std::string &optimization_confidence,
                              const std::string &optimization_cache_status,
                              const std::string &optimization_reasoning,
                              const std::string &optimization_normalization_reason,
                              int recommendation_version,
                              int paired_target_bitrate_kbps,
                              int effective_launch_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.requested_client_fps = requested_client_fps;
    current_stats.session_target_fps = session_target_fps;
    current_stats.encode_target_fps = encode_target_fps;
    current_stats.pacing_policy = pacing_policy;
    current_stats.optimization_source = optimization_source;
    current_stats.optimization_confidence = optimization_confidence;
    current_stats.optimization_cache_status = optimization_cache_status;
    current_stats.optimization_reasoning = optimization_reasoning;
    current_stats.optimization_normalization_reason = optimization_normalization_reason;
    current_stats.recommendation_version = recommendation_version;
    current_stats.paired_target_bitrate_kbps = paired_target_bitrate_kbps;
    current_stats.effective_launch_bitrate_kbps = effective_launch_bitrate_kbps;
  }

  void update_frame_delivery(double duplicate_frame_ratio,
                             double dropped_frame_ratio,
                             double avg_frame_age_ms,
                             double frame_jitter_ms) {
    std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
    doctor_video_policy_state.duplicate_frame_ratio = duplicate_frame_ratio;
    doctor_video_policy_state.dropped_frame_ratio = dropped_frame_ratio;
    doctor_video_policy_state.avg_frame_age_ms = avg_frame_age_ms;
    doctor_video_policy_state.frame_jitter_ms = frame_jitter_ms;
    publish_doctor_video_policy_locked();
    hot_duplicate_frame_ratio.store(duplicate_frame_ratio, std::memory_order_relaxed);
    hot_dropped_frame_ratio.store(dropped_frame_ratio, std::memory_order_relaxed);
    hot_avg_frame_age_ms.store(avg_frame_age_ms, std::memory_order_relaxed);
    hot_frame_jitter_ms.store(frame_jitter_ms, std::memory_order_relaxed);
    hot_video_sample_revision.fetch_add(1, std::memory_order_release);
  }

  bool record_capture_source(std::uint64_t session_generation, const capture_source_t &source) {
    if (session_generation == 0 || source.width <= 0 || source.height <= 0 ||
        source.stream_width <= 0 || source.stream_height <= 0) return false;
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    client->capture_source = source;
    client->capture_frame_since_publication = true;
    return true;
  }

  bool record_capture_backend(std::uint64_t session_generation, const capture_backend_t &backend) {
    if (session_generation == 0 || backend.opened.empty()) return false;
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    client->capture_backend = backend;
    // A publication names the display a session now encodes from. Frames an earlier display
    // delivered say nothing about this one, and neither does the PyroWave route an earlier
    // display's encoder took: the encoder for this one has encoded nothing yet. Its route reads
    // unknown until its own first frame, so neither the reason nor the last session gives a
    // display that delivered no frame the route of the one before it.
    client->capture_frame_since_publication = false;
    client->pyrowave_route.clear();
    return true;
  }

  bool record_pyrowave_route(std::uint64_t session_generation, std::string_view route) {
    if (session_generation == 0 || route.empty()) return false;
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    client->pyrowave_route = route;
    return true;
  }

  void record_app_stop(std::string_view path, int windows_asked, std::chrono::milliseconds waited) {
    app_stop_t stop;
    stop.path = path;
    stop.windows_asked = windows_asked;
    stop.waited = waited;
    record_app_stop(stop);
  }

  void record_app_stop(const app_stop_t &stop) {
    if (stop.path.empty()) return;
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (!last_ended_session) return;
    last_ended_session->app_stop = stop;
  }

  void record_app_stop_check(bool capture_complete, int unattributed, bool live_at_compositor_stop) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (!last_ended_session || last_ended_session->app_stop.path.empty()) return;
    auto &stop = last_ended_session->app_stop;
    stop.capture = capture_complete ? "complete" : "incomplete";
    stop.unattributed = unattributed;
    if (live_at_compositor_stop) {
      stop.path = "compositor_stop";
    }
  }

  bool record_start_outcome(std::uint64_t session_generation, std::string_view outcome,
                            std::int64_t client_left_after_ms) {
    if (session_generation == 0 || outcome.empty()) return false;
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    client->start_outcome = outcome;
    client->start_client_left_after_ms = client_left_after_ms >= 0 ? client_left_after_ms : -1;
    return true;
  }

  bool record_stream_request(std::uint64_t session_generation, bool yuv444, const stream_bitrate::request_t &request) {
    if (session_generation == 0) return false;
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    client->stream_chroma = yuv444 ? "444" : "420";
    client->bitrate_request = request;
    client->bitrate_request_recorded = true;
    client->pyrowave_ceiling_batches.clear();
    client->pyrowave_window_frames = 0;
    client->pyrowave_window_ceiling_frames = 0;
    current_stats.stream_chroma = client->stream_chroma;
    current_stats.bitrate_request = request;
    current_stats.bitrate_request_recorded = true;
    current_stats.pyrowave_window_frames = 0;
    current_stats.pyrowave_window_ceiling_frames = 0;
    return true;
  }

  bool record_pyrowave_frames(std::uint64_t session_generation, std::uint32_t frames, std::uint32_t ceiling_frames) {
    if (session_generation == 0 || frames == 0) return false;
    ceiling_frames = std::min(ceiling_frames, frames);
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end()) return false;
    auto &batches = client->pyrowave_ceiling_batches;
    batches.emplace_back(frames, ceiling_frames);
    client->pyrowave_window_frames += frames;
    client->pyrowave_window_ceiling_frames += ceiling_frames;
    // Drop the oldest batch while the rest still covers the window, so the share always speaks for
    // at least the last k_pyrowave_ceiling_window_frames frames once there are that many.
    while (batches.size() > 1 &&
           client->pyrowave_window_frames - batches.front().first >= k_pyrowave_ceiling_window_frames) {
      client->pyrowave_window_frames -= batches.front().first;
      client->pyrowave_window_ceiling_frames -= batches.front().second;
      batches.erase(batches.begin());
    }
    current_stats.pyrowave_window_frames = client->pyrowave_window_frames;
    current_stats.pyrowave_window_ceiling_frames = client->pyrowave_window_ceiling_frames;
    return true;
  }

  std::optional<double> pyrowave_ceiling_frame_share(const stats_t &stats) {
    if (stats.pyrowave_window_frames < k_pyrowave_ceiling_min_frames) return std::nullopt;
    return static_cast<double>(stats.pyrowave_window_ceiling_frames) /
           static_cast<double>(stats.pyrowave_window_frames);
  }

  pyrowave_bitrate_t evaluate_pyrowave_bitrate(const stats_t &stats, int set_encoder_kbps) {
    pyrowave_bitrate_t result;
    if (!stats.streaming || stats.codec != "pyrowave") return result;
    const double fps = stats.encode_target_fps > 0.0 ? stats.encode_target_fps : stats.session_target_fps;
    // The FEC share and audio the stream's request was split for, as its bitrate_units say.
    const auto link = pyrowave_advice::stream_link(
      stats.bitrate_request_recorded ? &stats.bitrate_request : nullptr, config::stream.fec_percentage
    );
    result.link = link;
    result.advice = pyrowave_advice::advise(
      stats.width, stats.height, static_cast<int>(std::lround(fps)), stats.stream_chroma == "444", link,
      config::video.max_bitrate
    );
    if (!result.advice.valid) return result;
    result.active = true;
    const int live_encoder_kbps = stats.adaptive_runtime_update_supported && stats.adaptive_target_bitrate_kbps > 0 ?
      stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    result.encoder_kbps = set_encoder_kbps > 0 ? set_encoder_kbps : live_encoder_kbps;
    result.request_kbps = pyrowave_advice::request_for_encoder(result.encoder_kbps, link);
    result.ceiling_frame_share = pyrowave_ceiling_frame_share(stats);
    // Starved against the one figure Doctor quotes, as a request, with a tenth to spare. The ceiling
    // share never makes a stream starved: a full budget says the codec would use more bits, not that
    // the stream is short of the calibrated far figure. It decides only whether a stream the cap or
    // max_bitrate holds below that figure wants more than Doctor raises it to.
    result.starved = pyrowave_advice::starved(result.advice, result.request_kbps);
    result.needs_more_than_allowed =
      pyrowave_advice::needs_more_than_allowed(result.advice, result.request_kbps, result.ceiling_frame_share);
    if (stats.adaptive_floor_source == "pyrowave_advice" && stats.adaptive_min_bitrate_kbps > 0) {
      result.floor_encoder_kbps = stats.adaptive_min_bitrate_kbps;
      result.floor_request_kbps = pyrowave_advice::request_for_encoder(result.floor_encoder_kbps, link);
      result.at_floor = live_encoder_kbps > 0 && live_encoder_kbps <= result.floor_encoder_kbps;
    }
    return result;
  }

  nlohmann::json pyrowave_bitrate_json(const stats_t &stats) {
    const auto pyrowave = evaluate_pyrowave_bitrate(stats);
    if (!pyrowave.active) return nullptr;
    auto value = pyrowave_advice::advice_json(pyrowave.advice);
    // What the requests were grossed up for, named the way the pre-launch advice names it. A reader
    // cannot take it from fec_protection, which records a percentage only once a frame outgrows FEC
    // and reads 0 on a healthy stream.
    value["assumes"] = {{"fec_percentage", pyrowave.link.fec_percentage}, {"audio_kbps", pyrowave.link.audio_kbps}};
    value["encoder_kbps"] = pyrowave.encoder_kbps;
    // The same rate as a request, the figure starved compares with raise_goal_kbps, so a reader compares
    // like with like instead of an encoder rate with a request.
    value["request_kbps"] = pyrowave.request_kbps;
    value["ceiling_frame_share"] = pyrowave.ceiling_frame_share ?
      nlohmann::json(std::round(*pyrowave.ceiling_frame_share * 1000.0) / 1000.0) : nlohmann::json(nullptr);
    value["starved"] = pyrowave.starved;
    value["live_tuning_floor_encoder_kbps"] = pyrowave.floor_encoder_kbps > 0 ?
      nlohmann::json(pyrowave.floor_encoder_kbps) : nlohmann::json(nullptr);
    const auto &request = stats.bitrate_request;
    value["request_cap"] = request.cap_kbps > 0 ?
      nlohmann::json {{"kbps", request.cap_kbps}, {"source", request.cap_source}} : nlohmann::json(nullptr);
    value["cap_set_aside"] = request.set_aside_kbps > 0 ?
      nlohmann::json {{"kbps", request.set_aside_kbps}, {"source", request.set_aside_source}} :
      nlohmann::json(nullptr);
    return value;
  }

  nlohmann::json bitrate_units_json(const stats_t &stats, std::uint64_t requester_generation) {
    if (!stats.streaming) {
      return nullptr;
    }
    // A client with a stream here is answered about that stream alone. While the stream is not in stats,
    // or has not recorded its handshake, it gets nothing: another stream's figures would read as its
    // own. That covers a stream in its handshake, a reconnect overlapping the stream it replaced, and a
    // stream torn down a moment before its session timing stops. A client with no stream here is
    // answered about the first stream that has recorded its handshake.
    const client_stats_t *client = nullptr;
    if (requester_generation != 0) {
      const auto asking = std::find_if(stats.clients.begin(), stats.clients.end(),
        [requester_generation](const client_stats_t &candidate) {
          return candidate.session_generation == requester_generation;
        });
      if (asking == stats.clients.end() || !asking->bitrate_request_recorded) {
        return nullptr;
      }
      client = &*asking;
    } else {
      const auto first = std::find_if(stats.clients.begin(), stats.clients.end(),
        [](const client_stats_t &candidate) {
          return candidate.bitrate_request_recorded;
        });
      if (first == stats.clients.end()) {
        return nullptr;
      }
      client = &*first;
    }
    const auto &request = client->bitrate_request;
    // The total the formula ran on, and null for a stream whose encoder rate is no split of its own
    // request: a client that sent no bitrate at all, or a watcher, which encodes at its owner's rate.
    // The formula says nothing about such a stream, and the null says so.
    const auto split_kbps = request.split_kbps > 0 ? nlohmann::json(request.split_kbps) : nlohmann::json(nullptr);
    // The cap that cut the request, and where it came from, or null for both when none did.
    const bool capped = request.cap_kbps > 0;
    const auto cap_kbps = capped ? nlohmann::json(request.cap_kbps) : nlohmann::json(nullptr);
    const auto cap_source = capped ? nlohmann::json(request.cap_source) : nlohmann::json(nullptr);
    // What the stream's own encode loop last reported it runs at, after a live bitrate, Live Tuning or
    // Doctor changed it. The encoder opens at the negotiated rate, which stands in until that report.
    const int live_encoder_kbps = client->bitrate_kbps > 0 ? client->bitrate_kbps : request.encoder_kbps;
    return {
      {"version", 1},
      {"requested_kbps", request.client_kbps},
      {"warp_factor", request.warp_factor},
      {"cap_kbps", cap_kbps},
      {"cap_source", cap_source},
      {"split_kbps", split_kbps},
      {"encoder_kbps", request.encoder_kbps},
      {"live_encoder_kbps", live_encoder_kbps},
      {"audio_kbps", request.audio_kbps},
      {"fec_percentage", request.fec_percentage},
      {"formula", stream_bitrate::k_formula},
    };
  }

  std::optional<stream_bitrate::request_t> recorded_stream_request(std::uint64_t session_generation) {
    if (session_generation == 0) {
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(stats_mutex);
    const auto client = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      });
    if (client == current_stats.clients.end() || !client->bitrate_request_recorded) {
      return std::nullopt;
    }
    return client->bitrate_request;
  }

  namespace {
    // The handshake sets a saved paired profile aside for a PyroWave stream, sized for H.264 as it is,
    // so it is no restore goal for one either.
    bool paired_profile_caps_restore(const stats_t &stats) {
      return stats.paired_target_bitrate_kbps > 0 && stats.codec != "pyrowave";
    }
  }  // namespace

  int doctor_launch_quality_goal_kbps(const stats_t &stats) {
    if (stats.effective_launch_bitrate_kbps <= 0) return 0;
    if (!paired_profile_caps_restore(stats)) return stats.effective_launch_bitrate_kbps;
    return std::min(stats.paired_target_bitrate_kbps, stats.effective_launch_bitrate_kbps);
  }

  doctor_quality_goal_t doctor_quality_goal(const stats_t &stats, std::string_view source) {
    if (source == "pyrowave_advice") {
      const auto pyrowave = evaluate_pyrowave_bitrate(stats);
      if (!pyrowave.active) return {};
      return {pyrowave.advice.raise_goal_encoder_kbps, pyrowave.advice.raise_goal_kbps, "pyrowave_advice"};
    }
    const int launch = doctor_launch_quality_goal_kbps(stats);
    return {launch, launch, paired_profile_caps_restore(stats) ? "launch_ceiling" : "launch_bitrate"};
  }

  namespace {
    std::string pyrowave_reason_for_route(std::string_view route) {
      // Each says what the encoder saw at its own input, which is all the route records. A
      // zero_copy frame was imported as a DMA-BUF, and that says nothing about how capture filled
      // the buffer. A repeated frame is encoded again without another upload or conversion, so
      // none of them says the work happens on each frame.
      if (route == "zero_copy") {
        return "PyroWave imports captured DMA-BUF frames and converts colour on the GPU, without a "
               "CPU upload at the encoder input.";
      }
      if (route == "gpu_upload") {
        return "PyroWave converts colour on the GPU after copying captured frames there from host memory.";
      }
      if (route == "cpu_convert") {
        return "PyroWave converts colour on the CPU and copies the planes to the GPU, which costs host "
               "CPU time on captured frames. POLARIS_PYROWAVE_GPU_INPUT=off asks for this, and a host "
               "falls back to it when the GPU path cannot start.";
      }
      return "PyroWave has not encoded a captured frame yet, so where it converts colour is not known.";
    }
  }  // namespace

  std::string pyrowave_route_reason(const stats_t &stats, std::uint64_t requester_generation) {
    // The stream that asked, when it is one of these. Watch Stream and every reconnect overlap put
    // more than one entry here, and a client is answered about its own stream in both: the old
    // entry of a reconnect stays until its teardown, and the asker's generation is the new one.
    if (requester_generation != 0) {
      const auto asking = std::find_if(stats.clients.begin(), stats.clients.end(),
        [requester_generation](const client_stats_t &client) {
          return client.session_generation == requester_generation;
        });
      if (asking != stats.clients.end()) {
        return pyrowave_reason_for_route(asking->pyrowave_route);
      }
    }
    // Asked from the host, or by a client with no stream here: one answer when every stream that has
    // reported a route reports the same one, which is the sole stream's route when there is one.
    std::string_view shared;
    for (const auto &client : stats.clients) {
      if (client.pyrowave_route.empty()) {
        continue;
      }
      if (shared.empty()) {
        shared = client.pyrowave_route;
      } else if (client.pyrowave_route != shared) {
        return "PyroWave streams on this host convert colour in different places, so no one answer "
               "covers them all.";
      }
    }
    return pyrowave_reason_for_route(shared);
  }

  std::string stream_instance_id(std::uint64_t session_generation) {
    if (session_generation == 0) {
      return {};
    }
    return process_instance_nonce() + "." + std::to_string(session_generation);
  }

  void record_oversized_fec_frame(std::uint64_t session_generation,
                                  std::size_t encoded_frame_bytes,
                                  std::size_t packetized_frame_bytes,
                                  std::size_t protected_payload_limit_bytes,
                                  std::size_t required_blocks,
                                  int fec_percentage,
                                  int packet_size,
                                  std::string_view frame_type) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    auto client = std::find_if(
      current_stats.clients.begin(), current_stats.clients.end(),
      [session_generation](const client_stats_t &candidate) {
        return candidate.session_generation == session_generation;
      }
    );
    if (client == current_stats.clients.end()) {
      return;
    }

    merge_oversized_fec_frame(
      client->fec_protection,
      encoded_frame_bytes,
      packetized_frame_bytes,
      protected_payload_limit_bytes,
      required_blocks,
      fec_percentage,
      packet_size,
      frame_type
    );
    if (client == current_stats.clients.begin()) {
      current_stats.fec_protection = client->fec_protection;
    }
  }

  double packet_loss_percent(uint64_t scaled_loss, uint64_t scale) {
    if (scale == 0) {
      return 0.0;
    }
    return std::clamp((double) scaled_loss * 100.0 / (double) scale, 0.0, 100.0);
  }

  namespace {
    using judge_clock = network_judge_t::clock_type;

    struct judged_loss_t {
      int samples = 0;
      double frames_expected = 0.0;
      double frames_lost = 0.0;
      judge_clock::time_point oldest {};
    };

    judged_loss_t judged_loss(const std::deque<network_judge_t::media_sample_t> &media,
                              judge_clock::time_point now) {
      judged_loss_t result;
      const auto from = now - network_judge_t::k_window;
      for (const auto &sample : media) {
        if (sample.at <= from || sample.at > now) {
          continue;
        }
        if (result.samples == 0) {
          result.oldest = sample.at;
        }
        ++result.samples;
        result.frames_expected += sample.frames_expected;
        result.frames_lost += sample.frames_lost;
      }
      return result;
    }

    std::optional<double> judged_loss_pct(const judged_loss_t &loss) {
      if (loss.samples < network_judge_t::k_min_media_samples || loss.frames_expected <= 0.0) {
        return std::nullopt;
      }
      return std::clamp(loss.frames_lost * 100.0 / loss.frames_expected, 0.0, 100.0);
    }

    struct judged_rtt_t {
      int readings = 0;
      std::optional<double> median_ms;
    };

    judged_rtt_t judged_rtt(const std::deque<network_judge_t::rtt_reading_t> &rtt,
                            judge_clock::time_point now) {
      std::vector<double> values;
      values.reserve(rtt.size());
      const auto from = now - network_judge_t::k_window;
      for (const auto &reading : rtt) {
        if (reading.at > from && reading.at <= now) {
          values.push_back(reading.rtt_ms);
        }
      }
      judged_rtt_t result;
      result.readings = static_cast<int>(values.size());
      if (result.readings < network_judge_t::k_min_rtt_readings) {
        return result;
      }
      std::sort(values.begin(), values.end());
      const auto middle = values.size() / 2;
      result.median_ms = values.size() % 2 == 1 ?
        values[middle] :
        (values[middle - 1] + values[middle]) / 2.0;
      return result;
    }

    // One verdict's step across its band: it turns on at enter and off only below exit. With no
    // figure there is no verdict, so a window that thinned out starts over instead of keeping one.
    void judge_band(bool &elevated, std::optional<double> figure, double enter, double exit) {
      if (!figure) {
        elevated = false;
        return;
      }
      if (elevated ? *figure < exit : *figure >= enter) {
        elevated = !elevated;
      }
    }

    template<class Readings>
    void drop_stale(Readings &readings, judge_clock::time_point now, std::size_t capacity) {
      const auto from = now - network_judge_t::k_window;
      while (!readings.empty() && (readings.front().at <= from || readings.size() > capacity)) {
        readings.pop_front();
      }
    }
  }  // namespace

  void network_judge_t::add_media(clock_type::time_point at, double frames_expected, double frames_lost) {
    if (!std::isfinite(frames_expected) || !std::isfinite(frames_lost) || frames_expected <= 0.0) {
      return;
    }
    media.push_back({at, frames_expected, std::clamp(frames_lost, 0.0, frames_expected)});
    drop_stale(media, at, k_max_media_samples);
    judge_media(at);
    note_risk(at);
  }

  void network_judge_t::add_rtt(clock_type::time_point at, double rtt_ms) {
    if (!std::isfinite(rtt_ms) || rtt_ms < 0.0) {
      return;
    }
    ++rtt_readings_seen;
    if (!rtt_armed) {
      rtt_armed = rtt_ms < k_rtt_enter_ms || rtt_readings_seen >= k_rtt_armed_after;
      if (!rtt_armed) {
        return;
      }
    }
    rtt.push_back({at, rtt_ms});
    drop_stale(rtt, at, k_max_rtt_readings);
    judge_rtt(at);
    note_risk(at);
  }

  void network_judge_t::add_control_loss(clock_type::time_point at, double loss_pct) {
    if (!std::isfinite(loss_pct) || loss_pct < 0.0) {
      return;
    }
    control.push_back({at, std::min(loss_pct, 100.0)});
    drop_stale(control, at, k_max_control_readings);
    judge_control(at);
  }

  void network_judge_t::restart(clock_type::time_point from) {
    const auto drop_before = [from](auto &readings) {
      while (!readings.empty() && readings.front().at < from) {
        readings.pop_front();
      }
    };
    drop_before(media);
    drop_before(rtt);
    drop_before(control);
    loss_elevated = false;
    rtt_elevated = false;
    control_loss_elevated = false;
    judged = network_verdict_t {};
    media_oldest = {};
    risk = false;
    risk_since.reset();
    std::optional<clock_type::time_point> newest;
    if (!media.empty()) {
      judge_media(media.back().at);
      newest = media.back().at;
    }
    if (!rtt.empty()) {
      judge_rtt(rtt.back().at);
      newest = newest ? std::max(*newest, rtt.back().at) : rtt.back().at;
    }
    if (!control.empty()) {
      judge_control(control.back().at);
    }
    if (newest) {
      note_risk(*newest);
    }
  }

  // The figure and its band are judged together and kept together until the next report, so the
  // verdict never quotes a figure the band was not judged on.
  void network_judge_t::judge_media(clock_type::time_point at) {
    const auto loss = judged_loss(media, at);
    const auto pct = judged_loss_pct(loss);
    judge_band(loss_elevated, pct, k_loss_enter_pct, k_loss_exit_pct);
    judged.media_samples = loss.samples;
    judged.frames_expected = static_cast<std::uint64_t>(std::llround(loss.frames_expected));
    judged.frames_lost = static_cast<std::uint64_t>(std::llround(loss.frames_lost));
    judged.loss_available = pct.has_value();
    judged.loss_pct = pct.value_or(0.0);
    judged.loss_elevated = judged.loss_available && loss_elevated;
    media_oldest = loss.oldest;
  }

  void network_judge_t::judge_rtt(clock_type::time_point at) {
    const auto round_trip = judged_rtt(rtt, at);
    judge_band(rtt_elevated, round_trip.median_ms, k_rtt_enter_ms, k_rtt_exit_ms);
    judged.rtt_samples = round_trip.readings;
    judged.rtt_available = round_trip.median_ms.has_value();
    judged.rtt_ms = round_trip.median_ms.value_or(0.0);
    judged.rtt_elevated = judged.rtt_available && rtt_elevated;
  }

  // ENet's estimate is already an average of its own, and it swings with each retransmission: the HEVC
  // run's read 1.08 and then 2.81 on consecutive polls. Averaged over the window and banded, it names
  // retries once they last.
  void network_judge_t::judge_control(clock_type::time_point at) {
    const auto from = at - k_window;
    int readings = 0;
    double sum = 0.0;
    for (const auto &reading : control) {
      if (reading.at > from && reading.at <= at) {
        ++readings;
        sum += reading.loss_pct;
      }
    }
    const auto mean = readings >= k_min_control_readings ? std::optional<double> {sum / readings} : std::nullopt;
    judge_band(control_loss_elevated, mean, k_loss_enter_pct, k_loss_exit_pct);
    judged.control_samples = readings;
    judged.control_loss_available = mean.has_value();
    judged.control_loss_pct = mean.value_or(0.0);
    judged.control_loss_elevated = judged.control_loss_available && control_loss_elevated;
  }

  void network_judge_t::note_risk(clock_type::time_point at) {
    const bool elevated = loss_elevated || rtt_elevated;
    if (!risk_since || elevated != risk) {
      risk = elevated;
      risk_since = at;
    }
  }

  network_verdict_t network_judge_t::verdict(clock_type::time_point now) const {
    auto result = judged;
    const auto window_start = now - k_window;
    // Once every reading of a kind has left the window, there is nothing to judge it on.
    if (media.empty() || media.back().at <= window_start) {
      result.loss_available = false;
      result.loss_pct = 0.0;
      result.loss_elevated = false;
      result.media_samples = 0;
      result.frames_expected = 0;
      result.frames_lost = 0;
    } else if (result.media_samples > 0) {
      result.media_span_ms = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(now - media_oldest).count()
      );
    }
    if (rtt.empty() || rtt.back().at <= window_start) {
      result.rtt_available = false;
      result.rtt_ms = 0.0;
      result.rtt_elevated = false;
      result.rtt_samples = 0;
    }
    if (control.empty() || control.back().at <= window_start) {
      result.control_loss_available = false;
      result.control_loss_pct = 0.0;
      result.control_loss_elevated = false;
      result.control_samples = 0;
    }
    result.risk = result.loss_elevated || result.rtt_elevated;
    if (risk_since) {
      result.risk_held_ms = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(now - *risk_since).count()
      );
    }
    return result;
  }

  std::string_view network_loss_state(const network_verdict_t &verdict) {
    if (verdict.loss_stale) {
      return "stale";
    }
    if (!verdict.loss_available) {
      return "collecting";
    }
    if (verdict.loss_elevated) {
      return "elevated";
    }
    return verdict.loss_pct >= network_judge_t::k_loss_exit_pct ? "light" : "clean";
  }

  std::string_view network_rtt_state(const network_verdict_t &verdict) {
    if (verdict.rtt_stale) {
      return "stale";
    }
    if (!verdict.rtt_available) {
      return "collecting";
    }
    return verdict.rtt_elevated ? "elevated" : "clean";
  }

  nlohmann::json network_verdict_json(const network_verdict_t &verdict) {
    return {
      {"loss_basis", "video_frames_lost_after_fec"},
      {"window_seconds", network_judge_t::k_window.count()},
      {"loss_state", network_loss_state(verdict)},
      {"loss_pct", verdict.loss_available ? nlohmann::json(verdict.loss_pct) : nlohmann::json(nullptr)},
      {"frames_expected", verdict.frames_expected},
      {"frames_lost", verdict.frames_lost},
      {"media_samples", verdict.media_samples},
      {"media_span_ms", verdict.media_span_ms},
      {"loss_enter_pct", network_judge_t::k_loss_enter_pct},
      {"loss_exit_pct", network_judge_t::k_loss_exit_pct},
      {"rtt_state", network_rtt_state(verdict)},
      {"rtt_median_ms", verdict.rtt_available ? nlohmann::json(verdict.rtt_ms) : nlohmann::json(nullptr)},
      {"rtt_samples", verdict.rtt_samples},
      {"rtt_enter_ms", network_judge_t::k_rtt_enter_ms},
      {"rtt_exit_ms", network_judge_t::k_rtt_exit_ms},
      {"control_loss_pct", verdict.control_loss_available ? nlohmann::json(verdict.control_loss_pct) : nlohmann::json(nullptr)},
      {"control_loss_elevated", verdict.control_loss_elevated},
      {"control_samples", verdict.control_samples},
      {"risk", verdict.risk},
      {"risk_held_ms", verdict.risk_held_ms}
    };
  }

  namespace {
    /// The frames a counted media report covers and how many of them were lost.
    struct media_report_frames_t {
      double expected = 0.0;
      double lost = 0.0;
    };

    void record_primary_network_observation(bool media_sample,
                                            double latency_ms,
                                            double loss,
                                            uint64_t bytes_sent,
                                            std::optional<media_report_frames_t> frames = std::nullopt) {
      std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
      const auto received_at = std::chrono::steady_clock::now();
      primary_network_state.received_at = received_at;
      primary_network_state.media_sample = media_sample;
      primary_network_state.latency_ms = latency_ms;
      primary_network_state.bytes_sent = bytes_sent;
      if (media_sample) {
        primary_network_state.packet_loss = loss;
        primary_network_state.packet_loss_available = true;
        primary_network_state.media_loss_received_at = received_at;
      } else {
        primary_network_state.control_channel_packet_loss = loss;
        ++primary_network_state.control_channel_samples;
      }

      // A control-channel ping contributes host-observed RTT, but it says
      // nothing about video delivery. Keep a current confirmed media-loss
      // reading in the shared debounce input until the next media report (or
      // until its host-received freshness expires). Otherwise the usually
      // faster clean ping cadence clears real media loss between Nova's
      // one-second counter reports and Doctor never offers the safe bitrate
      // fix. This does not refresh media provenance: the original timestamp
      // below remains authoritative for action eligibility.
      const bool current_media_loss =
        primary_network_state.packet_loss_available &&
        primary_network_state.media_loss_received_at !=
          std::chrono::steady_clock::time_point {} &&
        received_at - primary_network_state.media_loss_received_at <=
          std::chrono::milliseconds(DOCTOR_CURRENT_NETWORK_MAX_AGE_MS);
      const double confirmed_media_loss = current_media_loss ?
        primary_network_state.packet_loss : 0.0;
      primary_network_state.network_risk = network_risk_tracker.update(
        confirmed_media_loss,
        latency_ms
      );
      // The judgement every grading reader serves. A control-channel ping brings an RTT reading and
      // nothing about video, so only a media report enters the loss window.
      if (media_sample) {
        if (frames) {
          network_judge.add_media(received_at, frames->expected, frames->lost);
        } else {
          network_judge.add_media(
            received_at,
            network_judge_t::k_frames_per_percentage_report,
            std::clamp(loss, 0.0, 100.0) * network_judge_t::k_frames_per_percentage_report / 100.0
          );
        }
      }
      network_judge.add_rtt(received_at, latency_ms);
      if (!media_sample) {
        network_judge.add_control_loss(received_at, loss);
      }
      const bool suppresses_quality_restore =
        primary_network_state.network_risk ||
        (primary_network_state.packet_loss_available &&
         primary_network_state.packet_loss > 2.0) ||
        primary_network_state.latency_ms >= 45.0;
      // Advance or latch Doctor policy before exposing this complete
      // observation. Before a change, the controller epoch rejects a stale
      // action. During a guarded quality transaction, a regression is latched
      // atomically with the actuator so it cannot slip between verification
      // and the next step.
      adaptive_bitrate::note_network_evidence_arrival(
        suppresses_quality_restore
      );
      primary_network_state.revision =
        hot_network_sample_revision.fetch_add(1, std::memory_order_release) + 1;
      if (media_sample) {
        primary_network_state.media_loss_revision = primary_network_state.revision;
      }

      hot_latency_ms.store(latency_ms, std::memory_order_relaxed);
      hot_packet_loss.store(primary_network_state.packet_loss, std::memory_order_relaxed);
      hot_packet_loss_available.store(primary_network_state.packet_loss_available, std::memory_order_relaxed);
      hot_control_channel_packet_loss.store(primary_network_state.control_channel_packet_loss, std::memory_order_relaxed);
      hot_control_channel_samples.store(primary_network_state.control_channel_samples, std::memory_order_relaxed);
      hot_network_risk.store(primary_network_state.network_risk, std::memory_order_relaxed);
      hot_bytes_sent.store(bytes_sent, std::memory_order_relaxed);

      primary_network_observations.push_back(primary_network_state);
      if (primary_network_observations.size() > NETWORK_OBSERVATION_CAPACITY) {
        primary_network_observations.pop_front();
      }
    }
  }  // namespace

  void update_network_stats(double latency_ms, double packet_loss, uint64_t bytes_sent) {
    // No per-client mirror on this overload, and the risk tracker has its
    // own lock, so stats_mutex stays out of this path.
    record_primary_network_observation(true, latency_ms, packet_loss, bytes_sent);
  }

  void update_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, uint64_t bytes_sent) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it != current_stats.clients.end()) {
      it->latency_ms = latency_ms;
      it->packet_loss = packet_loss;
      it->packet_loss_available = true;
      it->packet_loss_source = "media_transport";
      it->bytes_sent = bytes_sent;
    }

    // Also update top-level stats (use first client for backward compat).
    // Only that primary client's observation may drive the top-level risk.
    const bool primary_client =
      !current_stats.clients.empty() && current_stats.clients.front().ip == client_ip;
    if (primary_client) {
      record_primary_network_observation(true, latency_ms, packet_loss, bytes_sent);
    }
  }

  std::string_view from_client_media_ingest_state(
      client_media_ingest_state_e state) {
    switch (state) {
      case client_media_ingest_state_e::baseline: return "baseline";
      case client_media_ingest_state_e::observed: return "observed";
      case client_media_ingest_state_e::waiting_for_frames: return "waiting_for_frames";
      case client_media_ingest_state_e::coverage_gap_reset: return "coverage_gap_reset";
      case client_media_ingest_state_e::counter_epoch_reset: return "counter_epoch_reset";
      case client_media_ingest_state_e::non_monotonic: return "non_monotonic";
      case client_media_ingest_state_e::scope_mismatch: return "scope_mismatch";
      case client_media_ingest_state_e::invalid: return "invalid";
    }
    return "invalid";
  }

  client_media_ingest_result_t ingest_client_media_counters(
      const client_media_counters_t &sample) {
    client_media_ingest_result_t result;
    if (sample.owner_uuid.empty() || sample.app_session_id.empty() ||
        sample.session_generation == 0 || sample.client_monotonic_ms == 0 ||
        sample.frames_received > sample.frames_expected ||
        sample.frames_lost != sample.frames_expected - sample.frames_received) {
      return result;
    }

    std::lock_guard<std::mutex> ingest_lock(client_media_ingest_mutex);
    {
      // The route's first check can race a disconnect/reconnect while this
      // request waits behind another ingest. Recheck the exact owner, token,
      // and generation under the same serialization used by timing-scope
      // start/stop and stream reset before accepting even a baseline.
      std::lock_guard<std::mutex> timing_lock(frame_timing_mutex);
      const auto *active = find_session_timing_locked(sample.owner_uuid);
      if (!active || active->session_generation != sample.session_generation ||
          active->session_token != sample.app_session_id) {
        result.state = client_media_ingest_state_e::scope_mismatch;
        return result;
      }
    }
    {
      std::lock_guard<std::mutex> stats_lock(stats_mutex);
      if (!current_stats.streaming) {
        result.state = client_media_ingest_state_e::scope_mismatch;
        return result;
      }
    }
    {
      std::lock_guard<std::mutex> counter_lock(client_media_counter_mutex);
      const auto received_at = std::chrono::steady_clock::now();
      const bool same_scope = last_client_media_counter_epoch &&
        last_client_media_counter_epoch->sample.owner_uuid == sample.owner_uuid &&
        last_client_media_counter_epoch->sample.app_session_id == sample.app_session_id &&
        last_client_media_counter_epoch->sample.session_generation == sample.session_generation;
      if (!same_scope) {
        last_client_media_counter_epoch = client_media_counter_epoch_t {sample, received_at};
        result.state = client_media_ingest_state_e::baseline;
        result.accepted = true;
        return result;
      }

      const auto previous_epoch = *last_client_media_counter_epoch;
      const auto &previous = previous_epoch.sample;
      if (sample.client_monotonic_ms <= previous.client_monotonic_ms) {
        result.state = client_media_ingest_state_e::non_monotonic;
        return result;
      }

      const auto client_gap = std::chrono::milliseconds(
        sample.client_monotonic_ms - previous.client_monotonic_ms
      );
      if (received_at - previous_epoch.received_at > CLIENT_MEDIA_COUNTER_MAX_GAP ||
          client_gap > CLIENT_MEDIA_COUNTER_MAX_GAP) {
        last_client_media_counter_epoch = client_media_counter_epoch_t {sample, received_at};
        result.state = client_media_ingest_state_e::coverage_gap_reset;
        result.accepted = true;
        // Accepted and never counted: reports spaced this far apart carry no loss Doctor can use,
        // and nothing else would say so.
        if (logged_media_gap_generation.exchange(sample.session_generation) != sample.session_generation) {
          BOOST_LOG(info) << "Doctor: a client media report for stream generation "sv << sample.session_generation
                          << " came "sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(received_at - previous_epoch.received_at).count()
                          << " ms after the one before it, so its loss starts a new baseline instead of counting"sv;
        }
        return result;
      }

      if (sample.frames_expected < previous.frames_expected ||
          sample.frames_received < previous.frames_received ||
          sample.frames_lost < previous.frames_lost) {
        last_client_media_counter_epoch = client_media_counter_epoch_t {sample, received_at};
        result.state = client_media_ingest_state_e::counter_epoch_reset;
        result.accepted = true;
        return result;
      }

      const auto expected_delta = sample.frames_expected - previous.frames_expected;
      const auto received_delta = sample.frames_received - previous.frames_received;
      const auto lost_delta = sample.frames_lost - previous.frames_lost;
      if (received_delta > expected_delta || lost_delta != expected_delta - received_delta) {
        result.state = client_media_ingest_state_e::invalid;
        return result;
      }
      last_client_media_counter_epoch = client_media_counter_epoch_t {sample, received_at};
      result.accepted = true;
      if (expected_delta == 0) {
        result.state = client_media_ingest_state_e::waiting_for_frames;
        return result;
      }
      result.media_loss_pct = packet_loss_percent(lost_delta, expected_delta);
      result.frames_expected = expected_delta;
      result.frames_lost = lost_delta;
      result.state = client_media_ingest_state_e::observed;
    }

    // Serialize counter advancement with evidence publication so concurrent
    // authenticated requests cannot publish an older delta after a newer one.
    // The media counters prove loss. RTT and byte totals remain host-owned.
    const auto host = get_current();
    record_primary_network_observation(
      true,
      host.latency_ms,
      result.media_loss_pct,
      host.bytes_sent,
      media_report_frames_t {static_cast<double>(result.frames_expected), static_cast<double>(result.frames_lost)}
    );
    {
      // The stream's own client row says what the top level says. It was never written on this
      // path, so every client row read "unavailable" while its loss was arriving.
      std::lock_guard<std::mutex> stats_lock(stats_mutex);
      for (auto &client : current_stats.clients) {
        if (client.session_generation == sample.session_generation) {
          client.packet_loss = result.media_loss_pct;
          client.packet_loss_available = true;
          client.packet_loss_source = "media_transport";
        }
      }
    }
    if (adaptive_bitrate::is_enabled()) {
      // Live Tuning acts when Doctor does: each report's own loss while the verdict calls loss network
      // pressure, and none while it does not. Not the window's figure: that stays at 1% or more for
      // most of the 20 seconds after the loss stops, and Live Tuning cut on it every second, late and
      // long after, where each report's figure lets its average fall within seconds.
      const auto verdict = current_network_verdict();
      adaptive_bitrate::update_network_stats(verdict.loss_elevated ? result.media_loss_pct : 0.0, host.latency_ms);
    }
    result.observation_published = true;
    if (logged_media_report_generation.exchange(sample.session_generation) != sample.session_generation) {
      BOOST_LOG(info) << "Doctor: client media reports reach this host for stream generation "sv
                      << sample.session_generation << "; the first counted one covered "sv
                      << result.frames_expected << " frames with "sv << result.frames_lost << " lost"sv;
    }
    return result;
  }

  network_verdict_t current_network_verdict() {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    return network_judge.verdict(std::chrono::steady_clock::now());
  }

  network_verdict_t network_verdict_since(std::chrono::steady_clock::time_point from) {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    auto judge = network_judge;
    judge.restart(from);
    return judge.verdict(std::chrono::steady_clock::now());
  }

  void restart_network_judgement(std::chrono::steady_clock::time_point from) {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    network_judge.restart(from);
  }

  network_verdict_t served_network_verdict(const stats_t &stats) {
    auto verdict = stats.network_verdict;
    const bool network_current =
      stats.network_sample_revision > 0 &&
      stats.network_last_received_age_ms >= 0 &&
      stats.network_last_received_age_ms <= DOCTOR_CURRENT_NETWORK_MAX_AGE_MS;
    const bool media_current =
      stats.media_loss_sample_revision > 0 &&
      stats.media_loss_last_received_age_ms >= 0 &&
      stats.media_loss_last_received_age_ms <= judged_network_t::k_media_report_max_age_ms;
    if (!(network_current && media_current) && verdict.loss_available) {
      verdict.loss_available = false;
      verdict.loss_stale = true;
      verdict.loss_pct = 0.0;
      verdict.loss_elevated = false;
    }
    if (!network_current && verdict.rtt_available) {
      verdict.rtt_available = false;
      verdict.rtt_stale = true;
      verdict.rtt_ms = 0.0;
      verdict.rtt_elevated = false;
    }
    if (!network_current) {
      verdict.control_loss_available = false;
      verdict.control_loss_pct = 0.0;
      verdict.control_loss_elevated = false;
    }
    verdict.risk = verdict.loss_elevated || verdict.rtt_elevated;
    return verdict;
  }

  judged_network_t judged_network(const stats_t &stats) {
    judged_network_t result;
    const auto verdict = served_network_verdict(stats);
    result.loss_judged = verdict.loss_available;
    result.rtt_judged = verdict.rtt_available;
    result.loss_pressure = result.loss_judged && verdict.loss_elevated;
    result.rtt_pressure = result.rtt_judged && verdict.rtt_elevated;
    result.rtt_fail = result.rtt_judged && verdict.rtt_ms >= network_judge_t::k_rtt_fail_ms;
    result.risk = result.loss_pressure || result.rtt_pressure;
    result.fail = result.risk && (result.loss_pressure || result.rtt_fail);
    return result;
  }

#ifdef POLARIS_TESTS
  void age_client_media_counter_baseline_for_tests(
      std::chrono::steady_clock::duration age) {
    std::lock_guard<std::mutex> lock(client_media_counter_mutex);
    if (last_client_media_counter_epoch) {
      last_client_media_counter_epoch->received_at =
        std::chrono::steady_clock::now() - age;
    }
  }

  void age_network_judge_for_tests(std::chrono::steady_clock::duration age) {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    network_judge.media_oldest -= age;
    for (auto &sample : network_judge.media) {
      sample.at -= age;
    }
    for (auto &reading : network_judge.rtt) {
      reading.at -= age;
    }
    for (auto &reading : network_judge.control) {
      reading.at -= age;
    }
    if (network_judge.risk_since) {
      *network_judge.risk_since -= age;
    }
  }
#endif

  void update_control_channel_stats(double latency_ms, double control_packet_loss, uint64_t bytes_sent) {
    // RTT describes the shared path. The ENet loss EWMA describes only its
    // reliable control channel, so it cannot enter the actionable loss term.
    record_primary_network_observation(false, latency_ms, control_packet_loss, bytes_sent);
  }

  void update_control_channel_stats(const std::string &client_ip,
                                    double latency_ms,
                                    double control_packet_loss,
                                    uint64_t bytes_sent) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it != current_stats.clients.end()) {
      it->latency_ms = latency_ms;
      it->control_channel_packet_loss = control_packet_loss;
      it->bytes_sent = bytes_sent;
    }

    const bool primary_client =
      !current_stats.clients.empty() && current_stats.clients.front().ip == client_ip;
    if (primary_client) {
      record_primary_network_observation(false, latency_ms, control_packet_loss, bytes_sent);
    }
  }

  void set_doctor_live_action_scope_available(bool available) {
    hot_doctor_live_action_scope_available.store(available, std::memory_order_release);
  }

  network_verification_window_t get_network_verification_window(
      std::uint64_t after_revision,
      std::chrono::steady_clock::time_point applied_at,
      std::chrono::steady_clock::duration required_duration) {
    network_verification_window_t result;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);

    const primary_network_observation_t *first = nullptr;
    const primary_network_observation_t *last = nullptr;
    for (const auto &observation : primary_network_observations) {
      if (observation.revision <= after_revision ||
          observation.received_at < applied_at || observation.received_at > now) {
        continue;
      }
      if (!first) first = &observation;
      last = &observation;
      ++result.sample_count;
      if (observation.media_sample) ++result.media_sample_count;
      result.max_latency_ms = std::max(result.max_latency_ms, observation.latency_ms);
      if (observation.packet_loss_available) {
        result.any_packet_loss_available = true;
        result.max_packet_loss = std::max(result.max_packet_loss, observation.packet_loss);
      }
      result.control_channel_packet_loss = std::max(
        result.control_channel_packet_loss,
        observation.control_channel_packet_loss
      );
      result.control_channel_samples = std::max(
        result.control_channel_samples,
        observation.control_channel_samples
      );
      result.any_network_risk = result.any_network_risk || observation.network_risk;
    }
    if (!first || !last) return result;

    result.last_revision = last->revision;
    result.first_delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      first->received_at - applied_at
    ).count();
    result.last_delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      last->received_at - applied_at
    ).count();
    result.span_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      last->received_at - first->received_at
    ).count();
    result.last_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last->received_at
    ).count();
    result.latency_ms = last->latency_ms;
    result.packet_loss = last->packet_loss;
    result.packet_loss_available = last->packet_loss_available;
    result.network_risk = last->network_risk;
    const auto required_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      required_duration
    ).count();
    const auto latest_required_ms = std::max<std::int64_t>(0, required_ms - 1000);
    const auto minimum_span_ms = std::max<std::int64_t>(0, required_ms - 3000);
    result.complete = result.sample_count >= 2 &&
      result.first_delay_ms <= 2000 &&
      result.last_delay_ms >= latest_required_ms &&
      result.span_ms >= minimum_span_ms &&
      result.last_age_ms <= 2000;
    return result;
  }

#ifdef POLARIS_TESTS
  void spread_network_verification_window_for_tests(
      std::uint64_t after_revision,
      std::chrono::steady_clock::time_point applied_at,
      std::chrono::steady_clock::time_point completed_at) {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    std::vector<primary_network_observation_t *> eligible;
    for (auto &observation : primary_network_observations) {
      if (observation.revision > after_revision) eligible.push_back(&observation);
    }
    if (eligible.size() < 2) return;
    const auto first_at = applied_at + std::chrono::seconds(1);
    const auto span = completed_at - first_at;
    for (std::size_t i = 0; i < eligible.size(); ++i) {
      eligible[i]->received_at = first_at + span * i / (eligible.size() - 1);
    }
  }

  void age_latest_network_observation_for_tests(
      std::chrono::steady_clock::duration age) {
    std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
    if (primary_network_observations.empty()) return;
    const auto aged_at = std::chrono::steady_clock::now() - age;
    primary_network_observations.back().received_at = aged_at;
    primary_network_state.received_at = aged_at;
    if (primary_network_observations.back().media_sample) {
      primary_network_observations.back().media_loss_received_at = aged_at;
      primary_network_state.media_loss_received_at = aged_at;
    }
  }
#endif

  void update_runtime_state(const platf::runtime_state_t &state) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (!state.backend_name.empty()) {
      current_stats.runtime_backend = state.backend_name;
    }
    current_stats.runtime_requested_headless = state.requested_headless;
    current_stats.runtime_effective_headless = state.effective_headless;
    current_stats.runtime_gpu_native_override_active = state.gpu_native_override_active;
    current_stats.runtime_reported_refresh_hz = state.reported_output_refresh_hz;
  }

  void update_capture_source_fps(double fps) {
    const double normalized_fps = std::max(0.0, fps);
    {
      std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
      doctor_video_policy_state.capture_source_fps = normalized_fps;
      publish_doctor_video_policy_locked();
      hot_capture_source_fps.store(normalized_fps, std::memory_order_relaxed);
    }
  }

  void note_doctor_video_policy_sample(double target_fps,
                                       double delivered_fps,
                                       double duplicate_frame_ratio,
                                       double dropped_frame_ratio,
                                       double avg_frame_age_ms,
                                       double frame_jitter_ms,
                                       double encode_time_ms) {
    std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
    doctor_video_policy_state.target_fps = target_fps;
    doctor_video_policy_state.delivered_fps = delivered_fps;
    doctor_video_policy_state.duplicate_frame_ratio = duplicate_frame_ratio;
    doctor_video_policy_state.dropped_frame_ratio = dropped_frame_ratio;
    doctor_video_policy_state.avg_frame_age_ms = avg_frame_age_ms;
    doctor_video_policy_state.frame_jitter_ms = frame_jitter_ms;
    doctor_video_policy_state.encode_time_ms = encode_time_ms;
    ++doctor_video_policy_state.sample_count;
    if (video_policy_has_pacing_warning(doctor_video_policy_state)) {
      ++doctor_video_policy_state.pacing_warning_streak;
    } else {
      doctor_video_policy_state.pacing_warning_streak = 0;
    }
    publish_doctor_video_policy_locked();
    // Publish every Doctor-policy field under the same lock as its class
    // transition. get_current() cannot observe the new authority with the old
    // evidence (or vice versa) while an Auto Fix is being re-derived.
    hot_fps.store(delivered_fps, std::memory_order_relaxed);
    hot_encode_time_ms.store(encode_time_ms, std::memory_order_relaxed);
    hot_duplicate_frame_ratio.store(duplicate_frame_ratio, std::memory_order_relaxed);
    hot_dropped_frame_ratio.store(dropped_frame_ratio, std::memory_order_relaxed);
    hot_avg_frame_age_ms.store(avg_frame_age_ms, std::memory_order_relaxed);
    hot_frame_jitter_ms.store(frame_jitter_ms, std::memory_order_relaxed);
  }

  void update_capture_pacing(const std::string &pacing) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.capture_pacing = pacing;
  }

  void update_runtime_display_warning(const std::string &warning) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.runtime_display_warning = warning;
  }

  void update_capture_metadata(const platf::frame_metadata_t &metadata) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
    const bool policy_input_changed =
      doctor_video_policy_state.capture_transport != metadata.transport ||
      doctor_video_policy_state.capture_residency != metadata.residency;
    doctor_video_policy_state.capture_transport = metadata.transport;
    doctor_video_policy_state.capture_residency = metadata.residency;
    if (policy_input_changed) {
      publish_doctor_video_policy_locked();
    }
    // Keep the action-visible capture path under both locks until the policy
    // transition is fully published. No status/action snapshot can bind the
    // new controller revision to the old capture metadata.
    current_stats.capture_transport = metadata.transport;
    current_stats.capture_residency = metadata.residency;
    current_stats.capture_format = metadata.format;
    current_stats.capture_device = metadata.device;
  }

  void update_wayland_main_device(const std::string &device) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.wayland_main_device = device;
  }

  void update_vaapi_vendor(const std::string &vendor) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.vaapi_vendor = vendor;
  }

  void reset_gpu_native_probe(bool requested, bool reset_capture_identity) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (reset_capture_identity) {
      current_stats.capture_device.clear();
      current_stats.wayland_main_device.clear();
    }
    current_stats.gpu_native_probe = gpu_native_probe_t {};
    current_stats.gpu_native_probe.requested = requested;
  }

  void update_gpu_native_probe_attempt(const std::string &strategy,
                                       const std::string &result,
                                       const std::string &failure_stage,
                                       const std::string &failure_reason,
                                       bool cached) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (!current_stats.gpu_native_probe.requested) {
      return;
    }
    auto *attempt = strategy == "headless_extcopy" ?
      &current_stats.gpu_native_probe.headless_extcopy :
      strategy == "windowed" ? &current_stats.gpu_native_probe.windowed : nullptr;
    if (!attempt) {
      return;
    }
    attempt->attempted = !cached && result != "ineligible" && result != "not_attempted";
    attempt->cached = cached;
    attempt->result = result;
    attempt->failure_stage = failure_stage;
    attempt->failure_reason = failure_reason;
  }

  void update_gpu_native_probe_selection(const std::string &selected_strategy,
                                         const std::string &fallback) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    if (!current_stats.gpu_native_probe.requested) {
      return;
    }
    current_stats.gpu_native_probe.selected_strategy = selected_strategy;
    current_stats.gpu_native_probe.fallback = fallback;
  }

  void update_encode_path_metadata(const std::string &target_device,
                                   platf::frame_residency_e target_residency,
                                   platf::frame_format_e target_format) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
    const bool policy_input_changed =
      doctor_video_policy_state.encode_target_residency != target_residency;
    doctor_video_policy_state.encode_target_residency = target_residency;
    if (policy_input_changed) {
      publish_doctor_video_policy_locked();
    }
    current_stats.encode_target_device = target_device;
    current_stats.encode_target_residency = target_residency;
    current_stats.encode_target_format = target_format;
  }

  void update_dynamic_range(int dynamic_range) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.dynamic_range = dynamic_range;
  }

  void update_client_declared_controller_type(int controller_type) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.client_declared_controller_type = controller_type;
  }

  int client_declared_controller_type() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return current_stats.client_declared_controller_type;
  }

  void update_hdr_policy(bool hdr, const std::string &reason_code, const std::string &device) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.hdr_policy_hdr = hdr;
    current_stats.hdr_policy_reason = reason_code;
    current_stats.hdr_policy_device = device;
  }

  void update_hdr_state(bool display_hdr,
                        bool hdr_metadata_available,
                        bool stream_hdr_enabled,
                        const std::string &color_coding) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.display_hdr = display_hdr;
    current_stats.hdr_metadata_available = hdr_metadata_available;
    current_stats.stream_hdr_enabled = stream_hdr_enabled;
    current_stats.color_coding = color_coding;
  }

  void update_controller_input_state(bool virtual_controller_created,
                                     int virtual_controller_number,
                                     const std::string &virtual_controller_kind,
                                     const std::string &virtual_controller_error,
                                     const std::string &host_controller_isolation,
                                     const std::string &host_controller_isolation_detail,
                                     bool haptics_supported,
                                     const std::string &haptics_detail) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.input_virtual_controller_created = virtual_controller_created;
    current_stats.input_virtual_controller_number = virtual_controller_number;
    current_stats.input_virtual_controller_kind = virtual_controller_kind;
    current_stats.input_virtual_controller_error = virtual_controller_error;
    current_stats.input_host_controller_isolation = host_controller_isolation.empty() ? "unknown" : host_controller_isolation;
    current_stats.input_host_controller_isolation_detail = host_controller_isolation_detail;
    current_stats.input_haptics_supported = haptics_supported;
    current_stats.input_haptics_detail = haptics_detail;
  }

  void note_virtual_pad(int global_index, int controller_number, const std::string &kind) {
    // Players are numbered by when their pad appeared, the order a game enumerates them in.
    // Controller numbers restart at 0 in every session, so two clients with a pad each both
    // held controller 0 and both read as player 1.
    static std::uint64_t pads_created = 0;
    std::lock_guard<std::mutex> lock(stats_mutex);
    auto &pads = current_stats.input_virtual_pads;
    std::erase_if(pads, [&](const virtual_pad_t &pad) {
      return pad.global_index == global_index;
    });
    pads.push_back({global_index, controller_number, kind, ++pads_created});
  }

  void forget_virtual_pad(int global_index) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    std::erase_if(current_stats.input_virtual_pads, [&](const virtual_pad_t &pad) {
      return pad.global_index == global_index;
    });
  }

  void update_steam_input_state(const std::string &status,
                                int profiles_checked,
                                int profiles_with_xbox_support,
                                int forced_app_count,
                                const std::string &detail) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.input_steam_input_status = status.empty() ? "unknown" : status;
    current_stats.input_steam_profiles_checked = std::max(0, profiles_checked);
    current_stats.input_steam_profiles_with_xbox_support = std::max(0, profiles_with_xbox_support);
    current_stats.input_steam_forced_app_count = std::max(0, forced_app_count);
    current_stats.input_steam_input_detail = detail;
  }

  void update_capture_profile(const capture_profile_sample_t &sample) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    auto &bucket = capture_profile_bucket(sample.transport);
    bucket.dispatch_us.push_back(sample.dispatch_time.count());
    bucket.ingest_us.push_back(sample.ingest_time.count());
    bucket.total_us.push_back(sample.total_time.count());
    if (sample.source_interval) {
      bucket.source_interval_us.push_back(sample.source_interval->count());
    }
    if (sample.ready_to_handoff) {
      bucket.ready_to_handoff_us.push_back(sample.ready_to_handoff->count());
    }

    if (bucket.total_us.size() < CAPTURE_PROFILE_SUMMARY_FRAMES) {
      return;
    }

    BOOST_LOG(info) << "capture_telemetry: transport="sv << platf::from_frame_transport(sample.transport)
                    << " frames="sv << bucket.total_us.size()
                    << " dispatch_us_p50="sv << percentile_value(bucket.dispatch_us, 0.50)
                    << " dispatch_us_p99="sv << percentile_value(bucket.dispatch_us, 0.99)
                    << " ingest_us_p50="sv << percentile_value(bucket.ingest_us, 0.50)
                    << " ingest_us_p99="sv << percentile_value(bucket.ingest_us, 0.99)
                    << " total_us_p50="sv << percentile_value(bucket.total_us, 0.50)
                    << " total_us_p99="sv << percentile_value(bucket.total_us, 0.99)
                    << " source_interval_samples="sv << bucket.source_interval_us.size()
                    << " source_interval_us_p50="sv
                    << (bucket.source_interval_us.empty() ? -1 : percentile_value(bucket.source_interval_us, 0.50))
                    << " source_interval_us_p99="sv
                    << (bucket.source_interval_us.empty() ? -1 : percentile_value(bucket.source_interval_us, 0.99))
                    << " ready_to_handoff_samples="sv << bucket.ready_to_handoff_us.size()
                    << " ready_to_handoff_us_p50="sv
                    << (bucket.ready_to_handoff_us.empty() ? -1 : percentile_value(bucket.ready_to_handoff_us, 0.50))
                    << " ready_to_handoff_us_p99="sv
                    << (bucket.ready_to_handoff_us.empty() ? -1 : percentile_value(bucket.ready_to_handoff_us, 0.99));

    bucket.dispatch_us.clear();
    bucket.ingest_us.clear();
    bucket.total_us.clear();
    bucket.source_interval_us.clear();
    bucket.ready_to_handoff_us.clear();
  }

  void start_session_timing(const std::string &device_uuid,
                            std::uint64_t session_generation,
                            std::string session_token) {
    std::lock_guard<std::mutex> ingest_lock(client_media_ingest_mutex);
    std::lock_guard<std::mutex> lock(frame_timing_mutex);
    session_timings.erase(
      std::remove_if(session_timings.begin(), session_timings.end(),
        [&device_uuid](const session_timing_state_t &s) { return s.device_uuid == device_uuid; }),
      session_timings.end());

    session_timing_state_t state;
    state.device_uuid = device_uuid;
    state.session_generation = session_generation;
    state.session_token = std::move(session_token);
    session_timings.push_back(std::move(state));
  }

  void stop_session_timing(const std::string &device_uuid, std::uint64_t session_generation) {
    std::lock_guard<std::mutex> ingest_lock(client_media_ingest_mutex);
    std::lock_guard<std::mutex> lock(frame_timing_mutex);
    session_timings.erase(
      std::remove_if(session_timings.begin(), session_timings.end(),
        [&device_uuid, session_generation](const session_timing_state_t &s) {
          return s.device_uuid == device_uuid && s.session_generation == session_generation;
        }),
      session_timings.end());
  }

  void record_frame_timing(const std::string &device_uuid,
                           std::uint64_t session_generation,
                           std::chrono::steady_clock::time_point capture_time,
                           std::chrono::steady_clock::time_point encode_done_time,
                           std::chrono::steady_clock::time_point send_time) {
    std::lock_guard<std::mutex> lock(frame_timing_mutex);
    auto *state = find_session_timing_locked(device_uuid);
    if (!state || state->session_generation != session_generation) {
      return;
    }

    auto record_stage = [](timing_ring_t &ring, std::chrono::steady_clock::duration d) {
      if (d < std::chrono::steady_clock::duration::zero()) {
        ring.invalid_count++;
        return;
      }
      ring.push(std::chrono::duration<double, std::milli>(d).count());
    };

    record_stage(state->capture_to_encode_ring, encode_done_time - capture_time);
    record_stage(state->encode_to_send_ring, send_time - encode_done_time);
    record_stage(state->capture_to_send_ring, send_time - capture_time);
  }

  session_timing_t get_session_timing(const std::string &device_uuid) {
    std::lock_guard<std::mutex> lock(frame_timing_mutex);
    auto *state = find_session_timing_locked(device_uuid);
    if (!state) {
      return {};
    }

    session_timing_t result {
      state->capture_to_encode_ring.percentiles(),
      state->encode_to_send_ring.percentiles(),
      state->capture_to_send_ring.percentiles()
    };
    result.session_generation = state->session_generation;
    result.session_active = true;
    result.ring_complete = state->capture_to_encode_ring.filled < FRAME_TIMING_RING_CAPACITY;
    return result;
  }

  std::optional<active_session_identity_t> get_single_active_session_identity() {
    std::lock_guard<std::mutex> lock(frame_timing_mutex);
    if (session_timings.size() != 1) {
      return std::nullopt;
    }
    return active_session_identity_t {
      session_timings.front().device_uuid,
      session_timings.front().session_generation,
      session_timings.front().session_token
    };
  }

  void bind_doctor_action_scope(nlohmann::json &doctor,
                                std::string_view app_session_id,
                                std::uint64_t session_generation,
                                std::uint64_t action_authority_revision,
                                std::uint64_t network_evidence_revision,
                                std::uint64_t video_evidence_revision) {
    if (!doctor.is_object() || app_session_id.empty() || session_generation == 0) {
      return;
    }
    auto action = doctor.find("safe_recovery_action");
    if (action == doctor.end() || !action->is_object()) return;
    auto payload = action->find("payload_preview");
    if (payload == action->end() || !payload->is_object()) return;
    const auto action_id = action->value("id", std::string {});
    if (action_id != "lower_bitrate" && action_id != "restore_quality" &&
        action_id != "recheck_network" && action_id != "recheck_pacing") {
      return;
    }
    (*payload)["app_session_id"] = app_session_id;
    (*payload)["session_generation"] = session_generation;
    if (action_id == "lower_bitrate" || action_id == "restore_quality") {
      (*payload)["controller_revision"] = action_authority_revision;
      (*payload)["evidence_revision"] = network_evidence_revision;
    }
    (void) video_evidence_revision;
  }

  // ---------------------------------------------------------------------
  // P0-5 benchmark-run-capture engine, piece 1: boundary classification and
  // bounded per-stage capture. See the section comment in stream_stats.h.
  // ---------------------------------------------------------------------

  boundary_classification_e classify_boundary(std::int64_t a_offset_us, std::int64_t b_offset_us, std::int64_t window_end_us) {
    if (a_offset_us < 0) {
      return boundary_classification_e::excluded_before_window;
    }
    if (a_offset_us >= window_end_us) {
      return boundary_classification_e::ignored_post_window;
    }
    if (b_offset_us >= window_end_us) {
      return boundary_classification_e::excluded_after_window;
    }
    if (a_offset_us <= b_offset_us) {
      return boundary_classification_e::accepted;
    }
    return boundary_classification_e::invalid_non_monotonic;
  }

  benchmark_stage_capture_t::benchmark_stage_capture_t(std::size_t sample_capacity):
      capacity(sample_capacity) {
    start_offset_us.reserve(capacity);
    end_offset_us.reserve(capacity);
    duration_us.reserve(capacity);
  }

  boundary_classification_e benchmark_stage_capture_t::record(std::int64_t a_offset_us, std::int64_t b_offset_us, std::int64_t window_end_us) {
    const auto classification = classify_boundary(a_offset_us, b_offset_us, window_end_us);
    switch (classification) {
      case boundary_classification_e::excluded_before_window:
        excluded_started_before_window++;
        break;
      case boundary_classification_e::excluded_after_window:
        excluded_completed_after_window++;
        break;
      case boundary_classification_e::ignored_post_window:
        // No state created - matches 6.5's "do not create run-owned
        // in-flight state" for this case.
        break;
      case boundary_classification_e::invalid_non_monotonic:
        invalid_count++;
        break;
      case boundary_classification_e::accepted:
        if (accepted_count >= capacity) {
          overflow_count++;
          break;
        }
        start_offset_us.push_back(static_cast<std::uint32_t>(a_offset_us));
        end_offset_us.push_back(static_cast<std::uint32_t>(b_offset_us));
        duration_us.push_back(static_cast<std::uint32_t>(b_offset_us - a_offset_us));
        accepted_count++;
        break;
    }
    return classification;
  }

  namespace {
    constexpr std::size_t MAX_TERMINAL_BENCHMARK_RUNS = 4;
    constexpr auto BENCHMARK_RUN_TTL = std::chrono::minutes(30);

    bool is_terminal_benchmark_run(const benchmark_run_t &run) {
      return run.state == benchmark_run_state_e::frozen || run.state == benchmark_run_state_e::aborted;
    }

    void clear_benchmark_run_payload(benchmark_run_t &run) {
      auto clear_stage = [](benchmark_stage_capture_t &stage) {
        stage.start_offset_us.clear();
        stage.start_offset_us.shrink_to_fit();
        stage.end_offset_us.clear();
        stage.end_offset_us.shrink_to_fit();
        stage.duration_us.clear();
        stage.duration_us.shrink_to_fit();
      };
      clear_stage(run.capture_to_encode);
      clear_stage(run.encode_to_send_release);
      clear_stage(run.capture_to_send_release);
    }

    // Caller must hold benchmark_run_mutex.
    void expire_benchmark_run_locked(benchmark_run_t &run) {
      run.state = benchmark_run_state_e::expired;
      clear_benchmark_run_payload(run);
    }

    // Caller must hold benchmark_run_mutex. Repeatedly expires the oldest
    // (by frozen_monotonic) terminal run until at most
    // MAX_TERMINAL_BENCHMARK_RUNS remain - measurement-spec-v1.md 6.4:
    // "when a fifth retained payload would be created, the oldest
    // non-active payload expires before the new payload is accepted."
    void enforce_terminal_retention_locked(std::vector<benchmark_run_t> &runs) {
      for (;;) {
        benchmark_run_t *oldest = nullptr;
        std::size_t terminal_count = 0;

        for (auto &run : runs) {
          if (!is_terminal_benchmark_run(run)) {
            continue;
          }
          terminal_count++;
          if (!oldest ||
              !oldest->frozen_monotonic ||
              (run.frozen_monotonic && *run.frozen_monotonic < *oldest->frozen_monotonic)) {
            oldest = &run;
          }
        }

        if (terminal_count <= MAX_TERMINAL_BENCHMARK_RUNS || !oldest) {
          break;
        }
        expire_benchmark_run_locked(*oldest);
      }
    }
  }  // namespace

  static std::mutex benchmark_run_mutex;
  static std::vector<benchmark_run_t> benchmark_runs;

  const std::string &process_instance_id() {
    static const std::string id = [] {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      return "polaris-" + std::to_string(ns);
    }();
    return id;
  }

  void insert_benchmark_run(benchmark_run_t run) {
    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    benchmark_runs.push_back(std::move(run));
    enforce_terminal_retention_locked(benchmark_runs);
  }

  bool with_benchmark_run(const std::string &run_id, const std::function<void(benchmark_run_t &)> &fn) {
    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    auto it = std::find_if(benchmark_runs.begin(), benchmark_runs.end(),
      [&run_id](const benchmark_run_t &r) { return r.run_id == run_id; });
    if (it == benchmark_runs.end()) {
      return false;
    }
    fn(*it);
    return true;
  }

  void erase_benchmark_run(const std::string &run_id) {
    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    benchmark_runs.erase(
      std::remove_if(benchmark_runs.begin(), benchmark_runs.end(),
        [&run_id](const benchmark_run_t &r) { return r.run_id == run_id; }),
      benchmark_runs.end());
  }

  void expire_benchmark_run(const std::string &run_id) {
    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    auto it = std::find_if(benchmark_runs.begin(), benchmark_runs.end(),
      [&run_id](const benchmark_run_t &r) { return r.run_id == run_id; });
    if (it != benchmark_runs.end()) {
      expire_benchmark_run_locked(*it);
    }
  }

  void expire_stale_benchmark_runs() {
    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto &run : benchmark_runs) {
      if (!is_terminal_benchmark_run(run) || !run.frozen_monotonic) {
        continue;
      }
      if (now - *run.frozen_monotonic > BENCHMARK_RUN_TTL) {
        expire_benchmark_run_locked(run);
      }
    }
  }

  namespace {
    std::atomic<bool> benchmark_control_plane_enabled_flag {false};

    bool is_valid_manifest_sha256(const std::string &value) {
      if (value.size() != 64) {
        return false;
      }
      return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
    }
  }  // namespace

  bool benchmark_control_plane_enabled() {
    return benchmark_control_plane_enabled_flag.load(std::memory_order_relaxed);
  }

  void set_benchmark_control_plane_enabled(bool enabled) {
    benchmark_control_plane_enabled_flag.store(enabled, std::memory_order_relaxed);
  }

  benchmark_run_create_result_e create_benchmark_run(
      const benchmark_run_create_request_t &request,
      const std::string &device_uuid,
      std::uint64_t session_generation,
      bool caller_is_authorized_harness) {
    // Cheapest / least-sensitive-information-first ordering. None of these
    // preconditions require benchmark_run_mutex - only the final
    // session-dedup + run_id-dedup + insert step does, taken once below so
    // that check-then-insert can't race a concurrent create call.
    if (!benchmark_control_plane_enabled()) {
      return benchmark_run_create_result_e::rejected_control_plane_not_enabled;
    }
    if (!caller_is_authorized_harness) {
      return benchmark_run_create_result_e::rejected_caller_not_authorized_as_harness;
    }
    if (active_client_count() != 1) {
      return benchmark_run_create_result_e::rejected_not_exactly_one_active_session;
    }
    if (request.expected_duration_s < 60 || request.expected_duration_s > 180) {
      return benchmark_run_create_result_e::rejected_duration_out_of_range;
    }
    if (request.duration_tolerance_ms < 0 || request.duration_tolerance_ms > 1000) {
      return benchmark_run_create_result_e::rejected_duration_tolerance_out_of_range;
    }
    if (request.drain_grace_ms < 1 || request.drain_grace_ms > 5000) {
      return benchmark_run_create_result_e::rejected_drain_grace_out_of_range;
    }
    if (request.target_fps < 30 || request.target_fps > 240) {
      return benchmark_run_create_result_e::rejected_target_fps_out_of_range;
    }

    // measurement-spec-v1.md 6.4: "nominal sample budget expected_duration_s
    // * target_fps of at least 1,000 frames". Given the range floors just
    // checked above (60s, 30fps -> 1800), this can't currently fail - kept
    // anyway as a direct, defensive expression of the spec's own precondition
    // rather than an assumption that the range floors will never change
    // independently (e.g. if either becomes config-driven later).
    const std::uint64_t nominal_sample_budget =
      static_cast<std::uint64_t>(request.expected_duration_s) * static_cast<std::uint64_t>(request.target_fps);
    if (nominal_sample_budget < 1000) {
      return benchmark_run_create_result_e::rejected_nominal_sample_budget_too_small;
    }
    if (request.sample_capacity_frames < nominal_sample_budget) {
      return benchmark_run_create_result_e::rejected_capacity_below_nominal_budget;
    }
    if (request.sample_capacity_frames > 65536) {
      return benchmark_run_create_result_e::rejected_capacity_exceeds_maximum;
    }
    if (!is_valid_manifest_sha256(request.manifest_sha256)) {
      return benchmark_run_create_result_e::rejected_invalid_manifest_sha256_format;
    }

    // Not checked here (documented gap, matching this engine's established
    // pattern - see the header comment on this function): "duration,
    // tolerance, drain grace, target fps, capacity, and workload exactly
    // match the frozen manifest". There is no manifest file infrastructure
    // yet for this function to check against; a later piece owns it.

    std::lock_guard<std::mutex> lock(benchmark_run_mutex);

    const bool session_already_has_active_run = std::any_of(
      benchmark_runs.begin(), benchmark_runs.end(),
      [&device_uuid](const benchmark_run_t &r) {
        return r.owning_device_uuid == device_uuid &&
               (r.state == benchmark_run_state_e::armed ||
                r.state == benchmark_run_state_e::active ||
                r.state == benchmark_run_state_e::draining);
      });
    if (session_already_has_active_run) {
      return benchmark_run_create_result_e::rejected_session_already_has_an_active_run;
    }

    const bool run_id_already_used = std::any_of(
      benchmark_runs.begin(), benchmark_runs.end(),
      [&request](const benchmark_run_t &r) { return r.run_id == request.run_id; });
    if (run_id_already_used) {
      return benchmark_run_create_result_e::rejected_run_id_already_used;
    }

    benchmark_run_t run(request.sample_capacity_frames);
    run.run_id = request.run_id;
    run.state = benchmark_run_state_e::armed;
    run.owning_device_uuid = device_uuid;
    run.owning_session_generation = session_generation;
    run.manifest_sha256 = request.manifest_sha256;
    run.label = request.label;
    run.workload_id = request.workload_id;
    run.expected_duration_ns = std::chrono::seconds(request.expected_duration_s);
    run.duration_tolerance_ns = std::chrono::milliseconds(request.duration_tolerance_ms);
    run.drain_grace_ns = std::chrono::milliseconds(request.drain_grace_ms);
    run.target_fps = request.target_fps;
    run.armed_monotonic = std::chrono::steady_clock::now();
    run.client_population_revision_at_arm = client_population_revision();

    benchmark_runs.push_back(std::move(run));
    enforce_terminal_retention_locked(benchmark_runs);

    return benchmark_run_create_result_e::created;
  }

  namespace {
    // Caller must hold benchmark_run_mutex. Returns none if run is not
    // currently active or draining, or if reality still matches what was
    // true at arm time.
    benchmark_abort_reason_e detect_abort_trigger_locked(const benchmark_run_t &run) {
      const auto timing = get_session_timing(run.owning_device_uuid);
      if (!timing.session_active) {
        return benchmark_abort_reason_e::session_ended;
      }
      if (timing.session_generation != run.owning_session_generation) {
        return benchmark_abort_reason_e::session_generation_changed;
      }
      if (client_population_revision() != run.client_population_revision_at_arm) {
        return benchmark_abort_reason_e::client_population_revision_changed;
      }
      return benchmark_abort_reason_e::none;
    }

    // Caller must hold benchmark_run_mutex. Reconciles run's state with
    // reality before whichever operation invoked this evaluates its own
    // preconditions - see start_benchmark_run's header comment for why
    // this exists instead of a background timer. Transitions can cascade
    // (active -> draining -> frozen) within a single call if nobody
    // touched this run for long enough that both deadlines already
    // passed.
    void apply_lazy_transitions_locked(benchmark_run_t &run) {
      const auto now = std::chrono::steady_clock::now();

      if (run.state == benchmark_run_state_e::active || run.state == benchmark_run_state_e::draining) {
        const auto reason = detect_abort_trigger_locked(run);
        if (reason != benchmark_abort_reason_e::none) {
          run.state = benchmark_run_state_e::aborted;
          run.abort_reason = reason;
          run.frozen_monotonic = now;
          enforce_terminal_retention_locked(benchmark_runs);
          return;
        }
      }

      if (run.state == benchmark_run_state_e::active && run.started_monotonic &&
          now >= *run.started_monotonic + run.expected_duration_ns) {
        // The declared deadline, not "now" - so actual_duration_ns reflects
        // the run's own contract rather than however long it took for some
        // caller to next touch this run and trigger this reconciliation.
        run.stopped_monotonic = *run.started_monotonic + run.expected_duration_ns;
        run.state = benchmark_run_state_e::draining;
      }

      if (run.state == benchmark_run_state_e::draining && run.stopped_monotonic &&
          now >= *run.stopped_monotonic + run.drain_grace_ns) {
        run.frozen_monotonic = now;
        run.client_population_revision_at_freeze = client_population_revision();
        run.state = benchmark_run_state_e::frozen;
        enforce_terminal_retention_locked(benchmark_runs);
      }
    }
  }  // namespace

  benchmark_run_start_result_e start_benchmark_run(
      const std::string &run_id,
      const std::string &device_uuid,
      std::uint64_t session_generation,
      bool caller_is_authorized_harness) {
    if (!benchmark_control_plane_enabled()) {
      return benchmark_run_start_result_e::rejected_control_plane_not_enabled;
    }
    if (!caller_is_authorized_harness) {
      return benchmark_run_start_result_e::rejected_caller_not_authorized_as_harness;
    }

    auto result = benchmark_run_start_result_e::rejected_run_not_found;
    const bool found = with_benchmark_run(run_id, [&](benchmark_run_t &run) {
      apply_lazy_transitions_locked(run);

      if (run.owning_device_uuid != device_uuid || run.owning_session_generation != session_generation) {
        result = benchmark_run_start_result_e::rejected_wrong_session;
        return;
      }
      if (run.state != benchmark_run_state_e::armed) {
        result = benchmark_run_start_result_e::rejected_run_not_in_armed_state;
        return;
      }
      if (client_population_revision() != run.client_population_revision_at_arm) {
        result = benchmark_run_start_result_e::rejected_population_changed_since_arm;
        return;
      }
      if (!get_session_timing(device_uuid).session_active) {
        result = benchmark_run_start_result_e::rejected_session_no_longer_active;
        return;
      }

      run.started_monotonic = std::chrono::steady_clock::now();
      run.state = benchmark_run_state_e::active;
      result = benchmark_run_start_result_e::started;
    });

    return found ? result : benchmark_run_start_result_e::rejected_run_not_found;
  }

  benchmark_run_stop_result_e stop_benchmark_run(
      const std::string &run_id,
      const std::string &device_uuid,
      std::uint64_t session_generation,
      bool caller_is_authorized_harness) {
    if (!benchmark_control_plane_enabled()) {
      return benchmark_run_stop_result_e::rejected_control_plane_not_enabled;
    }
    if (!caller_is_authorized_harness) {
      return benchmark_run_stop_result_e::rejected_caller_not_authorized_as_harness;
    }

    auto result = benchmark_run_stop_result_e::rejected_run_not_found;
    const bool found = with_benchmark_run(run_id, [&](benchmark_run_t &run) {
      apply_lazy_transitions_locked(run);

      if (run.owning_device_uuid != device_uuid || run.owning_session_generation != session_generation) {
        result = benchmark_run_stop_result_e::rejected_wrong_session;
        return;
      }
      // started_monotonic is always set alongside state=active by
      // start_benchmark_run - the "|| !run.started_monotonic" half is
      // unreachable through any real call path, kept only so the
      // dereference just below can never be undefined behavior if that
      // invariant is ever violated (matches apply_lazy_transitions_locked's
      // own defensive style on the same optional fields just above).
      if (run.state != benchmark_run_state_e::active || !run.started_monotonic) {
        result = benchmark_run_stop_result_e::rejected_not_currently_active;
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = now - *run.started_monotonic;
      const auto lower_bound = run.expected_duration_ns - run.duration_tolerance_ns;

      if (elapsed < lower_bound) {
        run.state = benchmark_run_state_e::aborted;
        run.abort_reason = benchmark_abort_reason_e::stopped_before_duration_lower_bound;
        run.frozen_monotonic = now;
        enforce_terminal_retention_locked(benchmark_runs);
        result = benchmark_run_stop_result_e::stopped_early_and_aborted;
        return;
      }

      run.stopped_monotonic = now;
      run.state = benchmark_run_state_e::draining;
      result = benchmark_run_stop_result_e::stopped;
    });

    return found ? result : benchmark_run_stop_result_e::rejected_run_not_found;
  }

  benchmark_run_get_result_e get_benchmark_run(
      const std::string &run_id,
      bool caller_is_authorized_harness,
      const std::function<void(benchmark_run_t &)> &fn) {
    if (!benchmark_control_plane_enabled()) {
      return benchmark_run_get_result_e::rejected_control_plane_not_enabled;
    }
    if (!caller_is_authorized_harness) {
      return benchmark_run_get_result_e::rejected_caller_not_authorized_as_harness;
    }

    const bool found = with_benchmark_run(run_id, [&](benchmark_run_t &run) {
      apply_lazy_transitions_locked(run);
      fn(run);
    });

    return found ? benchmark_run_get_result_e::found : benchmark_run_get_result_e::rejected_run_not_found;
  }

  benchmark_run_delete_result_e delete_benchmark_run(
      const std::string &run_id,
      bool caller_is_authorized_harness) {
    if (!benchmark_control_plane_enabled()) {
      return benchmark_run_delete_result_e::rejected_control_plane_not_enabled;
    }
    if (!caller_is_authorized_harness) {
      return benchmark_run_delete_result_e::rejected_caller_not_authorized_as_harness;
    }

    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    const auto before = benchmark_runs.size();
    benchmark_runs.erase(
      std::remove_if(benchmark_runs.begin(), benchmark_runs.end(),
        [&run_id](const benchmark_run_t &r) { return r.run_id == run_id; }),
      benchmark_runs.end());

    return benchmark_runs.size() < before
      ? benchmark_run_delete_result_e::deleted
      : benchmark_run_delete_result_e::rejected_run_not_found;
  }

  void record_benchmark_sample(const std::string &device_uuid,
                                std::uint64_t session_generation,
                                std::chrono::steady_clock::time_point capture_time,
                                std::chrono::steady_clock::time_point encode_done_time,
                                std::chrono::steady_clock::time_point send_time) {
    // Cheap common-case exit before touching benchmark_run_mutex at all -
    // today (no control surface exists yet to ever flip this on) every
    // real call takes this path, so the hot-path cost of the whole P0-5
    // engine is one relaxed atomic load until that surface ships.
    if (!benchmark_control_plane_enabled()) {
      return;
    }

    std::lock_guard<std::mutex> lock(benchmark_run_mutex);
    auto it = std::find_if(benchmark_runs.begin(), benchmark_runs.end(),
      [&device_uuid, session_generation](const benchmark_run_t &r) {
        return (r.state == benchmark_run_state_e::active || r.state == benchmark_run_state_e::draining) &&
               r.owning_device_uuid == device_uuid &&
               r.owning_session_generation == session_generation;
      });
    if (it == benchmark_runs.end() || !it->started_monotonic) {
      return;
    }

    const auto start = *it->started_monotonic;
    const auto window_end_us = std::chrono::duration_cast<std::chrono::microseconds>(it->expected_duration_ns).count();
    const auto a_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(capture_time - start).count();
    const auto b_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(encode_done_time - start).count();
    const auto c_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(send_time - start).count();

    it->capture_to_encode.record(a_offset_us, b_offset_us, window_end_us);
    it->encode_to_send_release.record(b_offset_us, c_offset_us, window_end_us);
    it->capture_to_send_release.record(a_offset_us, c_offset_us, window_end_us);
  }

  int active_client_count() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return static_cast<int>(current_stats.clients.size());
  }

  std::uint64_t client_population_revision() {
    return client_population_revision_counter.load(std::memory_order_relaxed);
  }

  void record_idr_request() {
    hot_idr_requests_total.fetch_add(1, std::memory_order_relaxed);
  }

  void record_invalidate_ref_frames_request() {
    hot_invalidate_ref_frames_requests_total.fetch_add(1, std::memory_order_relaxed);
  }

  stats_t get_current() {
    stats_t result;
    const auto adaptive_state = adaptive_bitrate::get_state();
    {
      std::lock_guard<std::mutex> lock(stats_mutex);
      const auto target_bitrate = adaptive_state.target_bitrate_kbps;
      current_stats.adaptive_target_bitrate_kbps = target_bitrate;
      current_stats.adaptive_bitrate_enabled = adaptive_state.enabled;
      current_stats.adaptive_bitrate_active = adaptive_state.active;
      current_stats.adaptive_bitrate_state = adaptive_state.state;
      current_stats.adaptive_runtime_update_supported = adaptive_state.runtime_update_supported;
      current_stats.adaptive_min_bitrate_kbps = adaptive_state.min_bitrate_kbps;
      current_stats.adaptive_floor_source = adaptive_state.floor_source;

      // Also update adaptive bitrate for all clients
      for (auto &c : current_stats.clients) {
        c.adaptive_target_bitrate_kbps = target_bitrate;
      }

      result = current_stats;
      result.last_session = last_ended_session;
    }
    if (const auto identity = get_single_active_session_identity()) {
      result.session_generation = identity->session_generation;
      result.app_session_id = identity->session_token;
    }

    // Doctor-policy video fields are read under the same narrow lock used to
    // publish a policy-class transition. Other hot fields remain independent
    // relaxed telemetry outside stats_mutex.
    result.bitrate_kbps = hot_bitrate_kbps.load(std::memory_order_relaxed);
    result.codec = codec_from_id(hot_codec_id.load(std::memory_order_relaxed));
    result.width = hot_width.load(std::memory_order_relaxed);
    result.height = hot_height.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> policy_lock(doctor_video_policy_mutex);
      result.fps = hot_fps.load(std::memory_order_relaxed);
      result.encode_time_ms = hot_encode_time_ms.load(std::memory_order_relaxed);
      result.duplicate_frame_ratio = hot_duplicate_frame_ratio.load(std::memory_order_relaxed);
      result.dropped_frame_ratio = hot_dropped_frame_ratio.load(std::memory_order_relaxed);
      result.avg_frame_age_ms = hot_avg_frame_age_ms.load(std::memory_order_relaxed);
      result.frame_jitter_ms = hot_frame_jitter_ms.load(std::memory_order_relaxed);
      result.capture_source_fps = hot_capture_source_fps.load(std::memory_order_relaxed);
      result.video_sample_revision = hot_video_sample_revision.load(std::memory_order_acquire);
      result.video_policy_sample_count = doctor_video_policy_state.sample_count;
      result.pacing_warning_streak = doctor_video_policy_state.pacing_warning_streak;
    }
    {
      // Keep the complete network group on one host-received observation.
      // Doctor must never pair a new revision with stale loss/RTT fields.
      std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
      result.network_verdict = network_judge.verdict(std::chrono::steady_clock::now());
      if (!primary_network_observations.empty()) {
        const auto &network = primary_network_observations.back();
        result.latency_ms = network.latency_ms;
        result.packet_loss = network.packet_loss;
        result.packet_loss_available = network.packet_loss_available;
        result.control_channel_packet_loss = network.control_channel_packet_loss;
        result.control_channel_samples = network.control_channel_samples;
        result.network_sample_revision = network.revision;
        result.network_last_received_age_ms = std::max<std::int64_t>(
          0,
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - network.received_at
          ).count()
        );
        result.media_loss_sample_revision = network.media_loss_revision;
        if (network.media_loss_received_at != std::chrono::steady_clock::time_point {}) {
          result.media_loss_last_received_age_ms = std::max<std::int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - network.media_loss_received_at
            ).count()
          );
        }
        result.network_risk = network.network_risk;
        result.bytes_sent = network.bytes_sent;
      } else {
        result.network_sample_revision = hot_network_sample_revision.load(std::memory_order_acquire);
      }
    }
    result.packet_loss_source = result.packet_loss_available ? "media_transport" : "unavailable";
    result.idr_requests_total = hot_idr_requests_total.load(std::memory_order_relaxed);
    result.invalidate_ref_frames_requests_total = hot_invalidate_ref_frames_requests_total.load(std::memory_order_relaxed);
    result.doctor_live_action_scope_available =
      hot_doctor_live_action_scope_available.load(std::memory_order_acquire);

    return result;
  }

}  // namespace stream_stats
