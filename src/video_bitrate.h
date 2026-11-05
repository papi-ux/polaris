/** @file src/video_bitrate.h
 * The encoder's live-bitrate transaction at the next not-yet-encoded frame.
 */
#pragma once

#include "adaptive_bitrate.h"

namespace video::bitrate {
  enum class result_e { unchanged, superseded, applied, recreate_session, rejected };

  struct result_t {
    result_e result = result_e::unchanged;
    int target_bitrate_kbps = 0;
  };

  // Called after a successful encode, independently of statistics aggregation. The caller polls
  // the request; the actuator still checks its exact revision/target under its mutex at application.
  // Session and trace sink are parameters so tests exercise this same transaction without a GPU.
  template<class Session, class Record>
  result_t apply_after_encode(const std::optional<adaptive_bitrate::live_bitrate_request_t> &request,
                             Session &session, int &applied_bitrate, std::int64_t next_frame_index,
                             bool trace, Record &&record) {
    if (!request) return {};
    if (request->target_bitrate_kbps == applied_bitrate) {
      adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, applied_bitrate, next_frame_index);
      return {};
    }

    using update_e = typename Session::bitrate_update_e;
    auto update = update_e::rejected;
    std::optional<vrr_trace::bitrate_observation_t> before;
    if (trace) before.emplace();
    auto record_update = [&](const char *result, std::optional<int> backend) {
      if (trace) record(*before, *request, applied_bitrate, result, backend);
    };
    const bool current = adaptive_bitrate::apply_live_bitrate_request(*request, [&] {
      update = session.update_bitrate(request->target_bitrate_kbps);
      return update == update_e::applied;
    }, trace ? &*before : nullptr, next_frame_index);
    if (!current) {
      record_update("superseded", std::nullopt);
      return {result_e::superseded, request->target_bitrate_kbps};
    }
    switch (update) {
      case update_e::applied:
        record_update("applied", static_cast<int>(update));
        applied_bitrate = request->target_bitrate_kbps;
        return {result_e::applied, request->target_bitrate_kbps};
      case update_e::recreate_session:
        if (!adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, request->target_bitrate_kbps)) {
          record_update("superseded", static_cast<int>(update));
          return {result_e::superseded, request->target_bitrate_kbps};
        }
        record_update("recreate", static_cast<int>(update));
        return {result_e::recreate_session, request->target_bitrate_kbps};
      case update_e::rejected:
        record_update("rejected", static_cast<int>(update));
        adaptive_bitrate::set_runtime_update_supported(false, "runtime_bitrate_update_failed");
        return {result_e::rejected, request->target_bitrate_kbps};
    }
    return {};
  }
}  // namespace video::bitrate
