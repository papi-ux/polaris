#pragma once

#include <string_view>

namespace rtsp_stream {

  struct session_input_capabilities_t {
    // Host input remains available unless the admitted runtime cannot deliver it.
    // Frozen before RTSP publication; DESCRIBE must not consult runtime health or
    // the requested topology, which can differ from the effective runtime.
    bool native_pen_touch = true;

    void bind_effective_runtime(std::string_view backend, bool effective_headless) {
      native_pen_touch = backend != "labwc" || !effective_headless;
    }
  };

}  // namespace rtsp_stream
