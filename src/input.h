/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>
#include <optional>
#include <span>

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "crypto.h"

namespace input {
  struct input_t;
  class retained_gamepad_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM& permission);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  /**
   * @brief Create controller 0 before the app launches.
   * @param client_controller_type An LI_CTYPE_* the client declared on a previous session,
   *        or 0 when nothing is known. The pad has to exist before the app starts so the
   *        game sees it at startup, which is earlier than the client's own arrival packet,
   *        so the only way to get the right pad is to remember the last one.
   * @param retained_owner Authenticated app owner for an isolated private launch.
   * @return An app-owned device lease when retained_owner is set and allocation succeeds.
   */
  std::shared_ptr<retained_gamepad_t> preallocate_gamepad(
    int client_controller_type = 0, const std::string &retained_owner = {});

  /** @brief Where a controller touch lands on the emulated pad's one touchpad. */
  struct controller_touch_point_t {
    float x;  ///< 0 to 1 across the emulated touchpad
    std::uint32_t finger;  ///< The emulated touchpad's finger slot
  };

  /**
   * @brief Map a client controller touch onto the emulated DualSense's touchpad.
   *
   * A pad with two touchpads (LI_CCAP_DUAL_TOUCHPAD, a Steam Controller) sends which one a
   * touch is on. The DualSense has one touchpad with two finger slots, so the left pad takes
   * the left half and the first slot, the right pad the right half and the second, and a game
   * sees both at once. A pad with one touchpad keeps the whole width and its own pointer ids.
   */
  controller_touch_point_t controller_touch_point(float x, std::uint32_t pointer_id, std::uint8_t touchpad_index, bool dual_touchpad);

  /**
   * @brief The input state for one session.
   * @param controllers Whether the session may send controller input. A watch-only session
   *        cannot, and creating controller 0 for it put a second pad on the host that nobody
   *        held, which a couch co-op game counted as a player.
   * @param retained Optional app-owned controller to reuse across reconnects.
   * @param owner Authenticated connection identity, checked before claiming that controller.
   * @return Input context, or null when an existing private controller lease cannot be claimed.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, bool controllers = true,
    std::shared_ptr<retained_gamepad_t> retained = {}, const std::string &owner = {});

#ifdef POLARIS_TESTS
  bool is_valid_input_packet_for_tests(std::span<const std::uint8_t> packet);
  std::shared_ptr<input_t> alloc_queue_for_tests();
  std::size_t queued_input_packet_count_for_tests(const std::shared_ptr<input_t> &input);
  bool batch_input_packets_for_tests(std::vector<std::uint8_t> &dest, const std::vector<std::uint8_t> &src);
#endif

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    // Degrees the compositor turns a touch before it delivers one: 0, 90, 180 or 270. A Steam
    // Deck's gamescope turns every touch from a device it cannot place on a screen by the internal
    // panel's orientation, so a virtual touchscreen's taps land a quarter turn away. A touch is
    // turned back by as much before it is sent.
    int compositor_touch_turn = 0;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Turn a normalized touch back by the compositor's turn, so the turn lands it where it was aimed.
   * @param compositor_touch_turn Degrees the compositor turns a touch: 0, 90, 180 or 270. Anything
   *        else is taken as no turn.
   * @param x The touch across the screen, 0 to 1.
   * @param y The touch down the screen, 0 to 1.
   * @return The touch to send.
   */
  std::pair<float, float> turn_back_touch(int compositor_touch_turn, float x, float y);

  /**
   * @brief Build the mapping from a client's stream onto the captured desktop.
   * @param capture The captured rectangle's offset and size, as the backend reports it.
   * @param env_width The full desktop width.
   * @param env_height The full desktop height.
   * @param stream_width The width the client is streaming at.
   * @param stream_height The height the client is streaming at.
   * @return The touchport for this session.
   */
  touch_port_t make_touch_port(
    const platf::touch_port_t &capture,
    int env_width,
    int env_height,
    int stream_width,
    int stream_height
  );

  /**
   * @brief Build the touchport for a screen its compositor fitted into the captured frame.
   *
   * The screen sits inside the frame by one fit and the frame inside the stream by another, so
   * its place in the stream is both. That is one fit only when the frame and the stream have the
   * same shape.
   * @param screen_width The screen's width, inside the frame.
   * @param screen_height The screen's height, inside the frame.
   * @param frame_width The captured frame's width.
   * @param frame_height The captured frame's height.
   * @param stream_width The width the client is streaming at.
   * @param stream_height The height the client is streaming at.
   * @return The touchport for this session.
   */
  touch_port_t make_touch_port_in_frame(
    int screen_width,
    int screen_height,
    int frame_width,
    int frame_height,
    int stream_width,
    int stream_height
  );

  /**
   * @brief Why a client coordinate could not be mapped onto the touchport.
   *
   * One warning covering every one of these told a reporter on nova#302 only
   * that something was out of bounds, several hundred times a second, while the
   * pointer sat still.
   */
  enum class touchport_reject_e {
    none,  ///< The coordinate mapped.
    client_surface_empty,  ///< The client described a surface with no area.
    capture_viewport_empty,  ///< The capture never reported its own size.
    letterbox_bounds_inverted,  ///< The letterbox offsets do not bracket the frame.
  };

  /**
   * @brief Name a rejection reason for a log line.
   * @param reason The reason to name.
   * @return A short human readable phrase.
   */
  std::string_view touchport_reject_name(touchport_reject_e reason);

  /**
   * @brief Convert client coordinates on the specified surface into touchport coordinates.
   * @param touch_port The active touchport mapping.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @param reason Optional out parameter naming why a mapping was refused.
   * @return The host-relative coordinate pair if the mapping bounds are valid.
   */
  std::optional<std::pair<float, float>> map_client_to_touchport(
    const touch_port_t &touch_port,
    const std::pair<float, float> &val,
    const std::pair<float, float> &size,
    touchport_reject_e *reason = nullptr
  );

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
