/**
 * @file src/stream_stats.h
 * @brief Thread-safe stream statistics collector for real-time monitoring.
 *
 * Supports tracking multiple simultaneous streaming clients.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json_fwd.hpp>

// local includes
#include "platform/common.h"

namespace stream_stats {

  /**
   * @brief Return true when delivered FPS is materially below target.
   *
   * Uses the same 95% healthy boundary as session grading while requiring at
   * least a 2 FPS absolute gap so low-rate telemetry noise does not trigger
   * recovery.
   */
  bool is_meaningful_fps_shortfall(double target_fps, double delivered_fps);

  /**
   * @brief Per-client statistics for multi-session tracking.
   */
  struct client_stats_t {
    std::string name;
    std::string ip;

    // Video
    double fps = 0;
    int bitrate_kbps = 0;
    double encode_time_ms = 0;
    std::string codec;
    int width = 0;
    int height = 0;

    // Network
    double latency_ms = 0;
    double packet_loss = 0;
    uint64_t bytes_sent = 0;

    // Adaptive bitrate
    int adaptive_target_bitrate_kbps = 0;
  };

  struct capture_profile_sample_t {
    platf::frame_transport_e transport = platf::frame_transport_e::unknown;
    std::chrono::microseconds dispatch_time {};
    std::chrono::microseconds ingest_time {};
    std::chrono::microseconds total_time {};
  };

  struct gpu_native_probe_attempt_t {
    bool attempted = false;
    bool cached = false;
    std::string result = "not_attempted";
    std::string failure_stage;
    std::string failure_reason;
  };

  struct gpu_native_probe_t {
    bool requested = false;
    gpu_native_probe_attempt_t headless_extcopy;
    gpu_native_probe_attempt_t windowed;
    std::string selected_strategy = "none";
    std::string fallback = "none";
  };

  struct stats_t {
    // Stream state
    bool streaming = false;
    std::string client_name;
    std::string client_ip;
    std::string runtime_backend;
    bool runtime_requested_headless = false;
    bool runtime_effective_headless = false;
    bool runtime_gpu_native_override_active = false;
    /** Why this session is not on the display the client asked for; empty when it is. */
    std::string runtime_display_warning;
    platf::frame_transport_e capture_transport = platf::frame_transport_e::unknown;
    platf::frame_residency_e capture_residency = platf::frame_residency_e::unknown;
    platf::frame_format_e capture_format = platf::frame_format_e::unknown;
    std::string capture_device;
    std::string wayland_main_device;
    std::string vaapi_vendor;  ///< VA-API vendor string, e.g. the Mesa driver and its generation.
    gpu_native_probe_t gpu_native_probe;
    std::string encode_target_device;
    platf::frame_residency_e encode_target_residency = platf::frame_residency_e::unknown;
    platf::frame_format_e encode_target_format = platf::frame_format_e::unknown;
    int dynamic_range = 0;
    bool display_hdr = false;
    bool hdr_metadata_available = false;
    bool stream_hdr_enabled = false;
    std::string color_coding;

    // Video (primary/first client for backward compatibility)
    double fps = 0;
    double requested_client_fps = 0;
    double session_target_fps = 0;
    double encode_target_fps = 0;
    int bitrate_kbps = 0;
    double encode_time_ms = 0;
    double duplicate_frame_ratio = 0;
    double dropped_frame_ratio = 0;
    double avg_frame_age_ms = 0;
    double frame_jitter_ms = 0;
    std::string codec;
    std::string pacing_policy;
    std::string optimization_source;
    std::string optimization_confidence;
    std::string optimization_cache_status;
    std::string optimization_reasoning;
    std::string optimization_normalization_reason;
    int recommendation_version = 0;
    int width = 0;
    int height = 0;

    // Network
    double latency_ms = 0;
    double packet_loss = 0;
    uint64_t bytes_sent = 0;

    // Adaptive bitrate
    int adaptive_target_bitrate_kbps = 0;

    // System
    double gpu_usage = 0;

    // Controller/input runtime evidence
    bool input_virtual_controller_created = false;
    int input_virtual_controller_number = 0;
    std::string input_virtual_controller_kind;
    std::string input_virtual_controller_error;
    std::string input_host_controller_isolation = "unknown";
    std::string input_host_controller_isolation_detail;
    bool input_haptics_supported = false;
    std::string input_haptics_detail;

    // Recovery telemetry: real client-observed events, not modeled/estimated.
    // idr_requests_total counts IDX_REQUEST_IDR_FRAME (full keyframe resets);
    // invalidate_ref_frames_requests_total counts IDX_INVALIDATE_REF_FRAMES
    // (partial recovery via reference-frame invalidation, no full IDR needed).
    uint64_t idr_requests_total = 0;
    uint64_t invalidate_ref_frames_requests_total = 0;

    // Multi-client
    std::vector<client_stats_t> clients;

    /**
     * @brief Serialize stats to a JSON string.
     * @return JSON string representation.
     */
    std::string to_json() const;
  };

  /**
   * @brief Compare two device-node paths by filesystem identity.
   * @return true/false when both identities resolve, or nullopt when unknown.
   */
  std::optional<bool> device_nodes_match(const std::string &lhs, const std::string &rhs);

  /**
   * @brief Whether the current capture/encode path requires a CPU-side copy.
   * @param stats Current stream statistics snapshot.
   */
  bool capture_path_uses_cpu_copy(const stats_t &stats);

  /**
   * @brief Whether both capture and encode conversion are GPU-resident.
   * @param stats Current stream statistics snapshot.
   */
  bool capture_path_is_gpu_native(const stats_t &stats);

  /**
   * @brief Human-readable capture path classification for diagnostics.
   * @param stats Current stream statistics snapshot.
   */
  std::string capture_path_summary(const stats_t &stats);

  /**
   * @brief Machine-readable reason behind the capture path classification.
   * @param stats Current stream statistics snapshot.
   */
  std::string capture_path_reason(const stats_t &stats);

  /**
   * @brief Whether a true-headless DMA-BUF path is crossing DRM render nodes.
   * @param stats Current stream statistics snapshot.
   */
  bool capture_path_has_cross_gpu_dmabuf_risk(const stats_t &stats);

  /**
   * @brief Structured Linux GPU/capture truth for diagnostics and v1 APIs.
   * @param stats Current stream statistics snapshot.
   */
  nlohmann::json linux_gpu_profile_json(const stats_t &stats);

  /** @brief Structured result of GPU-native capture probes for diagnostics. */
  nlohmann::json gpu_native_probe_json(const stats_t &stats);

  /**
   * @brief Human-readable explanation for a capture path reason.
   * @param reason Machine-readable reason returned by capture_path_reason().
   */
  std::string capture_path_reason_message(const std::string &reason);

  /**
   * @brief Build the deterministic Polaris Doctor result from stream telemetry and optional session health.
   * @param stats Current stream statistics snapshot.
   * @param health Existing deterministic health JSON, when available.
   */
  nlohmann::json build_doctor_json(const stats_t &stats, const nlohmann::json &health);

  /**
   * @brief Effective dynamic range mode Polaris is advertising for the stream.
   * @param stats Current stream statistics snapshot.
   */
  std::string hdr_effective_mode(const stats_t &stats);

  /**
   * @brief Machine-readable reason when a requested HDR stream was downgraded.
   * @param stats Current stream statistics snapshot.
   */
  std::string hdr_downgrade_reason(const stats_t &stats);

  /**
   * @brief Human-readable HDR downgrade explanation for diagnostics and clients.
   * @param stats Current stream statistics snapshot.
   */
  std::string hdr_downgrade_message(const stats_t &stats);

  /**
   * @brief Update stream active state.
   * @param active Whether streaming is active.
   * @param client_name Name of the connected client.
   * @param client_ip IP address of the connected client.
   */
  void update_stream_active(bool active, const std::string &client_name = "", const std::string &client_ip = "");

  /**
   * @brief Add a new client session to the stats tracker.
   * @param client_ip IP address used as session key.
   * @param client_name Display name of the client.
   */
  void add_client(const std::string &client_ip, const std::string &client_name);

  /**
   * @brief Remove a client session from the stats tracker.
   * @param client_ip IP address of the client to remove.
   */
  void remove_client(const std::string &client_ip);

  /**
   * @brief Update video statistics (backward-compatible single-client API).
   * @param fps Current frames per second.
   * @param bitrate_kbps Current bitrate in kbps.
   * @param encode_time_ms Current encode time in milliseconds.
   * @param codec Current codec name.
   * @param width Current video width.
   * @param height Current video height.
   */
  void update_video_stats(double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height);

  /**
   * @brief Update video statistics for a specific client.
   * @param client_ip IP address of the client.
   * @param fps Current frames per second.
   * @param bitrate_kbps Current bitrate in kbps.
   * @param encode_time_ms Current encode time in milliseconds.
   * @param codec Current codec name.
   * @param width Current video width.
   * @param height Current video height.
   */
  void update_video_stats(const std::string &client_ip, double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height);

  /**
   * @brief Update static session targets for pacing and optimization telemetry.
   * @param requested_client_fps FPS originally requested by the client session.
   * @param session_target_fps FPS Polaris selected for the session before encode pacing.
   * @param encode_target_fps FPS the encoder loop is targeting.
   * @param pacing_policy Human-readable pacing policy label.
   * @param optimization_source Optimization layer(s) that influenced the session.
   * @param optimization_confidence Confidence attached to the chosen recommendation.
   * @param optimization_cache_status Whether the recommendation came from cache or a fresh request.
   * @param optimization_reasoning Human-readable summary for the chosen recommendation.
   * @param optimization_normalization_reason Explanation for any server-side correction.
   * @param recommendation_version Optimization schema version used to produce the result.
   */
  void update_session_targets(double requested_client_fps,
                              double session_target_fps,
                              double encode_target_fps,
                              const std::string &pacing_policy,
                              const std::string &optimization_source,
                              const std::string &optimization_confidence,
                              const std::string &optimization_cache_status,
                              const std::string &optimization_reasoning,
                              const std::string &optimization_normalization_reason,
                              int recommendation_version);

  /**
   * @brief Update frame delivery telemetry derived from the encode loop.
   * @param duplicate_frame_ratio Ratio of encoded frames that reused a prior image.
   * @param dropped_frame_ratio Ratio of capture frames dropped before encode.
   * @param avg_frame_age_ms Mean frame age at encode submission time.
   * @param frame_jitter_ms Mean absolute inter-frame timing error.
   */
  void update_frame_delivery(double duplicate_frame_ratio,
                             double dropped_frame_ratio,
                             double avg_frame_age_ms,
                             double frame_jitter_ms);

  /**
   * @brief Update network statistics.
   * @param latency_ms Current latency in milliseconds.
   * @param packet_loss Current packet loss percentage (0-100).
   * @param bytes_sent Total bytes sent.
   */
  void update_network_stats(double latency_ms, double packet_loss, uint64_t bytes_sent);

  /**
   * @brief Update network statistics for a specific client.
   * @param client_ip IP address of the client.
   * @param latency_ms Current latency in milliseconds.
   * @param packet_loss Current packet loss percentage (0-100).
   * @param bytes_sent Total bytes sent.
   */
  void update_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, uint64_t bytes_sent);

  /**
   * @brief Convert a scaled loss ratio (e.g. ENet's peer packetLoss against
   *        ENET_PEER_PACKET_LOSS_SCALE) to the 0-100 percentage this module stores.
   * @param scaled_loss Loss ratio numerator as reported by the transport.
   * @param scale Value of the transport's full-loss denominator; 0 yields 0.0.
   * @return Packet loss percentage clamped to [0.0, 100.0].
   */
  double packet_loss_percent(uint64_t scaled_loss, uint64_t scale);

  /**
   * @brief Update runtime mode metadata exposed to the dashboard.
   * @param state Current runtime state for the active backend.
   */
  void update_runtime_state(const platf::runtime_state_t &state);

  /**
   * @brief Record why the session fell back from the display mode the client asked for.
   *
   * Set at the launch-time fallback sites and served on session status; the
   * end-of-stream stats reset clears it, so it is per-session state.
   */
  void update_runtime_display_warning(const std::string &warning);

  /**
   * @brief Update current capture path metadata exposed to the dashboard.
   * @param metadata Current frame transport/residency/format metadata.
   */
  void update_capture_metadata(const platf::frame_metadata_t &metadata);

  /**
   * @brief Update the DRM render node advertised as Wayland DMA-BUF main_device.
   * @param device Render-node path, or empty when the compositor did not advertise one.
   */
  void update_wayland_main_device(const std::string &device);

  /**
   * @brief Record the VA-API vendor string reported by the driver.
   *
   * Nova reads `vaapi_vendor` from the Linux GPU profile and has never received
   * it. The string names the driver and its generation, which is what separates
   * one radeonsi generation from another in a crash report.
   *
   * @param vendor Vendor string from vaQueryVendorString, or empty when unknown.
   */
  void update_vaapi_vendor(const std::string &vendor);

  /** @brief Reset GPU-native probe telemetry for a new cage launch decision. */
  void reset_gpu_native_probe(bool requested, bool reset_capture_identity = false);

  /** @brief Record one GPU-native probe strategy result. */
  void update_gpu_native_probe_attempt(const std::string &strategy,
                                       const std::string &result,
                                       const std::string &failure_stage = {},
                                       const std::string &failure_reason = {},
                                       bool cached = false);

  /** @brief Record the final capture strategy selected after probing. */
  void update_gpu_native_probe_selection(const std::string &selected_strategy,
                                         const std::string &fallback = "none");

  /**
   * @brief Update the encode conversion path metadata exposed to the dashboard.
   * @param target_device Human-readable target device label for the converter.
   * @param target_residency Residency of the converter output.
   * @param target_format Pixel format of the converter output.
   */
  void update_encode_path_metadata(const std::string &target_device,
                                   platf::frame_residency_e target_residency,
                                   platf::frame_format_e target_format);

  /**
   * @brief Update the currently negotiated dynamic range mode.
   * @param dynamic_range Dynamic range mode from the active RTSP session.
   */
  void update_dynamic_range(int dynamic_range);

  /**
   * @brief Update the display HDR state and negotiated stream color coding.
   * @param display_hdr Whether the active display path reports HDR.
   * @param hdr_metadata_available Whether HDR metadata was available from the display path.
   * @param stream_hdr_enabled Whether Polaris is advertising true HDR for the stream.
   * @param color_coding Human-readable color coding label.
   */
  void update_hdr_state(bool display_hdr,
                        bool hdr_metadata_available,
                        bool stream_hdr_enabled,
                        const std::string &color_coding);

  /**
   * @brief Update native controller/input diagnostics exposed to support self-tests.
   */
  void update_controller_input_state(bool virtual_controller_created,
                                     int virtual_controller_number,
                                     const std::string &virtual_controller_kind,
                                     const std::string &virtual_controller_error,
                                     const std::string &host_controller_isolation,
                                     const std::string &host_controller_isolation_detail,
                                     bool haptics_supported,
                                     const std::string &haptics_detail);

  /**
   * @brief Record a single capture timing sample into the shared telemetry sink.
   * @param sample Timing sample tagged with the frame transport used.
   */
  void update_capture_profile(const capture_profile_sample_t &sample);

  /**
   * @brief p50/p99 over a rolling window of recent samples for one pipeline stage.
   */
  struct frame_timing_percentiles_t {
    double p50_ms = 0;
    double p99_ms = 0;
    int sample_count = 0;

    /// Samples rejected for a non-monotonic or negative stage duration
    /// (e.g. a corrupted or misordered timestamp pair). Counted, not
    /// silently dropped - and never folded into p50_ms/p99_ms/sample_count.
    int invalid_count = 0;
  };

  /**
   * @brief Host-side T0-T2 stage timing for one active streaming session.
   *
   * T0 = capture frame available, T1 = encoder finished this frame, T2 =
   * this frame's packet handed off to the send thread's packetization path.
   * T2 is captured before FEC encoding, header/encryption work, the pacing
   * sleep, and the actual sendmsg() calls - it is deliberately not called
   * "handed to the NIC" or "kernel accept", both of which would overclaim
   * what's actually measured. See the Nordstern roadmap's canonical stage
   * vocabulary and its 2026-08-08 review for this naming correction.
   *
   * Genuinely per-session, not host-wide: Polaris runs one independent
   * encoder per connected client (config::stream.max_sessions, default 2),
   * so T1/T2 legitimately differ session to session even for two sessions'
   * frames captured at the same instant (T0 can coincide; T1/T2 generally
   * won't). Each session's rings are private to it - see
   * start_session_timing()/stop_session_timing().
   *
   * Deliberately out of scope: joining this session's T0-T2 with its
   * client-side T3/T4 samples (Nova's sampled logcat) into a per-frame
   * total, and any cross-clock (host/client) stitching. Both were assessed
   * as materially larger undertakings than what this endpoint needs to be
   * honest about today; this response reports only its own stage marginals.
   */
  struct session_timing_t {
    frame_timing_percentiles_t capture_to_encode;  ///< T0 -> T1
    frame_timing_percentiles_t encode_to_send;  ///< T1 -> T2
    frame_timing_percentiles_t capture_to_send;  ///< T0 -> T2

    /**
     * The session_t generation that produced this snapshot - a
     * process-lifetime-monotonic counter assigned once per session_t
     * allocation (session_t::session_generation), not per device. Two
     * sessions from the same reconnecting device get different
     * generations even though they share the same device_uuid, which is
     * what lets record_frame_timing()/stop_session_timing() reject a
     * write or stop from a session that already lost ownership of its
     * uuid slot to a newer generation, instead of corrupting or
     * prematurely discarding the new one. A changed value between two
     * reads means the session was torn down and restarted between them.
     */
    std::uint64_t session_generation = 0;

    /// Whether the queried device_uuid currently has any timing state at
    /// all. False (with all percentiles empty) means no session is active
    /// for that device right now - distinct from an active session that
    /// simply hasn't recorded a frame yet.
    bool session_active = false;

    /// True until this session's rings wrap. False means the ring evicted
    /// its oldest samples to make room for newer ones, so the percentiles
    /// above reflect only the most recent capacity's worth of frames, not
    /// every frame since this generation started.
    bool ring_complete = false;
  };

  /**
   * @brief Start (or restart, on reconnect) per-session T0-T2 timing state
   * for one device. Call once the session is admitted, alongside
   * add_client(). An existing entry for the same uuid is discarded and
   * replaced with a fresh, empty one - this is how a reconnecting device
   * gets a clean generation rather than inheriting its previous session's
   * tail.
   * @param device_uuid The connecting device's paired UUID (session_t::device_uuid).
   * @param session_generation The new session_t's own generation (session_t::session_generation).
   */
  void start_session_timing(const std::string &device_uuid, std::uint64_t session_generation);

  /**
   * @brief Stop and discard per-session T0-T2 timing state for one device -
   * but only if session_generation still matches the currently-registered
   * generation for that uuid. A stop from a generation that already lost
   * its uuid slot to a newer session (e.g. a fast reconnect racing this
   * session's own teardown) is a safe no-op, so it cannot discard the
   * newer session's state.
   * @param device_uuid The device UUID passed to the matching start_session_timing().
   * @param session_generation The generation passed to that same start_session_timing() call.
   */
  void stop_session_timing(const std::string &device_uuid, std::uint64_t session_generation);

  /**
   * @brief Record one frame's T0/T1/T2 timestamps into device_uuid's
   * session timing histograms. Wire-format telemetry
   * (frame_processing_latency) is untouched by this - this is purely an
   * additional, out-of-band record. A no-op if device_uuid has no active
   * timing state, or if session_generation no longer matches the
   * currently-registered generation (a stale write from a session that
   * already retired and lost its uuid slot to a newer one - e.g. packets
   * still draining through the send thread right as a fast reconnect
   * hands the same uuid to a new session). Per-stage durations that come
   * out negative or non-monotonic are rejected and counted rather than
   * silently corrupting the percentiles.
   * @param device_uuid The frame's owning session (session_t::device_uuid).
   * @param session_generation The frame's owning session's generation (session_t::session_generation).
   * @param capture_time T0.
   * @param encode_done_time T1.
   * @param send_time T2.
   */
  void record_frame_timing(const std::string &device_uuid,
                           std::uint64_t session_generation,
                           std::chrono::steady_clock::time_point capture_time,
                           std::chrono::steady_clock::time_point encode_done_time,
                           std::chrono::steady_clock::time_point send_time);

  /**
   * @brief Get a snapshot of device_uuid's own current session timing
   * percentiles. session_active is false (and all percentiles are empty)
   * if device_uuid has no active timing state right now.
   * @param device_uuid The caller's own device UUID (from its verified cert).
   */
  session_timing_t get_session_timing(const std::string &device_uuid);

  /**
   * @brief The identity of one session, as tracked by
   * start_session_timing()/stop_session_timing().
   */
  struct active_session_identity_t {
    std::string device_uuid;
    std::uint64_t session_generation = 0;
  };

  /**
   * @brief Look up the identity of the lone active session, for a caller
   * that needs to know which session to act on without already knowing
   * its device_uuid - unlike get_session_timing(), which requires already
   * knowing it. The P0-5 benchmark control surface's create route is the
   * motivating caller: a harness's create-and-arm request
   * (measurement-spec-v1.md 6.4) names no device_uuid at all, since the
   * harness isn't the streaming client itself - it arms a run for
   * whichever single session happens to be active.
   *
   * Returns std::nullopt for zero or more than one active session. This
   * is purely a lookup, not a validation step in its own right -
   * create_benchmark_run's own "exactly one active stream session"
   * precondition (checked via active_client_count()) is the actual gate;
   * a caller of this function still needs that same precondition to have
   * already been satisfied for the identity returned here to be
   * meaningful, since a session could in principle end between this call
   * and whatever uses its result.
   *
   * @return The active session's identity, or std::nullopt.
   */
  std::optional<active_session_identity_t> get_single_active_session_identity();

  // ---------------------------------------------------------------------
  // P0-5 benchmark-run-capture engine (measurement-spec-v1.md 6.4-6.5).
  // Gate-authoritative, bounded-duration raw sample capture for an armed
  // benchmark run - distinct from the always-on rolling diagnostics above.
  // Built incrementally, one reviewable piece at a time: this piece is
  // pure, state-machine-free logic (boundary classification + bounded
  // per-stage storage) that a later piece builds the run lifecycle
  // (armed/active/draining/frozen/aborted/expired) on top of. Nothing
  // here is reachable yet - no run can be armed, so none of this executes
  // in production until the remaining pieces land.
  // ---------------------------------------------------------------------

  /**
   * @brief Lifecycle state of one benchmark run (measurement-spec-v1.md 6.4).
   */
  enum class benchmark_run_state_e {
    armed,
    active,
    draining,
    frozen,
    aborted,
    expired
  };

  /**
   * @brief Why a benchmark run aborted. none for a run that hasn't (or
   * didn't) abort.
   */
  enum class benchmark_abort_reason_e {
    none,
    session_ended,
    session_generation_changed,
    client_population_revision_changed,
    sample_capacity_exceeded,
    invalid_stage_duration,
    stopped_before_duration_lower_bound,
    explicit_harness_abort,
    internal_telemetry_failure
  };

  /**
   * @brief Outcome of classifying one stage observation against a
   * benchmark run's half-open active window [S, E) (measurement-spec-v1.md
   * 6.5).
   */
  enum class boundary_classification_e {
    accepted,
    excluded_before_window,
    excluded_after_window,
    ignored_post_window,
    invalid_non_monotonic
  };

  /**
   * @brief Classify one stage observation's endpoints against a run's
   * active window, per measurement-spec-v1.md 6.5's 5-step rule, applied
   * in this order:
   *   1. a < 0 (before S)              -> excluded_before_window
   *   2. else a >= window_end_us (>=E) -> ignored_post_window
   *   3. else b >= window_end_us       -> excluded_after_window
   *   4. else a <= b                   -> accepted
   *   5. else (a > b)                  -> invalid_non_monotonic
   * a_offset_us/b_offset_us are already relative to S (the run's active
   * start) - a negative value means "before S".
   * @param a_offset_us The stage's first endpoint, in microseconds since S.
   * @param b_offset_us The stage's second endpoint, in microseconds since S.
   * @param window_end_us The window's end (E - S), in microseconds.
   */
  boundary_classification_e classify_boundary(std::int64_t a_offset_us, std::int64_t b_offset_us, std::int64_t window_end_us);

  /**
   * @brief Bounded raw-sample capture for one pipeline stage within one
   * benchmark run. Preallocated at construction (arm time) to a fixed
   * capacity; record() performs no allocation, matching the hot-path
   * contract in measurement-spec-v1.md 6.4.
   *
   * started_in_window_without_terminal_count is intentionally always 0 in
   * this implementation. Polaris's send thread only calls record() once
   * both of a stage's endpoints are already known - T0/T1/T2 all arrive
   * bundled on packet_raw_t by the time record_frame_timing() (P0-3/P0-3A)
   * or its benchmark-capture counterpart is called - so there is no
   * "endpoint a registered, waiting for b" gap the way the general spec
   * model assumes. A frame that gets T0/T1 but is dropped before ever
   * reaching the send thread currently produces no record() call at all,
   * rather than counting as an unresolved in-window sample. That is a
   * known, documented gap relative to the full spec model, not silent
   * data loss within what this capture does observe.
   */
  struct benchmark_stage_capture_t {
    std::size_t capacity = 0;
    std::vector<std::uint32_t> start_offset_us;
    std::vector<std::uint32_t> end_offset_us;
    std::vector<std::uint32_t> duration_us;

    std::size_t accepted_count = 0;
    std::size_t excluded_started_before_window = 0;
    std::size_t excluded_completed_after_window = 0;
    std::size_t started_in_window_without_terminal_count = 0;
    std::size_t overflow_count = 0;
    std::size_t invalid_count = 0;

    /**
     * @param sample_capacity Reserved size of the three parallel arrays -
     * the run's declared per-stage frame budget. Zero is a valid, harmless
     * default (every observation simply overflows).
     */
    explicit benchmark_stage_capture_t(std::size_t sample_capacity = 0);

    /**
     * @brief Classify and, if accepted, record one observation. An
     * accepted observation that would exceed capacity is counted in
     * overflow_count instead of being stored.
     * @param a_offset_us The stage's first endpoint, in microseconds since S.
     * @param b_offset_us The stage's second endpoint, in microseconds since S.
     * @param window_end_us The window's end (E - S), in microseconds.
     * @return The classification this observation received.
     */
    boundary_classification_e record(std::int64_t a_offset_us, std::int64_t b_offset_us, std::int64_t window_end_us);
  };

  /**
   * @brief One gate-authoritative benchmark run (measurement-spec-v1.md 6.4).
   * Not constructible without a sample capacity - the three stage captures
   * need it at construction, and there is no meaningful "run" without one.
   * Everything else is set by the caller after construction (matching
   * session_timing_state_t's own pattern above) - this piece provides the
   * struct and its storage; piece 3 (create_benchmark_run) owns validating
   * and populating one.
   */
  struct benchmark_run_t {
    std::string run_id;
    benchmark_run_state_e state = benchmark_run_state_e::armed;
    benchmark_abort_reason_e abort_reason = benchmark_abort_reason_e::none;

    std::string owning_device_uuid;
    std::uint64_t owning_session_generation = 0;

    std::string manifest_sha256;
    std::string label;
    std::string workload_id;

    std::chrono::nanoseconds expected_duration_ns {0};
    std::chrono::nanoseconds duration_tolerance_ns {0};
    std::chrono::nanoseconds drain_grace_ns {0};
    int target_fps = 0;
    std::size_t sample_capacity = 0;

    std::chrono::steady_clock::time_point armed_monotonic;
    std::optional<std::chrono::steady_clock::time_point> started_monotonic;
    std::optional<std::chrono::steady_clock::time_point> stopped_monotonic;

    /// Set when this run becomes immutable, whether by reaching frozen
    /// normally or by aborting - measurement-spec-v1.md 6.4 only names one
    /// "frozen_monotonic_ns" output field, used as the terminal-payload
    /// timestamp either way. Also this run's retention age for eviction/TTL.
    std::optional<std::chrono::steady_clock::time_point> frozen_monotonic;

    std::uint64_t client_population_revision_at_arm = 0;
    std::uint64_t client_population_revision_at_freeze = 0;

    benchmark_stage_capture_t capture_to_encode;
    benchmark_stage_capture_t encode_to_send_release;
    benchmark_stage_capture_t capture_to_send_release;

    explicit benchmark_run_t(std::size_t sample_capacity_in):
        sample_capacity(sample_capacity_in),
        capture_to_encode(sample_capacity_in),
        encode_to_send_release(sample_capacity_in),
        capture_to_send_release(sample_capacity_in) {
    }
  };

  /**
   * @brief A process-lifetime-stable identifier, generated on first use.
   * Doesn't need global uniqueness (nothing compares it across machines) -
   * only needs to change across a Polaris restart, so a benchmark run's
   * frozen output can't be mistaken for one produced by a different process
   * instance.
   */
  const std::string &process_instance_id();

  /**
   * @brief Insert a fully-constructed benchmark run into process-wide
   * storage. Does not validate uniqueness or any create-and-arm
   * precondition - that is piece 3 (create_benchmark_run)'s job; this is
   * the storage primitive it will call after validating. Enforces the
   * terminal-payload retention limit (measurement-spec-v1.md 6.4: at most
   * 4 frozen/aborted payloads per process instance) by expiring the oldest
   * terminal run first if this insert would exceed it.
   */
  void insert_benchmark_run(benchmark_run_t run);

  /**
   * @brief Look up a run by ID and, while still holding the storage lock,
   * invoke fn on it. This is the only safe way to read or mutate a
   * benchmark_run_t's fields from outside this file: insert_benchmark_run()
   * can reallocate the underlying storage, and erase_benchmark_run() can
   * shift it, so a raw pointer/reference returned across a call boundary
   * could dangle. Not for the hot path - piece 5's record_benchmark_sample()
   * will be its own dedicated, allocation-free function instead of using
   * this generic std::function-based visitor.
   * @return false (fn not invoked) if no run with that ID exists.
   */
  bool with_benchmark_run(const std::string &run_id, const std::function<void(benchmark_run_t &)> &fn);

  /**
   * @brief Immediately release a run's storage (measurement-spec-v1.md 6.4:
   * "DELETE releases payload storage immediately").
   */
  void erase_benchmark_run(const std::string &run_id);

  /**
   * @brief Clear a run's heavy payload (the three stage captures' sample
   * arrays) and mark it expired, leaving only the bounded tombstone
   * metadata (run_id, state, timestamps, abort_reason, counts) that
   * measurement-spec-v1.md 6.4 requires survive expiry. A no-op if run_id
   * doesn't exist.
   */
  void expire_benchmark_run(const std::string &run_id);

  /**
   * @brief Expire every frozen/aborted run whose frozen_monotonic is older
   * than the 30-minute TTL (measurement-spec-v1.md 6.4). Intended to be
   * called lazily (e.g. from a future get_benchmark_run()), not on a timer.
   */
  void expire_stale_benchmark_runs();

  /**
   * @brief Whether the host-local benchmark control plane is enabled
   * (measurement-spec-v1.md 6.4: "disabled by default... must require an
   * explicit benchmark-mode enable at process start"). Defaults to false.
   * No production caller flips this on yet - that's the network-facing
   * control-surface increment's job (config-driven, at process start).
   * Exposed as a plain setter rather than a "for tests" one because that
   * future increment will most likely just call this same setter itself
   * once it reads its config, not reimplement the flag.
   */
  bool benchmark_control_plane_enabled();
  void set_benchmark_control_plane_enabled(bool enabled);

  /**
   * @brief A validated create-and-arm request for one benchmark run
   * (measurement-spec-v1.md 6.4's create-and-arm JSON, minus run_id which
   * create_benchmark_run takes separately since it's also the storage key).
   */
  struct benchmark_run_create_request_t {
    std::string run_id;
    std::string manifest_sha256;
    std::string label;
    std::string workload_id;
    int expected_duration_s = 0;
    int duration_tolerance_ms = 0;
    int drain_grace_ms = 0;
    int target_fps = 0;
    std::size_t sample_capacity_frames = 0;
  };

  /**
   * @brief Why create_benchmark_run() accepted or rejected a request. Every
   * rejected value names exactly one precondition from measurement-spec-v1.md
   * 6.4's create-and-arm list.
   */
  enum class benchmark_run_create_result_e {
    created,
    rejected_control_plane_not_enabled,
    rejected_caller_not_authorized_as_harness,
    rejected_not_exactly_one_active_session,
    rejected_session_already_has_an_active_run,
    rejected_duration_out_of_range,
    rejected_duration_tolerance_out_of_range,
    rejected_drain_grace_out_of_range,
    rejected_target_fps_out_of_range,
    rejected_nominal_sample_budget_too_small,
    rejected_capacity_below_nominal_budget,
    rejected_capacity_exceeds_maximum,
    rejected_run_id_already_used,
    rejected_invalid_manifest_sha256_format
  };

  /**
   * @brief Validate a create-and-arm request against every precondition in
   * measurement-spec-v1.md 6.4 that this engine can check on its own, and
   * if all pass, construct and insert the armed run.
   *
   * Deliberately NOT checked here (documented gap, matching this engine's
   * established pattern of naming what it doesn't yet do rather than
   * silently skipping it): "duration, tolerance, drain grace, target fps,
   * capacity, and workload exactly match the frozen manifest" - that
   * requires the manifest file infrastructure a later piece (the
   * evidence/manifest writer and validator) owns. This function only
   * validates manifest_sha256's *format* (64 lowercase hex chars), not its
   * content against anything.
   *
   * @param request The request to validate and, if valid, arm.
   * @param device_uuid The calling session's device UUID (session_t::device_uuid).
   * @param session_generation The calling session's generation (session_t::session_generation).
   * @param caller_is_authorized_harness Whether the caller has been
   * authenticated as the local benchmark harness specifically - not merely
   * as an ordinary paired client. The actual authorization decision
   * (loopback/management-source policy, rate limits, audit logging) is the
   * network-facing control-surface increment's job; this function only
   * enforces the precondition given whatever the caller asserts.
   */
  benchmark_run_create_result_e create_benchmark_run(
    const benchmark_run_create_request_t &request,
    const std::string &device_uuid,
    std::uint64_t session_generation,
    bool caller_is_authorized_harness);

  /**
   * @brief Outcome of start_benchmark_run().
   */
  enum class benchmark_run_start_result_e {
    started,
    rejected_control_plane_not_enabled,
    rejected_caller_not_authorized_as_harness,
    rejected_run_not_found,
    rejected_wrong_session,
    rejected_run_not_in_armed_state,
    rejected_population_changed_since_arm,
    rejected_session_no_longer_active
  };

  /**
   * @brief Transition an armed run to active, starting its measurement
   * window. Every access to a run (this function and stop/get/delete
   * below) first reconciles its state with reality via an internal lazy
   * transition check - measurement-spec-v1.md 6.4's active->draining and
   * draining->frozen deadlines, and the abort triggers (session ended,
   * session generation changed, population changed) for a run already in
   * its measurement window - since there is no background timer driving
   * these; whichever caller happens to touch a run next is what notices.
   * That reconciliation runs regardless of whether this call's own
   * preconditions below end up rejecting it, so a stale run's state never
   * depends on who last happened to ask about it.
   *
   * device_uuid/session_generation are a sanity/integrity check, not the
   * authorization mechanism (that is caller_is_authorized_harness, same
   * as create_benchmark_run) - the harness is expected to pass the
   * identity of the session it is benchmarking, and this rejects a
   * mismatch against what the run recorded at arm time, catching a stale
   * run_id or a harness bookkeeping bug rather than silently starting the
   * wrong run.
   *
   * rejected_population_changed_since_arm covers a gap the abort-trigger
   * machinery above doesn't: that machinery only watches already-active or
   * -draining runs, so a population change that happens while still
   * *armed* (before start) needs its own check here, before the
   * measurement window - and the abort semantics that assume one - even
   * begins.
   *
   * @param run_id The run to start.
   * @param device_uuid The session being benchmarked (session_t::device_uuid).
   * @param session_generation That session's generation (session_t::session_generation).
   * @param caller_is_authorized_harness See create_benchmark_run's parameter of the same name.
   */
  benchmark_run_start_result_e start_benchmark_run(
    const std::string &run_id,
    const std::string &device_uuid,
    std::uint64_t session_generation,
    bool caller_is_authorized_harness);

  /**
   * @brief Outcome of stop_benchmark_run().
   */
  enum class benchmark_run_stop_result_e {
    stopped,
    stopped_early_and_aborted,
    rejected_control_plane_not_enabled,
    rejected_caller_not_authorized_as_harness,
    rejected_run_not_found,
    rejected_wrong_session,
    rejected_not_currently_active
  };

  /**
   * @brief Explicitly stop an active run. measurement-spec-v1.md 6.4: "An
   * explicit stop before the lower bound aborts the run; it cannot freeze
   * a short run as valid merely because percentile sample floors were
   * met." The lower bound is expected_duration_ns - duration_tolerance_ns;
   * stopping at or after it transitions to draining (a normal stop, same
   * as the automatic lazy transition at the declared deadline would have
   * done); stopping before it aborts with stopped_before_duration_lower_bound.
   *
   * Calling this on a run that is not (after the same lazy reconciliation
   * start_benchmark_run runs) currently active - already draining, frozen,
   * aborted, still armed, or unknown - is rejected rather than treated as
   * a no-op success, so a duplicate stop is visible as such to the caller.
   *
   * @param run_id The run to stop.
   * @param device_uuid The session being benchmarked - see start_benchmark_run.
   * @param session_generation That session's generation - see start_benchmark_run.
   * @param caller_is_authorized_harness See create_benchmark_run's parameter of the same name.
   */
  benchmark_run_stop_result_e stop_benchmark_run(
    const std::string &run_id,
    const std::string &device_uuid,
    std::uint64_t session_generation,
    bool caller_is_authorized_harness);

  /**
   * @brief Outcome of get_benchmark_run().
   */
  enum class benchmark_run_get_result_e {
    found,
    rejected_control_plane_not_enabled,
    rejected_caller_not_authorized_as_harness,
    rejected_run_not_found
  };

  /**
   * @brief Look up a run by ID, applying the same lazy state reconciliation
   * described on start_benchmark_run first, then invoke fn with the
   * up-to-date run while still holding the storage lock - the same
   * locked-visitor contract as with_benchmark_run (piece 2), reused here
   * rather than copying a potentially large run (up to 65,536 samples per
   * stage) out on every read. fn must not call back into any
   * stream_stats function that takes benchmark_run_mutex.
   *
   * Unlike start/stop, this takes no device_uuid/session_generation - the
   * harness reads any run by its ID directly, not scoped to "the session
   * it currently owns."
   *
   * @param run_id The run to look up.
   * @param caller_is_authorized_harness See create_benchmark_run's parameter of the same name.
   * @param fn Invoked with the run if found.
   */
  benchmark_run_get_result_e get_benchmark_run(
    const std::string &run_id,
    bool caller_is_authorized_harness,
    const std::function<void(benchmark_run_t &)> &fn);

  /**
   * @brief Outcome of delete_benchmark_run().
   */
  enum class benchmark_run_delete_result_e {
    deleted,
    rejected_control_plane_not_enabled,
    rejected_caller_not_authorized_as_harness,
    rejected_run_not_found
  };

  /**
   * @brief Delete a run's storage immediately, regardless of its current
   * state (measurement-spec-v1.md 6.4: "DELETE releases payload storage
   * immediately") - unlike expire, this leaves no tombstone at all.
   * @param run_id The run to delete.
   * @param caller_is_authorized_harness See create_benchmark_run's parameter of the same name.
   */
  benchmark_run_delete_result_e delete_benchmark_run(
    const std::string &run_id,
    bool caller_is_authorized_harness);

  /**
   * @brief Record one frame's T0/T1/T2 timestamps into whichever active or
   * draining benchmark run, if any, is owned by (device_uuid,
   * session_generation) - a no-op if there is none. Intended to be called
   * from the same send-thread call site as record_frame_timing(), with the
   * same five arguments, right alongside it; the two are independent
   * (this engine's bounded run capture vs. the always-on rolling
   * diagnostics ring) and neither reads the other's state.
   *
   * This is the hot path measurement-spec-v1.md 6.4 describes: no
   * allocation after run start (classify_boundary()/
   * benchmark_stage_capture_t::record() only ever write into each stage's
   * already-reserved arrays), no sorting/serializing/logging/percentiles,
   * and at most one record per frame/session (one call per already-bundled
   * T0/T1/T2 triple, same as record_frame_timing).
   *
   * Deliberately does NOT run apply_lazy_transitions_locked - checking the
   * abort triggers on every frame would mean an extra get_session_timing()
   * call (a second mutex) on every single frame, which the hot-path
   * constraints above rule out. This does not corrupt anything: a sample
   * arriving after the run's true deadline but before something else
   * (start/stop/get/delete) lazily notices is still classified correctly
   * by each stage's own window-relative boundary check below, just under
   * whatever state label happened to be stored at the time. Being active
   * or draining is precondition enough to look - a draining run's own
   * drain grace exists specifically to let already-in-flight frames like
   * this keep arriving and being classified without being admitted past
   * the window.
   *
   * A generation that no longer owns (device_uuid)'s run - the run's own
   * owning_session_generation simply won't match session_generation - is
   * silently dropped by the same lookup, same as record_frame_timing.
   *
   * @param device_uuid The frame's owning session (session_t::device_uuid).
   * @param session_generation The frame's owning session's generation (session_t::session_generation).
   * @param capture_time T0.
   * @param encode_done_time T1.
   * @param send_time T2.
   */
  void record_benchmark_sample(const std::string &device_uuid,
                                std::uint64_t session_generation,
                                std::chrono::steady_clock::time_point capture_time,
                                std::chrono::steady_clock::time_point encode_done_time,
                                std::chrono::steady_clock::time_point send_time);

  /**
   * @brief Get the number of active client sessions.
   * @return Count of active clients.
   */
  int active_client_count();

  /**
   * @brief Get the current client population revision - a monotonic counter
   * (measurement-spec-v1.md 6.1) that increments on every
   * add_client()/remove_client() call, so a leave/join replacement that
   * returns the client count to its original value still moves the
   * revision. Deliberately NOT reset by update_stream_active(false)'s
   * stats_t reset ("all sessions ended") - it lives outside stats_t so a
   * full stream restart between a benchmark run's arm and freeze can never
   * be masked by the counter coincidentally returning to its starting
   * value. Used to detect and abort a benchmark run whose client
   * population changed mid-run.
   */
  std::uint64_t client_population_revision();

  /**
   * @brief Record a client-requested full IDR (keyframe) frame reset.
   */
  void record_idr_request();

  /**
   * @brief Record a client-requested partial reference-frame invalidation
   * (recovery without a full IDR).
   */
  void record_invalidate_ref_frames_request();

  /**
   * @brief Get a snapshot of the current stats.
   * @return Copy of the current stats.
   */
  stats_t get_current();

}  // namespace stream_stats
