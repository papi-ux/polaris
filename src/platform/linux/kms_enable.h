/**
 * @file src/platform/linux/kms_enable.h
 * @brief Turn DRM/KMS capture on for the account that streams, but only once its session can use it.
 *
 * --enable-kms points the polaris user service at the packaged helper, and only members of the
 * polaris-kms group may execute that helper. A user service runs with the groups its service
 * manager started with, and the manager started at login. So on the run that adds the account to
 * the group, and on every run until the account logs in again, a drop-in pointing at the helper
 * leaves a service that cannot start at all: systemd reports status=203/EXEC and nothing else
 * explains it. Host setup therefore reads the groups the running `systemd --user` really holds.
 * Until the group is among them it parks the drop-in under a name systemd ignores, leaves the
 * capture the host has now exactly as it is, and says what finishes the job.
 *
 * Everything outside this process that the decision reads or changes arrives through host_t, so
 * the tests stand in for /proc, the account database, setcap and the home directory.
 */
#pragma once

#include "user_unit_override.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace platf::kms_enable {
  /// What /proc/<pid>/status says about whose a process is and which groups the kernel checks for it.
  struct process_status_t {
    std::string name;  ///< the Name: line, the command the kernel knows the process by
    std::optional<std::uint32_t> real_uid;  ///< the first Uid: value
    std::optional<std::uint32_t> fs_gid;  ///< the last Gid: value, which the kernel checks for file access
    std::vector<std::uint32_t> groups;  ///< the Groups: line, the supplementary groups

    bool holds(std::uint32_t gid) const {
      return fs_gid == gid || std::find(groups.begin(), groups.end(), gid) != groups.end();
    }
  };

  /// Whitespace separated decimal ids. A word that is not one is skipped rather than guessed at.
  inline std::vector<std::uint32_t> parse_ids(std::string_view text) {
    std::vector<std::uint32_t> ids;
    while (!text.empty()) {
      const auto start = text.find_first_not_of(" \t");
      if (start == std::string_view::npos) {
        break;
      }
      text.remove_prefix(start);
      const auto end = text.find_first_of(" \t");
      const auto word = text.substr(0, end);
      std::uint32_t id = 0;
      const auto [last, err] = std::from_chars(word.data(), word.data() + word.size(), id);
      if (err == std::errc() && last == word.data() + word.size()) {
        ids.push_back(id);
      }
      if (end == std::string_view::npos) {
        break;
      }
      text.remove_prefix(end);
    }
    return ids;
  }

  inline process_status_t parse_process_status(std::string_view status) {
    process_status_t out;
    while (!status.empty()) {
      const auto newline = status.find('\n');
      const auto line = status.substr(0, newline);
      status.remove_prefix(newline == std::string_view::npos ? status.size() : newline + 1);
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      const auto key = line.substr(0, colon);
      const auto value = line.substr(colon + 1);
      if (key == "Name") {
        out.name = std::string {user_unit::trim_view(value)};
      } else if (key == "Uid") {
        if (const auto ids = parse_ids(value); !ids.empty()) {
          out.real_uid = ids.front();
        }
      } else if (key == "Gid") {
        // real, effective, saved, filesystem
        if (const auto ids = parse_ids(value); ids.size() == 4) {
          out.fs_gid = ids.back();
        }
      } else if (key == "Groups") {
        out.groups = parse_ids(value);
      }
    }
    return out;
  }

  /// Whether a NUL separated command line is a per-account service manager, `systemd --user`.
  inline bool is_user_manager_cmdline(std::string_view cmdline) {
    bool first = true;
    while (!cmdline.empty()) {
      const auto end = cmdline.find('\0');
      const auto arg = cmdline.substr(0, end);
      if (!first && arg == "--user") {
        return true;
      }
      first = false;
      if (end == std::string_view::npos) {
        break;
      }
      cmdline.remove_prefix(end + 1);
    }
    return false;
  }

  inline std::optional<std::string> read_small_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    return std::string {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  /// Where the account's running service manager stands with a group.
  enum class session_group_e {
    live,  ///< the manager holds the group, so a service it starts can execute the helper
    not_live,  ///< it runs without the group, which only a new login brings in
    no_manager,  ///< no manager runs for the account, so nothing here can say what the next one holds
  };

  /**
   * @brief Whether the account's `systemd --user` holds a group, read from /proc.
   *
   * The account database answers what the next login will hold. The kernel checks what the running
   * manager holds, and every user service, Polaris included, inherits exactly that.
   *
   * @param proc_root /proc, or a stand in for it.
   */
  inline session_group_e user_manager_group(const std::filesystem::path &proc_root, std::uint32_t uid, std::uint32_t gid) {
    bool found = false;
    bool all_hold = true;
    std::error_code ec;
    for (std::filesystem::directory_iterator it {proc_root, ec}, end; !ec && it != end; it.increment(ec)) {
      const auto pid = it->path().filename().string();
      if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) {
            return c >= '0' && c <= '9';
          })) {
        continue;
      }
      // A process can exit between listing and reading, which just means it is not the manager.
      const auto status_text = read_small_file(it->path() / "status");
      if (!status_text) {
        continue;
      }
      const auto status = parse_process_status(*status_text);
      if (status.name != "systemd" || status.real_uid != uid) {
        continue;
      }
      const auto cmdline = read_small_file(it->path() / "cmdline");
      if (!cmdline || !is_user_manager_cmdline(*cmdline)) {
        continue;
      }
      found = true;
      all_hold = all_hold && status.holds(gid);
    }
    if (!found) {
      return session_group_e::no_manager;
    }
    return all_hold ? session_group_e::live : session_group_e::not_live;
  }

  /**
   * @brief Whether the account's service manager outlives its logins.
   *
   * Lingering keeps it running from boot to shutdown, so logging out and back in does not restart
   * it, and only a reboot brings a new group in. Headless boot turns lingering on, but so does
   * `loginctl enable-linger` for any other user service, so the file systemd keeps is what counts,
   * together with a headless boot this same run turns on after DRM/KMS setup.
   *
   * @param linger_dir /var/lib/systemd/linger, or a stand in for it.
   * @param enabling_headless_boot This run also turns headless boot on, and lingering with it.
   */
  inline bool lingers(const std::filesystem::path &linger_dir, std::string_view account, bool enabling_headless_boot) {
    if (enabling_headless_boot) {
      return true;
    }
    std::error_code ec;
    return std::filesystem::exists(linger_dir / std::string {account}, ec);
  }

  /// The account the polaris user service runs as.
  struct account_t {
    std::string name;
    std::filesystem::path home;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
  };

  /// Everything outside this process that turning DRM/KMS capture on reads or changes.
  struct host_t {
    std::filesystem::path helper {user_unit::packaged_kms_helper};
    std::filesystem::path runtime_copy {user_unit::guide_runtime_copy};
    std::function<bool(const std::filesystem::path &)> holds_capability;
    std::function<bool(const account_t &)> in_group;  ///< the account database says it is a member
    std::function<session_group_e(const account_t &)> session_group;
    std::function<bool(const std::string &description, const std::string &command)> run;
    std::function<void(const std::filesystem::path &, const account_t &)> hand_to;  ///< chown to the account
    /// Lingering keeps the account's service manager running from boot to shutdown; unset means no.
    std::function<bool(const account_t &)> lingering;
  };

  enum class result_e {
    failed,  ///< refused or broke off; refusal says why, and summary what was done before that
    unchanged,  ///< DRM/KMS capture was left as it was
    on,  ///< the service runs the helper from its next restart
    parked,  ///< the drop-in waits under a name systemd ignores until the account logs in again
    off,  ///< DRM/KMS capture was taken off
  };

  struct outcome_t {
    result_e result = result_e::failed;
    std::string summary;  ///< what was done and what is left to do, for the end of the run
    std::string refusal;  ///< why nothing more was done, to print at once
    bool copy_refreshed = false;  ///< the guide's copy was rewritten from the running binary
  };

  /**
   * @brief What to print at once when a DRM/KMS step failed: why it stopped, then what it had changed.
   *
   * A step can fail after it already changed the service, and the end of the run, where the summary
   * would go, is never reached once host setup has failed. A drop-in written or a copy removed that
   * nobody is told about is a service that runs something else after its next reload.
   */
  inline std::string failure_report(const outcome_t &outcome) {
    auto text = outcome.refusal;
    if (!outcome.summary.empty()) {
      text += "Before that, host setup had already done this:\n" + outcome.summary;
    }
    return text;
  }

  inline std::filesystem::path drop_in_dir(const account_t &account) {
    return account.home / ".config/systemd/user/polaris.service.d";
  }

  inline std::string drop_in_body(const std::filesystem::path &helper) {
    return "[Service]\n"
           "ExecStart=\n"
           "ExecStart=" +
           helper.string() + '\n';
  }

  inline constexpr std::string_view active_header =
    "# Written by polaris --setup-host --enable-kms. Remove it with --disable-kms.\n";

  inline constexpr std::string_view parked_header =
    "# Parked by polaris --setup-host, so systemd ignores it: the polaris-kms group was not live in\n"
    "# this account's session yet, and a service cannot execute the helper without it. After the\n"
    "# next login, run sudo -H polaris --setup-host to turn this on, or --disable-kms to drop it.\n";

  inline bool write_text(const std::filesystem::path &path, std::string_view text) {
    std::ofstream out {path, std::ios::trunc | std::ios::binary};
    if (!out) {
      return false;
    }
    out << text;
    out.close();
    return static_cast<bool>(out);
  }

  enum class helper_e {
    ready,  ///< installed, and carrying cap_sys_admin
    missing,  ///< the polaris-kms package is not installed
    no_capability,  ///< installed, but the capability did not arrive
  };

  inline helper_e helper_state(const host_t &host) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(host.helper, ec)) || ec) {
      return helper_e::missing;
    }
    // The package applies the capability; this only checks it arrived. A filesystem mounted nosuid
    // drops file capabilities silently, and so does a deb whose postinst could not create the group.
    return host.holds_capability(host.helper) ? helper_e::ready : helper_e::no_capability;
  }

  inline std::string helper_refusal(helper_e state, const host_t &host) {
    const auto helper = host.helper.string();
    if (state == helper_e::missing) {
      return "DRM/KMS capture needs the polaris-kms package, which provides\n"
             "  " +
             helper +
             "\n"
             "It is separate because it is a second copy of the binary, and most hosts never capture\n"
             "this way. Install it with one of:\n"
             "  sudo dnf install polaris-kms\n"
             "  sudo rpm-ostree install polaris-kms\n"
             "  sudo pacman -S polaris-kms\n"
             "  sudo apt install polaris-kms\n"
             "then run this command again.\n";
    }
    return helper + " is installed but carries no cap_sys_admin, so it cannot capture\n"
                    "through DRM/KMS. That happens when its filesystem is mounted nosuid, or when the package's\n"
                    "install step could not finish. Reinstalling polaris-kms is the first thing to try.\n";
  }

  /**
   * @brief Remove drop-ins from the top of the service's ExecStart for as long as each one is doomed.
   *
   * systemd runs the ExecStart of the last drop-in that sets one, so taking that drop-in away hands
   * the service to the one beneath it, which can be just as unable to start: the old recipe's
   * 10-bazzite-kms.conf still names the copy an earlier host setup removed. So this carries on down
   * until the service would run something that is not doomed, or nothing but the unit's own command.
   *
   * @param doomed Whether an effective override has to go.
   * @param ec Why removal stopped early, when it did.
   * @return What each removed drop-in pointed the service at, top first.
   */
  template<class Doomed>
  std::vector<user_unit::exec_override_t> peel_drop_ins(const std::filesystem::path &dir, Doomed doomed, std::error_code &ec) {
    std::vector<user_unit::exec_override_t> removed;
    // Each pass removes a file, so the directory runs out long before this bound does.
    for (int pass = 0; pass < 256; ++pass) {
      auto top = user_unit::effective_exec_override(dir);
      if (!top.active() || !doomed(top)) {
        break;
      }
      if (!std::filesystem::remove(top.drop_in, ec) || ec) {
        if (!ec) {
          ec = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        break;
      }
      removed.push_back(std::move(top));
    }
    return removed;
  }

  /// What the service runs once it is reloaded, as the end of a sentence.
  inline std::string service_runs(const std::filesystem::path &dir, const host_t &host) {
    const auto now = user_unit::effective_exec_override(dir);
    if (!now.active()) {
      return "the packaged binary again";
    }
    const auto named = (now.binary.empty() ? now.exec_start : now.binary.string()) + ", which " + now.drop_in.string() + " names";
    if (now.binary == host.runtime_copy) {
      return named + ": the old recipe's copy, with its own capability";
    }
    if (now.binary_missing) {
      return named + " and which is not an executable file,\n"
                     "so it still cannot start until that drop-in is fixed or removed";
    }
    return named;
  }

  inline std::string reload_steps(const account_t &account) {
    return "Reload and restart it now, as " + account.name + ":\n"
           "  systemctl --user daemon-reload\n"
           "  systemctl --user restart polaris\n";
  }

  /// Point the service at the helper, and take away a parked drop-in that did the same.
  inline bool turn_on(const account_t &account, const host_t &host, outcome_t &out) {
    namespace fs = std::filesystem;
    const auto helper = host.helper.string();
    const auto dir = drop_in_dir(account);
    const auto active = dir / std::string {user_unit::kms_drop_in_name};
    const auto parked = dir / std::string {user_unit::kms_parked_drop_in_name};

    if (!write_text(active, std::string {active_header} + drop_in_body(host.helper))) {
      out.result = result_e::failed;
      out.refusal = "Could not write [" + active.string() + "]\n";
      return false;
    }
    host.hand_to(active, account);
    std::error_code ec;
    const bool had_parked = fs::exists(fs::symlink_status(parked, ec));
    if (had_parked) {
      fs::remove(parked, ec);
    }
    out.summary += "Pointed the polaris user service at " + helper + " (" + active.string() + ").\n";
    if (had_parked) {
      out.summary += "That replaces the drop-in an earlier run parked until " + account.name + " logged in again.\n";
    }
    out.summary += "The service still runs the old command until it is reloaded. As " + account.name + ":\n"
                   "  systemctl --user daemon-reload\n"
                   "  systemctl --user restart polaris\n"
                   "DRM/KMS capture is on, and an update cannot take it away again.\n";
    out.result = result_e::on;
    return true;
  }

  /// How the account stands with the group in the account database, which is what its next login gets.
  enum class membership_e {
    joined_now,  ///< this run added it
    member,  ///< it was a member already
    not_member,  ///< it is not one, and only --enable-kms makes it one
  };

  /**
   * @brief Leave the drop-in under a name systemd ignores, and say what turns it on.
   *
   * An active drop-in already there points the service at a helper its session cannot execute, so
   * that service cannot start; it goes, and so does anything beneath it that could not start either.
   *
   * @param finish_command The command that turns the drop-in on after the login, as the summary prints it.
   */
  inline bool park(const account_t &account, const host_t &host, session_group_e state, membership_e membership, std::string_view finish_command, outcome_t &out) {
    namespace fs = std::filesystem;
    const std::string group {user_unit::kms_group};
    const auto dir = drop_in_dir(account);
    const auto active = dir / std::string {user_unit::kms_drop_in_name};
    const auto parked = dir / std::string {user_unit::kms_parked_drop_in_name};

    if (!write_text(parked, std::string {parked_header} + drop_in_body(host.helper))) {
      out.result = result_e::failed;
      out.refusal = "Could not write [" + parked.string() + "]\n";
      return false;
    }
    host.hand_to(parked, account);

    std::error_code ec;
    std::vector<user_unit::exec_override_t> removed;
    bool stuck = false;  // a drop-in beneath could not be removed, so the service still cannot start
    if (fs::exists(fs::symlink_status(active, ec))) {
      if (!fs::remove(active, ec) || ec) {
        out.result = result_e::failed;
        out.refusal = "Could not remove [" + active.string() + "], which points the polaris user service at a helper its\n"
                      "session cannot execute yet, so the service cannot start: " +
                      ec.message() + "\n";
        return false;
      }
      user_unit::exec_override_t gone;
      gone.drop_in = active;
      gone.binary = host.helper;
      removed.push_back(gone);
      // What was beneath it runs the service now. The helper is no more executable through another
      // drop-in, and a drop-in naming the copy an earlier host setup removed cannot start at all.
      const auto beneath = peel_drop_ins(
        dir,
        [&](const user_unit::exec_override_t &top) {
          std::error_code copy_ec;
          const bool copy_gone = !fs::is_regular_file(fs::symlink_status(host.runtime_copy, copy_ec));
          return top.binary == host.helper || (top.binary == host.runtime_copy && copy_gone);
        },
        ec
      );
      removed.insert(removed.end(), beneath.begin(), beneath.end());
      if (ec) {
        stuck = true;
        out.result = result_e::failed;
        out.refusal = "Could not remove a drop-in in [" + dir.string() + "] that still points the service at\n"
                      "a binary it cannot start: " +
                      ec.message() + "\n";
      }
    }

    // The service gets its groups from the account's service manager, which starts with the first
    // session and ends with the last one. Lingering keeps it from boot to shutdown, so there only a
    // reboot brings the group in, and saying "log out" would send someone round in a circle.
    const bool lingering = host.lingering && host.lingering(account);
    std::string how_to_finish = "log out and back in";
    if (membership == membership_e::not_member) {
      out.summary += account.name + " is not in the " + group + " group, so the service cannot execute the helper, and no\n"
                                                                "login changes that. So DRM/KMS capture is set up but not on.\n";
    } else if (state == session_group_e::no_manager) {
      how_to_finish = "log in as " + account.name;
      out.summary += "No session is running for " + account.name + ", so host setup cannot confirm that the " + group + " group\n"
                     "reaches the polaris user service, and pointing the service at a helper it cannot execute would\n"
                     "stop Polaris from starting at all. So DRM/KMS capture is set up but not on yet.\n";
    } else {
      out.summary += account.name + (membership == membership_e::member ? " is in the " + group + " group, but its session started before that" :
                                                                          " joined the " + group + " group just now") +
                     ",\n"
                     "and a session only picks up its groups when it starts. Pointing the polaris user service at the\n"
                     "helper now would stop Polaris from starting at all, so DRM/KMS capture is set up but not on yet.\n";
      if (lingering) {
        how_to_finish = "reboot";
        out.summary += "Lingering (loginctl enable-linger, which headless boot turns on) keeps the service manager\n"
                       "for " +
                       account.name + " running from boot to shutdown, so logging out and back in does not restart it; a reboot does.\n";
      } else if (membership == membership_e::member) {
        out.summary += "If you already logged out and back in since joining the group, another session of " + account.name + "\n"
                       "stayed open through it (an SSH login counts), so its service manager kept running. End every\n"
                       "session of " +
                       account.name + ", or reboot.\n";
      }
    }
    for (const auto &gone : removed) {
      if (gone.drop_in == active) {
        out.summary += "Removed " + active.string() + ", which pointed the service at the helper\n"
                       "before its session could execute it.\n";
      } else {
        out.summary += "Removed " + gone.drop_in.string() + ", which pointed the service at " + gone.binary.string() +
                       (gone.binary == host.helper ? " as well.\n" : ",\na copy that is no longer there.\n");
      }
    }
    if (!removed.empty()) {
      out.summary += "Once reloaded, the service runs " + service_runs(dir, host) + ".\n" + reload_steps(account);
    }
    if (stuck) {
      return false;
    }
    out.summary += "The drop-in waits at " + parked.string() + ", where systemd ignores it" +
                   (removed.empty() ? ",\nand capture carries on the way it does now." : ".");
    if (membership == membership_e::not_member) {
      out.summary += " To finish, run\n"
                     "  sudo -H polaris --setup-host --enable-kms\n"
                     "which adds " +
                     account.name + " to the group and says what comes next, or drop the parked drop-in with\n"
                                    "  sudo -H polaris --setup-host --disable-kms\n";
    } else {
      out.summary += " To finish, " + how_to_finish + ", then run\n"
                                                      "  " +
                     std::string {finish_command} +
                     "\n"
                     "again. It turns the drop-in on and is safe to run more than once" +
                     (finish_command == "sudo -H polaris --setup-host" ? std::string {".\n"} :
                                                                         std::string {";\nsudo -H polaris --setup-host on its own finishes it too.\n"});
    }
    out.result = result_e::parked;
    return true;
  }

  /**
   * @brief Point the polaris user service at the packaged helper, or park that until the next login.
   *
   * Safe to run again: with the group live it turns a parked drop-in on, and without it a second
   * run leaves the same one parked drop-in. A drop-in an earlier release wrote active while the group
   * was not live is exactly the service that cannot start, so parking takes that one away.
   *
   * @param finish_command The command that finishes the job after a login, as the summary prints it.
   */
  inline outcome_t enable(const account_t &account, const host_t &host, std::string_view finish_command) {
    namespace fs = std::filesystem;
    outcome_t out;
    const std::string group {user_unit::kms_group};

    if (const auto helper = helper_state(host); helper != helper_e::ready) {
      out.refusal = helper_refusal(helper, host);
      return out;
    }

    auto membership = membership_e::member;
    if (!host.in_group(account)) {
      // Only members may execute the helper, so a capability here is not a capability for every
      // local account, which is what marking /usr/bin/polaris used to mean.
      if (!host.run("add " + account.name + " to the " + group + " group", std::format(R"(usermod -aG "{}" "{}")", group, account.name))) {
        out.refusal = "Could not add " + account.name + " to the " + group + " group, so DRM/KMS capture was left as it was.\n";
        return out;
      }
      out.summary += "Added " + account.name + " to the " + group + " group.\n";
      membership = membership_e::joined_now;
    }

    const auto dir = drop_in_dir(account);
    // Written as root into somebody else's home, so every directory made on the way has to end up
    // theirs, or their own later `systemctl --user edit` fails.
    std::error_code ec;
    std::vector<fs::path> created;
    for (auto d = dir; !d.empty() && d != account.home && !fs::exists(d, ec); d = d.parent_path()) {
      created.push_back(d);
    }
    fs::create_directories(dir, ec);
    if (ec) {
      out.refusal = "Could not create [" + dir.string() + "]: " + ec.message() + "\n";
      return out;
    }
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
      host.hand_to(*it, account);
    }
    host.hand_to(dir, account);

    const auto state = host.session_group(account);
    if (state == session_group_e::live) {
      turn_on(account, host, out);
    } else {
      park(account, host, state, membership, finish_command, out);
    }
    return out;
  }

  /**
   * @brief Whether the service is pointed at the helper by a manager that cannot execute it.
   *
   * What 1.4.13 left behind: --enable-kms wrote 20-polaris-kms.conf straight away, so until the
   * account's service manager restarted with the group, the service failed with status=203/EXEC.
   * A manager that is not running yet starts with what the account database says, so there only a
   * missing membership means it will not be able to either.
   */
  inline bool helper_unreachable(const user_unit::exec_override_t &override, const account_t &account, const host_t &host) {
    if (!override.active() || override.binary != host.helper ||
        override.drop_in.filename() != std::filesystem::path {std::string {user_unit::kms_drop_in_name}}) {
      return false;
    }
    // An uninstalled helper is setup_host_advice's to explain, with its own two ways out.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(host.helper, ec)) || ec) {
      return false;
    }
    switch (host.session_group(account)) {
      case session_group_e::live:
        return false;
      case session_group_e::not_live:
        return true;
      case session_group_e::no_manager:
        return !host.in_group(account);
    }
    return false;
  }

  /**
   * @brief What a plain --setup-host does about DRM/KMS: finish a parked drop-in, or park one that stops the service.
   *
   * A plain run is the command every install's notes print, so it never adds an account to a group,
   * which is --enable-kms's decision to make, and never fails host setup over DRM/KMS: a drop-in it
   * cannot turn on yet stays parked, and the summary says why and what turns it on.
   */
  inline outcome_t settle(const account_t &account, const host_t &host) {
    namespace fs = std::filesystem;
    outcome_t out;
    out.result = result_e::unchanged;
    const auto dir = drop_in_dir(account);
    const auto parked = dir / std::string {user_unit::kms_parked_drop_in_name};

    std::error_code ec;
    const bool has_parked = fs::exists(fs::symlink_status(parked, ec));
    if (!has_parked && !helper_unreachable(user_unit::effective_exec_override(dir), account, host)) {
      return out;
    }

    const auto membership = host.in_group(account) ? membership_e::member : membership_e::not_member;
    const auto state = host.session_group(account);
    if (has_parked && membership == membership_e::member && state == session_group_e::live) {
      if (const auto helper = helper_state(host); helper != helper_e::ready) {
        out.result = result_e::parked;
        out.summary = "DRM/KMS capture for " + account.name + " waits at " + parked.string() + ", and its session holds the\n" +
                      std::string {user_unit::kms_group} + " group now, but " + host.helper.string() +
                      (helper == helper_e::missing ? " is not installed" : " carries no cap_sys_admin") +
                      ", so it stays parked. Install\n"
                      "polaris-kms and run sudo -H polaris --setup-host again to turn it on, or drop it with\n"
                      "  sudo -H polaris --setup-host --disable-kms\n";
        return out;
      }
      turn_on(account, host, out);
      return out;
    }
    park(account, host, state, membership, "sudo -H polaris --setup-host", out);
    return out;
  }

  /**
   * @brief Move a host that runs the Bazzite guide's copy onto the packaged helper.
   *
   * The copy carries its own capability and is what captures today. It goes, and so do the drop-in
   * that pointed at it and the capability on the packaged binary, only once the helper has replaced
   * it; while the helper has to wait for a login, the copy stays exactly as it is, refreshed if it
   * fell behind this build.
   *
   * @param exe The packaged binary running host setup, which a stale copy is refreshed from.
   * @param copy_stale The copy holds another build than @p exe.
   */
  inline outcome_t move_off_runtime_copy(const account_t &account, const host_t &host, const std::filesystem::path &exe, bool copy_stale) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto copy = host.runtime_copy.string();

    const auto refresh_copy = [&](outcome_t &out) {
      // install(1) unlinks the old copy first, so a service still running it keeps its image and the
      // write cannot fail with ETXTBSY.
      if (host.run("refresh the DRM/KMS runtime copy", std::format(R"(install -D -m 0755 "{}" "{}")", exe.string(), copy)) &&
          host.run("restore the runtime copy's DRM/KMS capability", std::format(R"(setcap cap_sys_admin+ep "{}")", copy))) {
        out.copy_refreshed = true;
        return true;
      }
      out.result = result_e::failed;
      out.refusal = "Could not refresh " + copy + " from " + exe.string() + ".\n";
      return false;
    };

    if (helper_state(host) != helper_e::ready) {
      // No helper to move to yet. A working host that cannot migrate today is not a host to break,
      // so a copy that fell behind is refreshed rather than left running an older Polaris.
      outcome_t out;
      out.result = result_e::unchanged;
      if (copy_stale && refresh_copy(out)) {
        out.summary = "Refreshed " + copy + ", the copy the polaris user service for [" + account.name + "] runs, from " + exe.string() + ".\n"
                      "It held another build, so that service was still running an older Polaris than the package. Restart it as " +
                      account.name +
                      ":\n"
                      "  systemctl --user restart polaris\n"
                      "Install the polaris-kms package and run this again to stop needing that: the capability\n"
                      "moves into a file the package manager owns, and updates stop taking it away.\n";
      }
      return out;
    }

    // The old recipe's drop-in, which has to go with the copy it names: left behind, it is what the
    // service falls back to, and execs a file that is gone, the moment 20-polaris-kms.conf is removed.
    const auto dir = drop_in_dir(account);
    const auto before = user_unit::effective_exec_override(dir);
    const auto copy_drop_in = before.active() && before.binary == host.runtime_copy ? before.drop_in : fs::path {};

    auto out = enable(account, host, "sudo -H polaris --setup-host");
    if (out.result == result_e::on) {
      out.summary = "Moved DRM/KMS capture off " + copy + ", the copy no package owned.\n" + out.summary;
      if (fs::remove(host.runtime_copy, ec) && !ec) {
        out.summary += "Removed " + copy + ".\n";
        if (!copy_drop_in.empty() && copy_drop_in.filename() != fs::path {std::string {user_unit::kms_drop_in_name}} &&
            fs::remove(copy_drop_in, ec) && !ec) {
          out.summary += "Removed " + copy_drop_in.string() + ", the old recipe's drop-in, which pointed the service at that copy.\n";
        }
      }
      if (host.holds_capability(exe)) {
        if (!host.run("remove the DRM/KMS capability from the packaged binary", std::format(R"(setcap -r "{}")", exe.string()))) {
          out.result = result_e::failed;
          out.refusal = "Could not remove cap_sys_admin from " + exe.string() + ".\n";
          return out;
        }
        out.summary += "Took cap_sys_admin off " + exe.string() + ", which the helper holds instead.\n";
      }
      out.summary += "Updates leave this alone from now on; there is nothing to re-run after one.\n";
      return out;
    }
    if (out.result == result_e::parked) {
      out.summary = "DRM/KMS capture is moving off " + copy + ", the copy no package owned, onto the packaged helper.\n" + out.summary +
                    "Until then the service keeps running " + copy + " with its own capability. The run\n"
                    "after the login removes that copy, and the capability on " +
                    exe.string() + ", once the helper has taken over.\n";
      if (copy_stale && refresh_copy(out)) {
        out.summary += "Refreshed " + copy + " from " + exe.string() + " meanwhile, because it held another build.\n"
                       "Restart the service to run this one, as " +
                       account.name + ":\n"
                                      "  systemctl --user restart polaris\n";
      }
    }
    return out;
  }

  /**
   * @brief Take DRM/KMS capture off this host, including the parts the Bazzite guide added by hand.
   *
   * The inverse of --enable-kms, and of the copy recipe, because a host that keeps any one piece
   * keeps the capability. The drop-ins go first, every one from the top that points the service at
   * the helper or the copy: a drop-in left beneath would take over, and one naming the copy removed
   * next would leave a service that cannot exec, which systemd reports as status=203/EXEC and nothing
   * else explains.
   *
   * @param account The account whose service to put back; null when there is none to name.
   * @param exe The packaged binary, whose own capability comes off too.
   */
  inline outcome_t disable(const account_t *account, const host_t &host, const std::filesystem::path &exe) {
    namespace fs = std::filesystem;
    outcome_t out;
    std::error_code ec;
    const auto copy = host.runtime_copy.string();
    const bool copy_exists = fs::is_regular_file(fs::symlink_status(host.runtime_copy, ec)) && !ec;

    fs::path dir;
    user_unit::exec_override_t service_override;
    fs::path parked;
    if (account) {
      dir = drop_in_dir(*account);
      service_override = user_unit::effective_exec_override(dir);
      if (const auto candidate = dir / std::string {user_unit::kms_parked_drop_in_name}; fs::exists(fs::symlink_status(candidate, ec))) {
        parked = candidate;
      }
    }

    const auto plan = user_unit::kms_teardown_plan(service_override, host.holds_capability(exe), copy_exists, host.runtime_copy, parked, host.helper);
    if (plan.empty()) {
      out.result = result_e::unchanged;
      out.summary = "DRM/KMS capture was not enabled here: no capability on " + exe.string() + ", no " + copy +
                    ", and no service drop-in pointing at one. Nothing to remove.\n";
      return out;
    }

    std::vector<user_unit::exec_override_t> removed;
    if (!plan.drop_in.empty()) {
      removed = peel_drop_ins(
        dir,
        [&](const user_unit::exec_override_t &top) {
          return top.binary == host.helper || top.binary == host.runtime_copy;
        },
        ec
      );
      for (const auto &gone : removed) {
        out.summary += "Removed " + gone.drop_in.string() + ", which pointed the service at " + gone.binary.string() + ".\n";
      }
      if (ec) {
        out.refusal = "Could not remove a DRM/KMS service drop-in in [" + dir.string() + "]: " + ec.message() + "\n";
        return out;
      }
      out.summary += "Once reloaded, the service runs " + service_runs(dir, host) + ".\n";
    }
    if (!plan.parked_drop_in.empty()) {
      if (!fs::remove(plan.parked_drop_in, ec) || ec) {
        out.refusal = "Could not remove the parked DRM/KMS service drop-in [" + plan.parked_drop_in.string() + "]: " + ec.message() + "\n";
        return out;
      }
      out.summary += "Removed " + plan.parked_drop_in.string() + ", which --enable-kms parked until the next login.\n";
    }
    // Only after the drop-ins: a service still pointed at a copy that is gone cannot start at all,
    // which is worse than one still holding a capability it does not need.
    if (plan.remove_guide_copy) {
      if (!fs::remove(host.runtime_copy, ec) || ec) {
        out.refusal = "Could not remove the DRM/KMS runtime copy [" + copy + "]: " + ec.message() + "\n";
        return out;
      }
      out.summary += "Removed " + copy + ", the copy that carried its own capability.\n";
    }
    if (plan.clear_binary_capability) {
      if (!host.run("remove the DRM/KMS capability", std::format(R"(setcap -r "{}")", exe.string()))) {
        out.refusal = "Could not remove cap_sys_admin from " + exe.string() + ".\n";
        return out;
      }
      out.summary += "Removed cap_sys_admin from " + exe.string() + ".\n";
    }

    if (!removed.empty()) {
      out.summary += "The service still runs the old command until it is reloaded. As " + account->name + ":\n"
                     "  systemctl --user daemon-reload\n"
                     "  systemctl --user restart polaris\n";
    }
    out.summary += "DRM/KMS capture is off. Polaris keeps capturing through its other paths; --enable-kms puts it back.\n";
    out.result = result_e::off;
    return out;
  }

  /// What host setup finds about DRM/KMS before it changes anything.
  struct setup_facts_t {
    bool enable_kms = false;  ///< --enable-kms was asked for
    bool disable_kms = false;  ///< --disable-kms was asked for
    bool runtime_copy_in_use = false;  ///< the service runs the guide's copy, and the packaged binary is running setup
    bool parked = false;  ///< an earlier run parked the drop-in until the account logged in again
    bool helper_unreachable = false;  ///< the service is pointed at the helper by a manager that cannot execute it
  };

  /// The one thing host setup does about DRM/KMS on a run.
  enum class setup_step_e {
    none,  ///< nothing: a plain run with nothing waiting, which may then say "nothing to do"
    enable,  ///< --enable-kms
    disable,  ///< --disable-kms
    move_off_copy,  ///< a plain run on the old recipe's copy
    settle,  ///< a plain run with a parked drop-in, or one that stops the service
  };

  /**
   * @brief Decide what a run does about DRM/KMS; "nothing to do" is only true when this says none.
   *
   * A plain run is the command the package notes print, so it is also the one that has to notice a
   * drop-in waiting for a login, and one an earlier release wrote that stops the service.
   */
  inline setup_step_e setup_step(const setup_facts_t &facts) {
    if (facts.enable_kms) {
      return setup_step_e::enable;
    }
    if (facts.disable_kms) {
      return setup_step_e::disable;
    }
    if (facts.runtime_copy_in_use) {
      return setup_step_e::move_off_copy;
    }
    if (facts.parked || facts.helper_unreachable) {
      return setup_step_e::settle;
    }
    return setup_step_e::none;
  }

  /// Fill in what the account's drop-in directory and service manager say; nothing without an account.
  inline setup_facts_t read_facts(setup_facts_t facts, const account_t *account, const host_t &host) {
    if (!account) {
      return facts;
    }
    const auto dir = drop_in_dir(*account);
    std::error_code ec;
    facts.parked = std::filesystem::exists(std::filesystem::symlink_status(dir / std::string {user_unit::kms_parked_drop_in_name}, ec));
    facts.helper_unreachable = helper_unreachable(user_unit::effective_exec_override(dir), *account, host);
    return facts;
  }

  /// What a run without root says about a drop-in that waits or stops the service, before it asks for root.
  inline std::string waiting_notice(const setup_facts_t &facts, const account_t &account, const host_t &host) {
    const auto dir = drop_in_dir(account);
    const std::string group {user_unit::kms_group};
    if (facts.helper_unreachable) {
      return "The polaris user service for [" + account.name + "] is pointed at " + host.helper.string() + " by\n" +
             (dir / std::string {user_unit::kms_drop_in_name}).string() + ", but the session it starts from does not hold the " + group +
             "\ngroup, so the service cannot start (systemd reports status=203/EXEC). Host setup parks that drop-in,\n"
             "so Polaris starts again, and says what turns DRM/KMS capture back on.\n";
    }
    if (facts.parked) {
      return "DRM/KMS capture for [" + account.name + "] is parked at " + (dir / std::string {user_unit::kms_parked_drop_in_name}).string() +
             "\nuntil that account logs in with the " + group + " group. Host setup turns it on once it has.\n";
    }
    return {};
  }
}  // namespace platf::kms_enable
