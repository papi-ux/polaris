/**
 * @file src/platform/linux/portal_capability.cpp
 * @brief Process capability policy for XDG Desktop Portal capture.
 */

#include "portal_capability.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <cctype>
  #include <cerrno>
  #include <cstring>
  #include <filesystem>
  #include <string>

  #include <linux/capability.h>
  #include <sys/prctl.h>
  #include <sys/syscall.h>
  #include <unistd.h>

using namespace std::literals;

namespace portal_capability {

  namespace {
    std::size_t thread_count() {
      std::error_code ec;
      std::size_t count = 0;
      for (std::filesystem::directory_iterator it {"/proc/self/task", ec}, end; !ec && it != end; it.increment(ec)) {
        ++count;
      }
      return ec ? 0 : count;
    }

    void say(std::string *outcome, std::string text) {
      if (outcome) {
        *outcome = std::move(text);
      }
    }

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
    std::string_view stream_mode,
    std::string_view virtual_display_backend
  ) {
    const auto capture = lower_copy(configured_capture);
    if (!capture.empty() && capture != "auto") {
      return capture == "portal";
    }

    // KWin offers its screencast protocol only to a client it can match to a
    // permission entry by /proc/<pid>/exe, and the kernel hides that for a
    // process holding file capabilities. A host set to KWin screens would
    // never get one, and nothing on that path uses KMS unless capture says so.
    if (lower_copy(virtual_display_backend) == "kwin") {
      return true;
    }

    const auto mode = lower_copy(stream_mode);
    return mode == "desktop_display" ||
           mode == "desktop_takeover" ||
           mode == "gamescope_stream" ||
           mode == "headless_dongle";
  }

  prepare_result_e prepare_process_for_capture(
    std::string_view configured_capture,
    std::string_view stream_mode,
    std::string_view virtual_display_backend,
    std::string *outcome
  ) {
    if (!requires_unprivileged_process(configured_capture, stream_mode, virtual_display_backend)) {
      return prepare_result_e::not_needed;
    }

    if (geteuid() == 0) {
      say(outcome, "refusing to alter capabilities for a root Polaris process; run Polaris as the desktop user");
      return prepare_result_e::failed;
    }

    __user_cap_header_struct header {
      .version = _LINUX_CAPABILITY_VERSION_3,
      .pid = 0,
    };
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capabilities {};
    if (syscall(SYS_capget, &header, capabilities.data()) != 0) {
      say(outcome, "could not inspect process capabilities before ScreenCast setup: "s + std::strerror(errno));
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
      // No thread holds a capability, yet the process can still be left
      // non-dumpable: a capability-enabled binary started under
      // NoNewPrivileges gets none. Nothing is privileged, so let the portal
      // and KWin identify it.
      if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1 && prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0) {
        say(outcome, "restored normal process access for ScreenCast; the process held no capabilities");
      }
      return prepare_result_e::unchanged;
    }

    // capset() below changes only this thread, while making the process
    // dumpable exposes all of them. A thread started before this point would
    // keep the capability inside a process any same-user program may then
    // attach to.
    if (thread_count() != 1) {
      say(outcome, "kept inherited capabilities because other threads already run, and dropping them would "
                   "cover only this one");
      return prepare_result_e::failed;
    }

    // Portal capture needs no Linux capability. Clearing every set also covers
    // locally-added capabilities such as CAP_SYS_NICE: any capability that is
    // not available to the portal daemon can make its /proc caller check fail.
    capabilities = {};
    if (syscall(SYS_capset, &header, capabilities.data()) != 0) {
      say(outcome, "could not drop inherited executable capabilities before ScreenCast setup: "s + std::strerror(errno));
      return prepare_result_e::failed;
    }

    // Executing a file-capability-enabled binary resets dumpability. capset()
    // does not restore it after the privilege has been discarded, so do that
    // explicitly before xdg-desktop-portal opens /proc/<pid>/root.
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
      say(outcome, "dropped inherited capabilities but could not restore normal process dumpability: "s + std::strerror(errno));
      return prepare_result_e::failed;
    }

    say(outcome, "dropped inherited executable capabilities and restored normal process access for ScreenCast; "
                 "DRM/KMS capture remains available after a restart with capture=kms");
    return prepare_result_e::dropped;
  }

}  // namespace portal_capability

#endif  // __linux__
