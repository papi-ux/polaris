/**
 * @file src/stream_pacing.h
 * @brief When the video sender lets each frame's packets leave, and how wire pacing changes it.
 *
 * The sender spaces a frame's packets at a fixed rate: about 80% of 1 Gbps, whatever the session
 * negotiated. On Wi-Fi that sends short bursts far above the stream's own rate into the access point's
 * queue. Wire pacing (`wire_pacing`, experimental and off by default) paces a session at its own
 * negotiated total instead, times `wire_pacing_headroom` percent, and never faster than the old rate.
 *
 * Pacing changes when packets leave, not which packets are sent or what they carry. The one value that
 * follows the schedule is a duplicate frame's timestamp, which has always come from it for want of a
 * capture time. With wire pacing off, every value here is the sender's long-standing arithmetic.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string_view>
#include <vector>

#ifdef __linux__
  #include <cerrno>
  #include <time.h>
#endif

namespace stream_pacing {
  using namespace std::literals;

  /// The longest the sender waits at once, so a cancelled frame stops within this.
  inline constexpr std::chrono::nanoseconds k_max_wait_step = 10ms;

  /// Inputs past these are no real session, and keep the legacy rate rather than overflow the arithmetic.
  inline constexpr std::int64_t k_max_negotiated_kbps = 100'000'000;
  inline constexpr int k_max_headroom_percent = 100'000;

  /// The rate the sender has always used: around 80% of 1 Gbps, in whole packets of `blocksize` bytes per millisecond.
  inline std::size_t legacy_packets_in_1ms(std::size_t blocksize) {
    //      1Gbps            percent    ms     packet      byte
    return std::giga::num * 80 / 100 / 1000 / blocksize / 8;
  }

  /// The rate one frame is sent at.
  struct rate_t {
    /// Wire pacing is on: this session keeps its own schedule and sleeps to absolute deadlines.
    bool enabled = false;
    /// The rate is this session's negotiated total times the headroom. false: the legacy rate.
    bool session_rate = false;
    /// How many packets go out before the sender checks its schedule again, about one millisecond's worth.
    std::size_t packets_in_1ms = 0;
    /// The rate in whole packets a second, as the send_constants trace reports it.
    std::uint64_t packets_per_second = 0;
    /// A session rate exactly, which its schedule uses so no rounding of the rate mispaces it. 0 for the legacy rate.
    std::uint64_t bits_per_second = 0;
    /// Bits in one packet on the wire.
    std::uint64_t bits_per_packet = 0;
  };

  /**
   * @brief Choose the rate a frame is sent at.
   * @param enabled `wire_pacing`.
   * @param negotiated_kbps The session's negotiated total, `stream_bitrate::request_t::split_kbps`. 0 when the
   *                        handshake split none, for a client that sent no bitrate or a watcher.
   * @param headroom_percent `wire_pacing_headroom`, the share of the negotiated total to pace at.
   * @param blocksize Bytes per packet on the wire.
   * @return The legacy rate with wire pacing off, with no negotiated total, or when the session's rate would
   *         not be slower than it. Otherwise the session's own rate.
   */
  inline rate_t select_rate(bool enabled, std::int64_t negotiated_kbps, int headroom_percent, std::size_t blocksize) {
    const auto legacy = legacy_packets_in_1ms(blocksize);
    const std::uint64_t bits_per_packet = static_cast<std::uint64_t>(blocksize) * 8;
    const rate_t legacy_rate {enabled, false, legacy, static_cast<std::uint64_t>(legacy) * 1000, 0, bits_per_packet};
    if (!enabled || negotiated_kbps <= 0 || negotiated_kbps > k_max_negotiated_kbps ||
        headroom_percent <= 0 || headroom_percent > k_max_headroom_percent || blocksize == 0) {
      return legacy_rate;
    }

    // kbps times percent over 100 is kbps times 10 in bits a second: exact, with nothing rounded away.
    const auto bits_per_second = static_cast<std::uint64_t>(negotiated_kbps) * 10 * static_cast<std::uint64_t>(headroom_percent);
    // Streams past the old rate are #223's, and a rate under one packet a second paces nothing usefully.
    if (bits_per_second < bits_per_packet || bits_per_second >= legacy_rate.packets_per_second * bits_per_packet) {
      return legacy_rate;
    }

    const auto packets_in_1ms = std::max<std::uint64_t>(1, bits_per_second / (bits_per_packet * 1000));
    return {true, true, static_cast<std::size_t>(packets_in_1ms), bits_per_second / bits_per_packet, bits_per_second, bits_per_packet};
  }

  /// What a frame's send_constants trace says paced it.
  inline std::string_view rate_name(const rate_t &rate) {
    if (!rate.enabled) {
      return "off"sv;
    }
    return rate.session_rate ? "session"sv : "legacy"sv;
  }

  /// How long after a frame's start its first `packets_sent` packets are due.
  inline std::chrono::nanoseconds send_offset(const rate_t &rate, std::size_t packets_sent) {
    if (!rate.session_rate) {
      // The sender's own expression for the legacy rate.
      return std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) * packets_sent / rate.packets_in_1ms;
    }

    // packets times bits over the rate, in nanoseconds, rounded down once. Splitting off whole seconds keeps
    // the product in range: the remainder is under the rate, which is under the legacy rate.
    const std::uint64_t bits = static_cast<std::uint64_t>(packets_sent) * rate.bits_per_packet;
    const std::uint64_t whole_seconds = bits / rate.bits_per_second;
    const std::uint64_t remainder_ns = bits % rate.bits_per_second * std::uint64_t {1'000'000'000} / rate.bits_per_second;
    return std::chrono::seconds {static_cast<std::int64_t>(whole_seconds)} + std::chrono::nanoseconds {static_cast<std::int64_t>(remainder_ns)};
  }

  /// The most packets sent at once. At its own rate a session sends at most its millisecond's budget, so a send
  /// is no larger a burst than the rate it is paced at.
  inline std::size_t send_batch_limit(const rate_t &rate, std::size_t legacy_batch) {
    return rate.session_rate ? std::min(legacy_batch, rate.packets_in_1ms) : legacy_batch;
  }

  /// When the sender next wakes to check for cancellation on its way to `due`.
  inline std::chrono::steady_clock::time_point next_wake(std::chrono::steady_clock::time_point due, std::chrono::steady_clock::time_point now) {
    return std::min(due, now + k_max_wait_step);
  }

  /**
   * @brief Each session's own schedule on the shared sender.
   *
   * One thread sends every session's video. With wire pacing on, each session keeps the time its next frame
   * may start, so the time another session's frame is still due to take never delays this one. That time is
   * also what a duplicate frame of the session is stamped with, as the shared schedule stamps one with pacing
   * off. A session that has sent nothing for a while is forgotten.
   */
  class session_starts_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    /// How long after its schedule ran out a session that sent nothing is forgotten.
    static constexpr std::chrono::seconds k_forget_after = 1s;

    /// When a frame of `session` may start, given the time is `now`.
    time_point frame_start(std::uint64_t session, time_point now) {
      std::erase_if(next_, [now](const entry_t &entry) {
        return entry.next + k_forget_after < now;
      });
      const auto scheduled = next(session);
      return scheduled ? std::max(*scheduled, now) : now;
    }

    /// Records that `session`'s next frame may start no earlier than `next`.
    void frame_end(std::uint64_t session, time_point next) {
      const auto found = std::ranges::find(next_, session, &entry_t::session);
      if (found == next_.end()) {
        next_.push_back({session, next});
      } else {
        found->next = next;
      }
    }

    /// When `session`'s next frame may start, if it has a schedule.
    std::optional<time_point> next(std::uint64_t session) const {
      const auto found = std::ranges::find(next_, session, &entry_t::session);
      return found == next_.end() ? std::nullopt : std::optional<time_point> {found->next};
    }

    /// How many sessions have a schedule.
    std::size_t size() const {
      return next_.size();
    }

  private:
    struct entry_t {
      std::uint64_t session;
      time_point next;
    };

    std::vector<entry_t> next_;
  };

  /**
   * @brief Sleep until `until` as an absolute CLOCK_MONOTONIC deadline.
   *
   * Nothing guarantees std::chrono::steady_clock and CLOCK_MONOTONIC share an epoch, so the two are read
   * together and `until` is placed on CLOCK_MONOTONIC from the pair. steady_clock is read first, so the
   * deadline errs late by the gap between the reads, never early. A signal does not restart the wait, it
   * resumes toward the same deadline.
   * @return true once `until` has passed. false where there is no absolute sleep, or when the clock refused
   *         it; the caller then keeps its relative timer, so a failure never turns into a busy wait.
   */
  inline bool sleep_until(std::chrono::steady_clock::time_point until) {
#ifdef __linux__
    const auto steady_now = std::chrono::steady_clock::now();
    timespec monotonic_now {};
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic_now) != 0) {
      return false;
    }
    if (until <= steady_now) {
      return true;
    }

    const auto target = std::chrono::seconds {monotonic_now.tv_sec} + std::chrono::nanoseconds {monotonic_now.tv_nsec} +
                        std::chrono::duration_cast<std::chrono::nanoseconds>(until - steady_now);
    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(target);
    timespec deadline {};
    deadline.tv_sec = static_cast<time_t>(whole_seconds.count());
    deadline.tv_nsec = static_cast<long>((target - whole_seconds).count());

    for (;;) {
      const int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
      if (error == 0) {
        return true;
      }
      if (error != EINTR) {
        return false;
      }
    }
#else
    (void) until;
    return false;
#endif
  }
}  // namespace stream_pacing
