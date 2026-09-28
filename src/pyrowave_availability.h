/**
 * @file src/pyrowave_availability.h
 * @brief Whether PyroWave can serve a stream on this host, and what a client is told when it cannot.
 *
 * PyroWave has no software fallback and reads only the pixel formats it was written for. A host that
 * offers it where capture hands over something else negotiates a stream, the client builds a decoder,
 * and then nothing arrives. Every answer here is decided from facts the caller gathers: whether the
 * codec was built, whether a Vulkan device can run it, and what the capture route a stream would take
 * hands over. No platform header is needed, so the decisions can be tested without a GPU or a display.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/launch_failure.h"

namespace pyrowave_availability {

  /**
   * @brief Why capabilities leaves PyroWave out of capture.codecs.
   *
   * The ids are a client contract, served as capture.pyrowave_unavailable.reason. A client words its
   * own screen from them, and only not_built may send a player to look for another build.
   */
  enum class reason_e {
    not_built,  ///< this Polaris binary was built without the encoder
    no_vulkan_device,  ///< built, but no Vulkan device on this host can run it
    fp16_capture,  ///< capture hands over sixteen bit float frames, which is how KWin composites HDR
    capture_route_unsupported,  ///< capture cannot hand PyroWave frames by any reading it has
  };

  inline std::string_view reason_id(reason_e reason) {
    switch (reason) {
      case reason_e::not_built:
        return "not_built";
      case reason_e::no_vulkan_device:
        return "no_vulkan_device";
      case reason_e::fp16_capture:
        return "fp16_capture";
      case reason_e::capture_route_unsupported:
        return "capture_route_unsupported";
    }
    return "capture_route_unsupported";
  }

  /**
   * @brief What one capture route hands PyroWave, as far as it can be known before a stream opens it.
   *
   * Only KMS capture can hand over a format PyroWave has no reading for: every other backend
   * negotiates or converts its format, and PyroWave's own memory type makes them offer only what it
   * reads. So a route is judged by the backend dispatch would open for PyroWave and, for KMS, by the
   * format of the framebuffer on the plane it would read.
   */
  enum class route_e {
    readable,  ///< a backend and a format PyroWave reads
    fp16_scanout,  ///< KMS capture of a scanout in sixteen bit float, which is KWin's HDR composition
    unreadable_scanout,  ///< KMS capture of a scanout in another format PyroWave has no reading for
    unsupported_backend,  ///< the capture request names a backend that cannot hand frames to PyroWave
    unknown,  ///< nothing has looked yet, or the answer is not knowable without opening a display
  };

  /// A DRM format code, built the way drm_fourcc.h builds one, so this header needs no libdrm.
  constexpr std::uint32_t drm_fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24;
  }

  /// Whether a DRM format is one of the four sixteen bit float ones, XR4H, AR4H, XB4H and AB4H.
  constexpr bool is_fp16_fourcc(std::uint32_t fourcc) {
    return fourcc == drm_fourcc('X', 'R', '4', 'H') || fourcc == drm_fourcc('A', 'R', '4', 'H') ||
           fourcc == drm_fourcc('X', 'B', '4', 'H') || fourcc == drm_fourcc('A', 'B', '4', 'H');
  }

  /// What PyroWave makes of the framebuffer a KMS capture would read.
  struct scanout_t {
    bool readable = false;  ///< PyroWave has a reading for its format
    bool fp16 = false;  ///< its format is one of the four sixteen bit float ones
  };

  /**
   * @brief Judge a capture route.
   * @param backend What dispatch opens for PyroWave's memory type: nvfbc, wlr, portal, kms, x11 or
   *        none, and empty when the host has not evaluated its capture sources yet.
   * @param encoder_backend What dispatch opens for the probed encoder's memory type on the same
   *        request. Only read when backend is none, to tell a request PyroWave alone cannot use from
   *        one nothing can use, which is refused elsewhere with its own reason.
   * @param scanout The framebuffer a KMS route would read, or nothing when it could not be read.
   */
  inline route_e classify_route(std::string_view backend, std::string_view encoder_backend,
                                std::optional<scanout_t> scanout) {
    if (backend.empty()) {
      return route_e::unknown;
    }
    if (backend == "none") {
      return !encoder_backend.empty() && encoder_backend != "none" ? route_e::unsupported_backend :
                                                                     route_e::unknown;
    }
    if (backend != "kms") {
      return route_e::readable;
    }
    if (!scanout) {
      return route_e::unknown;
    }
    if (scanout->readable) {
      return route_e::readable;
    }
    return scanout->fp16 ? route_e::fp16_scanout : route_e::unreadable_scanout;
  }

  /// The contract object served as capture.pyrowave_unavailable.
  struct unavailable_t {
    reason_e reason;
    std::string message;  ///< player text a client shows as it is
  };

  /// Facts about this host that decide whether capabilities offers PyroWave.
  struct offer_facts_t {
    bool built = false;  ///< the binary carries the encoder
    bool device = false;  ///< a Vulkan device on this host can run it
    route_e host_route = route_e::unknown;  ///< the route a launch that names no stream mode takes
    /// A stream mode a client may pick for one launch runs its own compositor, which no KMS scanout
    /// reaches, so PyroWave can still stream there whatever the desktop is doing.
    bool private_mode_available = false;
  };

  /**
   * @brief Why capabilities leaves PyroWave out, or nothing when it offers it.
   *
   * Offered whenever some launch a client can make would stream. The host's own route is only one of
   * them: a client picks a stream mode per launch, and a private compositor is captured the same way
   * whether the desktop is in HDR or not. Leaving PyroWave out there would take it away from launches
   * that work, so those launches are left to the refusal at launch, which knows the mode.
   *
   * An unknown route offers it too. Refusing on a guess would hide a working codec, and the refusal
   * at launch still stands behind the offer.
   */
  inline std::optional<unavailable_t> unavailable(const offer_facts_t &facts) {
    if (!facts.built) {
      return unavailable_t {
        reason_e::not_built,
        "This Polaris build has no PyroWave encoder. Install a Polaris release that includes PyroWave "
        "on the host to use it.",
      };
    }
    if (!facts.device) {
      return unavailable_t {
        reason_e::no_vulkan_device,
        "The host has no GPU that can run PyroWave. It needs Vulkan 1.3 compute on the host's GPU, "
        "and no such device could be opened.",
      };
    }
    if (facts.private_mode_available) {
      return std::nullopt;
    }
    switch (facts.host_route) {
      case route_e::fp16_scanout:
        return unavailable_t {
          reason_e::fp16_capture,
          "The host's display is scanned out in sixteen bit float, which is how KDE shows HDR, and "
          "PyroWave cannot read that format. Turn HDR off on the host's display to use PyroWave, or "
          "choose another codec.",
        };
      case route_e::unreadable_scanout:
        return unavailable_t {
          reason_e::capture_route_unsupported,
          "The host's display is captured in a pixel format PyroWave cannot read. Choose another "
          "codec.",
        };
      case route_e::unsupported_backend:
        return unavailable_t {
          reason_e::capture_route_unsupported,
          "The capture method set on the host cannot hand frames to PyroWave. Set the host's capture "
          "method to Autodetect to use PyroWave, or choose another codec.",
        };
      case route_e::readable:
      case route_e::unknown:
        break;
    }
    return std::nullopt;
  }

  /// The launch_failure code for a PyroWave launch its capture route cannot feed.
  inline constexpr std::string_view k_capture_unreadable = "pyrowave_capture_unreadable";

  /**
   * @brief The refusal for a PyroWave launch whose capture route cannot feed the codec, or nothing.
   *
   * Written for any client, because a Moonlight client shows the message as it is: no screen names
   * that only one client has.
   *
   * @param route The route this launch captures through.
   * @param capture_request The capture backend this launch asks for, named in the message when it is
   *        the reason. Empty or auto reads as Autodetect.
   */
  inline std::optional<launch_failure::record_t> launch_refusal(route_e route,
                                                                std::string_view capture_request = {}) {
    switch (route) {
      case route_e::fp16_scanout:
        return launch_failure::record_t {
          503,
          std::string {k_capture_unreadable},
          "PyroWave cannot stream this host's desktop. Its display is scanned out in sixteen bit "
          "float, which is how KDE shows HDR, and PyroWave cannot read that format, so the stream "
          "would carry no picture.",
          "Turn HDR off on the host's display, choose another codec, or use Private Stream, which "
          "captures its own session rather than the desktop.",
        };
      case route_e::unreadable_scanout:
        return launch_failure::record_t {
          503,
          std::string {k_capture_unreadable},
          "PyroWave cannot read the pixel format the host's display is captured in, so the stream "
          "would carry no picture.",
          "Choose another codec, or use Private Stream, which captures its own session rather than "
          "the desktop.",
        };
      case route_e::unsupported_backend:
        {
          const auto named = capture_request.empty() || capture_request == "auto" ?
                               std::string {"Autodetect"} :
                               std::string {capture_request};
          return launch_failure::record_t {
            503,
            std::string {k_capture_unreadable},
            "PyroWave cannot use the capture method set on the host, " + named +
              ", which cannot hand frames to PyroWave.",
            "On the host, set Force a Specific Capture Method under Advanced to Autodetect, or choose "
            "another codec.",
          };
        }
      case route_e::readable:
      case route_e::unknown:
        break;
    }
    return std::nullopt;
  }

  /// The launch_failure code for a stream that would share a capture another codec's stream holds.
  inline constexpr std::string_view k_capture_in_use = "capture_in_use_by_other_codec";

  /**
   * @brief Whether a stream can share the capture the running streams use.
   *
   * One capture thread serves every stream on this host, and it opens its display for whichever
   * stream reached it first. PyroWave reads frames from a device of its own and every other codec
   * reads them from the probed encoder's, so a stream on one side of that line gets frames it cannot
   * read from a capture opened for the other: a picture that never arrives. Streams on the same side
   * share it as they always have.
   *
   * @param incoming_pyrowave Whether the stream asking to join is PyroWave.
   * @param running_pyrowave One entry per stream already capturing on the host.
   */
  inline bool shares_capture(bool incoming_pyrowave, const std::vector<bool> &running_pyrowave) {
    for (const bool running : running_pyrowave) {
      if (running != incoming_pyrowave) {
        return false;
      }
    }
    return true;
  }

  /// The refusal for a stream that cannot share the running capture; see shares_capture.
  inline launch_failure::record_t capture_in_use_refusal(bool incoming_pyrowave) {
    return launch_failure::record_t {
      503,
      std::string {k_capture_in_use},
      incoming_pyrowave ?
        "Another stream on this host is running with a codec other than PyroWave, and one capture "
        "cannot serve PyroWave and another codec at the same time." :
        "Another stream on this host is running with PyroWave, and one capture cannot serve PyroWave "
        "and another codec at the same time.",
      "Choose the codec the running stream uses, or launch again once it has ended.",
    };
  }

}  // namespace pyrowave_availability
