#pragma once

#include <cstdint>
#include <string>
#include <vector>

/** Private graph evidence. Never serialize process IDs, credentials or object IDs to clients. */
namespace capture_producer {
  struct object_t {
    std::uint32_t id = 0;
    std::uint64_t serial = 0;
    bool operator==(const object_t &) const = default;
  };

  struct link_t {
    object_t object;
    std::uint32_t source = 0;
    std::uint32_t sink = 0;
    bool operator==(const link_t &) const = default;
  };

  /**
   * A synchronized observation of the SAME PipeWire core that owns the capture stream.
   * verified means the actual links and server credentials were resolved; it is not
   * proof of executable identity, PID-reuse exclusion, display identity or VRR state.
   */
  struct binding_t {
    bool verified = false;
    std::string reason = "producer_unbound";
    std::string server_version;
    object_t capture_node;
    object_t source_node;
    object_t producer_client;
    object_t capture_client;
    std::vector<link_t> links;
    std::int64_t producer_pid = 0;
    std::uint64_t producer_uid = 0;

    bool operator==(const binding_t &) const = default;
  };
}
