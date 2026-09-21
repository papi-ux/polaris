/**
 * @file src/platform/linux/game_mode_repaint.h
 * @brief Get a first frame out of a Steam Game Mode screen that is standing still.
 */
#pragma once

#ifdef __linux__

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace platf::game_mode_host {

  /**
   * @brief What asking for a repaint came to.
   */
  enum class repaint_result_e {
    sent,  ///< the window Game Mode is showing was asked to draw itself again
    no_session_display,  ///< no X display on this host belongs to a gamescope
    no_focused_window,  ///< the session's gamescope names no focused window, or the window is gone
    unavailable,  ///< this build carries no X client
  };

  /**
   * @brief The X display a gamescope says holds its focused window.
   *
   * gamescope writes the name as the bytes of a C string, terminating NUL included, into a property it
   * declares as 32-bit cardinals, so what arrives is the name followed by whatever padded the last
   * word. Only a plain `:<number>` is accepted, because the answer is handed straight to a connect
   * call.
   */
  std::optional<std::string> focus_display_from_property(std::span<const std::uint8_t> value);

  /**
   * @brief The local X displays, lowest first.
   *
   * One per `X<number>` socket in the directory. Whether a display belongs to a gamescope is for the
   * caller to settle; this only says where to look.
   */
  std::vector<std::string> local_x_displays(const std::filesystem::path &socket_dir = "/tmp/.X11-unix");

  /**
   * @brief When a capture of the Game Mode screen asks for a repaint.
   *
   * Once as the capture starts, then after each wait that ended with no frame, until the first frame
   * is in or the attempts run out. After the first frame a still screen is no longer a problem: the
   * stream repeats the picture it already has.
   */
  class first_frame_t {
  public:
    static constexpr int k_attempts = 5;

    explicit first_frame_t(bool game_mode_screen):
        attempts_left_ {game_mode_screen ? k_attempts : 0} {
    }

    /// Whether to ask now. Every true answer spends one attempt.
    bool ask() {
      if (frame_seen_ || attempts_left_ <= 0) {
        return false;
      }
      --attempts_left_;
      return true;
    }

    void frame_arrived() {
      frame_seen_ = true;
    }

  private:
    int attempts_left_;
    bool frame_seen_ {false};
  };

  /**
   * @brief Ask the window Game Mode is showing to draw itself again.
   *
   * gamescope pushes a frame to its PipeWire node only when the focused window commits, and it keeps
   * the last commit it pushed across consumers. A capture that starts while the screen stands still
   * therefore gets no frame at all, and the player looks at black until something moves. A synthetic
   * Expose makes the toolkit behind the window present once, which is the frame the stream is waiting
   * for. Nothing is drawn over the window and no input is sent.
   *
   * Only an X server whose root carries gamescope's own properties is ever written to.
   */
  repaint_result_e request_focused_window_repaint();

  /**
   * @brief request_focused_window_repaint() off the calling thread.
   *
   * The capture thread asks, and it must stay interruptible even if the session's Xwayland has
   * stopped answering. At most one request runs at a time; a second one while the first is still out
   * is dropped.
   */
  void request_focused_window_repaint_async();

}  // namespace platf::game_mode_host

#endif
