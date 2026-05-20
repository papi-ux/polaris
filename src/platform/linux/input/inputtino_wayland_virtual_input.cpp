/**
 * @file src/platform/linux/input/inputtino_wayland_virtual_input.cpp
 * @brief Labwc-local Wayland virtual input routing for Linux.
 */

#include "inputtino_wayland_virtual_input.h"

// standard includes
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// lib includes
#include <boost/locale.hpp>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>

// local includes
#include "src/logging.h"
#include "src/platform/linux/cage_display_router.h"

using namespace std::literals;

#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
  #include <inputtino/keyboard.hpp>
  #include <sys/mman.h>
  #include <virtual-keyboard-unstable-v1.h>
  #include <wayland-client.h>
  #include <wlr-virtual-pointer-unstable-v1.h>
  #include <xkbcommon/xkbcommon.h>
#endif

namespace platf {

#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
  namespace {
    std::uint32_t now_ms() {
      using clock_t = std::chrono::steady_clock;
      return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now().time_since_epoch()).count()
      );
    }

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

    int moonlight_key_to_evdev(std::uint16_t modcode) {
      auto key = inputtino::keyboard::key_mappings.find(static_cast<short>(modcode));
      if (key == inputtino::keyboard::key_mappings.end()) {
        return -1;
      }
      return key->second.linux_code;
    }

    bool write_all(int fd, const char *data, std::size_t size) {
      std::size_t written = 0;
      while (written < size) {
        auto rc = ::write(fd, data + written, size - written);
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        written += static_cast<std::size_t>(rc);
      }
      return true;
    }

    xkb_keycode_t evdev_to_xkb_keycode(int evdev_keycode) {
      return static_cast<xkb_keycode_t>(evdev_keycode + 8);
    }

    xkb_rule_names current_xkb_rule_names() {
      return xkb_rule_names {
        .rules = std::getenv("XKB_DEFAULT_RULES"),
        .model = std::getenv("XKB_DEFAULT_MODEL"),
        .layout = std::getenv("XKB_DEFAULT_LAYOUT"),
        .variant = std::getenv("XKB_DEFAULT_VARIANT"),
        .options = std::getenv("XKB_DEFAULT_OPTIONS"),
      };
    }

    wayland_virtual_input::modifier_state_t serialize_modifier_state(xkb_state *state) {
      return wayland_virtual_input::modifier_state_t {
        .depressed = xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
        .latched = xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
        .locked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
        .group = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE),
      };
    }

    xkb_state_component update_modifier_state(xkb_state *state, int evdev_keycode, bool release) {
      return xkb_state_update_key(
        state,
        evdev_to_xkb_keycode(evdev_keycode),
        release ? XKB_KEY_UP : XKB_KEY_DOWN
      );
    }

    struct keymap_resources_t {
      int fd {-1};
      std::uint32_t size {0};
      xkb_context *ctx {nullptr};
      xkb_keymap *keymap {nullptr};
      xkb_state *state {nullptr};

      keymap_resources_t() = default;

      keymap_resources_t(keymap_resources_t &&other) noexcept {
        *this = std::move(other);
      }

      keymap_resources_t &operator=(keymap_resources_t &&other) noexcept {
        if (this == &other) {
          return *this;
        }

        reset();
        fd = std::exchange(other.fd, -1);
        size = std::exchange(other.size, 0);
        ctx = std::exchange(other.ctx, nullptr);
        keymap = std::exchange(other.keymap, nullptr);
        state = std::exchange(other.state, nullptr);
        return *this;
      }

      keymap_resources_t(const keymap_resources_t &) = delete;
      keymap_resources_t &operator=(const keymap_resources_t &) = delete;

      ~keymap_resources_t() {
        reset();
      }

      void reset() {
        if (fd >= 0) {
          close(fd);
          fd = -1;
        }
        if (state) {
          xkb_state_unref(state);
          state = nullptr;
        }
        if (keymap) {
          xkb_keymap_unref(keymap);
          keymap = nullptr;
        }
        if (ctx) {
          xkb_context_unref(ctx);
          ctx = nullptr;
        }
        size = 0;
      }

      int release_fd() {
        return std::exchange(fd, -1);
      }

      xkb_state *release_state() {
        return std::exchange(state, nullptr);
      }
    };

    std::unique_ptr<keymap_resources_t> create_keymap_resources(const xkb_rule_names &names) {
      auto resources = std::make_unique<keymap_resources_t>();

      resources->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (!resources->ctx) {
        BOOST_LOG(error) << "Wayland virtual input: unable to create xkb context"sv;
        return nullptr;
      }

      resources->keymap = xkb_keymap_new_from_names(resources->ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (!resources->keymap) {
        BOOST_LOG(error) << "Wayland virtual input: unable to create xkb keymap"sv;
        return nullptr;
      }

      resources->state = xkb_state_new(resources->keymap);
      if (!resources->state) {
        BOOST_LOG(error) << "Wayland virtual input: unable to create xkb state"sv;
        return nullptr;
      }

      auto *keymap_text = xkb_keymap_get_as_string(resources->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (!keymap_text) {
        BOOST_LOG(error) << "Wayland virtual input: unable to serialize xkb keymap"sv;
        return nullptr;
      }

      const auto keymap_size = std::strlen(keymap_text) + 1;
      resources->fd = memfd_create("polaris-virtual-keyboard", MFD_CLOEXEC);
      if (resources->fd < 0) {
        BOOST_LOG(error) << "Wayland virtual input: unable to create keymap memfd"sv;
        std::free(keymap_text);
        return nullptr;
      }

      if (ftruncate(resources->fd, static_cast<off_t>(keymap_size)) < 0 || !write_all(resources->fd, keymap_text, keymap_size)) {
        BOOST_LOG(error) << "Wayland virtual input: unable to write keymap memfd"sv;
        close(resources->fd);
        resources->fd = -1;
      } else {
        lseek(resources->fd, 0, SEEK_SET);
        resources->size = static_cast<std::uint32_t>(keymap_size);
      }

      std::free(keymap_text);
      return resources->fd >= 0 ? std::move(resources) : nullptr;
    }
  }  // namespace

#ifdef POLARIS_TESTS
  namespace wayland_virtual_input {
    std::optional<modifier_state_t> modifier_state_after_key_events_for_tests(
      const std::vector<std::pair<int, bool>> &evdev_key_events,
      std::string_view layout
    ) {
      std::string layout_value {layout};
      xkb_rule_names names {};
      names.layout = layout_value.empty() ? nullptr : layout_value.c_str();

      auto resources = create_keymap_resources(names);
      if (!resources || !resources->state) {
        return std::nullopt;
      }

      for (const auto &[evdev_keycode, release] : evdev_key_events) {
        update_modifier_state(resources->state, evdev_keycode, release);
      }

      return serialize_modifier_state(resources->state);
    }
  }  // namespace wayland_virtual_input
#endif

  struct wayland_virtual_input_t::impl_t {
    std::mutex mutex;
    wl_display *display {nullptr};
    wl_registry *registry {nullptr};
    wl_seat *seat {nullptr};
    zwlr_virtual_pointer_manager_v1 *pointer_manager {nullptr};
    zwlr_virtual_pointer_v1 *pointer {nullptr};
    zwp_virtual_keyboard_manager_v1 *keyboard_manager {nullptr};
    zwp_virtual_keyboard_v1 *keyboard {nullptr};
    xkb_state *keyboard_state {nullptr};
    std::string socket_name;
    pid_t connected_pid {0};
    bool logged_ready {false};
    bool logged_pointer_unavailable {false};
    bool logged_keyboard_unavailable {false};
    bool logged_fallback {false};

    ~impl_t() {
      disconnect();
    }

    static void registry_global(
      void *data,
      wl_registry *registry,
      std::uint32_t name,
      const char *interface,
      std::uint32_t version
    ) {
      auto *self = static_cast<impl_t *>(data);

      if (!std::strcmp(interface, wl_seat_interface.name)) {
        if (!self->seat) {
          self->seat = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 7u))
          );
        }
      } else if (!std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
        if (!self->pointer_manager) {
          self->pointer_manager = static_cast<zwlr_virtual_pointer_manager_v1 *>(
            wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, std::min(version, 2u))
          );
        }
      } else if (!std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        if (!self->keyboard_manager) {
          self->keyboard_manager = static_cast<zwp_virtual_keyboard_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1)
          );
        }
      }
    }

    static void registry_remove(void *data, wl_registry *registry, std::uint32_t name) {}

    static constexpr wl_registry_listener registry_listener {
      .global = registry_global,
      .global_remove = registry_remove,
    };

    bool cage_runtime_active() const {
      if (!cage_display_router::is_healthy()) {
        return false;
      }

      const auto runtime = cage_display_router::runtime_state();
      return runtime.backend_name == "labwc"sv && runtime.effective_headless;
    }

    std::string_view host_fallback_status() const {
      return cage_runtime_active() ? "host uinput fallback blocked for headless labwc"sv : "falling back to host uinput"sv;
    }

    void destroy_keyboard() {
      if (keyboard) {
        zwp_virtual_keyboard_v1_destroy(keyboard);
        keyboard = nullptr;
      }
      if (keyboard_state) {
        xkb_state_unref(keyboard_state);
        keyboard_state = nullptr;
      }
    }

    void disconnect() {
      destroy_keyboard();
      if (pointer) {
        zwlr_virtual_pointer_v1_destroy(pointer);
        pointer = nullptr;
      }
      if (keyboard_manager) {
        wl_proxy_destroy(reinterpret_cast<wl_proxy *>(keyboard_manager));
        keyboard_manager = nullptr;
      }
      if (pointer_manager) {
        zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
        pointer_manager = nullptr;
      }
      if (seat) {
        wl_seat_destroy(seat);
        seat = nullptr;
      }
      if (registry) {
        wl_registry_destroy(registry);
        registry = nullptr;
      }
      if (display) {
        wl_display_disconnect(display);
        display = nullptr;
      }
      socket_name.clear();
      connected_pid = 0;
      logged_ready = false;
    }

    bool flush() {
      if (!display) {
        return false;
      }

      if (wl_display_flush(display) < 0 || wl_display_get_error(display) != 0) {
        BOOST_LOG(warning) << "Wayland virtual input: labwc connection failed; "sv << host_fallback_status();
        disconnect();
        return false;
      }
      return true;
    }

    bool connect_if_needed() {
      if (!cage_runtime_active()) {
        if (display) {
          disconnect();
        }
        return false;
      }

      const auto socket = cage_display_router::get_wayland_socket();
      if (socket.empty()) {
        return false;
      }

      const auto current_pid = cage_display_router::get_pid();
      if (current_pid <= 0) {
        return false;
      }

      if (display && socket_name == socket && connected_pid == current_pid) {
        return true;
      }

      disconnect();
      display = wl_display_connect(socket.c_str());
      if (!display) {
        if (!logged_fallback) {
          BOOST_LOG(warning) << "Wayland virtual input: unable to connect to labwc socket ["sv << socket
                             << "]; "sv << host_fallback_status();
          logged_fallback = true;
        }
        return false;
      }

      socket_name = socket;
      connected_pid = current_pid;
      registry = wl_display_get_registry(display);
      if (!registry) {
        disconnect();
        return false;
      }

      wl_registry_add_listener(registry, &registry_listener, this);
      wl_display_roundtrip(display);
      wl_display_roundtrip(display);

      if (!pointer_manager && !logged_pointer_unavailable) {
        BOOST_LOG(warning) << "Wayland virtual input: labwc does not expose zwlr_virtual_pointer_manager_v1; "sv
                           << (cage_runtime_active() ? "mouse input will be dropped to protect the host session"sv : "mouse will use host uinput fallback"sv);
        logged_pointer_unavailable = true;
      }
      if ((!keyboard_manager || !seat) && !logged_keyboard_unavailable) {
        BOOST_LOG(warning) << "Wayland virtual input: labwc does not expose virtual keyboard support; "sv
                           << (cage_runtime_active() ? "keyboard input will be dropped to protect the host session"sv : "keyboard will use host uinput fallback"sv);
        logged_keyboard_unavailable = true;
      }

      if (!pointer_manager && (!keyboard_manager || !seat)) {
        disconnect();
        return false;
      }

      if (!logged_ready) {
        BOOST_LOG(info) << "Wayland virtual input: routing supported devices to labwc socket ["sv << socket << ']';
        logged_ready = true;
      }

      return true;
    }

    bool ensure_pointer() {
      if (!connect_if_needed() || !pointer_manager) {
        return false;
      }

      if (!pointer) {
        pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
        if (!pointer) {
          BOOST_LOG(warning) << "Wayland virtual input: unable to create virtual pointer; "sv << host_fallback_status();
          return false;
        }
        wl_display_roundtrip(display);
      }

      return true;
    }

    bool ensure_keyboard() {
      if (!connect_if_needed() || !keyboard_manager || !seat) {
        return false;
      }

      if (keyboard) {
        return true;
      }

      keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
      if (!keyboard) {
        BOOST_LOG(warning) << "Wayland virtual input: unable to create virtual keyboard; "sv << host_fallback_status();
        return false;
      }

      auto keymap = create_keymap_resources(current_xkb_rule_names());
      if (!keymap) {
        destroy_keyboard();
        return false;
      }

      auto keymap_fd = keymap->release_fd();
      zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap_fd, keymap->size);
      close(keymap_fd);
      keyboard_state = keymap->release_state();
      wl_display_roundtrip(display);
      return flush();
    }

    void send_modifier_state() {
      if (!keyboard_state) {
        return;
      }

      const auto state = serialize_modifier_state(keyboard_state);
      zwp_virtual_keyboard_v1_modifiers(
        keyboard,
        state.depressed,
        state.latched,
        state.locked,
        state.group
      );
    }

    bool send_keycode(int evdev_keycode, bool release) {
      if (evdev_keycode < 0 || !ensure_keyboard()) {
        return false;
      }

      zwp_virtual_keyboard_v1_key(
        keyboard,
        now_ms(),
        static_cast<std::uint32_t>(evdev_keycode),
        release ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED
      );

      if (keyboard_state && update_modifier_state(keyboard_state, evdev_keycode, release) != 0) {
        send_modifier_state();
      }

      return flush();
    }
  };
