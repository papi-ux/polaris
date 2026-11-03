/** @file
 * Bounded native accepted-original reception observations (#615). No controller consumes this.
 * Packet API handoff and client socket-read clocks are kept separate; only differences are joined.
 */
#pragma once

#include "stream_frame_feedback.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace stream_native_reception {
  using clock = std::chrono::steady_clock;
  inline constexpr std::uint16_t packet_type = 0x7F10;  // Private matched Polaris/Nova allocation.
  inline constexpr std::string_view offer_attribute = "x-ss-video.packetReceptionFeedbackVersion";
  inline constexpr std::string_view echo_attribute = "x-ml-video.packetReceptionFeedbackVersion";
  inline constexpr std::size_t header_bytes = 52, sample_bytes = 10, max_samples = 16;
  inline constexpr std::size_t max_payload_bytes = header_bytes + max_samples * sample_bytes;
  inline constexpr std::size_t ledger_capacity = 16384;
  inline constexpr auto retention = std::chrono::seconds(2);
  inline constexpr std::uint32_t max_age_us = 2'000'000;
  inline constexpr std::uint8_t has_accepted = 1, incomplete = 2, clock_unavailable = 4, timing_sample_limit = 8;

  /// Exact optional handshake; malformed/duplicate/unknown echoes disable only this observation.
  inline bool negotiated(bool sunshine, bool offered, std::string_view echo, std::size_t echoes,
                          int client_features, int control_protocol, std::uint32_t encryption) noexcept {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!echo.empty() && space(echo.front())) echo.remove_prefix(1);
    while (!echo.empty() && space(echo.back())) echo.remove_suffix(1);
    // Existing namespaces: ML_FF_SESSION_ID_V1=2, SS_ENC_CONTROL_V2=1, SS_ENC_VIDEO=2.
    return sunshine && offered && echoes == 1 && echo == "1" &&
      (client_features & 2) && control_protocol == 13 && (encryption & 1) == 1;
  }

  enum class reason_e {
    joined, waiting, not_negotiated, retired, unauthenticated, malformed_size, malformed_version,
    malformed_flags, malformed_counts, malformed_range, malformed_time, malformed_sample,
    report_order, clock_regression, loop_regression, clock_unavailable, incomplete, no_accepted,
    frame_unavailable, packet_missing, packet_ambiguous, packet_stale, sample_counts,
    report_stale, no_samples,
  };
  inline const char *name(reason_e reason) noexcept {
    switch (reason) {
#define RECEPTION_REASON(value) case reason_e::value: return #value
      RECEPTION_REASON(joined); RECEPTION_REASON(waiting); RECEPTION_REASON(not_negotiated);
      RECEPTION_REASON(retired); RECEPTION_REASON(unauthenticated); RECEPTION_REASON(malformed_size);
      RECEPTION_REASON(malformed_version); RECEPTION_REASON(malformed_flags); RECEPTION_REASON(malformed_counts);
      RECEPTION_REASON(malformed_range); RECEPTION_REASON(malformed_time); RECEPTION_REASON(malformed_sample);
      RECEPTION_REASON(report_order); RECEPTION_REASON(clock_regression); RECEPTION_REASON(loop_regression);
      RECEPTION_REASON(clock_unavailable); RECEPTION_REASON(incomplete); RECEPTION_REASON(no_accepted);
      RECEPTION_REASON(frame_unavailable); RECEPTION_REASON(packet_missing); RECEPTION_REASON(packet_ambiguous);
      RECEPTION_REASON(packet_stale); RECEPTION_REASON(sample_counts); RECEPTION_REASON(report_stale);
      RECEPTION_REASON(no_samples);
#undef RECEPTION_REASON
    }
    return "waiting";
  }

  struct sample_t { std::uint32_t frame = 0; std::uint16_t sequence = 0; std::uint32_t age_us = 0; };
  struct report_t {
    std::uint8_t flags = 0, count = 0;
    std::uint32_t sequence = 0, interval_us = 0;
    std::uint64_t end_us = 0;
    std::uint32_t loop_tick = 0, data_packets = 0, parity_packets = 0, data_bytes = 0, parity_bytes = 0;
    std::uint32_t last_accepted_age_us = 0, first_frame = 0, last_frame = 0;
    std::array<sample_t, max_samples> samples {};
  };
  struct parsed_t { reason_e reason = reason_e::malformed_size; std::optional<report_t> report; };
  inline parsed_t parse(std::string_view bytes) noexcept {
    const auto fail = [](reason_e reason) { return parsed_t {reason, std::nullopt}; };
    if (bytes.size() < header_bytes || bytes.size() > max_payload_bytes) return fail(reason_e::malformed_size);
    const auto u8 = [&bytes](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    const auto be = [&u8](std::size_t i, std::size_t n) {
      std::uint64_t value = 0;
      for (std::size_t j = 0; j < n; ++j) value = (value << 8) | u8(i + j);
      return value;
    };
    if (u8(0) != 1) return fail(reason_e::malformed_version);
    if ((u8(1) & 0xF0) || u8(3)) return fail(reason_e::malformed_flags);
    if (u8(2) > max_samples || bytes.size() != header_bytes + u8(2) * sample_bytes) return fail(reason_e::malformed_size);
    report_t r;
    r.flags = u8(1); r.count = u8(2); r.sequence = be(4, 4); r.interval_us = be(8, 4);
    if (!r.sequence) return fail(reason_e::report_order);
    r.end_us = be(12, 8); r.loop_tick = be(20, 4);
    r.data_packets = be(24, 4); r.parity_packets = be(28, 4);
    r.data_bytes = be(32, 4); r.parity_bytes = be(36, 4);
    r.last_accepted_age_us = be(40, 4); r.first_frame = be(44, 4); r.last_frame = be(48, 4);
    const auto packets = static_cast<std::uint64_t>(r.data_packets) + r.parity_packets;
    if (static_cast<bool>(r.flags & has_accepted) != (packets != 0)) return fail(reason_e::malformed_counts);
    const auto consistent = [](std::uint32_t count, std::uint32_t bytes) {
      return count == 0 ? bytes == 0 : bytes >= count && bytes <= static_cast<std::uint64_t>(count) * 65535;
    };
    if (!consistent(r.data_packets, r.data_bytes) || !consistent(r.parity_packets, r.parity_bytes) || r.count > packets)
      return fail(reason_e::malformed_counts);
    if ((!packets && (r.first_frame || r.last_frame || r.count)) || r.first_frame > r.last_frame)
      return fail(reason_e::malformed_range);
    if (r.flags & clock_unavailable) {
      if (!(r.flags & incomplete) || r.count || r.interval_us || r.end_us ||
          r.last_accepted_age_us != std::numeric_limits<std::uint32_t>::max()) return fail(reason_e::malformed_time);
    } else {
      if (!r.interval_us || r.end_us < r.interval_us) return fail(reason_e::malformed_time);
      // ROOT wire clarification01: a long scheduling interval is diagnostic only, with no samples.
      if (r.interval_us > max_age_us && (!(r.flags & incomplete) || r.count)) return fail(reason_e::malformed_time);
      if (packets && !(r.flags & incomplete) && r.last_accepted_age_us > r.interval_us)
        return fail(reason_e::malformed_time);
      if (!packets && r.last_accepted_age_us < r.interval_us) return fail(reason_e::malformed_time);
    }
    for (std::size_t i = 0; i < r.count; ++i) {
      const auto pos = header_bytes + i * sample_bytes;
      auto &s = r.samples[i];
      s.frame = be(pos, 4); s.sequence = be(pos + 4, 2); s.age_us = be(pos + 6, 4);
      if (s.frame < r.first_frame || s.frame > r.last_frame || s.age_us > max_age_us || s.age_us > r.end_us ||
          (!(r.flags & incomplete) && s.age_us < r.last_accepted_age_us)) return fail(reason_e::malformed_sample);
      for (std::size_t j = 0; j < i; ++j)
        if (s.frame == r.samples[j].frame && s.sequence == r.samples[j].sequence) return fail(reason_e::malformed_sample);
    }
    return {reason_e::joined, r};
  }

  enum class loop_e { baseline, advancing, unchanged };
  struct correlated_sample_t {
    bool matched = false, parity = false;
    std::uint32_t udp_bytes = 0;
    std::int64_t handoff_duration_ns = 0;
    bool dispersion_available = false;
    std::int64_t dispersion_lower_ns = 0, dispersion_upper_ns = 0;
  };
  struct readback_t {
    bool negotiated = false, has_report = false, available = false;
    bool media_authenticated = false, control_authenticated = false;
    reason_e reason = reason_e::waiting;
    std::uint64_t host_report_age_ms = 0;
    std::uint32_t missing_reports = 0;
    loop_e receive_loop = loop_e::baseline;
    const char *frame_reason = "unavailable";
    report_t report;
    std::optional<stream_frame_feedback::encoder_setting_t> setting;
    std::array<correlated_sample_t, max_samples> samples {};
  };

  /// One allocated instance per trusted stream generation. 512KiB packet storage, no packet heap.
  class scope_t {
  public:
    scope_t(std::uint64_t generation, std::shared_ptr<stream_frame_feedback::sender_ledger_t> frames,
             bool enabled, bool media_authenticated = false) noexcept:
        generation_ {generation}, frames_ {std::move(frames)}, enabled_ {enabled && generation && frames_ &&
          frames_->stream_generation() == generation}, media_authenticated_ {media_authenticated} {
      latest_.negotiated = enabled_; latest_.media_authenticated = media_authenticated_;
    }
    std::uint64_t generation() const noexcept { return generation_; }

    // Exact keys, not unwrapped RTP or capture timestamps. Collision overwrites only retention;
    // a reused identical key remains ambiguous, including batch->single fallback.
    void record(std::int64_t frame, std::uint16_t sequence, std::uint32_t bytes, bool parity,
                clock::time_point begin, clock::time_point end, bool succeeded) noexcept {
      std::lock_guard lock {mutex_};
      if (!enabled_ || retired_) return;
      if (frame < 0 || frame > std::numeric_limits<std::uint32_t>::max()) { key_poison_ = true; return; }
      auto &slot = slots_[index(static_cast<std::uint32_t>(frame), sequence)];
      const bool reused = slot.state && slot.frame == frame && slot.sequence == sequence;
      slot = packet_t {static_cast<std::uint32_t>(frame), sequence,
        static_cast<std::uint16_t>(used | (parity ? is_parity : 0) | ((!succeeded || reused) ? ambiguous : 0)),
        ns(begin), ns(end), bytes, 0};
    }

    reason_e accept(std::string_view payload, bool authenticated, clock::time_point now = clock::now()) noexcept {
      const auto parsed = parse(payload);
      std::lock_guard lock {mutex_};
      if (retired_) return reason_e::retired;
      if (!enabled_) return reason_e::not_negotiated;
      if (!authenticated) return reason_e::unauthenticated;
      if (!parsed.report) return parsed.reason;
      const auto &r = *parsed.report;
      if (clock_failed_) return reason_e::clock_unavailable;
      if (latest_.has_report) {
        if (r.sequence <= latest_.report.sequence) return reason_e::report_order;
        if (!(r.flags & clock_unavailable) && r.end_us <= latest_.report.end_us) return reason_e::clock_regression;
        if (!(r.flags & clock_unavailable)) {
          const auto elapsed = r.end_us - latest_.report.end_us;
          if (elapsed < r.interval_us || (r.sequence == latest_.report.sequence + 1 &&
              r.interval_us != std::numeric_limits<std::uint32_t>::max() && elapsed != r.interval_us))
            return reason_e::malformed_time;
        }
        const auto loop_delta = r.loop_tick - latest_.report.loop_tick;
        if (loop_delta >= 0x80000000u) return reason_e::loop_regression;
        if (now < received_at_) return reason_e::clock_regression;
      }
      readback_t next;
      next.negotiated = true; next.has_report = true; next.report = r;
      next.media_authenticated = media_authenticated_; next.control_authenticated = true;
      if (latest_.has_report) {
        next.missing_reports = r.sequence - latest_.report.sequence - 1;
        next.receive_loop = r.loop_tick == latest_.report.loop_tick ? loop_e::unchanged : loop_e::advancing;
      }
      // Publish only structurally and temporally accepted reports. No rejected packet refreshes age.
      next.reason = correlate(next, now);
      // A contradiction with retained sender identity/bytes is malformed input, not a new reading.
      if (next.reason == reason_e::sample_counts) return next.reason;
      next.available = next.reason == reason_e::joined;
      latest_ = next;
      received_at_ = now;
      clock_failed_ = (r.flags & clock_unavailable) != 0;
      return reason_e::joined;  // Admission, not a claim that the measurement joined.
    }

    std::optional<readback_t> readback(clock::time_point now = clock::now()) const noexcept {
      std::lock_guard lock {mutex_};
      if (retired_ || !enabled_) return std::nullopt;
      auto result = latest_;
      if (result.has_report) {
        if (now < received_at_ || now - received_at_ > retention) clear_metrics(result, reason_e::report_stale);
        if (now >= received_at_) result.host_report_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - received_at_).count();
        // A join cannot outlive any of its sender API boundaries even while the control report is fresh.
        if (result.available && (now < oldest_send_ || now - oldest_send_ > retention)) clear_metrics(result, reason_e::packet_stale);
        if (result.available) {
          const auto frames = frames_->join_inclusive(result.report.first_frame, result.report.last_frame, now, retention);
          if (!frames.available || !result.setting || !(frames.setting == *result.setting)) {
            result.frame_reason = frames.reason;
            clear_metrics(result, reason_e::frame_unavailable);
          }
        }
      }
      return result;
    }
    void retire() noexcept { std::lock_guard lock {mutex_}; retired_ = true; latest_ = {}; }

  private:
    enum : std::uint16_t { used = 1, is_parity = 2, ambiguous = 4 };
    struct packet_t {
      std::uint32_t frame = 0;
      std::uint16_t sequence = 0, state = 0;
      std::int64_t begin_ns = 0, end_ns = 0;
      std::uint32_t bytes = 0, reserved = 0;
    };
    static_assert(sizeof(packet_t) == 32);
    static_assert(ledger_capacity * sizeof(packet_t) == 524288);
    static std::size_t index(std::uint32_t frame, std::uint16_t sequence) noexcept {
      return (static_cast<std::uint32_t>(frame * 2654435761u) ^ sequence) & (ledger_capacity - 1);
    }
    static std::int64_t ns(clock::time_point time) noexcept {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
    static void clear_metrics(readback_t &out, reason_e reason) noexcept {
      out.available = false; out.reason = reason; out.setting.reset(); out.samples = {};
    }
    reason_e correlate(readback_t &out, clock::time_point now) noexcept {
      const auto &r = out.report;
      if (r.flags & clock_unavailable) return reason_e::clock_unavailable;
      if (r.flags & incomplete) return reason_e::incomplete;
      if (!(r.flags & has_accepted)) return reason_e::no_accepted;
      const auto frames = frames_->join_inclusive(r.first_frame, r.last_frame, now, retention);
      out.frame_reason = frames.reason;
      if (!frames.available || key_poison_) return reason_e::frame_unavailable;
      if (!r.count) return reason_e::no_samples;
      std::array<packet_t, max_samples> packets {};
      std::uint64_t data_count = 0, parity_count = 0, data_bytes = 0, parity_bytes = 0;
      const auto now_ns = ns(now);
      const auto bound = std::chrono::duration_cast<std::chrono::nanoseconds>(retention).count();
      auto oldest = now_ns;
      std::uint64_t offered_data = 0, offered_parity = 0, offered_data_bytes = 0, offered_parity_bytes = 0;
      // Fixed 16384-entry upper-bound scan, not a packet census. If retained sender records cannot
      // bound claimed accepted counters, refuse them; eviction never fabricates missing sends.
      for (const auto &p : slots_) {
        if (!p.state || p.frame < r.first_frame || p.frame > r.last_frame) continue;
        if (p.state & ambiguous) return reason_e::packet_ambiguous;
        if (!p.bytes || p.bytes > 65535 || p.begin_ns > p.end_ns || p.end_ns > now_ns || now_ns - p.begin_ns > bound)
          return reason_e::packet_stale;
        oldest = std::min(oldest, p.begin_ns);
        if (p.state & is_parity) { ++offered_parity; offered_parity_bytes += p.bytes; }
        else { ++offered_data; offered_data_bytes += p.bytes; }
      }
      for (std::size_t i = 0; i < r.count; ++i) {
        const auto &s = r.samples[i];
        const auto &p = slots_[index(s.frame, s.sequence)];
        if (!p.state || p.frame != s.frame || p.sequence != s.sequence) return reason_e::packet_missing;
        if (p.state & ambiguous) return reason_e::packet_ambiguous;
        if (!p.bytes || p.bytes > 65535 || p.begin_ns > p.end_ns || p.end_ns > now_ns || now_ns - p.begin_ns > bound)
          return reason_e::packet_stale;
        packets[i] = p;
        oldest = std::min(oldest, p.begin_ns);
        if (p.state & is_parity) { ++parity_count; parity_bytes += p.bytes; }
        else { ++data_count; data_bytes += p.bytes; }
      }
      if (data_count > r.data_packets || parity_count > r.parity_packets || data_bytes > r.data_bytes || parity_bytes > r.parity_bytes)
        return reason_e::sample_counts;
      if (r.data_packets > offered_data || r.parity_packets > offered_parity ||
          r.data_bytes > offered_data_bytes || r.parity_bytes > offered_parity_bytes) return reason_e::sample_counts;
      // Populate derived fields only after every sample and the whole frame range passed.
      out.setting = frames.setting;
      oldest_send_ = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(oldest)));
      for (std::size_t i = 0; i < r.count; ++i) {
        const auto &p = packets[i]; auto &s = out.samples[i];
        s.matched = true; s.parity = p.state & is_parity; s.udp_bytes = p.bytes;
        s.handoff_duration_ns = p.end_ns - p.begin_ns;
        // No absolute host/client subtraction. Reordered API/read pairs do not become a trend.
        if (i && p.begin_ns >= packets[i - 1].begin_ns && r.samples[i - 1].age_us >= r.samples[i].age_us) {
          const auto receive_delta_ns = static_cast<std::int64_t>(r.samples[i - 1].age_us - r.samples[i].age_us) * 1000;
          s.dispersion_available = true;
          s.dispersion_lower_ns = receive_delta_ns - (p.end_ns - packets[i - 1].begin_ns);
          s.dispersion_upper_ns = receive_delta_ns - (p.begin_ns - packets[i - 1].end_ns);
        }
      }
      return reason_e::joined;
    }

    const std::uint64_t generation_;
    const std::shared_ptr<stream_frame_feedback::sender_ledger_t> frames_;
    const bool enabled_, media_authenticated_;
    mutable std::mutex mutex_;
    bool retired_ = false, key_poison_ = false, clock_failed_ = false;
    clock::time_point received_at_ {}, oldest_send_ {};
    readback_t latest_;
    std::array<packet_t, ledger_capacity> slots_ {};
  };
}  // namespace stream_native_reception
