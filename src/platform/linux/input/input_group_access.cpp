/**
 * @file src/platform/linux/input/input_group_access.cpp
 * @brief Whether the streaming account can still open its own isolated input devices.
 */
#include "input_group_access.h"
#include "src/config.h"
#include "src/platform/linux/executable_path.h"
#include "src/logging.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <grp.h>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace std::literals;

namespace platf::input_access {
  namespace {
    std::string account_name_for(uid_t uid) {
      if (const auto *entry = getpwuid(uid); entry && entry->pw_name) {
        return entry->pw_name;
      }

      // An account with no passwd entry still has to be named in the remedy, and
      // usermod takes a uid just as well as a name.
      return std::to_string(uid);
    }

    /// The `input` group's gid, when the host has one.
    std::optional<gid_t> input_group_id() {
      if (const auto *entry = getgrnam(std::string(input_group).c_str()); entry) {
        return entry->gr_gid;
      }

      return std::nullopt;
    }

    bool process_belongs_to(gid_t gid) {
      if (getegid() == gid || getgid() == gid) {
        return true;
      }

      const auto count = getgroups(0, nullptr);
      if (count <= 0) {
        return false;
      }

      std::vector<gid_t> groups(static_cast<std::size_t>(count));
      if (getgroups(count, groups.data()) < 0) {
        return false;
      }

      return std::find(groups.begin(), groups.end(), gid) != groups.end();
    }

    bool account_belongs_to(const passwd &account, gid_t gid) {
      if (account.pw_gid == gid) {
        return true;
      }

      // getgrouplist reports how many entries it needed when the buffer was too
      // small, so the first call sizes the second.
      int count = 0;
      getgrouplist(account.pw_name, account.pw_gid, nullptr, &count);
      if (count <= 0) {
        return false;
      }

      std::vector<gid_t> groups(static_cast<std::size_t>(count));
      if (getgrouplist(account.pw_name, account.pw_gid, groups.data(), &count) < 0) {
        return false;
      }

      groups.resize(static_cast<std::size_t>(count));
      return std::find(groups.begin(), groups.end(), gid) != groups.end();
    }
  }  // namespace

  seat_isolation_options_t configured_seat_isolation() {
    return {
      config::input.client_gamepad_seat_isolation,
      config::input.client_keyboard_mouse_seat_isolation,
    };
  }

  std::string isolated_device_summary(const seat_isolation_options_t &options) {
    if (options.gamepad && options.keyboard_mouse) {
      return "client gamepads and the virtual keyboard and mouse";
    }
    if (options.gamepad) {
      return "client gamepads";
    }
    if (options.keyboard_mouse) {
      return "the virtual keyboard and mouse";
    }

    return {};
  }

  bool host_is_ostree_booted() {
    std::error_code ec;
    return std::filesystem::exists("/run/ostree-booted", ec);
  }

  bool host_has_ujust() {
    return !platf::linux_util::find_executable_in_path("ujust").empty();
  }

  std::string input_group_remedy_command(std::string_view user, bool ostree_host, bool has_ujust) {
    if (!ostree_host) {
      std::string command = "sudo usermod -aG ";
      command += input_group;
      command += ' ';
      command += user;
      return command;
    }

    // Universal Blue ships a recipe that copies the group definition out of
    // /usr/lib/group into /etc/group before running usermod, which is the step
    // that makes it work at all. Reported from Bazzite in #274, where plain
    // usermod silently accomplishes nothing.
    if (has_ujust) {
      return "ujust add-user-to-input-group";
    }

    // The same two steps by hand, for an ostree host without ujust.
    std::string command = "sudo bash -c 'grep ^input: /usr/lib/group >> /etc/group' && sudo usermod -aG ";
    command += input_group;
    command += ' ';
    command += user;
    return command;
  }

  std::string input_group_remedy_command(std::string_view user) {
    return input_group_remedy_command(user, host_is_ostree_booted(), host_has_ujust());
  }

  std::string seat_isolation_access_warning(
    const seat_isolation_options_t &options,
    const input_group_status_t &status
  ) {
    if (!options.any() || status.member) {
      return {};
    }

    std::ostringstream message;
    message << "seat isolation is enabled for "sv << isolated_device_summary(options) << ", but ";

    if (!status.group_exists) {
      // Without the group, udev cannot apply GROUP="input" and the nodes end up
      // owned by root alone — a harder failure than a missing membership.
      message << "this host has no ["sv << input_group << "] group. The bundled udev rules give the "sv
              << "isolated devices to that group, so they are created without an owner Polaris can "sv
              << "reach."sv;
    } else {
      message << "the account Polaris runs as ["sv << status.user << "] is not in the ["sv << input_group
              << "] group. Isolated devices are created root:"sv << input_group << " mode 0660 and "sv
              << "deliberately get no logind ACL, so the group is the only way in."sv;
    }

    message << " Polaris cannot open the devices it creates for the client and the streamed game will "sv
            << "not see them at all."sv;

    if (status.group_exists) {
      message << " Fix it with ["sv << input_group_remedy_command(status.user)
              << "] and then log out and back in — group membership only applies to new sessions."sv;
    }

    message << " Disabling seat isolation restores the previous uaccess behaviour."sv;
    return message.str();
  }

  input_group_status_t input_group_status_for_user(std::string_view user) {
    input_group_status_t status;
    status.user = user;

    const auto gid = input_group_id();
    status.group_exists = gid.has_value();
    if (!gid) {
      return status;
    }

    if (const auto *account = getpwnam(status.user.c_str()); account) {
      status.member = account_belongs_to(*account, *gid);
    }

    return status;
  }

  input_group_status_t current_input_group_status() {
    input_group_status_t status;
    status.user = account_name_for(geteuid());

    const auto gid = input_group_id();
    status.group_exists = gid.has_value();
    if (gid) {
      // The running process' supplementary groups are what the kernel will
      // actually check, and they can differ from the passwd database when the
      // session predates a usermod.
      status.member = process_belongs_to(*gid);
    }

    return status;
  }

  std::string setup_host_target_user() {
    if (const auto *sudo_user = std::getenv("SUDO_USER"); sudo_user && *sudo_user) {
      return sudo_user;
    }

    return account_name_for(geteuid());
  }

  std::string setup_host_input_group_advice() {
    const auto status = input_group_status_for_user(setup_host_target_user());
    if (status.member) {
      return {};
    }

    std::ostringstream advice;
    advice << "Client gamepad and keyboard/mouse seat isolation additionally need ["sv << status.user
           << "] to be in the ["sv << input_group << "] group; isolated devices get no logind ACL, so "sv
           << "the group is the only way Polaris and the streamed game can open them."sv;

    if (status.group_exists) {
      advice << " Run ["sv << input_group_remedy_command(status.user)
             << "] and log out and back in. Leaving seat isolation disabled needs nothing."sv;
    } else {
      advice << " This host has no ["sv << input_group << "] group, so seat isolation cannot work here."sv;
    }

    return advice.str();
  }

  void warn_if_seat_isolation_lacks_input_group() {
    static std::once_flag reported;
    std::call_once(reported, [] {
      const auto options = configured_seat_isolation();
      if (!options.any()) {
        return;
      }

      const auto advice = seat_isolation_access_warning(options, current_input_group_status());
      if (advice.empty()) {
        return;
      }

      BOOST_LOG(warning) << "input_access: "sv << advice;
    });
  }

}  // namespace platf::input_access