#else
  struct wayland_virtual_input_t::impl_t {};
#endif

  wayland_virtual_input_t::wayland_virtual_input_t():
      impl(std::make_unique<impl_t>()) {
  }

  wayland_virtual_input_t::~wayland_virtual_input_t() = default;

  void wayland_virtual_input_t::reset() {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    impl->disconnect();
#endif
  }

  bool wayland_virtual_input_t::should_block_host_fallback() {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    return impl->cage_runtime_active();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::move(int delta_x, int delta_y) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_pointer()) {
      return false;
    }

    zwlr_virtual_pointer_v1_motion(impl->pointer, now_ms(), wl_fixed_from_int(delta_x), wl_fixed_from_int(delta_y));
    zwlr_virtual_pointer_v1_frame(impl->pointer);
    return impl->flush();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::move_abs(const touch_port_t &touch_port, float x, float y) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_pointer() || touch_port.width <= 0 || touch_port.height <= 0) {
      return false;
    }

    const auto px = std::clamp(static_cast<int>(std::lround(x)), 0, touch_port.width);
    const auto py = std::clamp(static_cast<int>(std::lround(y)), 0, touch_port.height);
    zwlr_virtual_pointer_v1_motion_absolute(
      impl->pointer,
      now_ms(),
      static_cast<std::uint32_t>(px),
      static_cast<std::uint32_t>(py),
      static_cast<std::uint32_t>(touch_port.width),
      static_cast<std::uint32_t>(touch_port.height)
    );
    zwlr_virtual_pointer_v1_frame(impl->pointer);
    return impl->flush();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::button(int button, bool release) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    const auto evdev_button = mouse_button_to_evdev(button);
    if (evdev_button < 0) {
      BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
      return true;
    }
    if (!impl->ensure_pointer()) {
      return false;
    }

    zwlr_virtual_pointer_v1_button(
      impl->pointer,
      now_ms(),
      static_cast<std::uint32_t>(evdev_button),
      release ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED
    );
    zwlr_virtual_pointer_v1_frame(impl->pointer);
    return impl->flush();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::scroll(int high_res_distance) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_pointer()) {
      return false;
    }

    zwlr_virtual_pointer_v1_axis_source(impl->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
    zwlr_virtual_pointer_v1_axis(
      impl->pointer,
      now_ms(),
      WL_POINTER_AXIS_VERTICAL_SCROLL,
      wl_fixed_from_double(static_cast<double>(high_res_distance) / 12.0)
    );
    zwlr_virtual_pointer_v1_axis_discrete(
      impl->pointer,
      now_ms(),
      WL_POINTER_AXIS_VERTICAL_SCROLL,
      wl_fixed_from_double(static_cast<double>(high_res_distance) / 12.0),
      high_res_distance / 120
    );
    zwlr_virtual_pointer_v1_frame(impl->pointer);
    return impl->flush();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::hscroll(int high_res_distance) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_pointer()) {
      return false;
    }

    zwlr_virtual_pointer_v1_axis_source(impl->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
    zwlr_virtual_pointer_v1_axis(
      impl->pointer,
      now_ms(),
      WL_POINTER_AXIS_HORIZONTAL_SCROLL,
      wl_fixed_from_double(static_cast<double>(high_res_distance) / 12.0)
    );
    zwlr_virtual_pointer_v1_axis_discrete(
      impl->pointer,
      now_ms(),
      WL_POINTER_AXIS_HORIZONTAL_SCROLL,
      wl_fixed_from_double(static_cast<double>(high_res_distance) / 12.0),
      high_res_distance / 120
    );
    zwlr_virtual_pointer_v1_frame(impl->pointer);
    return impl->flush();
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::keyboard_update(std::uint16_t modcode, bool release) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    const auto evdev_keycode = moonlight_key_to_evdev(modcode);
    if (evdev_keycode < 0) {
      BOOST_LOG(warning) << "Wayland virtual input: unknown keyboard code: "sv << modcode;
      return true;
    }
    return impl->send_keycode(evdev_keycode, release);
#else
    return false;
#endif
  }

  bool wayland_virtual_input_t::unicode(std::string_view hex_unicode) {
#ifdef POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_keyboard()) {
      return false;
    }

    const auto send = [this](std::uint16_t modcode, bool release) {
      return impl->send_keycode(moonlight_key_to_evdev(modcode), release);
    };

    if (!send(0xA2, false) || !send(0xA0, false) || !send(0x55, false) || !send(0x55, true)) {
      return false;
    }

    for (auto ch : hex_unicode) {
      auto key_str = "KEY_"s + static_cast<char>(ch);
      auto keycode = libevdev_event_code_from_name(EV_KEY, key_str.c_str());
      if (keycode == -1) {
        BOOST_LOG(warning) << "Unicode, unable to find keycode for: "sv << ch;
        continue;
      }

      std::uint16_t moonlight_code = 0;
      switch (ch) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          moonlight_code = static_cast<std::uint16_t>(0x30 + (ch - '0'));
          break;
        default:
          moonlight_code = static_cast<std::uint16_t>(0x41 + (std::toupper(ch) - 'A'));
          break;
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
