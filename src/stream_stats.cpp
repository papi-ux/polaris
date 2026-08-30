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
#include <deque>
#include <filesystem>
#include <mutex>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "adaptive_bitrate.h"
#include "config.h"
#include "crypto.h"
#include "logging.h"
#include "stream_stats.h"
#include "utility.h"
#include "verified_action.h"
#ifdef __linux__
  #include "platform/linux/stream_runtime.h"
  #include "platform/linux/stream_display_policy.h"
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

  static std::mutex stats_mutex;
  static stats_t current_stats;

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
    std::atomic<int> hot_codec_id {-1};  // -1=unset, 0=h264, 1=hevc, 2=av1
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
    };
    doctor_video_policy_state_t doctor_video_policy_state;
    std::mutex doctor_video_policy_mutex;

    bool video_policy_suppresses_quality_restore(
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
      const bool pacing_watch =
        sample.dropped_frame_ratio >= 0.04 ||
        (!static_or_duplicate_content &&
         (sample.frame_jitter_ms >= 2.2 ||
          (is_meaningful_fps_shortfall(
             sample.target_fps,
             sample.delivered_fps
           ) && source_cadence_confirms_motion)));
      const bool capture_cpu_copy =
        sample.capture_transport == platf::frame_transport_e::shm ||
        sample.capture_residency == platf::frame_residency_e::cpu ||
        sample.encode_target_residency == platf::frame_residency_e::cpu;
      const bool encoder_watch =
        sample.encode_time_ms >= 8.0 || sample.avg_frame_age_ms >= 18.0;
      return capture_cpu_copy || encoder_watch || pacing_watch;
    }

    void publish_doctor_video_policy_locked() {
      adaptive_bitrate::note_doctor_video_policy_evidence(
        video_policy_suppresses_quality_restore(doctor_video_policy_state)
      );
    }

    // Guarded by its own lock so the lock-free update_network_stats
    // overload stays clear of stats_mutex.
    network_risk_tracker_t network_risk_tracker;
    std::mutex network_risk_mutex;

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

    // codec is a 3-value string in practice (h264/hevc/av1); storing it as
    // the same small int id callers already derive it from (see
    // stream_recorder.cpp's identical convention) keeps the hot write path
    // lock-free without inventing a new scheme.
    int codec_to_id(const std::string &codec) {
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
  }  // namespace

  std::string stats_t::to_json() const {
    nlohmann::json j;

    j["streaming"] = streaming;
    j["client_name"] = client_name;
    j["client_ip"] = client_ip;
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
    j["width"] = width;
    j["height"] = height;
    j["latency_ms"] = latency_ms;
    j["packet_loss"] = packet_loss;
    j["packet_loss_available"] = packet_loss_available;
    j["packet_loss_source"] = packet_loss_source;
    j["control_channel_packet_loss"] = control_channel_packet_loss;
    j["control_channel_samples"] = control_channel_samples;
    j["video_sample_revision"] = video_sample_revision;
    j["network_sample_revision"] = network_sample_revision;
    j["network_last_received_age_ms"] = network_last_received_age_ms;
    j["media_loss_sample_revision"] = media_loss_sample_revision;
    j["media_loss_last_received_age_ms"] = media_loss_last_received_age_ms;
    j["bytes_sent"] = bytes_sent;
    j["gpu_usage"] = gpu_usage;
    j["adaptive_target_bitrate_kbps"] = adaptive_target_bitrate_kbps;
    j["adaptive_bitrate_active"] = adaptive_bitrate_active;
    j["adaptive_runtime_update_supported"] = adaptive_runtime_update_supported;
    j["doctor_live_action_scope_available"] = doctor_live_action_scope_available;
    j["idr_requests_total"] = idr_requests_total;
    j["invalidate_ref_frames_requests_total"] = invalidate_ref_frames_requests_total;
    j["headless_mode"] = config::video.linux_display.headless_mode;
    j["ai_enabled"] = config::video.ai_optimizer.enabled;
    j["controller_input"] = {
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
      cj["fps"] = c.fps;
      cj["bitrate_kbps"] = c.bitrate_kbps;
      cj["encode_time_ms"] = c.encode_time_ms;
      cj["codec"] = c.codec;
      cj["width"] = c.width;
      cj["height"] = c.height;
      cj["latency_ms"] = c.latency_ms;
      cj["packet_loss"] = c.packet_loss;
      cj["packet_loss_available"] = c.packet_loss_available;
      cj["packet_loss_source"] = c.packet_loss_source;
      cj["control_channel_packet_loss"] = c.control_channel_packet_loss;
      cj["bytes_sent"] = c.bytes_sent;
      cj["adaptive_target_bitrate_kbps"] = c.adaptive_target_bitrate_kbps;
      clients_json.push_back(cj);
    }
    j["clients"] = clients_json;
    j["active_sessions"] = static_cast<int>(clients.size());
    j["doctor"] = build_doctor_json(*this, nlohmann::json::object());
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

  nlohmann::json linux_gpu_profile_json(const stats_t &stats) {
    const auto &linux_display = config::video.linux_display;
    const bool gpu_native_requested =
      stats.gpu_native_probe.requested ||
      stats.runtime_gpu_native_override_active ||
      linux_display.prefer_gpu_native_capture;
    const auto capture_device_pairing = device_nodes_match(stats.capture_device, config::video.adapter_name);
    const auto wayland_device_pairing = device_nodes_match(stats.wayland_main_device, config::video.adapter_name);
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
    } else if (!stats.wayland_main_device.empty()) {
      adapter_pairing_device = stats.wayland_main_device;
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
#ifdef __linux__
    if (
      linux_display.headless_mode &&
      linux_display.use_cage_compositor &&
      !linux_display.prefer_gpu_native_capture &&
      config::video.encoder == "nvenc"
    ) {
      configuration_warnings.push_back({
        {"id", "nvidia_headless_gpu_native_disabled"},
        {"severity", "warning"},
        {"message", "NVIDIA true-headless labwc is configured with GPU-native capture disabled; cold or missing encoder cache can fail launch with 503 encoder initialization even when NVENC is healthy."},
        {"action", "Set linux_prefer_gpu_native_capture = enabled, restart Polaris, and retry Private Stream (GPU-native) before chasing CUDA/NVENC driver issues."}
      });
    }
#endif
    if (adapter_pairing_status == "mismatched") {
      configuration_warnings.push_back({
        {"id", "linux_gpu_adapter_mismatch"},
        {"severity", "warning"},
        {"message", "The configured encoder adapter " + config::video.adapter_name + " differs from the " + adapter_pairing_device_source + " render node " + adapter_pairing_device + "; cross-GPU DMA-BUF import can fail or fall back to system memory."},
        {"action", "Verify the render-node mapping under /dev/dri/by-path, then select the adapter used by the compositor or keep the conservative SHM fallback."}
      });
    }

    nlohmann::json profile = {
      {"encoder_api", stats.encode_target_device},
      {"encoder_adapter", config::video.adapter_name},
      {"capture_device", stats.capture_device},
      {"wayland_main_device", stats.wayland_main_device},
      {"adapter_matches_capture_device", adapter_matches_capture_device},
      {"adapter_matches_wayland_main_device", adapter_matches_wayland_main_device},
      {"adapter_pairing_status", adapter_pairing_status},
      {"adapter_pairing_device", adapter_pairing_device},
      {"adapter_pairing_device_source", adapter_pairing_device_source},
      {"vaapi_vendor", stats.vaapi_vendor},
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
      return "Capture is GPU-resident, but encoder upload/conversion crosses system memory.";
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

    nlohmann::json doctor_recommendation(const std::string &primary_issue,
                                         const std::string &summary,
                                         const nlohmann::json &health,
                                         bool live_bitrate_tunable,
                                         bool single_session_scope) {
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
        if (live_bitrate_tunable) {
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
        body = "The reliable control channel retried packets, but Polaris has no confirmed video-loss evidence and RTT remains stable. Do not lower quality from this observation alone.";
        next_step = "Keep monitoring";
        expected = "Visible media loss or sustained RTT pressure must appear before Doctor recommends a network recovery action.";
      } else if (primary_issue == "quality_reduced_live") {
        if (live_bitrate_tunable) {
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
      } else if (primary_issue == "steam_input_conflict") {
        body = "Local Steam Input settings can claim the Polaris Xbox virtual controller while strict isolation prevents Steam from creating its replacement controller. Disable Steam Input for Xbox controllers in Steam Settings, and set any per-game Force On overrides to Default or Disable.";
        next_step = "Adjust Steam Input";
        expected = "Proton games should read the Polaris virtual controller directly without a per-game workaround.";
      } else if (primary_issue == "encoder_load") {
        body = "Trim bitrate, resolution, or FPS to give the active encoder more frame time.";
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
                                      int paired_target_bitrate_kbps,
                                      bool live_bitrate_tunable,
                                      bool single_session_scope,
                                      const std::string &source_result_id,
                                      std::string_view app_uuid) {
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

      if (primary_issue == "network_jitter" && live_bitrate_tunable) {
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
        payload["target_bitrate_kbps"] = health_bitrate_kbps > 0 ? health_bitrate_kbps : derived_bitrate_kbps;
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
        payload["target_bitrate_kbps"] = paired_target_bitrate_kbps;
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
    // Packet-loss actions require an explicitly confirmed media source. ENet's
    // peer->packetLoss is a reliable control-channel EWMA: useful context, but
    // not a measurement of video packets dropped at the client.
    const bool current_network_observation =
      stats.network_sample_revision > 0 &&
      stats.network_last_received_age_ms >= 0 &&
      stats.network_last_received_age_ms <= DOCTOR_CURRENT_NETWORK_MAX_AGE_MS;
    const bool current_media_loss_observation =
      stats.media_loss_sample_revision > 0 &&
      stats.media_loss_last_received_age_ms >= 0 &&
      stats.media_loss_last_received_age_ms <= DOCTOR_CURRENT_NETWORK_MAX_AGE_MS;
    const bool confirmed_media_loss = current_media_loss_observation &&
      stats.packet_loss_available && stats.packet_loss > 2.0;
    const bool network_fail = stats.network_risk &&
      (confirmed_media_loss ||
       (current_network_observation && stats.latency_ms >= 45.0));
    const bool network_watch = current_network_observation &&
      stats.network_risk && !network_fail;
    const bool control_channel_observation =
      stats.streaming &&
      current_network_observation &&
      stats.control_channel_samples > 0 &&
      stats.control_channel_packet_loss >= network_risk_tracker_t::k_loss_elevated_pct;
    const int live_bitrate_kbps = stats.adaptive_runtime_update_supported && stats.adaptive_target_bitrate_kbps > 0 ?
      stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    const int effective_quality_target_kbps =
      stats.paired_target_bitrate_kbps > 0 && stats.effective_launch_bitrate_kbps > 0 ?
        std::min(stats.paired_target_bitrate_kbps, stats.effective_launch_bitrate_kbps) :
        0;
    const bool network_evidence_available = current_network_observation &&
      (current_media_loss_observation || stats.control_channel_samples > 0);
    const bool quality_reduced_live =
      stats.streaming && network_evidence_available && !stats.network_risk &&
      (!current_media_loss_observation || stats.packet_loss <= 2.0) &&
      stats.latency_ms < 45.0 &&
      stats.adaptive_runtime_update_supported && effective_quality_target_kbps > live_bitrate_kbps;
    const bool single_session_scope = stats.clients.size() <= 1 &&
      stats.doctor_live_action_scope_available;
    const bool live_bitrate_tunable =
      stats.adaptive_runtime_update_supported && single_session_scope;
    // Frame age is capture→encoder latency. On a CPU-copy capture path it is
    // dominated by the SHM copy/convert, so an over-budget age indicts the
    // capture path, not the encoder — the old verdict here sent an SHM-bound
    // user to lower bitrate, which cannot recover capture throughput (issue
    // #367: encode at 5.8 ms of budget while frames arrived 26.5 ms old).
    const bool encoder_time_fail = stats.encode_time_ms >= 12.0;
    const bool frame_age_counts_against_encoder = !capture_cpu_copy;
    const bool encoder_fail = encoder_time_fail ||
                              (frame_age_counts_against_encoder && stats.avg_frame_age_ms >= 22.0);
    const bool encoder_watch = !encoder_fail &&
                               (stats.encode_time_ms >= 8.0 ||
                                (frame_age_counts_against_encoder && stats.avg_frame_age_ms >= 18.0));
    const bool capture_latency_fail = capture_cpu_copy && !encoder_time_fail && stats.avg_frame_age_ms >= 22.0;
    const bool source_cadence_available = stats.capture_source_fps > 0.0 && target_fps > 0.0;
    const bool source_cadence_confirms_motion =
      source_cadence_available && stats.capture_source_fps >= target_fps * 0.85;
    const bool observed_target_shortfall = meaningful_fps_shortfall && source_cadence_confirms_motion;
    const bool static_or_duplicate_content =
      stats.duplicate_frame_ratio >= 0.50 ||
      (source_cadence_available && stats.capture_source_fps < target_fps * 0.50 &&
       stats.duplicate_frame_ratio >= 0.10);
    const bool pacing_watch =
      stats.dropped_frame_ratio >= 0.04 ||
      (!static_or_duplicate_content &&
       (stats.frame_jitter_ms >= 2.2 || observed_target_shortfall));
    const bool strict_gamepad_isolation =
      stats.input_host_controller_isolation == "strict_bwrap";
    const bool steam_input_known =
      stats.input_steam_input_status != "unknown" &&
      stats.input_steam_input_status != "not_applicable";
    const bool steam_input_conflict =
      strict_gamepad_isolation &&
      (stats.input_steam_profiles_with_xbox_support > 0 ||
       stats.input_steam_forced_app_count > 0);

    std::string primary_issue = health.value("primary_issue", std::string {});
    if (primary_issue == "steady" || primary_issue == "none") primary_issue.clear();
    const bool health_claims_network_jitter = primary_issue == "network_jitter";
    const bool health_claims_unconfirmed_frame_pacing =
      primary_issue == "frame_pacing" && !pacing_watch;
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
    if (steam_input_conflict && !network_fail && !encoder_fail && !capture_latency_fail) {
      // This is a deterministic host-state conflict, not an inference from a
      // missing controller event. Keep critical live stream failures ahead of
      // it, but do not let a generic pacing or capture watch hide why the
      // controller is structurally dead inside the strict sandbox.
      primary_issue = "steam_input_conflict";
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
      else if (capture_cpu_copy) primary_issue = capture_reason;
      else if (!capture_known) primary_issue = "capture_missing";
      else if (pacing_watch) primary_issue = "frame_pacing";
      else if (quality_reduced_live) primary_issue = "quality_reduced_live";
      else if (control_channel_observation) primary_issue = "control_channel_observation";
      else primary_issue = "none";
    }

    std::string traffic = "green";
    std::string status = "ok";
    std::string severity = "info";
    std::string simple_state = "Streaming ready";
    const auto health_grade = health.value("grade", std::string {});
    const bool honor_health_grade =
      (!health_claims_network_jitter || network_fail) && !health_claims_unconfirmed_frame_pacing;
    if (primary_issue == "no_active_stream" || primary_issue == "capture_missing") {
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
               (honor_health_grade && health_grade == "watch") || capture_cpu_copy || pacing_watch) {
      traffic = "amber";
      status = capture_cpu_copy ? "watch" : "needs_action";
      severity = "warning";
      simple_state = capture_cpu_copy ? "Advanced issue detected" : "Needs attention";
    }

    const std::string summary =
      primary_issue == "none" ? "Streaming telemetry looks ready." :
      primary_issue == "no_active_stream" ? "No active stream is running, so Doctor cannot verify the live path yet." :
      primary_issue == "capture_missing" ? "Capture metadata has not arrived yet; start a stream before tuning advanced settings." :
      primary_issue == "network_jitter" ? "Sustained network pressure is affecting this stream." :
      primary_issue == "network_observation" ? "A network warning needs more live evidence before Doctor changes quality." :
      primary_issue == "control_channel_observation" ? "Control-channel retries were observed, but video packet loss is not confirmed." :
      primary_issue == "quality_reduced_live" ? "The reversible live bitrate target is below the capability-validated launch ceiling and current network evidence is clean." :
      primary_issue == "steam_input_conflict" ? "Local Steam Input settings conflict with strict gamepad isolation for the Polaris Xbox virtual controller." :
      primary_issue == "encoder_load" ? "Encoder load is above the low-latency budget." :
      primary_issue == "frame_pacing" ? "Frame pacing telemetry needs attention." :
      health.contains("summary") && !suppressed_stale_network_finding ? health.value("summary", std::string {}) :
      capture_path_reason_message(capture_reason);

    nlohmann::json evidence = nlohmann::json::array();
    append_doctor_evidence(evidence, "streaming", "Active stream", stats.streaming, "", stats.streaming ? "pass" : "unknown", "stream_stats", stats.streaming ? "A stream is active." : "No active stream is reporting live telemetry.");
    append_doctor_evidence(evidence, "capture_path", "Capture path", capture_path, "", !capture_known ? "unknown" : capture_latency_fail ? "fail" : capture_cpu_copy ? "watch" : capture_gpu_native ? "pass" : "watch", "stream_stats", capture_path_reason_message(capture_reason));
    append_doctor_evidence(evidence, "encoder", "Encoder", stats.encode_target_device, "", encoder_fail ? "fail" : encoder_watch ? "watch" : "pass", "stream_stats", stats.encode_time_ms > 0.0 ? "Encode timing is reported by stream telemetry." : "Encoder timing has not been reported yet.");
    append_doctor_evidence(
      evidence,
      "packet_loss",
      "Video packet loss",
      current_media_loss_observation ? nlohmann::json(stats.packet_loss) : nlohmann::json(nullptr),
      "%",
      !current_media_loss_observation ? "unknown" : confirmed_media_loss ? "fail" : "pass",
      current_media_loss_observation ? stats.packet_loss_source : "unavailable",
      current_media_loss_observation ?
        "Packet loss confirmed by media-path telemetry." :
        "No current confirmed media packet-loss measurement is available for this live stream."
    );
    append_doctor_evidence(
      evidence,
      "control_channel_packet_loss",
      "Control-channel loss estimate",
      stats.control_channel_samples > 0 ? nlohmann::json(stats.control_channel_packet_loss) : nlohmann::json(nullptr),
      "%",
      stats.control_channel_samples == 0 ? "unknown" : control_channel_observation ? "watch" : "pass",
      "enet_control_channel",
      "ENet reliable-channel EWMA. Retransmissions can make this read high even when video delivery is healthy; it cannot grade the stream or authorize a bitrate reduction."
    );
    append_doctor_evidence(
      evidence,
      "latency",
      "Network latency",
      network_evidence_available ? nlohmann::json(stats.latency_ms) : nlohmann::json(nullptr),
      "ms",
      !network_evidence_available ? "unknown" : stats.latency_ms >= 45.0 ? "fail" : network_watch ? "watch" : "pass",
      network_evidence_available ? "stream_stats" : "unavailable",
      network_evidence_available ?
        "Round-trip latency reported by the active client control channel." :
        "No current media or control-channel latency sample is available for this stream."
    );
    append_doctor_evidence(evidence, "bitrate", "Live bitrate", live_bitrate_kbps, "kbps", "pass", "stream_stats", stats.adaptive_runtime_update_supported ? "Current live encoder target; Doctor changes it only for confirmed pressure or a verified same-stream restore." : "Applied encoder bitrate; this encoder does not expose live bitrate updates.");
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
        "The launch ceiling is the paired preference after host capability validation."
    );
    append_doctor_evidence(
      evidence,
      "target_fps_gap",
      "Target FPS gap",
      target_fps_gap,
      "FPS",
      observed_target_shortfall ? "watch" : meaningful_fps_shortfall && !source_cadence_available ? "unknown" : "pass",
      "stream_stats",
      observed_target_shortfall ?
        "Encoded FPS is below the requested cadence while measured source cadence confirms moving content." :
      meaningful_fps_shortfall && !source_cadence_available ?
        "Encoded FPS is below the requested cadence, but source cadence is unavailable; static content is not a pacing fault." :
        "No source-confirmed target cadence shortfall is present."
    );
    append_doctor_evidence(evidence, "frame_pacing", "Mean target interval error", stats.frame_jitter_ms, "ms", pacing_watch ? "watch" : "pass", "stream_stats", "Mean absolute distance between actual source-frame intervals and the requested interval; this is not statistical network jitter.");
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

    auto advanced = nlohmann::json::object();
    advanced["stream_stats_keys"] = nlohmann::json::array({"capture_path", "capture_path_reason", "capture_transport", "capture_residency", "capture_format", "capture_cpu_copy", "capture_gpu_native", "capture_cross_gpu_dmabuf_risk", "encode_target_device", "encode_target_residency", "fps", "encode_time_ms", "packet_loss", "packet_loss_available", "packet_loss_source", "control_channel_packet_loss", "control_channel_samples", "frame_interval_error_ms", "frame_jitter_ms"});
    advanced["linux_gpu_profile"] = linux_gpu_profile_json(stats);
    advanced["gpu_native_probe"] = gpu_native_probe_json(stats);
    advanced["controller_input"] = {
      {"host_controller_isolation", stats.input_host_controller_isolation},
      {"steam_input_status", stats.input_steam_input_status},
      {"steam_profiles_checked", stats.input_steam_profiles_checked},
      {"steam_profiles_with_xbox_support", stats.input_steam_profiles_with_xbox_support},
      {"steam_forced_app_count", stats.input_steam_forced_app_count},
      {"steam_input_detail", stats.input_steam_input_detail}
    };
    advanced["health"] = health;
    advanced["recent_issue_codes"] = nlohmann::json::array();
    if (steam_input_conflict) {
      advanced["recent_issue_codes"].push_back("steam_input_conflict");
    }
    advanced["raw_fields_redacted"] = true;

    double confidence_score = 0.35;
    std::string confidence_level = "low";
    std::string basis = "insufficient_data";
    if (primary_issue == "steam_input_conflict") {
      confidence_score = 0.98;
      confidence_level = "high";
      basis = "local_steam_config_and_isolation_plan";
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
    doctor["confidence"] = {{"level", confidence_level}, {"score", confidence_score}, {"basis", basis}, {"sample_window", {{"samples", stats.control_channel_samples}, {"seconds", 0}}}};
    doctor["summary"] = summary;
    doctor["recommendation"] = doctor_recommendation(
      primary_issue, summary, health, live_bitrate_tunable, single_session_scope
    );
    doctor["evidence"] = std::move(evidence);
    doctor["advanced_evidence"] = std::move(advanced);
    doctor["safe_recovery_action"] = doctor_safe_action(
      primary_issue,
      health,
      live_bitrate_kbps,
      effective_quality_target_kbps,
      live_bitrate_tunable,
      single_session_scope,
      doctor["result_id"].get<std::string>(),
      app_uuid
    );
    doctor["suppressed_findings"] = nlohmann::json::array();
    if (suppressed_stale_network_finding) {
      doctor["suppressed_findings"].push_back({
        {"id", "stale_network_jitter"},
        {"reason", "Confirmed media loss is unavailable and current RTT evidence is below the action threshold; the older health label cannot trigger a bitrate change."}
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
    }
  }

  void add_client(const std::string &client_ip, const std::string &client_name) {
    // Every call is a real attach from rtsp_stream::start() - bump the
    // population revision unconditionally, matching measurement-spec-v1.md
    // 6.1's "increments on every client attach or detach" (the dedup guard
    // just below is only about not duplicating the clients_t bookkeeping
    // entry, not about whether this call represents a real event).
    client_population_revision_counter.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(stats_mutex);

    // Check if client already exists
    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it == current_stats.clients.end()) {
      client_stats_t client;
      client.name = client_name;
      client.ip = client_ip;
      current_stats.clients.push_back(std::move(client));
    }

    // Always mark streaming as active and update primary client info
    current_stats.streaming = true;
    if (current_stats.clients.size() == 1) {
      current_stats.client_name = client_name;
      current_stats.client_ip = client_ip;
    }
  }

  void remove_client(const std::string &client_ip) {
    client_population_revision_counter.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.clients.erase(
      std::remove_if(current_stats.clients.begin(), current_stats.clients.end(),
        [&client_ip](const client_stats_t &c) { return c.ip == client_ip; }),
      current_stats.clients.end());

    if (current_stats.clients.empty()) {
      current_stats.streaming = false;
      current_stats.client_name.clear();
      current_stats.client_ip.clear();
    } else {
      // Update primary client info to first remaining client
      current_stats.client_name = current_stats.clients.front().name;
      current_stats.client_ip = current_stats.clients.front().ip;
    }
  }

  void update_video_stats(double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height) {
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
    if (!current_stats.clients.empty()) {
      auto &c = current_stats.clients.front();
      c.fps = fps;
      c.bitrate_kbps = bitrate_kbps;
      c.encode_time_ms = encode_time_ms;
      c.codec = codec;
      c.width = width;
      c.height = height;
    }
  }

  void update_video_stats(const std::string &client_ip, double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it != current_stats.clients.end()) {
      it->fps = fps;
      it->bitrate_kbps = bitrate_kbps;
      it->encode_time_ms = encode_time_ms;
      it->codec = codec;
      it->width = width;
      it->height = height;
    }

    // Also update top-level stats (use first client for backward compat)
    if (!current_stats.clients.empty() && current_stats.clients.front().ip == client_ip) {
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

  double packet_loss_percent(uint64_t scaled_loss, uint64_t scale) {
    if (scale == 0) {
      return 0.0;
    }
    return std::clamp((double) scaled_loss * 100.0 / (double) scale, 0.0, 100.0);
  }

  namespace {
    void record_primary_network_observation(bool media_sample,
                                            double latency_ms,
                                            double loss,
                                            uint64_t bytes_sent) {
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
      result.state = client_media_ingest_state_e::observed;
    }

    // Serialize counter advancement with evidence publication so concurrent
    // authenticated requests cannot publish an older delta after a newer one.
    // The media counters prove loss. RTT and byte totals remain host-owned.
    const auto host = get_current();
    update_network_stats(host.latency_ms, result.media_loss_pct, host.bytes_sent);
    if (adaptive_bitrate::is_enabled()) {
      adaptive_bitrate::update_network_stats(result.media_loss_pct, host.latency_ms);
    }
    result.observation_published = true;
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
      current_stats.adaptive_bitrate_active = adaptive_state.active;
      current_stats.adaptive_runtime_update_supported = adaptive_state.runtime_update_supported;

      // Also update adaptive bitrate for all clients
      for (auto &c : current_stats.clients) {
        c.adaptive_target_bitrate_kbps = target_bitrate;
      }

      result = current_stats;
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
    }
    {
      // Keep the complete network group on one host-received observation.
      // Doctor must never pair a new revision with stale loss/RTT fields.
      std::lock_guard<std::mutex> risk_lock(network_risk_mutex);
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
