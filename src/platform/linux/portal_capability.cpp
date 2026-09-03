/**
 * @file src/platform/linux/portal_capability.cpp
 * @brief Process capability policy for XDG Desktop Portal capture.
 */

#include "portal_capability.h"

#ifdef __linux__

  #include "src/logging.h"

  #include <algorithm>
  #include <array>
  #include <cctype>
  #include <cerrno>
  #include <cstring>
  #include <string>

  #include <linux/capability.h>
  #include <sys/prctl.h>
  #include <sys/syscall.h>
  #include <unistd.h>

using namespace std::literals;

namespace portal_capability {

  namespace {
    std::string lower_copy(std::string_view value) {
      std::string result(value);
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return result;
    }
  }  // namespace

  bool requires_unprivileged_process(
    std::string_view configured_capture,
    std::string_view stream_mode
  ) {
    const auto capture = lower_copy(configured_capture);
    if (!capture.empty() && capture != "auto") {
      return capture == "portal";
    }

    const auto mode = lower_copy(stream_mode);
    return mode == "desktop_display" ||
           mode == "desktop_takeover" ||
           mode == "gamescope_stream" ||
           mode == "headless_dongle";
  }

  prepare_result_e prepare_process_for_capture(
    std::string_view configured_capture,
    std::string_view stream_mode
  ) {
    if (!requires_unprivileged_process(configured_capture, stream_mode)) {
      return prepare_result_e::not_needed;
    }

    if (geteuid() == 0) {
      BOOST_LOG(error) << "portal: refusing to alter capabilities for a root Polaris process; run Polaris as the desktop user"sv;
      return prepare_result_e::failed;
    }

    __user_cap_header_struct header {
      .version = _LINUX_CAPABILITY_VERSION_3,
      .pid = 0,
    };
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capabilities {};
    if (syscall(SYS_capget, &header, capabilities.data()) != 0) {
      BOOST_LOG(error) << "portal: could not inspect process capabilities before ScreenCast setup: "sv
                       << std::strerror(errno);
      return prepare_result_e::failed;
    }

    const bool has_capabilities = std::any_of(
      capabilities.begin(),
      capabilities.end(),
      [](const auto &entry) {
        return entry.effective != 0 || entry.permitted != 0 || entry.inheritable != 0;
      }
    );
    if (!has_capabilities) {
      return prepare_result_e::unchanged;
    }

    // Portal capture needs no Linux capability. Clearing every set also covers
    // locally-added capabilities such as CAP_SYS_NICE: any capability that is
    // not available to the portal daemon can make its /proc caller check fail.
    capabilities = {};
    if (syscall(SYS_capset, &header, capabilities.data()) != 0) {
      BOOST_LOG(error) << "portal: could not drop inherited executable capabilities before ScreenCast setup: "sv
                       << std::strerror(errno);
      return prepare_result_e::failed;
    }

    // Executing a file-capability-enabled binary resets dumpability. capset()
    // does not restore it after the privilege has been discarded, so do that
    // explicitly before xdg-desktop-portal opens /proc/<pid>/root.
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
      BOOST_LOG(error) << "portal: dropped inherited capabilities but could not restore normal process dumpability: "sv
                       << std::strerror(errno);
      return prepare_result_e::failed;
    }

    BOOST_LOG(info) << "portal: dropped inherited executable capabilities and restored normal process access for ScreenCast; DRM/KMS capture remains available after a restart with capture=kms"sv;
    return prepare_result_e::dropped;
  }

}  // namespace portal_capability

#endif  // __linux__
