/**
 * @file src/platform/linux/kwingrab.h
 * @brief KWin direct ScreenCast (zkde_screencast_unstable_v1) → local PipeWire node.
 *
 * Host KDE paths (desktop_display / headless_dongle / host_virtual_display)
 * try this before portal. Private gamescope/labwc paths are unchanged.
 * HDR: depends on KWin export; do not claim product HDR — see
 * docs/research/kwingrab-spike.md.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "src/capture_generation.h"

namespace kwingrab {

  struct stream_source_t {
    std::uint32_t node_id = 0;
    std::uint64_t object_serial = 0;
    int width = 0;
    int height = 0;
    std::string output_name;
  };

  /**
   * @brief Keep the Wayland screencast stream alive while PipeWire capture runs.
   * Destroying this may tear down the KWin PW node.
   */
  class session_t {
  public:
    session_t();
    ~session_t();

    session_t(const session_t &) = delete;
    session_t &operator=(const session_t &) = delete;
    session_t(session_t &&) noexcept;
    session_t &operator=(session_t &&) noexcept;

    bool valid() const;
    const stream_source_t &source() const;

  private:
    friend std::unique_ptr<session_t> start_output_session(std::string_view output_name);
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  /**
   * @brief Start zkde stream_output for the named output (or first).
   * @return Session holding Wayland objects + PW node/serial, or nullptr on failure.
   */
  std::unique_ptr<session_t> start_output_session(std::string_view output_name = {});

  /// True when an absent requested output may use configured/first output.
  bool output_selection_can_fallback(std::string_view requested_output_name);

  /// True when the immutable generation is a host KDE path that may prefer kwingrab.
  bool prefer_for_generation(const capture_generation::identity_t &generation);

  /// True when this generation must not fall through to generic portal.
  bool require_for_generation(const capture_generation::identity_t &generation);

}  // namespace kwingrab
