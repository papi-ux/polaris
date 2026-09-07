/**
 * @file src/platform/linux/input/inputtino_multiseat_backend.cpp
 * @brief Trusted host lifecycle backend for multiseat virtual input.
 */
#include "inputtino_multiseat_backend.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <cerrno>
  #include <charconv>
  #include <fcntl.h>
  #include <inputtino/input.hpp>
  #include <limits>
  #include <map>
  #include <mutex>
  #include <set>
  #include <stdexcept>
  #include <string_view>
  #include <sys/stat.h>
  #include <sys/sysmacros.h>
  #include <thread>
  #include <tuple>
  #include <type_traits>
  #include <unistd.h>
  #include <utility>
  #include <variant>

namespace multiseat::input {
  namespace {
    enum class input_node_class_e {
      event,
      joystick,
    };

    struct parsed_input_node_t {
      input_node_class_e node_class = input_node_class_e::event;
      std::uint32_t index = 0;
      std::uint32_t character_minor = 0;
      std::string filename;
    };

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool valid_name_token(std::string_view value, std::size_t maximum = 128) {
      return !value.empty() && value.size() <= maximum &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) || character == '-' ||
                        character == '_' || character == '.';
               }
             );
    }

    std::optional<std::uint32_t> canonical_number(std::string_view value) {
      if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return std::nullopt;
      }
      std::uint32_t number = 0;
      const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        number
      );
      if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
      }
      return number;
    }

    std::optional<parsed_input_node_t> parse_input_node(
      const std::filesystem::path &path,
      const std::filesystem::path &device_root = "/dev/input"
    ) {
      const auto root = device_root.lexically_normal().native();
      const auto value = path.native();
      if (root.empty()) {
        return std::nullopt;
      }
      const auto prefix = root.back() == '/' ? root : root + "/";
      if (path != path.lexically_normal() ||
          value.size() <= prefix.size() ||
          std::string_view {value}.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
      }
      const auto filename = std::string_view {value}.substr(prefix.size());
      if (filename.find('/') != std::string_view::npos ||
          filename.find('\0') != std::string_view::npos) {
        return std::nullopt;
      }

      constexpr std::string_view event_prefix = "event";
      constexpr std::string_view joystick_prefix = "js";
      input_node_class_e node_class;
      std::string_view suffix;
      if (filename.starts_with(event_prefix)) {
        node_class = input_node_class_e::event;
        suffix = filename.substr(event_prefix.size());
      } else if (filename.starts_with(joystick_prefix)) {
        node_class = input_node_class_e::joystick;
        suffix = filename.substr(joystick_prefix.size());
      } else {
        return std::nullopt;
      }
      const auto index = canonical_number(suffix);
      if (!index ||
          (node_class == input_node_class_e::event &&
           *index > std::numeric_limits<std::uint32_t>::max() - 64)) {
        return std::nullopt;
      }
      return parsed_input_node_t {
        .node_class = node_class,
        .index = *index,
        .character_minor = node_class == input_node_class_e::event ?
                             64 + *index : *index,
        .filename = std::string {filename},
      };
    }

    node_observation_status_e observation_status(node_io_status_e status) {
      switch (status) {
        case node_io_status_e::ok:
          return node_observation_status_e::observed;
        case node_io_status_e::absent:
          return node_observation_status_e::absent;
        case node_io_status_e::retryable:
          return node_observation_status_e::retryable;
        case node_io_status_e::unsafe:
          return node_observation_status_e::unsafe;
      }
      return node_observation_status_e::unsafe;
    }

    std::optional<std::string> single_kernel_line(
      std::string text,
      bool allow_empty
    ) {
      if (!text.empty() && text.back() == '\n') {
        text.pop_back();
      }
      if ((!allow_empty && text.empty()) ||
          text.find('\0') != std::string::npos ||
          text.find('\n') != std::string::npos ||
          text.find('\r') != std::string::npos) {
        return std::nullopt;
      }
      return text;
    }

    bool sysfs_device_matches(
      std::string text,
      std::uint32_t expected_major,
      std::uint32_t expected_minor
    ) {
      const auto normalized = single_kernel_line(std::move(text), false);
      if (!normalized) {
        return false;
      }
      const auto delimiter = normalized->find(':');
      if (delimiter == std::string::npos ||
          normalized->find(':', delimiter + 1) != std::string::npos) {
        return false;
      }
      const auto parsed_major = canonical_number(
        std::string_view {*normalized}.substr(0, delimiter)
      );
      const auto parsed_minor = canonical_number(
        std::string_view {*normalized}.substr(delimiter + 1)
      );
      return parsed_major == expected_major && parsed_minor == expected_minor;
    }

    std::optional<std::string> seat_from_udev_data(std::string_view text) {
      if (text.size() > maximum_kernel_metadata_bytes ||
          text.find('\0') != std::string_view::npos ||
          text.find('\r') != std::string_view::npos) {
        return std::nullopt;
      }
      constexpr std::string_view prefix = "E:ID_SEAT=";
      std::optional<std::string> seat;
      std::size_t offset = 0;
      while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto line = text.substr(
          offset,
          end == std::string_view::npos ? text.size() - offset : end - offset
        );
        if (line.starts_with(prefix)) {
          const auto candidate = line.substr(prefix.size());
          if (seat || !valid_name_token(candidate)) {
            return std::nullopt;
          }
          seat = std::string {candidate};
        }
        if (end == std::string_view::npos) {
          break;
        }
        offset = end + 1;
      }
      return seat.value_or("seat0");
    }

    bool same_slot(const seat_handle_t &left, const seat_handle_t &right) {
      return left.logical_gpu_id == right.logical_gpu_id && left.slot == right.slot;
    }

    bool same_numeric_identity(
      const kernel_node_snapshot_t &left,
      const kernel_node_snapshot_t &right
    ) {
      return left.filesystem_device == right.filesystem_device &&
             left.inode == right.inode &&
             left.character_major == right.character_major &&
             left.character_minor == right.character_minor;
    }

    bool resources_collide(
      const kernel_node_snapshot_t &left,
      const kernel_node_snapshot_t &right
    ) {
      return left.host_path == right.host_path ||
             std::tie(left.filesystem_device, left.inode) ==
               std::tie(right.filesystem_device, right.inode) ||
             std::tie(left.character_major, left.character_minor) ==
               std::tie(right.character_major, right.character_minor);
    }

    float normalized(std::uint16_t value) {
      return static_cast<float>(value) /
             static_cast<float>(std::numeric_limits<std::uint16_t>::max());
    }

    std::optional<inputtino::Mouse::MOUSE_BUTTON> native_mouse_button(
      mouse_button_e button
    ) {
      switch (button) {
        case mouse_button_e::left:
          return inputtino::Mouse::LEFT;
        case mouse_button_e::middle:
          return inputtino::Mouse::MIDDLE;
        case mouse_button_e::right:
          return inputtino::Mouse::RIGHT;
        case mouse_button_e::side:
          return inputtino::Mouse::SIDE;
        case mouse_button_e::extra:
          return inputtino::Mouse::EXTRA;
      }
      return std::nullopt;
    }

    managed_device_apply_result_e apply_inputtino(
      inputtino::Keyboard &keyboard,
      const input_event_t &event,
      std::uint32_t slot
    ) {
      const auto *key = std::get_if<keyboard_key_event_t>(&event.payload);
      if (!key || event.slot != slot || !supported_keyboard_code(key->key_code)) {
        return managed_device_apply_result_e::rejected;
      }
      if (key->state == button_state_e::pressed) {
        keyboard.press(static_cast<short>(key->key_code));
      } else {
        keyboard.release(static_cast<short>(key->key_code));
      }
      return managed_device_apply_result_e::applied;
    }

    managed_device_apply_result_e apply_inputtino(
      inputtino::Mouse &mouse,
      const input_event_t &event,
      std::uint32_t slot
    ) {
      if (event.slot != slot) {
        return managed_device_apply_result_e::rejected;
      }
      if (const auto *relative = std::get_if<mouse_relative_event_t>(&event.payload)) {
        mouse.move(relative->delta_x, relative->delta_y);
      } else if (const auto *absolute =
                   std::get_if<mouse_absolute_event_t>(&event.payload)) {
        mouse.move_abs(
          static_cast<int>(absolute->x),
          static_cast<int>(absolute->y),
          static_cast<int>(absolute->width),
          static_cast<int>(absolute->height)
        );
      } else if (const auto *button =
                   std::get_if<mouse_button_event_t>(&event.payload)) {
        const auto native = native_mouse_button(button->button);
        if (!native) {
          return managed_device_apply_result_e::rejected;
        }
        if (button->state == button_state_e::pressed) {
          mouse.press(*native);
        } else {
          mouse.release(*native);
        }
      } else if (const auto *scroll =
                   std::get_if<mouse_scroll_event_t>(&event.payload)) {
        if (scroll->vertical != 0) {
          mouse.vertical_scroll(scroll->vertical);
        }
        if (scroll->horizontal != 0) {
          mouse.horizontal_scroll(scroll->horizontal);
        }
      } else {
        return managed_device_apply_result_e::rejected;
      }
      return managed_device_apply_result_e::applied;
    }

    managed_device_apply_result_e apply_inputtino(
      inputtino::TouchScreen &touchscreen,
      const input_event_t &event,
      std::uint32_t slot
    ) {
      const auto *touch = std::get_if<touch_contact_event_t>(&event.payload);
      if (!touch || event.slot != slot) {
        return managed_device_apply_result_e::rejected;
      }
      if (touch->action == touch_action_e::release) {
        touchscreen.release_finger(static_cast<int>(touch->pointer_id));
      } else {
        touchscreen.place_finger(
          static_cast<int>(touch->pointer_id),
          normalized(touch->x),
          normalized(touch->y),
          normalized(touch->pressure),
          touch->orientation_degrees
        );
      }
      return managed_device_apply_result_e::applied;
    }

    managed_device_apply_result_e apply_inputtino(
      inputtino::PenTablet &tablet,
      const input_event_t &event,
      std::uint32_t slot
    ) {
      const auto *pen = std::get_if<pen_tool_event_t>(&event.payload);
      if (!pen || event.slot != slot) {
        return managed_device_apply_result_e::rejected;
      }
      tablet.set_btn(inputtino::PenTablet::PRIMARY, (pen->buttons & 0x01U) != 0);
      tablet.set_btn(inputtino::PenTablet::SECONDARY, (pen->buttons & 0x02U) != 0);
      tablet.set_btn(inputtino::PenTablet::TERTIARY, (pen->buttons & 0x04U) != 0);
      const auto tool = pen->tool == pen_tool_e::eraser ?
                          inputtino::PenTablet::ERASER :
                          inputtino::PenTablet::PEN;
      const auto value = normalized(pen->pressure_or_distance);
      const bool contact = pen->proximity == pen_proximity_e::contact;
      tablet.place_tool(
        tool,
        normalized(pen->x),
        normalized(pen->y),
        contact ? value : -1.0F,
        contact ? -1.0F : value,
        static_cast<float>(pen->tilt_x_degrees),
        static_cast<float>(pen->tilt_y_degrees)
      );
      return managed_device_apply_result_e::applied;
    }

    managed_device_apply_result_e apply_inputtino(
      inputtino::XboxOneJoypad &gamepad,
      const input_event_t &event,
      std::uint32_t slot
    ) {
      const auto *state = std::get_if<gamepad_state_event_t>(&event.payload);
      if (!state || event.slot != slot) {
        return managed_device_apply_result_e::rejected;
      }
      gamepad.set_pressed_buttons(state->buttons);
      gamepad.set_triggers(state->left_trigger, state->right_trigger);
      gamepad.set_stick(
        inputtino::Joypad::LS,
        state->left_stick_x,
        state->left_stick_y
      );
      gamepad.set_stick(
        inputtino::Joypad::RS,
        state->right_stick_x,
        state->right_stick_y
      );
      return managed_device_apply_result_e::applied;
    }

    template <typename Device>
    class inputtino_managed_device_t final : public managed_device_t {
    public:
      inputtino_managed_device_t(Device device, std::uint32_t slot):
          device_(std::move(device)),
          slot_(slot) {
      }

      std::vector<std::filesystem::path> nodes() const override {
        const auto native_nodes = device_.get_nodes();
        std::vector<std::filesystem::path> result;
        result.reserve(native_nodes.size());
        for (const auto &node : native_nodes) {
          result.emplace_back(node);
        }
        return result;
      }

      managed_device_apply_result_e apply(const input_event_t &event) override {
        return apply_inputtino(device_, event, slot_);
      }

    private:
      Device device_;
      std::uint32_t slot_ = 0;
    };

    template<typename Device, typename Creator>
    std::unique_ptr<managed_device_t> create_inputtino_device(
      const device_spec_t &spec,
      managed_device_feedback_fn_t feedback,
      Creator &&creator
    ) {
      auto created = std::forward<Creator>(creator)();
      if (!created) {
        return {};
      }
      if constexpr (std::is_same_v<Device, inputtino::XboxOneJoypad>) {
        (*created).set_on_rumble(
          [slot = spec.slot, feedback = std::move(feedback)](int low, int high) {
            if (!feedback || low < 0 || high < 0 ||
                low > std::numeric_limits<std::uint16_t>::max() ||
                high > std::numeric_limits<std::uint16_t>::max()) {
              return;
            }
            feedback({
              .kind = feedback_kind_e::rumble,
              .gamepad_slot = slot,
              .low_frequency = static_cast<std::uint16_t>(low),
              .high_frequency = static_cast<std::uint16_t>(high),
            });
          }
        );
      }
      return std::make_unique<inputtino_managed_device_t<Device>>(
        std::move(*created),
        spec.slot
      );
    }
  }  // namespace

  std::optional<std::vector<device_spec_t>> build_device_specs(
    const expectation_t &expectation
  ) {
    if (!expectation.handle.valid() ||
        !valid_name_token(expectation.input_seat) ||
        !valid_plan(expectation.plan)) {
      return std::nullopt;
    }

    std::vector<device_spec_t> result;
    result.reserve(
      2 + static_cast<std::size_t>(expectation.plan.touch) +
      static_cast<std::size_t>(expectation.plan.pen) +
      expectation.plan.gamepad_slots
    );
    const auto add_single = [&](managed_device_kind_e managed_kind,
                                device_kind_e node_kind,
                                std::uint32_t slot,
                                std::uint16_t vendor,
                                std::uint16_t product,
                                std::uint16_t version,
                                bool permits_joystick) {
      const auto kernel_name = expected_kernel_name(
        expectation.input_seat,
        node_kind,
        slot
      );
      const auto worker_path = expected_worker_path(node_kind, slot);
      const auto phys = expected_phys(expectation.input_seat, node_kind, slot);
      if (kernel_name.empty() || worker_path.empty() || phys.empty()) {
        return false;
      }
      result.push_back({
        .kind = managed_kind,
        .slot = slot,
        .kernel_name = kernel_name,
        .expected_phys = phys,
        .vendor_id = vendor,
        .product_id = product,
        .version = version,
        .permits_joystick_node = permits_joystick,
        .event_nodes = {{
          .kind = node_kind,
          .slot = slot,
          .kernel_name = kernel_name,
          .worker_path = worker_path,
        }},
      });
      return true;
    };

    if (!add_single(
          managed_device_kind_e::keyboard,
          device_kind_e::keyboard,
          0,
          0xAB00,
          0xAB05,
          0xAB00,
          false
        )) {
      return std::nullopt;
    }

    const auto mouse_name = expected_kernel_name(
      expectation.input_seat,
      device_kind_e::mouse_relative,
      0
    );
    const auto mouse_absolute_name = expected_kernel_name(
      expectation.input_seat,
      device_kind_e::mouse_absolute,
      0
    );
    const auto mouse_phys = expected_phys(
      expectation.input_seat,
      device_kind_e::mouse_relative,
      0
    );
    if (mouse_name.empty() || mouse_absolute_name.empty() || mouse_phys.empty()) {
      return std::nullopt;
    }
    result.push_back({
      .kind = managed_device_kind_e::mouse,
      .slot = 0,
      .kernel_name = mouse_name,
      .expected_phys = mouse_phys,
      .vendor_id = 0xAB00,
      .product_id = 0xAB01,
      .version = 0xAB00,
      .permits_joystick_node = false,
      .event_nodes = {
        {
          .kind = device_kind_e::mouse_relative,
          .slot = 0,
          .kernel_name = mouse_name,
          .worker_path = expected_worker_path(device_kind_e::mouse_relative, 0),
        },
        {
          .kind = device_kind_e::mouse_absolute,
          .slot = 0,
          .kernel_name = mouse_absolute_name,
          .worker_path = expected_worker_path(device_kind_e::mouse_absolute, 0),
        },
      },
    });

    if (expectation.plan.touch &&
        !add_single(
          managed_device_kind_e::touch,
          device_kind_e::touch,
          0,
          0xAB00,
          0xAB03,
          0xAB00,
          false
        )) {
      return std::nullopt;
    }
    if (expectation.plan.pen &&
        !add_single(
          managed_device_kind_e::pen,
          device_kind_e::pen,
          0,
          0xAB00,
          0xAB04,
          0xAB00,
          false
        )) {
      return std::nullopt;
    }
    for (std::uint32_t slot = 0; slot < expectation.plan.gamepad_slots; ++slot) {
      if (!add_single(
            managed_device_kind_e::gamepad,
            device_kind_e::gamepad,
            slot,
            0x045E,
            0x02EA,
            0x0408,
            true
          )) {
        return std::nullopt;
      }
    }
    return result;
  }

  std::unique_ptr<managed_device_t> inputtino_device_factory_t::create(
    const device_spec_t &spec,
    managed_device_feedback_fn_t feedback
  ) {
    inputtino::DeviceDefinition definition {
      .name = spec.kernel_name,
      .vendor_id = spec.vendor_id,
      .product_id = spec.product_id,
      .version = spec.version,
      .device_phys = spec.expected_phys,
      .device_uniq = {},
    };
    switch (spec.kind) {
      case managed_device_kind_e::keyboard:
        return create_inputtino_device<inputtino::Keyboard>(spec, {}, [&]() {
          return inputtino::Keyboard::create(definition);
        });
      case managed_device_kind_e::mouse:
        return create_inputtino_device<inputtino::Mouse>(spec, {}, [&]() {
          return inputtino::Mouse::create(definition);
        });
      case managed_device_kind_e::touch:
        return create_inputtino_device<inputtino::TouchScreen>(spec, {}, [&]() {
          return inputtino::TouchScreen::create(definition);
        });
      case managed_device_kind_e::pen:
        return create_inputtino_device<inputtino::PenTablet>(spec, {}, [&]() {
          return inputtino::PenTablet::create(definition);
        });
      case managed_device_kind_e::gamepad:
        return create_inputtino_device<inputtino::XboxOneJoypad>(
          spec,
          std::move(feedback),
          [&]() {
            return inputtino::XboxOneJoypad::create(definition);
          }
        );
    }
    return {};
  }

  input_node_read_t posix_kernel_node_io_t::inspect_input_node(
    const std::filesystem::path &path
  ) {
    const auto &native = path.native();
    if (!path.is_absolute() || path != path.lexically_normal() ||
        native.find('\0') != std::string::npos) {
      return {.status = node_io_status_e::unsafe};
    }
    int descriptor;
    do {
      descriptor = ::open(native.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
      return {
        .status = errno == ENOENT || errno == ENOTDIR ?
                    node_io_status_e::absent :
                    (errno == ELOOP ? node_io_status_e::unsafe :
                                      node_io_status_e::retryable),
      };
    }
    struct stat metadata {};
    const auto inspected = ::fstat(descriptor, &metadata) == 0;
    (void) ::close(descriptor);
    if (!inspected) {
      return {.status = node_io_status_e::retryable};
    }
    return {
      .status = node_io_status_e::ok,
      .metadata = input_node_metadata_t {
        .filesystem_device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .character_major = S_ISCHR(metadata.st_mode) ?
                             static_cast<std::uint32_t>(::major(metadata.st_rdev)) : 0,
        .character_minor = S_ISCHR(metadata.st_mode) ?
                             static_cast<std::uint32_t>(::minor(metadata.st_rdev)) : 0,
        .character_device = S_ISCHR(metadata.st_mode),
      },
    };
  }

  trusted_text_read_t posix_kernel_node_io_t::read_trusted_text(
    const std::filesystem::path &path,
    std::size_t maximum_bytes
  ) {
    const auto &native = path.native();
    if (!path.is_absolute() || path != path.lexically_normal() ||
        native.find('\0') != std::string::npos || maximum_bytes == 0 ||
        maximum_bytes > maximum_kernel_metadata_bytes) {
      return {.status = node_io_status_e::unsafe};
    }
    int descriptor;
    do {
      descriptor = ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
      return {
        .status = errno == ENOENT || errno == ENOTDIR ?
                    node_io_status_e::absent :
                    (errno == ELOOP ? node_io_status_e::unsafe :
                                      node_io_status_e::retryable),
      };
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      (void) ::close(descriptor);
      return {.status = node_io_status_e::unsafe};
    }

    std::string text;
    std::array<char, 4096> buffer {};
    while (true) {
      ssize_t count;
      do {
        count = ::read(descriptor, buffer.data(), buffer.size());
      } while (count < 0 && errno == EINTR);
      if (count < 0) {
        (void) ::close(descriptor);
        return {.status = node_io_status_e::retryable};
      }
      if (count == 0) {
        break;
      }
      if (static_cast<std::size_t>(count) > maximum_bytes - text.size()) {
        (void) ::close(descriptor);
        return {.status = node_io_status_e::unsafe};
      }
      text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    (void) ::close(descriptor);
    return {
      .status = node_io_status_e::ok,
      .text = std::move(text),
    };
  }

  linux_kernel_node_probe_t::linux_kernel_node_probe_t(
    kernel_node_io_t &io,
    linux_kernel_probe_paths_t paths
  ) :
      io_(io),
      paths_(std::move(paths)) {
  }

  node_observation_t linux_kernel_node_probe_t::observe(
    const std::filesystem::path &path
  ) {
    const auto parsed = parse_input_node(path, paths_.device_root);
    if (!parsed) {
      return {.status = node_observation_status_e::unsafe};
    }
    const auto inspected = io_.inspect_input_node(path);
    if (inspected.status != node_io_status_e::ok) {
      return {
        .status = observation_status(inspected.status),
        .snapshot = std::nullopt,
      };
    }
    if (!inspected.metadata) {
      return {.status = node_observation_status_e::unsafe};
    }
    kernel_node_snapshot_t snapshot {
      .host_path = path,
      .filesystem_device = inspected.metadata->filesystem_device,
      .inode = inspected.metadata->inode,
      .character_major = inspected.metadata->character_major,
      .character_minor = inspected.metadata->character_minor,
    };
    const auto partial = [&snapshot](node_observation_status_e status) {
      return node_observation_t {
        .status = status,
        .snapshot = snapshot,
      };
    };
    if (!inspected.metadata->character_device ||
        snapshot.filesystem_device == 0 || snapshot.inode == 0 ||
        snapshot.character_major != 13 ||
        snapshot.character_minor != parsed->character_minor) {
      return partial(node_observation_status_e::unsafe);
    }

    const auto sysfs_node = paths_.sys_class_input_root / parsed->filename;
    const auto sysfs_device = io_.read_trusted_text(
      sysfs_node / "dev",
      maximum_kernel_metadata_bytes
    );
    if (sysfs_device.status != node_io_status_e::ok) {
      return partial(observation_status(
        sysfs_device.status == node_io_status_e::absent ?
          node_io_status_e::retryable : sysfs_device.status
      ));
    }
    if (!sysfs_device_matches(
          sysfs_device.text,
          snapshot.character_major,
          snapshot.character_minor
        )) {
      return partial(node_observation_status_e::unsafe);
    }

    const auto name_read = io_.read_trusted_text(
      sysfs_node / "device/name",
      maximum_kernel_metadata_bytes
    );
    if (name_read.status != node_io_status_e::ok) {
      return partial(observation_status(
        name_read.status == node_io_status_e::absent ?
          node_io_status_e::retryable : name_read.status
      ));
    }
    const auto kernel_name = single_kernel_line(name_read.text, false);
    if (!kernel_name || kernel_name->size() > maximum_kernel_device_name_bytes) {
      return partial(node_observation_status_e::unsafe);
    }
    snapshot.kernel_name = *kernel_name;

    const auto phys_read = io_.read_trusted_text(
      sysfs_node / "device/phys",
      maximum_kernel_metadata_bytes
    );
    if (phys_read.status == node_io_status_e::ok) {
      const auto phys = single_kernel_line(phys_read.text, true);
      if (!phys) {
        return partial(node_observation_status_e::unsafe);
      }
      snapshot.phys = *phys;
    } else if (phys_read.status != node_io_status_e::absent) {
      return partial(observation_status(phys_read.status));
    }

    const auto udev_read = io_.read_trusted_text(
      paths_.udev_data_root /
        ("c" + std::to_string(snapshot.character_major) + ":" +
         std::to_string(snapshot.character_minor)),
      maximum_kernel_metadata_bytes
    );
    if (udev_read.status != node_io_status_e::ok) {
      return partial(observation_status(
        udev_read.status == node_io_status_e::absent ?
          node_io_status_e::retryable : udev_read.status
      ));
    }
    const auto seat = seat_from_udev_data(udev_read.text);
    if (!seat) {
      return partial(node_observation_status_e::unsafe);
    }
    snapshot.host_seat = *seat;

    // Fence the metadata reads against an event-number reuse between the
    // initial O_PATH open and the final identity returned to the authority.
    const auto reinspected = io_.inspect_input_node(path);
    if (reinspected.status != node_io_status_e::ok) {
      return partial(observation_status(
        reinspected.status == node_io_status_e::absent ?
          node_io_status_e::retryable : reinspected.status
      ));
    }
    if (!reinspected.metadata) {
      return partial(node_observation_status_e::unsafe);
    }
    const input_node_metadata_t initial {
      .filesystem_device = snapshot.filesystem_device,
      .inode = snapshot.inode,
      .character_major = snapshot.character_major,
      .character_minor = snapshot.character_minor,
      .character_device = true,
    };
    if (*reinspected.metadata != initial) {
      return partial(node_observation_status_e::unsafe);
    }
    return {
      .status = node_observation_status_e::observed,
      .snapshot = std::move(snapshot),
    };
  }

  struct inputtino_host_backend_t::impl_t {
    class feedback_gate_t {
    public:
      feedback_gate_t(
        seat_handle_t handle,
        plan_t plan,
        controller_feedback_sink_t sink
      ):
          handle_(std::move(handle)),
          plan_(plan),
          sink_(std::move(sink)) {
      }

      void activate() {
        std::scoped_lock lock {mutex_};
        active_ = true;
      }

      void deactivate() {
        std::scoped_lock lock {mutex_};
        active_ = false;
      }

      void publish(const feedback_event_t &event) {
        std::scoped_lock lock {mutex_};
        if (!active_ || !sink_ || !valid_feedback_event(event) ||
            event.gamepad_slot >= plan_.gamepad_slots || next_sequence_ == 0) {
          return;
        }
        const controller_feedback_t packet {
          .handle = handle_,
          .sequence = next_sequence_,
          .event = event,
        };
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
          next_sequence_ = 0;
        } else {
          ++next_sequence_;
        }
        try {
          // Keep delivery serialized with assignment. The injected sink is a
          // bounded queue edge and must not block or re-enter this backend.
          sink_(packet);
        } catch (...) {
        }
      }

    private:
      std::mutex mutex_;
      seat_handle_t handle_;
      plan_t plan_;
      controller_feedback_sink_t sink_;
      std::uint64_t next_sequence_ = 1;
      bool active_ = false;
    };

    struct managed_state_t {
      device_spec_t spec;
      std::unique_ptr<managed_device_t> device;
      std::vector<kernel_node_snapshot_t> identities;
      std::set<std::uint16_t> pressed_keys;
      std::set<mouse_button_e> pressed_mouse_buttons;
      std::set<std::uint32_t> active_touch_contacts;
    };

    struct active_t {
      expectation_t expectation;
      allocation_t allocation;
      std::vector<managed_state_t> devices;
      std::shared_ptr<feedback_gate_t> feedback_gate;
      std::uint64_t last_sequence = 0;
      bool routing_indeterminate = false;
    };

    struct cleanup_target_t {
      std::filesystem::path path;
      std::optional<kernel_node_snapshot_t> identity;
    };

    struct tombstone_t {
      seat_handle_t handle;
      std::string input_seat;
      std::vector<cleanup_target_t> targets;
      bool unknown_identity = false;
    };

    struct candidate_t {
      allocation_t allocation;
      std::vector<std::vector<kernel_node_snapshot_t>> identities;

      bool operator==(const candidate_t &) const = default;
    };

    enum class discovery_status_e {
      ready,
      rejected,
      timed_out,
      indeterminate,
    };

    struct discovery_result_t {
      discovery_status_e status = discovery_status_e::indeterminate;
      std::optional<candidate_t> candidate;
    };

    impl_t(
      device_factory_t &factory,
      kernel_node_probe_t &probe,
      inputtino_host_backend_options_t options,
      inputtino_backend_waiter_t waiter,
      controller_feedback_sink_t feedback_sink
    ):
        factory(factory),
        probe(probe),
        options(options),
        waiter(std::move(waiter)),
        feedback_sink(std::move(feedback_sink)) {
      if (!this->waiter) {
        this->waiter = [](std::chrono::milliseconds delay) {
          std::this_thread::sleep_for(delay);
        };
      }
    }

    ~impl_t() {
      for (auto &entry : active) {
        entry.feedback_gate->deactivate();
      }
    }

    static bool valid_options(const inputtino_host_backend_options_t &options) {
      constexpr std::size_t maximum_attempts = 1024;
      return options.discovery_attempts > 0 &&
             options.discovery_attempts <= maximum_attempts &&
             options.cleanup_attempts > 0 &&
             options.cleanup_attempts <= maximum_attempts &&
             options.stable_observations > 0 &&
             options.stable_observations <= options.discovery_attempts &&
             options.retry_delay >= std::chrono::milliseconds::zero() &&
             options.retry_delay <= std::chrono::seconds {1};
    }

    static std::pair<managed_device_kind_e, std::uint32_t> target_for(
      const input_event_t &event
    ) {
      return std::visit(
        [&event](const auto &payload) {
          using value_t = std::remove_cvref_t<decltype(payload)>;
          if constexpr (std::is_same_v<value_t, keyboard_key_event_t>) {
            return std::pair {managed_device_kind_e::keyboard, 0U};
          } else if constexpr (
            std::is_same_v<value_t, mouse_relative_event_t> ||
            std::is_same_v<value_t, mouse_absolute_event_t> ||
            std::is_same_v<value_t, mouse_button_event_t> ||
            std::is_same_v<value_t, mouse_scroll_event_t>
          ) {
            return std::pair {managed_device_kind_e::mouse, 0U};
          } else if constexpr (std::is_same_v<value_t, touch_contact_event_t>) {
            return std::pair {managed_device_kind_e::touch, 0U};
          } else if constexpr (std::is_same_v<value_t, pen_tool_event_t>) {
            return std::pair {managed_device_kind_e::pen, 0U};
          } else {
            return std::pair {managed_device_kind_e::gamepad, event.slot};
          }
        },
        event.payload
      );
    }

    static bool state_allows(
      const managed_state_t &managed,
      const input_event_t &event
    ) {
      if (const auto *key = std::get_if<keyboard_key_event_t>(&event.payload)) {
        const bool pressed = managed.pressed_keys.contains(key->key_code);
        return key->state == button_state_e::pressed ? !pressed : pressed;
      }
      if (const auto *button = std::get_if<mouse_button_event_t>(&event.payload)) {
        const bool pressed = managed.pressed_mouse_buttons.contains(button->button);
        return button->state == button_state_e::pressed ? !pressed : pressed;
      }
      if (const auto *touch = std::get_if<touch_contact_event_t>(&event.payload)) {
        const bool active = managed.active_touch_contacts.contains(touch->pointer_id);
        switch (touch->action) {
          case touch_action_e::down:
            return !active &&
                   managed.active_touch_contacts.size() < maximum_touch_contacts;
          case touch_action_e::move:
          case touch_action_e::release:
            return active;
        }
        return false;
      }
      return true;
    }

    static void commit_state(
      managed_state_t &managed,
      const input_event_t &event
    ) {
      if (const auto *key = std::get_if<keyboard_key_event_t>(&event.payload)) {
        if (key->state == button_state_e::pressed) {
          managed.pressed_keys.insert(key->key_code);
        } else {
          managed.pressed_keys.erase(key->key_code);
        }
      } else if (const auto *button =
                   std::get_if<mouse_button_event_t>(&event.payload)) {
        if (button->state == button_state_e::pressed) {
          managed.pressed_mouse_buttons.insert(button->button);
        } else {
          managed.pressed_mouse_buttons.erase(button->button);
        }
      } else if (const auto *touch =
                   std::get_if<touch_contact_event_t>(&event.payload)) {
        if (touch->action == touch_action_e::down) {
          managed.active_touch_contacts.insert(touch->pointer_id);
        } else if (touch->action == touch_action_e::release) {
          managed.active_touch_contacts.erase(touch->pointer_id);
        }
      }
    }

    bool identity_matches_spec(
      const kernel_node_snapshot_t &snapshot,
      const device_spec_t &spec,
      std::string_view expected_name
    ) const {
      return snapshot.kernel_name == expected_name &&
             (snapshot.phys.empty() || snapshot.phys == spec.expected_phys);
    }

    discovery_result_t discover(
      const expectation_t &expectation,
      const std::vector<managed_state_t> &devices
    ) {
      std::optional<candidate_t> previous;
      std::size_t stable = 0;
      for (std::size_t attempt = 0; attempt < options.discovery_attempts; ++attempt) {
        candidate_t candidate {
          .allocation = allocation_t {
            .handle = expectation.handle,
            .input_seat = expectation.input_seat,
            .plan = expectation.plan,
            .nodes = {},
          },
          .identities = {},
        };
        bool pending = false;
        std::set<std::filesystem::path> all_paths;
        std::set<std::pair<std::uint64_t, std::uint64_t>> all_inodes;
        std::set<std::pair<std::uint32_t, std::uint32_t>> all_devices;

        for (const auto &managed : devices) {
          std::vector<std::filesystem::path> paths;
          try {
            paths = managed.device->nodes();
          } catch (...) {
            return {.status = discovery_status_e::indeterminate};
          }
          if (paths.size() > managed.spec.event_nodes.size() +
                               static_cast<std::size_t>(managed.spec.permits_joystick_node)) {
            return {.status = discovery_status_e::rejected};
          }
          std::sort(paths.begin(), paths.end());
          if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) {
            return {.status = discovery_status_e::rejected};
          }

          std::map<std::string, kernel_node_snapshot_t> events_by_name;
          std::vector<kernel_node_snapshot_t> identities;
          std::size_t joystick_nodes = 0;
          for (const auto &path : paths) {
            const auto parsed = parse_input_node(path);
            if (!parsed || !all_paths.emplace(path).second) {
              return {.status = discovery_status_e::rejected};
            }
            node_observation_t observation;
            try {
              observation = probe.observe(path);
            } catch (...) {
              return {.status = discovery_status_e::indeterminate};
            }
            if (observation.status == node_observation_status_e::unsafe) {
              return {.status = discovery_status_e::rejected};
            }
            if (observation.status != node_observation_status_e::observed ||
                !observation.snapshot) {
              pending = true;
              continue;
            }
            auto snapshot = *observation.snapshot;
            if (snapshot.host_path != path ||
                !all_inodes.emplace(
                  snapshot.filesystem_device,
                  snapshot.inode
                ).second ||
                !all_devices.emplace(
                  snapshot.character_major,
                  snapshot.character_minor
                ).second) {
              return {.status = discovery_status_e::rejected};
            }

            if (parsed->node_class == input_node_class_e::joystick) {
              ++joystick_nodes;
              if (!managed.spec.permits_joystick_node || joystick_nodes > 1 ||
                  !identity_matches_spec(
                    snapshot,
                    managed.spec,
                    managed.spec.kernel_name
                  )) {
                return {.status = discovery_status_e::rejected};
              }
              if (snapshot.host_seat != isolated_host_seat) {
                pending = true;
                continue;
              }
            } else {
              const auto expected = std::find_if(
                managed.spec.event_nodes.begin(),
                managed.spec.event_nodes.end(),
                [&snapshot](const auto &node) {
                  return node.kernel_name == snapshot.kernel_name;
                }
              );
              if (expected == managed.spec.event_nodes.end() ||
                  !identity_matches_spec(
                    snapshot,
                    managed.spec,
                    expected->kernel_name
                  )) {
                return {.status = discovery_status_e::rejected};
              }
              if (snapshot.host_seat != isolated_host_seat) {
                pending = true;
                continue;
              }
              if (!events_by_name.emplace(expected->kernel_name, snapshot).second) {
                return {.status = discovery_status_e::rejected};
              }
            }
            identities.push_back(std::move(snapshot));
          }

          for (const auto &expected : managed.spec.event_nodes) {
            const auto observed = events_by_name.find(expected.kernel_name);
            if (observed == events_by_name.end()) {
              pending = true;
              continue;
            }
            const auto &snapshot = observed->second;
            candidate.allocation.nodes.push_back({
              .kind = expected.kind,
              .slot = expected.slot,
              .host_path = snapshot.host_path,
              .worker_path = expected.worker_path,
              .filesystem_device = snapshot.filesystem_device,
              .inode = snapshot.inode,
              .character_major = snapshot.character_major,
              .character_minor = snapshot.character_minor,
              .kernel_name = snapshot.kernel_name,
              .phys = snapshot.phys,
              .host_seat = snapshot.host_seat,
            });
          }
          std::sort(
            identities.begin(),
            identities.end(),
            [](const auto &left, const auto &right) {
              return left.host_path < right.host_path;
            }
          );
          candidate.identities.push_back(std::move(identities));
        }

        if (!pending && valid_allocation(candidate.allocation, expectation)) {
          if (previous && candidate == *previous) {
            ++stable;
          } else {
            previous = candidate;
            stable = 1;
          }
          if (stable >= options.stable_observations) {
            return {
              .status = discovery_status_e::ready,
              .candidate = std::move(candidate),
            };
          }
        } else {
          previous.reset();
          stable = 0;
        }
        if (attempt + 1 < options.discovery_attempts) {
          try {
            waiter(options.retry_delay);
          } catch (...) {
            return {.status = discovery_status_e::indeterminate};
          }
        }
      }
      return {.status = discovery_status_e::timed_out};
    }

    std::vector<cleanup_target_t> cleanup_targets(
      const std::vector<managed_state_t> &devices,
      bool &unknown_identity
    ) {
      std::map<std::filesystem::path, cleanup_target_t> targets;
      for (const auto &managed : devices) {
        for (const auto &identity : managed.identities) {
          targets.emplace(
            identity.host_path,
            cleanup_target_t {
              .path = identity.host_path,
              .identity = identity,
            }
          );
        }
        std::vector<std::filesystem::path> paths;
        try {
          paths = managed.device->nodes();
        } catch (...) {
          unknown_identity = true;
          continue;
        }
        std::size_t event_nodes = 0;
        for (const auto &path : paths) {
          const auto parsed = parse_input_node(path);
          if (!parsed) {
            unknown_identity = true;
            continue;
          }
          if (parsed->node_class == input_node_class_e::event) {
            ++event_nodes;
          }
          if (targets.contains(path)) {
            continue;
          }
          node_observation_t observation;
          try {
            observation = probe.observe(path);
          } catch (...) {
            unknown_identity = true;
            continue;
          }
          if (observation.snapshot) {
            targets.emplace(path, cleanup_target_t {
              .path = path,
              .identity = std::move(observation.snapshot),
            });
          } else if (observation.status != node_observation_status_e::absent) {
            targets.emplace(path, cleanup_target_t {.path = path});
          }
        }
        if (event_nodes < managed.spec.event_nodes.size()) {
          unknown_identity = true;
        }
      }
      std::vector<cleanup_target_t> result;
      result.reserve(targets.size());
      for (auto &[path, target] : targets) {
        (void) path;
        result.push_back(std::move(target));
      }
      return result;
    }

    bool confirm_cleanup(const tombstone_t &tombstone) {
      if (tombstone.unknown_identity) {
        return false;
      }
      for (std::size_t attempt = 0; attempt < options.cleanup_attempts; ++attempt) {
        if (cleanup_observed_gone(tombstone)) {
          return true;
        }
        if (attempt + 1 < options.cleanup_attempts) {
          try {
            waiter(options.retry_delay);
          } catch (...) {
            return false;
          }
        }
      }
      return false;
    }

    bool cleanup_observed_gone(const tombstone_t &tombstone) {
      if (tombstone.unknown_identity) {
        return false;
      }
      for (const auto &target : tombstone.targets) {
        node_observation_t observation;
        try {
          observation = probe.observe(target.path);
        } catch (...) {
          return false;
        }
        if (observation.status == node_observation_status_e::absent) {
          continue;
        }
        if (target.identity && observation.snapshot &&
            !same_numeric_identity(*target.identity, *observation.snapshot)) {
          continue;
        }
        return false;
      }
      return true;
    }

    bool release_devices(
      const seat_handle_t &handle,
      std::string input_seat,
      std::vector<managed_state_t> &devices
    ) {
      bool unknown_identity = false;
      auto targets = cleanup_targets(devices, unknown_identity);
      devices.clear();
      tombstone_t tombstone {
        .handle = handle,
        .input_seat = std::move(input_seat),
        .targets = std::move(targets),
        .unknown_identity = unknown_identity,
      };
      if (confirm_cleanup(tombstone)) {
        return true;
      }
      tombstones.push_back(std::move(tombstone));
      return false;
    }

    bool resolve_tombstones() {
      for (std::size_t attempt = 0; attempt < options.cleanup_attempts; ++attempt) {
        auto current = tombstones.begin();
        while (current != tombstones.end()) {
          if (cleanup_observed_gone(*current)) {
            current = tombstones.erase(current);
          } else {
            ++current;
          }
        }
        if (tombstones.empty()) {
          return true;
        }
        if (attempt + 1 < options.cleanup_attempts) {
          try {
            waiter(options.retry_delay);
          } catch (...) {
            return false;
          }
        }
      }
      return false;
    }

    bool collides(const expectation_t &expectation, const candidate_t &candidate) const {
      for (const auto &active : active) {
        if (active.expectation.handle == expectation.handle ||
            same_slot(active.expectation.handle, expectation.handle) ||
            active.expectation.input_seat == expectation.input_seat) {
          return true;
        }
        for (const auto &candidate_group : candidate.identities) {
          for (const auto &candidate_node : candidate_group) {
            for (const auto &active_device : active.devices) {
              for (const auto &active_node : active_device.identities) {
                if (resources_collide(candidate_node, active_node) ||
                    candidate_node.kernel_name == active_node.kernel_name) {
                  return true;
                }
              }
            }
          }
        }
      }
      return false;
    }

    bool validate_active(active_t &entry) {
      const auto discovered = discover(entry.expectation, entry.devices);
      if (discovered.status != discovery_status_e::ready ||
          !discovered.candidate ||
          discovered.candidate->allocation != entry.allocation ||
          discovered.candidate->identities.size() != entry.devices.size()) {
        return false;
      }
      for (std::size_t index = 0; index < entry.devices.size(); ++index) {
        if (discovered.candidate->identities[index] != entry.devices[index].identities) {
          return false;
        }
      }
      return true;
    }

    device_factory_t &factory;
    kernel_node_probe_t &probe;
    inputtino_host_backend_options_t options;
    inputtino_backend_waiter_t waiter;
    controller_feedback_sink_t feedback_sink;
    std::mutex mutex;
    std::vector<active_t> active;
    std::vector<tombstone_t> tombstones;
  };

  inputtino_host_backend_t::inputtino_host_backend_t(
    device_factory_t &factory,
    kernel_node_probe_t &probe,
    inputtino_host_backend_options_t options,
    inputtino_backend_waiter_t waiter,
    controller_feedback_sink_t feedback_sink
  ) {
    if (!impl_t::valid_options(options)) {
      throw std::invalid_argument {"invalid multiseat input backend options"};
    }
    impl_ = std::make_unique<impl_t>(
      factory,
      probe,
      options,
      std::move(waiter),
      std::move(feedback_sink)
    );
  }

  inputtino_host_backend_t::~inputtino_host_backend_t() = default;

  backend_create_result_t inputtino_host_backend_t::create(
    const expectation_t &expectation
  ) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->resolve_tombstones()) {
      return {.result = backend_result_e::indeterminate};
    }
    const auto specs = build_device_specs(expectation);
    if (!specs || impl_->active.size() >= maximum_input_allocations) {
      return {.result = backend_result_e::rejected};
    }
    const auto exact = std::find_if(
      impl_->active.begin(),
      impl_->active.end(),
      [&expectation](const auto &entry) {
        return entry.expectation.handle == expectation.handle;
      }
    );
    if (exact != impl_->active.end()) {
      if (exact->expectation != expectation) {
        return {.result = backend_result_e::rejected};
      }
      if (exact->routing_indeterminate) {
        return {.result = backend_result_e::indeterminate};
      }
      if (!impl_->validate_active(*exact)) {
        return {.result = backend_result_e::indeterminate};
      }
      return {
        .result = backend_result_e::already_applied,
        .allocation = exact->allocation,
      };
    }
    if (std::any_of(
          impl_->active.begin(),
          impl_->active.end(),
          [&expectation](const auto &entry) {
            return same_slot(entry.expectation.handle, expectation.handle) ||
                   entry.expectation.input_seat == expectation.input_seat;
          }
        )) {
      return {.result = backend_result_e::rejected};
    }

    std::vector<impl_t::managed_state_t> devices;
    devices.reserve(specs->size());
    auto feedback_gate = std::make_shared<impl_t::feedback_gate_t>(
      expectation.handle,
      expectation.plan,
      impl_->feedback_sink
    );
    try {
      for (const auto &spec : *specs) {
        const auto expected_kind = spec.kind;
        const auto expected_slot = spec.slot;
        auto device = impl_->factory.create(
          spec,
          [gate = std::weak_ptr {feedback_gate}, expected_kind, expected_slot](
            const feedback_event_t &event
          ) {
            if (expected_kind != managed_device_kind_e::gamepad ||
                event.gamepad_slot != expected_slot) {
              return;
            }
            if (const auto active_gate = gate.lock()) {
              active_gate->publish(event);
            }
          }
        );
        if (!device) {
          const auto cleaned = impl_->release_devices(
            expectation.handle,
            expectation.input_seat,
            devices
          );
          return {
            .result = cleaned ? backend_result_e::rejected :
                                backend_result_e::indeterminate,
          };
        }
        devices.push_back({
          .spec = spec,
          .device = std::move(device),
          .identities = {},
        });
      }
    } catch (...) {
      const auto cleaned = impl_->release_devices(
        expectation.handle,
        expectation.input_seat,
        devices
      );
      return {
        .result = cleaned ? backend_result_e::rejected :
                            backend_result_e::indeterminate,
      };
    }

    const auto discovered = impl_->discover(expectation, devices);
    if (discovered.status != impl_t::discovery_status_e::ready ||
        !discovered.candidate ||
        impl_->collides(expectation, *discovered.candidate)) {
      const auto cleaned = impl_->release_devices(
        expectation.handle,
        expectation.input_seat,
        devices
      );
      return {
        .result = cleaned &&
                    discovered.status != impl_t::discovery_status_e::indeterminate ?
                    backend_result_e::rejected : backend_result_e::indeterminate,
      };
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
      devices[index].identities = discovered.candidate->identities[index];
    }
    auto allocation = discovered.candidate->allocation;
    impl_->active.push_back({
      .expectation = expectation,
      .allocation = allocation,
      .devices = std::move(devices),
      .feedback_gate = feedback_gate,
    });
    feedback_gate->activate();
    return {
      .result = backend_result_e::applied,
      .allocation = std::move(allocation),
    };
  }

  backend_result_e inputtino_host_backend_t::destroy(
    const seat_handle_t &handle,
    std::string_view input_seat
  ) {
    std::scoped_lock lock {impl_->mutex};
    if (!handle.valid() || !valid_name_token(input_seat)) {
      return backend_result_e::rejected;
    }
    const auto existing = std::find_if(
      impl_->active.begin(),
      impl_->active.end(),
      [&handle](const auto &entry) {
        return entry.expectation.handle == handle;
      }
    );
    if (existing == impl_->active.end()) {
      const auto tombstone = std::find_if(
        impl_->tombstones.begin(),
        impl_->tombstones.end(),
        [&handle](const auto &entry) {
          return entry.handle == handle;
        }
      );
      if (tombstone != impl_->tombstones.end()) {
        if (tombstone->input_seat != input_seat) {
          return backend_result_e::rejected;
        }
        return impl_->resolve_tombstones() ? backend_result_e::already_applied :
                                            backend_result_e::indeterminate;
      }
      return backend_result_e::not_found;
    }
    if (existing->expectation.input_seat != input_seat) {
      return backend_result_e::rejected;
    }
    existing->feedback_gate->deactivate();
    auto devices = std::move(existing->devices);
    impl_->active.erase(existing);
    return impl_->release_devices(handle, std::string {input_seat}, devices) ?
             backend_result_e::applied : backend_result_e::indeterminate;
  }

  backend_result_e inputtino_host_backend_t::route(
    const seat_handle_t &handle,
    std::string_view input_seat,
    std::uint64_t sequence,
    const input_event_t &event
  ) {
    std::scoped_lock lock {impl_->mutex};
    if (!handle.valid() || !valid_name_token(input_seat) || sequence == 0 ||
        !valid_input_event(event)) {
      return backend_result_e::rejected;
    }
    const auto existing = std::find_if(
      impl_->active.begin(),
      impl_->active.end(),
      [&handle](const auto &entry) {
        return entry.expectation.handle == handle;
      }
    );
    if (existing == impl_->active.end()) {
      return backend_result_e::not_found;
    }
    if (existing->routing_indeterminate) {
      return backend_result_e::indeterminate;
    }
    if (existing->expectation.input_seat != input_seat ||
        existing->last_sequence == std::numeric_limits<std::uint64_t>::max() ||
        sequence != existing->last_sequence + 1) {
      return backend_result_e::rejected;
    }

    const auto [target_kind, target_slot] = impl_t::target_for(event);
    const auto managed = std::find_if(
      existing->devices.begin(),
      existing->devices.end(),
      [target_kind, target_slot](const auto &candidate) {
        return candidate.spec.kind == target_kind &&
               candidate.spec.slot == target_slot;
      }
    );
    if (managed == existing->devices.end() ||
        !impl_t::state_allows(*managed, event)) {
      return backend_result_e::rejected;
    }

    managed_device_apply_result_e applied;
    try {
      applied = managed->device->apply(event);
    } catch (...) {
      applied = managed_device_apply_result_e::indeterminate;
    }
    switch (applied) {
      case managed_device_apply_result_e::applied:
        impl_t::commit_state(*managed, event);
        existing->last_sequence = sequence;
        return backend_result_e::applied;
      case managed_device_apply_result_e::rejected:
        return backend_result_e::rejected;
      case managed_device_apply_result_e::indeterminate:
        existing->routing_indeterminate = true;
        return backend_result_e::indeterminate;
    }
    return backend_result_e::indeterminate;
  }

  std::vector<allocation_t> inputtino_host_backend_t::inventory() {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->resolve_tombstones()) {
      throw std::runtime_error {"multiseat input cleanup remains indeterminate"};
    }
    std::vector<allocation_t> result;
    result.reserve(impl_->active.size());
    for (auto &entry : impl_->active) {
      if (entry.routing_indeterminate || !impl_->validate_active(entry)) {
        throw std::runtime_error {"multiseat input identity changed"};
      }
      result.push_back(entry.allocation);
    }
    return result;
  }

}  // namespace multiseat::input

#endif
