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

  /**
   * @brief Whether an existing ext-image-copy session can serve a capture as requested.
   *
   * Whether the compositor paints the cursor is fixed when a session is created, so a
   * session made without it cannot start painting it later. Reusing one after the cursor
   * setting changed kept the cursor out of the stream, and asked the old session for a
   * second frame: on an idle output none comes within the probe window, initialization
   * failed, and the whole pipeline rebuilt about once a second until the screen changed.
   * A new session delivers its first frame straight away.
   */
  inline bool extcopy_session_reusable(bool session_ready, bool session_paints_cursor, bool blend_cursor) {
    return session_ready && session_paints_cursor == blend_cursor;
  }
}  // namespace wl
