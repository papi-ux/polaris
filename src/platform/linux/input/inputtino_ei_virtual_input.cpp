/**
 * @file src/platform/linux/input/inputtino_ei_virtual_input.cpp
 * @brief Emulated-input routing into a Polaris-owned gamescope.
 */
// local includes
#include "inputtino_ei_virtual_input.h"

#include "src/config.h"
#include "src/logging.h"

#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <cctype>
  #include <cerrno>
  #include <chrono>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>

  #include <fcntl.h>
  #include <poll.h>
  #include <unistd.h>

  // lib includes
  #include <inputtino/keyboard.hpp>
  #include <libei.h>
  #include <linux/input-event-codes.h>
#endif

using namespace std::literals;

namespace platf {

#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT

  namespace {
    /**
     * @brief Map a Moonlight mouse button to its evdev code.
     *
     * gamescope hands EIS_EVENT_BUTTON_BUTTON straight to wlserver_mousebutton,
     * which expects evdev, so no translation layer is needed.
     */
    int mouse_button_to_evdev(int button) {
      switch (button) {
        case BUTTON_LEFT:
          return BTN_LEFT;
        case BUTTON_MIDDLE:
          return BTN_MIDDLE;
        case BUTTON_RIGHT:
          return BTN_RIGHT;
        case BUTTON_X1:
          return BTN_SIDE;
        case BUTTON_X2:
          return BTN_EXTRA;
        default:
          return -1;
      }
    }

    /**
     * @brief Map a Moonlight key to its evdev scancode.
     *
     * libei keyboard codes are evdev scancodes, the same thing the Wayland
     * route sends, so the mapping table is shared and needs no offset.
     */
    int moonlight_key_to_evdev(std::uint16_t modcode) {
      auto key = inputtino::keyboard::key_mappings.find(static_cast<short>(modcode));
      if (key == inputtino::keyboard::key_mappings.end()) {
        return -1;
      }
      return key->second.linux_code;
    }

    /**
     * @brief Scroll scale, in Moonlight units per wheel notch.
     *
     * gamescope's wlserver_mousewheel() takes notches, not axis units: it
     * passes the value straight through as the continuous wl_pointer axis and
     * derives axis_value120 from it as value * WLR_POINTER_AXIS_DISCRETE_STEP.
     * Its own physical-input paths feed it value120 / 120.0 (LibInputHandler
     * and the Wayland backend both do), so one notch has to arrive here as
     * 1.0 to scroll like the mouse plugged into the host.
     */
    constexpr double k_scroll_units_per_notch = 120.0;

    /// Wait between connection attempts, so a missing socket cannot make every
    /// motion event rebuild a sender.
    constexpr auto k_reconnect_interval = 1s;

    /// How long a connection that answered may go without offering a device
    /// before it is treated as dead. Bounds the window in which input is
    /// swallowed by a handshake that is never going to finish.
    constexpr auto k_handshake_deadline = 2s;

    /// Wait before trying again after a handshake that stalled. Longer than
    /// k_reconnect_interval: every attempt at a socket that answers and then
    /// says nothing costs a reader thread and the whole deadline.
    constexpr auto k_stalled_retry_interval = 10s;
  }  // namespace

  struct ei_virtual_input_t::impl_t {
    std::mutex mutex;
    ei *ctx = nullptr;
    ei_device *device = nullptr;
    bool emulating = false;
    std::uint32_t sequence = 0;
    bool logged_ready = false;
    bool logged_unavailable = false;
    bool logged_stalled = false;
    /// Whether this connection has offered a device at all. The handshake
    /// deadline is about a connection that never does; a device that was
    /// offered and later paused is the compositor's call, not a dead socket.
    bool device_seen = false;
    /// Set when the EIS socket could not be reached, so input can fall back to
    /// host uinput instead of vanishing into a compositor that never started.
    bool connect_failed = false;
    std::chrono::steady_clock::time_point next_connect_attempt {};
    std::chrono::steady_clock::time_point connected_at {};
    /// Last absolute position, so absolute input can be sent as relative motion.
    std::optional<std::pair<double, double>> last_absolute;

    /// Reader thread: the EIS handshake needs several round trips, and none of
    /// them may happen on the caller's thread — Polaris runs task_pool.start(1),
    /// so a blocking wait here would also hold up key repeat, the click delay
    /// and the force-shutdown timer.
    std::thread pumper;
    std::atomic<bool> pumper_stop {false};
    std::atomic<bool> pumper_exited {false};
    int wake_fds[2] = {-1, -1};

    ~impl_t() {
      shutdown();
      close_wake_pipe();
    }

