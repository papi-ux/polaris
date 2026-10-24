/** @file src/vrr_trace_clock.h
 * Observe the exact reads already used by collect()/map_source_time(). No extra clock decisions.
 */
#pragma once
#include "vrr_clock_proof.h"
#include "vrr_trace.h"

namespace vrr_trace {
  class clock_system_t final: public vrr_clock::system_t {
  public:
    clock_system_t(vrr_clock::system_t &inner, std::uint64_t epoch): inner_(inner), epoch_(epoch) {}
    std::uint64_t sequence = 0;  // zero during epoch proof; image sequence during its mapping
    std::optional<peer_cred_t> peer_credentials(int fd) override {
      auto result = inner_.peer_credentials(fd);
      auto e = base("clock_peer_read");
      if (result) e.i("pid", result->pid).u("uid", result->uid);
      else e.n("pid").n("uid");
      record(e); return result;
    }
    std::optional<vrr_clock::ns_id_t> time_namespace(std::int64_t pid) override {
      auto result = inner_.time_namespace(pid);
      auto e = base("clock_namespace_read"); e.i("pid", pid);
      if (result) e.u("dev", result->dev).u("ino", result->ino);
      else e.n("dev").n("ino");
      record(e); return result;
    }
    std::optional<std::uint64_t> process_starttime(std::int64_t pid) override {
      auto result = inner_.process_starttime(pid);
      auto e = base("clock_starttime_read"); e.i("pid", pid);
      if (result) e.u("starttime", *result); else e.n("starttime");
      record(e); return result;
    }
    std::uint32_t current_uid() override {
      const auto result = inner_.current_uid();
      record(base("clock_uid_read").u("uid", result)); return result;
    }
    vrr_clock::probe_sample_t sample_clocks() override {
      const auto result = inner_.sample_clocks();
      record(base("clock_bracket").i("mono_before_ns", result.mono_before_ns)
        .i("steady_ns", result.steady_ns).i("mono_after_ns", result.mono_after_ns));
      return result;
    }
    std::int64_t monotonic_now_ns() override {
      const auto result = inner_.monotonic_now_ns();
      record(base("clock_mono_read").i("mono_ns", result)); return result;
    }
  private:
    event_t base(const char *kind) const noexcept {
      return event_t(kind).u("epoch", epoch_).u("source_sequence", sequence);
    }
    vrr_clock::system_t &inner_;
    std::uint64_t epoch_;
  };
}  // namespace vrr_trace
