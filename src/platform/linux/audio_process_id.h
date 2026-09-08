/** @file src/platform/linux/audio_process_id.h
 * Resolve stream ownership without treating missing metadata as a PID.
 */
#pragma once

#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <pulse/proplist.h>

namespace platf::audio_process_id {
  inline std::optional<pid_t> parse(std::string_view value) {
    if (value.empty()) return std::nullopt;
    pid_t pid = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), pid);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size() || pid <= 1) {
      return std::nullopt;
    }
    return pid;
  }

  struct property_t {
    std::optional<std::string> value;
    bool malformed = false;
  };

  inline property_t property(const pa_proplist *properties, const char *key) {
    if (!properties || !pa_proplist_contains(properties, key)) return {};
    const auto *value = pa_proplist_gets(properties, key);
    // gets() also returns null for present binary or malformed strings. Keep
    // that distinct from absence so a bad explicit identity cannot fall back.
    if (!value) return {std::nullopt, true};
    return {std::string(value), false};
  }

  inline std::optional<pid_t> client(const pa_proplist *properties) {
    const auto api = property(properties, "client.api");
    if (api.malformed) return std::nullopt;
    auto pid = property(properties, PA_PROP_APPLICATION_PROCESS_ID);
    // The native protocol's host PID handles PID namespaces. For Pulse proxy
    // clients, those credentials identify pipewire-pulse itself: use the
    // application PID instead, and never fall back to the proxy daemon.
    if (api.value != "pipewire-pulse") {
      const auto security_pid = property(properties, "pipewire.sec.pid");
      if (security_pid.malformed || security_pid.value) pid = security_pid;
    }
    if (pid.malformed || !pid.value) return std::nullopt;
    return parse(*pid.value);
  }

  using client_lookup_t = std::function<std::optional<pid_t>(std::uint32_t)>;

  inline std::optional<pid_t> sink_input(
    const property_t &stream_pid,
    std::uint32_t client_index,
    const client_lookup_t &lookup
  ) {
    // A stream can identify a different process from its client (for example,
    // a launcher proxy). Preserve that explicit association when present.
    if (stream_pid.malformed) return std::nullopt;
    if (stream_pid.value) return parse(*stream_pid.value);
    if (client_index == std::numeric_limits<std::uint32_t>::max() || !lookup) {
      return std::nullopt;
    }
    return lookup(client_index);
  }
}