    bool open_wake_pipe() {
      if (wake_fds[0] >= 0) {
        return true;
      }
      if (::pipe2(wake_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        wake_fds[0] = wake_fds[1] = -1;
        return false;
      }
      return true;
    }

    void close_wake_pipe() {
      for (int &fd : wake_fds) {
        if (fd >= 0) {
          ::close(fd);
          fd = -1;
        }
      }
    }

    void wake_pumper() {
      if (wake_fds[1] >= 0) {
        const char byte = 0;
        [[maybe_unused]] const auto written = ::write(wake_fds[1], &byte, 1);
      }
    }

    void drain_wake_pipe() {
      char buffer[64];
      while (wake_fds[0] >= 0 && ::read(wake_fds[0], buffer, sizeof(buffer)) > 0) {
      }
    }

    void release_device() {
      if (device) {
        ei_device_unref(device);
        device = nullptr;
      }
      emulating = false;
      last_absolute.reset();
    }

    /// Drop the connection. Caller holds the mutex; the reader thread notices
    /// the null context and retires itself.
    void disconnect_locked() {
      release_device();
      if (ctx) {
        ei_unref(ctx);
        ctx = nullptr;
      }
      logged_ready = false;
      device_seen = false;
      wake_pumper();
    }

    /**
     * @brief Reap the reader thread.
     *
     * @param force Stop a thread that is still running. Only ever called with
     *              the mutex released — the thread takes it on every pass, so
     *              joining while holding it would deadlock. Without @p force
     *              this joins only a thread that has already run to completion,
     *              which is safe from under the lock.
     */
    void join_pumper(bool force) {
      if (!pumper.joinable()) {
        return;
      }
      if (force) {
        pumper_stop = true;
        wake_pumper();
      } else if (!pumper_exited) {
        return;
      }
      pumper.join();
      pumper_stop = false;
      pumper_exited = false;
      drain_wake_pipe();
    }

    void shutdown() {
      {
        std::scoped_lock lock(mutex);
        disconnect_locked();
      }
      join_pumper(true);
    }

    /**
     * @brief Whether this host streams through a Polaris-owned gamescope.
     *
     * The same pair of signals process.cpp classifies a gamescope launch by
     * (streaming_launch_requests_private_family). Every other mode either has
     * the Wayland route or wants host uinput.
     */
    bool gamescope_runtime_active() const {
      return config::video.linux_display.stream_mode == "gamescope_stream"sv ||
             config::video.linux_display.private_runtime == "gamescope"sv;
    }

    /**
     * @brief Whether an event we cannot deliver should still be swallowed.
     *
     * Until a connection fails, and after a failure only once a device is
     * emulating again. A host configured for gamescope whose compositor never
     * came up, or whose socket answers and then offers nothing, has nothing to
     * receive the event, and claiming it there would leave that host with no
     * input at all rather than falling back to uinput.
     */
    bool owns_input() const {
      return gamescope_runtime_active() && !connect_failed;
    }

    /**
     * @brief The EIS socket gamescope listens on.
     *
     * gamescope names it after its own Wayland display ("%s-ei") and exports it
     * as LIBEI_SOCKET for its children. Polaris is gamescope's parent, so it
     * derives the same name instead of inheriting it. libei resolves a relative
     * name against XDG_RUNTIME_DIR, which is where gamescope puts it.
     */
    std::string socket_name() const {
      if (const char *inherited = std::getenv("LIBEI_SOCKET"); inherited && *inherited) {
        return inherited;
      }
      const char *display = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
      return std::string {display && *display ? display : "gamescope-0"} + "-ei";
    }

    /// Handle everything the server has already sent. Never blocks.
    void drain_locked() {
      if (!ctx) {
        return;
      }

      ei_dispatch(ctx);

      while (ei_event *event = ei_get_event(ctx)) {
        switch (ei_event_get_type(event)) {
          case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(
              ei_event_get_seat(event),
              EI_DEVICE_CAP_POINTER,
              EI_DEVICE_CAP_POINTER_ABSOLUTE,
              EI_DEVICE_CAP_BUTTON,
              EI_DEVICE_CAP_SCROLL,
              EI_DEVICE_CAP_KEYBOARD,
              nullptr
            );
            break;
          case EI_EVENT_DEVICE_ADDED:
            release_device();
            device = ei_device_ref(ei_event_get_device(event));
            device_seen = true;
            break;
          case EI_EVENT_DEVICE_RESUMED:
            if (device) {
              ei_device_start_emulating(device, ++sequence);
              emulating = true;
              // Only now is the input ours again after a failure: see the
              // note where ensure_ready() connects.
              connect_failed = false;
              logged_stalled = false;
              if (!logged_ready) {
                BOOST_LOG(info) << "EI virtual input: routing mouse and keyboard to the gamescope session on ["sv
                                << socket_name() << ']';
                logged_ready = true;
              }
            }
            break;
          case EI_EVENT_DEVICE_PAUSED:
            emulating = false;
            break;
          case EI_EVENT_DEVICE_REMOVED:
            release_device();
            break;
          case EI_EVENT_DISCONNECT:
            BOOST_LOG(info) << "EI virtual input: gamescope closed the input socket; "sv
                            << "reconnecting on the next input event"sv;
            ei_event_unref(event);
            disconnect_locked();
            return;
          default:
            break;
        }
        ei_event_unref(event);
      }
    }

    /// Reader thread body.
    void pump_loop() {
      for (;;) {
        int fd = -1;
        {
          std::scoped_lock lock(mutex);
          if (pumper_stop || !ctx) {
            break;
          }
          fd = ei_get_fd(ctx);
        }
        if (fd < 0) {
          break;
        }

        pollfd pfds[2] = {
          {fd, POLLIN, 0},
          {wake_fds[0], POLLIN, 0},
        };
        if (::poll(pfds, 2, -1) < 0 && errno != EINTR) {
          break;
        }
        if (pfds[1].revents & POLLIN) {
          // A wake only ever means "look at the state again", which the checks
          // below do. The byte still has to go: left in the pipe it makes every
          // later poll() return at once. disconnect_locked() writes one even
          // when no reader is running to be joined and drained after, so the
          // next connection's reader would otherwise spin on it, taking the
          // mutex the input path needs on every pass.
          drain_wake_pipe();
        }
        if (pumper_stop) {
          break;
        }

        std::scoped_lock lock(mutex);
        if (pumper_stop || !ctx) {
          break;
        }
        drain_locked();
      }
      // Last thing this thread touches: join_pumper() reads it to decide
      // whether it may join from under the mutex.
      pumper_exited = true;
    }

    /**
     * @brief Connect if needed and report whether a device is emulating.
     *
     * Never blocks. The handshake runs on the reader thread, so the first
     * event of a session is dropped rather than delivered late; every event
     * after it — a millisecond or so later — finds the device ready.
     */
    bool ensure_ready() {
      join_pumper(false);

      if (!gamescope_runtime_active()) {
        if (ctx) {
          disconnect_locked();
        }
        return false;
      }

      const auto now = std::chrono::steady_clock::now();

      if (ctx) {
        if (emulating) {
          return true;
        }
        // The socket answered but no device ever arrived. Let go of it rather
        // than keep swallowing input into a connection that is not going to
        // carry any; owns_input() then lets uinput have the events back.
        if (!device_seen && now - connected_at > k_handshake_deadline) {
          if (!logged_stalled) {
            BOOST_LOG(warning) << "EI virtual input: gamescope accepted the connection but never offered a "sv
                               << "device; mouse and keyboard will use host uinput"sv;
            logged_stalled = true;
          }
          disconnect_locked();
          connect_failed = true;
          next_connect_attempt = now + k_stalled_retry_interval;
        }
        return false;
      }

      if (now < next_connect_attempt) {
        return false;
      }
      next_connect_attempt = now + k_reconnect_interval;

      if (pumper.joinable()) {
        // The previous reader has not retired yet; try again after the backoff.
        return false;
      }
      if (!open_wake_pipe()) {
        connect_failed = true;
        return false;
      }

      ctx = ei_new_sender(nullptr);
      if (!ctx) {
        connect_failed = true;
        return false;
      }
      ei_configure_name(ctx, "polaris");

      const auto socket = socket_name();
      if (const int rc = ei_setup_backend_socket(ctx, socket.c_str()); rc != 0) {
        if (!logged_unavailable) {
          BOOST_LOG(warning) << "EI virtual input: cannot reach gamescope's input socket ["sv << socket
                             << "]: "sv << strerror(-rc) << "; mouse and keyboard will use host uinput"sv;
          logged_unavailable = true;
        }
        ei_unref(ctx);
        ctx = nullptr;
        connect_failed = true;
        return false;
      }
      logged_unavailable = false;
      // connect_failed stays as it was. After a failure the events belong to
      // host uinput until a device is emulating, and drain_locked() clears the
      // flag there. Clearing it here handed a socket that answers and then
      // stalls the input back on every retry: two seconds swallowed, one event
      // through, two seconds swallowed, for as long as the socket stayed up.
      connected_at = now;

      drain_locked();
      pumper_stop = false;
      pumper_exited = false;
      pumper = std::thread([this] {
        pump_loop();
      });
      return emulating;
    }

    /**
     * @brief Close the event group so gamescope applies it.
     */
    bool frame() {
      if (!device) {
        return false;
      }
      ei_device_frame(device, ei_now(ctx));
      return true;
    }

    bool send_scroll(double dx, double dy) {
      if (!ensure_ready()) {
        return false;
      }
      ei_device_scroll_delta(device, dx, dy);
      return frame();
    }

    bool send_key(std::uint16_t modcode, bool release) {
      const auto evdev_keycode = moonlight_key_to_evdev(modcode);
      if (evdev_keycode < 0) {
        return false;
      }
      ei_device_keyboard_key(device, static_cast<std::uint32_t>(evdev_keycode), !release);
      return frame();
    }
  };

#else

