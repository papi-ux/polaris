#pragma once

#include <cstdint>
#include <string>

namespace capture_generation {

  // Immutable source-selection authority for one capture generation.
  struct identity_t {
    std::uint64_t generation_id = 0;
    std::string exact_display_name;
    std::string requested_output_name;
    std::string stream_mode;
    std::string capture_backend;
    std::string private_runtime;
    std::string private_wayland_socket;
    std::string private_runtime_instance_id;
    std::string adapter_name;
    bool headless_mode = false;
    bool use_cage_compositor = false;

    [[nodiscard]] bool empty() const {
      return generation_id == 0 &&
             exact_display_name.empty() &&
             requested_output_name.empty() &&
             stream_mode.empty() &&
             capture_backend.empty() &&
             private_runtime.empty() &&
             private_wayland_socket.empty() &&
             private_runtime_instance_id.empty() &&
             adapter_name.empty() &&
             !headless_mode &&
             !use_cage_compositor;
    }

    bool operator==(const identity_t &) const = default;
  };

}  // namespace capture_generation
