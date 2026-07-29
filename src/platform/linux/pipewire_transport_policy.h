/**
 * @file src/platform/linux/pipewire_transport_policy.h
 * @brief PipeWire buffer allocation policy independent of SPA declarations.
 */
#pragma once

#include <vector>

namespace pipewire_transport {
  enum class buffer_transport_e {
    dmabuf,
    memfd,
    memptr,
  };

  inline std::vector<buffer_transport_e> offered_buffer_transports(bool dmabuf_negotiated) {
    if (dmabuf_negotiated) {
      return {buffer_transport_e::dmabuf};
    }
    return {buffer_transport_e::memfd, buffer_transport_e::memptr};
  }
}  // namespace pipewire_transport