  struct ei_virtual_input_t::impl_t {};

#endif

  ei_virtual_input_t::ei_virtual_input_t():
      impl(std::make_unique<impl_t>()) {
  }

  ei_virtual_input_t::~ei_virtual_input_t() = default;

  void ei_virtual_input_t::reset() {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    impl->shutdown();
#endif
  }

  bool ei_virtual_input_t::should_block_host_fallback() {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    // Host uinput cannot reach a headless gamescope, and letting it through
    // would drive the host session instead.
    return impl->owns_input();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::move(int delta_x, int delta_y) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_ready()) {
      return false;
    }
    ei_device_pointer_motion(impl->device, delta_x, delta_y);
    // The pointer has moved out from under the remembered absolute position,
    // and Moonlight mixes relative drag with absolute taps — keeping the old
    // anchor would turn the next absolute event into a delta from a stale
    // origin and the offset would stick for the rest of the session.
    impl->last_absolute.reset();
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::move_abs(const touch_port_t &touch_port, float x, float y) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_ready() || touch_port.width <= 0 || touch_port.height <= 0) {
      return false;
    }

    // gamescope maps absolute motion to wlserver_mousewarp() with
    // bSynthetic = true, and that path deliberately leaves bCursorHidden alone
    // — an absolute-only client would move the pointer without ever revealing
    // the cursor. Relative motion goes to wlserver_mousemotion(), which does
    // reveal it, so carry absolute input as deltas once there is a previous
    // position to subtract.
    const double target_x = std::clamp(double(x), 0.0, double(touch_port.width));
    const double target_y = std::clamp(double(y), 0.0, double(touch_port.height));

