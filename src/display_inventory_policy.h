/**
 * @file src/display_inventory_policy.h
 * @brief When the system-stats route may enumerate the host's displays again.
 *
 * The console polls /api/stats/system every three seconds while it is open.
 * Enumerating Wayland outputs is a fresh client connection to the compositor,
 * a DMA-BUF feedback round trip and nine log lines, and on a CachyOS laptop a
 * whole-machine freeze landed on exactly one of those polls, 9.5 seconds into
 * a Mirror Desktop stream, while KWin was reading the screen back for the
 * stream. Whether or not the poll was the cause, a compositor that is
 * screencasting does not need a new client every three seconds to be told
 * which monitors it has. The inventory is served from a cache: refreshed at
 * most every 30 seconds while idle, once when a private-compositor stream
 * starts or ends (its outputs are its own), and never during a stream of the
 * desktop itself, whose outputs the idle inventory already describes.
 */
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace display_inventory {
  constexpr std::chrono::seconds k_idle_refresh_interval {30};

  /**
   * @brief What the cached inventory describes.
   * @param private_compositor_stream A stream is running inside Polaris' own compositor.
   * @param private_socket That compositor's Wayland socket, when it is running.
   * @return "desktop" for the host desktop, "cage:<socket>" for the private compositor.
   */
  inline std::string cache_key(bool private_compositor_stream, std::string_view private_socket) {
    if (private_compositor_stream && !private_socket.empty()) {
      return "cage:" + std::string {private_socket};
    }
    return "desktop";
  }

  /**
   * @brief Whether to enumerate now instead of serving the cache.
   * @param has_entry The cache holds an inventory.
   * @param same_key The cached inventory describes the same compositor as this poll wants.
   * @param streaming A stream is active.
   * @param age How old the cached inventory is.
   */
  inline bool should_enumerate(bool has_entry, bool same_key, bool streaming, std::chrono::steady_clock::duration age) {
    if (!has_entry || !same_key) {
      return true;
    }
    if (streaming) {
      return false;
    }
    return age >= k_idle_refresh_interval;
  }
}  // namespace display_inventory
