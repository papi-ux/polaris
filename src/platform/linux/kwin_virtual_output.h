/**
 * @file src/platform/linux/kwin_virtual_output.h
 * @brief A new screen from KWin for Host Virtual Display.
 *
 * KWin creates an output for a zkde_screencast_unstable_v1
 * stream_virtual_output stream and removes it when that stream closes or the
 * client that asked for it disconnects. Polaris keeps that stream, the anchor,
 * open for as long as the display is in use and never consumes it: capture
 * opens its own stream of the output through kwingrab, so a capture rebuild
 * cannot take the screen away. A process that dies takes its screens with it.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kwin_virtual_output {

  /** @brief Whether KWin will create an output for this process. */
  struct probe_t {
    bool available = false;
    std::uint32_t version = 0;  ///< zkde_screencast_unstable_v1 version KWin offers, 0 when none
    std::string reason;  ///< Why not, when not available
  };

  /**
   * @brief Ask KWin whether it offers the screencast protocol at a usable version.
   *
   * Writes the permission entry KWin needs when none exists. KWin reads that
   * entry when a client connects, so a probe that just wrote it connects again
   * for a few seconds before giving up.
   */
  probe_t probe();

  /** @brief Every output name the compositor publishes now; nullopt when it cannot be asked. */
  std::optional<std::vector<std::string>> output_names();

  /** @brief An output KWin created and this process now holds. */
  struct created_t {
    std::string output_name;  ///< The name KWin published, proven to be a new output
    int width = 0;
    int height = 0;
  };

  /**
   * @brief Ask KWin for a new output and hold its anchor stream.
   * @param request_name The name to ask for; KWin publishes `Virtual-<request_name>`.
   * @param scale How many pixels the screen puts in a point, so a phone-sized
   *        panel gets a desktop it can be read at. KWin takes width and height
   *        as the pixel mode, so the scale divides the logical size rather than
   *        multiplying the mode: 2560x1600 at 2 is a 1280x800 desktop, sharp.
   * @param error Set to the reason when this returns nullopt.
   * @return nullopt when KWin refused, or the output did not appear as a new
   *         output with exactly the expected name. Nothing is held then.
   */
  std::optional<created_t> create(const std::string &request_name, int width, int height, double scale, std::string &error);

  /** @brief Whether this process image holds the anchor for an output. */
  bool anchored(const std::string &output_name);

  /**
   * @brief Load the KWin script that moves new windows onto an output.
   *
   * The output stays secondary, so Plasma leaves the desktop and panel where
   * they are, and a game still opens on the stream. With several Polaris
   * screens, the newest takes new windows and the others stop, so they never
   * race for one. Replaces a copy a crashed Polaris left loaded.
   * @param error Set to the reason when this returns false, including when KWin
   *        dropped the script because it did not evaluate.
   */
  bool follow_windows(const std::string &output_name, std::string &error);

  /**
   * @brief Unload the script follow_windows loaded for an output. Safe when none is loaded.
   *
   * When that output was the newest, the one before it takes new windows again.
   */
  void stop_following_windows(const std::string &output_name);

  /**
   * @brief Close the anchor so KWin removes the output, then wait until the
   * output has stayed absent for 500 ms.
   * @return true when removal was verified before the deadline.
   */
  bool release(const std::string &output_name, std::chrono::milliseconds budget);

  /**
   * @brief The name KWin gives an input device, looked up by its kernel event name.
   * @param sys_name The device's `eventN`.
   * @param error Set to the reason when this returns nullopt, including while
   *        KWin has not picked up a device that was just created.
   */
  std::optional<std::string> input_device_name(const std::string &sys_name, std::string &error);

  /** @brief The output an input device is tied to, empty for none; nullopt when KWin cannot say. */
  std::optional<std::string> input_device_output(const std::string &sys_name, std::string &error);

  /**
   * @brief Point an input device at an output, or at none with an empty name.
   *
   * KWin maps a touch screen, a tablet or an absolute pointer onto the output
   * named here, and saves the choice in kcminputrc under the device's name, so
   * it outlives the device and the output alike.
   * @param error Set to the reason when this returns false.
   */
  bool set_input_device_output(const std::string &sys_name, const std::string &output_name, std::string &error);

}  // namespace kwin_virtual_output