    if (impl->last_absolute) {
      const auto [previous_x, previous_y] = *impl->last_absolute;
      impl->last_absolute = {target_x, target_y};
      ei_device_pointer_motion(impl->device, target_x - previous_x, target_y - previous_y);
    } else {
      impl->last_absolute = {target_x, target_y};
      ei_device_pointer_motion_absolute(impl->device, target_x, target_y);
    }
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::button(int button, bool release) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    const auto evdev_button = mouse_button_to_evdev(button);
    if (evdev_button < 0) {
      BOOST_LOG(warning) << "EI virtual input: unknown mouse button: "sv << button;
      // Claim it anyway: falling through would send it to the host session.
      return impl->owns_input();
    }
    if (!impl->ensure_ready()) {
      return false;
    }
    ei_device_button_button(impl->device, static_cast<std::uint32_t>(evdev_button), !release);
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::scroll(int high_res_distance) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    // Scroll deltas are continuous here, so a sub-notch flick is carried
    // rather than rounded away as it would be with discrete wheel clicks.
    return impl->send_scroll(0.0, -high_res_distance / k_scroll_units_per_notch);
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::hscroll(int high_res_distance) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    return impl->send_scroll(high_res_distance / k_scroll_units_per_notch, 0.0);
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::keyboard_update(std::uint16_t modcode, bool release) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (moonlight_key_to_evdev(modcode) < 0) {
      return impl->owns_input();
    }
    if (!impl->ensure_ready()) {
      return false;
    }
    return impl->send_key(modcode, release);
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::unicode(std::string_view hex_unicode) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_ready()) {
      return false;
    }

    // Same ibus hex entry the Wayland route types: ctrl+shift+u, the
    // codepoint's hex digits, then release. Whether the focused client picks
    // it up is its business — what matters is that the text stays inside the
    // session instead of being typed into the host desktop.
    const auto send = [this](std::uint16_t modcode, bool release) {
      return impl->send_key(modcode, release);
    };

    if (!send(0xA2, false) || !send(0xA0, false) || !send(0x55, false) || !send(0x55, true)) {
      return false;
    }

    for (const auto ch : hex_unicode) {
      std::uint16_t moonlight_code = 0;
      if (ch >= '0' && ch <= '9') {
        moonlight_code = static_cast<std::uint16_t>(0x30 + (ch - '0'));
      } else {
        const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (upper < 'A' || upper > 'F') {
          BOOST_LOG(warning) << "EI virtual input: unable to find keycode for: "sv << ch;
          continue;
        }
        moonlight_code = static_cast<std::uint16_t>(0x41 + (upper - 'A'));
      }

      if (!send(moonlight_code, false) || !send(moonlight_code, true)) {
        return false;
      }
    }

    return send(0xA0, true) && send(0xA2, true);
#else
    return false;
#endif
  }

}  // namespace platf
