#pragma once

namespace wl {
  /**
   * @brief Describes how the next ext-image-copy snapshot obtains its frame.
   */
  enum class extcopy_frame_source_e {
    initialize,
    prefetched,
    capture,
  };

  /**
   * @brief Select the frame source for an ext-image-copy snapshot.
   *
   * extcopy initialization performs a real capture to validate its negotiated
   * buffer. That frame must be handed to the first snapshot exactly once;
   * otherwise an idle, damage-driven output waits unnecessarily for a second
   * frame. Reinitialization always replaces any stale prefetched frame.
   */
  inline extcopy_frame_source_e select_extcopy_frame_source(
    bool capture_ready,
    bool cursor_changed,
    bool &prefetched_frame_pending
  ) {
    if (!capture_ready || cursor_changed) {
      prefetched_frame_pending = false;
      return extcopy_frame_source_e::initialize;
    }

    if (prefetched_frame_pending) {
      prefetched_frame_pending = false;
      return extcopy_frame_source_e::prefetched;
    }

    return extcopy_frame_source_e::capture;
  }
}  // namespace wl
