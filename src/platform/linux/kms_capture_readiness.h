/**
 * @file src/platform/linux/kms_capture_readiness.h
 * @brief Whether DRM/KMS capture matters for the capture this host is set to, and where it does,
 *        whether it is ready or which step is still missing.
 *
 * DRM/KMS capture takes three things that live in three places: the polaris-kms package's helper
 * on disk, a drop-in from host setup that points the polaris user service at it, and CAP_SYS_ADMIN
 * in the running process, which the service only gets when its session holds the polaris-kms group.
 * The console showed none of them, so a host with all three looked exactly like a host with none.
 *
 * The capability alone is not the answer, though. Polaris gives up every capability it started
 * with when the portal or KWin will capture, because both refuse a program that holds one, so a
 * host set to portal or kwin runs the helper without CAP_SYS_ADMIN on purpose. Reading that as a
 * helper without its capability sends someone to repair a host that works. So the route comes
 * first: KMS readiness only counts where capture would use KMS, and everywhere else KMS is simply
 * not in use.
 *
 * This says what the host can do. What a stream actually captured with is the session's to report.
 */
#pragma once

#include "kms_enable.h"
#include "stream_display_policy.h"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace platf::kms_readiness {
  namespace detail {
    inline std::atomic<bool> &capability_set_aside_flag() {
      static std::atomic<bool> set_aside {false};
      return set_aside;
    }
  }  // namespace detail

  /**
   * @brief Record whether this process starts without capabilities for the portal or KWin.
   *
   * main() asks portal_capability::requires_unprivileged_process() once, before it drops anything,
   * and the answer holds for the life of the process: capture and the stream mode are read at load,
   * and a changed setting waits for a restart.
   */
  inline void note_capability_set_aside(bool set_aside) {
    detail::capability_set_aside_flag().store(set_aside, std::memory_order_relaxed);
  }

  /// Whether this process set its capabilities aside at startup for portal or KWin capture.
  inline bool capability_set_aside() {
    return detail::capability_set_aside_flag().load(std::memory_order_relaxed);
  }

  /// What the running process can see about DRM/KMS capture on this host.
  struct facts_t {
    std::string capture;  ///< polaris.conf's capture as loaded, read the way dispatch reads it: kms, portal (kwin too), wlr, x11, nvfbc, or empty for Autodetect
    std::string route;  ///< what a launch into the host's stream mode asks capture for, read the same way; empty is the automatic search
    bool kms_substituted = false;  ///< capture asked for KMS, KMS found nothing, and the automatic search took its place
    bool cap_sys_admin = false;  ///< CAP_SYS_ADMIN is in this process's permitted set
    bool capability_set_aside = false;  ///< this process starts without capabilities, on purpose, for portal or KWin capture
    bool helper_installed = false;  ///< the polaris-kms package's helper is on disk
    bool helper_has_capability = false;  ///< the helper file carries the capability set the package applies
    bool running_helper = false;  ///< this process was started from the helper file that is on disk now
    bool in_service = false;  ///< this process belongs to the polaris user service, the one the drop-in points
    bool service_points_at_helper = false;  ///< the user service's drop-ins run the helper
    bool parked = false;  ///< host setup parked that drop-in until the account logs in again
    bool group_member = false;  ///< the account database puts the account in the polaris-kms group
    /// Whether the account's service manager holds that group. Read only while a drop-in waits on it.
    kms_enable::session_group_e session_group = kms_enable::session_group_e::no_manager;
  };

  /// Where KMS stands in the capture the host's stream mode asks for.
  enum class route_e {
    kms,  ///< capture asks for KMS, so KMS has to be ready
    automatic,  ///< capture searches, and the search can reach KMS
    other,  ///< capture goes another way, so KMS is not used and needs nothing
  };

  enum class state_e {
    ready,  ///< capture asks for KMS, and this process holds the capability
    kms_found_nothing,  ///< capture asks for KMS and holds the capability, and KMS found no screen it could read
    automatic,  ///< capture searches, and this process holds the capability, so the search can use KMS
    not_in_use,  ///< capture goes another way, so KMS is not used
    mode_sets_kms_aside,  ///< capture is set to KMS, and the host's stream mode captures another way
    not_installed,  ///< KMS is wanted, and the polaris-kms package is not installed
    helper_broken,  ///< the helper is installed without the capability it exists to carry
    no_capability,  ///< the helper runs, and something in how it was started withheld the capability
    not_enabled,  ///< the package is installed, and nothing points the service at its helper
    not_in_group,  ///< a drop-in points at the helper, and the account may not execute it
    waiting_for_login,  ///< a drop-in points at the helper, and the account's session started without the group
    finish_setup,  ///< the session holds the group now, and the drop-in is still parked
    outside_service,  ///< the service runs the helper, and this Polaris was started another way
    restart_needed,  ///< the service is pointed at the helper, and this process started before that
  };

  inline route_e route_of(const facts_t &facts) {
    if (facts.route == "kms") {
      return route_e::kms;
    }
    if (!facts.route.empty()) {
      return route_e::other;
    }
    // A capture set to KMS that found nothing searches in its place, and what it asked for is still KMS.
    if (facts.kms_substituted) {
      return route_e::kms;
    }
    // The search passes over KMS in a process that started without capabilities.
    return facts.capability_set_aside ? route_e::other : route_e::automatic;
  }

  /**
   * @brief The step DRM/KMS capture still needs, for a process without the capability.
   *
   * The steps are checked in the order host setup takes them, so the state names the earliest one
   * still missing. A service pointed at a helper its session cannot execute does not start at all,
   * so the login comes before any restart.
   */
  inline state_e next_step(const facts_t &facts) {
    if (!facts.helper_installed) {
      return state_e::not_installed;
    }
    if (!facts.helper_has_capability) {
      return state_e::helper_broken;
    }
    if (facts.running_helper) {
      // The route asks for KMS, and a process that set its capabilities aside was asked for something else at startup.
      return facts.capability_set_aside ? state_e::restart_needed : state_e::no_capability;
    }
    if (!facts.parked && !facts.service_points_at_helper) {
      return state_e::not_enabled;
    }
    if (!facts.group_member) {
      return state_e::not_in_group;
    }
    if (facts.parked) {
      return facts.session_group == kms_enable::session_group_e::live ? state_e::finish_setup : state_e::waiting_for_login;
    }
    if (facts.session_group == kms_enable::session_group_e::not_live) {
      return state_e::waiting_for_login;
    }
    if (!facts.in_service) {
      return state_e::outside_service;
    }
    return state_e::restart_needed;
  }

  /**
   * @brief Whether the decision reads the account's service manager, which takes a walk over /proc.
   *
   * Only a host partway through setup gets there, so every other host skips the walk.
   */
  inline bool needs_session_group(const facts_t &facts) {
    return !facts.cap_sys_admin && facts.helper_installed && facts.helper_has_capability && !facts.running_helper &&
           (facts.parked || facts.service_points_at_helper) && facts.group_member;
  }

  /**
   * @brief The one state that says where DRM/KMS capture stands.
   *
   * The route decides first. Where capture goes another way, KMS is not in use and nothing about it
   * is a fault, whatever the capability says. Where capture asks for KMS or searches for it, a
   * process that holds the capability is ready, and one without it is somewhere in setup. A host
   * that searches and never installed the package is a host that captures another way.
   */
  inline state_e decide(const facts_t &facts) {
    const auto route = route_of(facts);
    if (route == route_e::other) {
      return facts.capture == "kms" ? state_e::mode_sets_kms_aside : state_e::not_in_use;
    }
    if (facts.cap_sys_admin) {
      if (route == route_e::automatic) {
        return state_e::automatic;
      }
      return facts.kms_substituted ? state_e::kms_found_nothing : state_e::ready;
    }
    if (route == route_e::automatic && !facts.helper_installed) {
      return state_e::not_in_use;
    }
    return next_step(facts);
  }

  /// The id the console reads the route by.
  inline std::string_view id(route_e route) {
    switch (route) {
      case route_e::kms:
        return "kms";
      case route_e::automatic:
        return "automatic";
      case route_e::other:
        return "other";
    }
    return "other";
  }

  /// The id the console reads the state by.
  inline std::string_view id(state_e state) {
    switch (state) {
      case state_e::ready:
        return "ready";
      case state_e::kms_found_nothing:
        return "kms_found_nothing";
      case state_e::automatic:
        return "automatic";
      case state_e::not_in_use:
        return "not_in_use";
      case state_e::mode_sets_kms_aside:
        return "mode_sets_kms_aside";
      case state_e::not_installed:
        return "not_installed";
      case state_e::helper_broken:
        return "helper_broken";
      case state_e::no_capability:
        return "no_capability";
      case state_e::not_enabled:
        return "not_enabled";
      case state_e::not_in_group:
        return "not_in_group";
      case state_e::waiting_for_login:
        return "waiting_for_login";
      case state_e::finish_setup:
        return "finish_setup";
      case state_e::outside_service:
        return "outside_service";
      case state_e::restart_needed:
        return "restart_needed";
    }
    return "not_in_use";
  }

  /**
   * @brief Whether a stream mode captures through KMS once capture is set to KMS.
   *
   * Private Stream captures its own compositor through wlroots whatever capture says, and Host
   * Virtual Display and Desktop Takeover capture their own display through the portal or wlroots.
   * Every other mode keeps a capture set to KMS. The console names these modes when it says where
   * KMS applies, so the sentence comes from the rule a launch follows rather than from a list.
   */
  inline bool kms_possible_in_mode(std::string_view stream_mode) {
    if (stream_mode == stream_display_policy::k_host_virtual_display || stream_mode == stream_display_policy::k_desktop_takeover) {
      return false;
    }
    const auto booleans = stream_display_policy::legacy_booleans_for_selection(stream_mode);
    const auto capture = stream_display_policy::capture_for_mode(
      stream_display_policy::capture_filled_for_mode(stream_mode, "kms"),
      stream_mode,
      booleans.use_cage_compositor,
      false,
      false
    );
    return stream_display_policy::canonical_capture_backend(capture) == "kms";
  }

  /**
   * @brief Whether a /proc/<pid>/status text puts CAP_SYS_ADMIN in the permitted set.
   *
   * Permitted, not effective: Polaris raises the capability only around the framebuffer read and
   * drops it again, so an idle host that can capture through KMS shows it permitted and nothing else.
   */
  inline bool cap_sys_admin_permitted(std::string_view status) {
    constexpr std::string_view key = "CapPrm:";
    constexpr int cap_sys_admin_bit = 21;
    std::size_t at = 0;
    while (at < status.size()) {
      const auto newline = status.find('\n', at);
      const auto line = status.substr(at, newline == std::string_view::npos ? std::string_view::npos : newline - at);
      at = newline == std::string_view::npos ? status.size() : newline + 1;
      if (!line.starts_with(key)) {
        continue;
      }
      auto value = line.substr(key.size());
      const auto start = value.find_first_not_of(" \t");
      if (start == std::string_view::npos) {
        return false;
      }
      value.remove_prefix(start);
      std::uint64_t mask = 0;
      const auto [end, err] = std::from_chars(value.data(), value.data() + value.size(), mask, 16);
      if (err != std::errc() || end == value.data()) {
        return false;
      }
      return (mask >> cap_sys_admin_bit) & 1u;
    }
    return false;
  }
}  // namespace platf::kms_readiness
