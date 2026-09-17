/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <algorithm>
#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#ifdef __linux__
  #include "platform/linux/game_mode_host.h"
  #include "platform/linux/user_unit_override.h"
  #include "platform/linux/input/input_group_access.h"
#endif

extern "C" {
#ifdef __linux__
  #include <pwd.h>
  #include <unistd.h>
#endif
#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

using namespace std::literals;

#ifdef __linux__
namespace fs = std::filesystem;

namespace {
  std::optional<fs::path> executable_path() {
    std::array<char, 4096> path {};
    const auto len = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (len <= 0) {
      return std::nullopt;
    }

    path[len] = '\0';
    return fs::path(path.data());
  }

  fs::path resolve_bundled_host_asset(const fs::path &relative_path) {
    const auto installed_path = fs::path(POLARIS_ASSETS_DIR) / relative_path;
    if (fs::exists(installed_path)) {
      return installed_path;
    }

    const auto exe_path = executable_path();
    if (exe_path) {
      const auto local_build_path = exe_path->parent_path() / "assets" / relative_path;
      if (fs::exists(local_build_path)) {
        return local_build_path;
      }
    }

    return installed_path;
  }

  bool write_file_with_parent_dirs(const fs::path &target, const std::string &contents, std::string_view label) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
      BOOST_LOG(error) << "Failed to create parent directory for "sv << label << " ["sv << target << "]: "sv << ec.message();
      return false;
    }

    if (file_handler::write_file(target.c_str(), contents) != 0) {
      BOOST_LOG(error) << "Failed to write "sv << label << " ["sv << target << ']';
      return false;
    }

    return true;
  }

  /**
   * @brief Check whether a distribution package already provides a host asset.
   *
   * Packages install the udev rules and the modules-load configuration to their
   * live system paths, which makes them package-manager owned and removable on
   * uninstall. When the packaged copy is already current there is nothing for
   * `--setup-host` to write, and writing a second copy into /etc would shadow
   * the packaged one with a file that no uninstall ever removes.
   */
  bool host_asset_provided_by_package(const fs::path &source, const fs::path &packaged_target, std::string_view label) {
    if (packaged_target.empty() || !fs::exists(packaged_target)) {
      return false;
    }

    const auto packaged = file_handler::read_file(packaged_target.c_str());
    if (packaged.empty() || packaged != file_handler::read_file(source.c_str())) {
      return false;
    }

    BOOST_LOG(info) << "Linux host setup: "sv << label << " is provided by the package at ["sv << packaged_target << "]; nothing to install"sv;
    return true;
  }

  /**
   * @brief Retire a copy an earlier Polaris wrote into /etc.
   *
   * /etc wins over the vendor directory, so a copy left by an older install
   * shadows the packaged file — every later fix to the rules would be installed
   * and then ignored. A copy that matches any version Polaris shipped is Polaris'
   * own leftover and is removed; anything else may be a local edit and is kept,
   * with a warning that it is the file in effect.
   *
   * @return Whether the /etc copy is gone afterwards; false means it still overrides the packaged file.
   */
  bool retire_shadowing_etc_asset(const fs::path &etc_target, const fs::path &packaged_source, std::string_view label) {
    std::error_code ec;
    if (!fs::exists(etc_target, ec)) {
      return true;
    }

    const auto existing = file_handler::read_file(etc_target.c_str());
    const bool identical = existing == file_handler::read_file(packaged_source.c_str());
    if (!identical && !is_shipped_host_asset_version(etc_target.filename().string(), existing)) {
      // It matches no version Polaris shipped, so it may hold a local edit, and
      // deleting somebody's edit is worse than leaving a stale file. This only
      // reports it, with the command, because the shadowed file is the packaged one.
      BOOST_LOG(warning) << "Linux host setup: ["sv << etc_target << "] differs from every "sv << label
                         << " version Polaris shipped and overrides the packaged file. If you did not edit it yourself, "sv
                         << "remove it with [sudo rm "sv << etc_target << "] so the packaged file applies."sv;
      return false;
    }

    if (fs::remove(etc_target, ec) && !ec) {
      BOOST_LOG(info) << "Linux host setup: removed the superseded "sv << label << " copy at ["sv << etc_target << ']'
                      << (identical ? ""sv : " that an older Polaris installed"sv)
                      << "; the packaged file applies from now on"sv;
      return true;
    }

    BOOST_LOG(warning) << "Linux host setup: could not remove the superseded "sv << label << " copy at ["sv
                       << etc_target << "]: "sv << ec.message();
    return false;
  }

  bool install_host_asset(const fs::path &source, const fs::path &target, std::string_view label) {
    const auto contents = file_handler::read_file(source.c_str());
    if (contents.empty()) {
      BOOST_LOG(error) << "Required Polaris asset is missing ["sv << source << "] for "sv << label;
      return false;
    }

    if (file_handler::read_file(target.c_str()) == contents) {
      BOOST_LOG(info) << "Linux host setup: "sv << label << " already up to date at ["sv << target << ']';
      return true;
    }

    if (!write_file_with_parent_dirs(target, contents, label)) {
      return false;
    }

    BOOST_LOG(info) << "Linux host setup: installed "sv << label << " at ["sv << target << ']';
    return true;
  }

  bool run_host_command(const std::string &description, const std::string &cmd, bool required = true) {
    std::error_code ec;
    auto env = boost::this_process::environment();
    auto working_dir = boost::filesystem::path("/");
    BOOST_LOG(info) << "Linux host setup: "sv << description << " with ["sv << cmd << ']';
    auto child = platf::run_command(false, true, cmd, working_dir, env, nullptr, ec, nullptr);

    if (ec) {
      if (required) {
        BOOST_LOG(error) << "Linux host setup failed to launch ["sv << cmd << "]: "sv << ec.message();
        return false;
      }

      BOOST_LOG(warning) << "Linux host setup could not launch optional command ["sv << cmd << "]: "sv << ec.message();
      return true;
    }

    child.wait();
    const auto rc = child.exit_code();
    if (rc != 0) {
      if (required) {
        BOOST_LOG(error) << "Linux host setup command ["sv << cmd << "] returned ["sv << rc << ']';
        return false;
      }

      BOOST_LOG(warning) << "Linux host setup optional command ["sv << cmd << "] returned ["sv << rc << ']';
      return true;
    }

    return true;
  }

  struct headless_boot_account_t {
    std::string name;
    uid_t uid;
    gid_t gid;
    fs::path home;
  };

  /**
   * @brief Resolve the account headless boot should apply to.
   *
   * Host setup runs as root, but lingering and the default.target hook belong
   * to the account that streams. SUDO_USER identifies it when the command is
   * run the documented way; a bare root login cannot name it, and this command
   * deliberately never guesses which account streams.
   */
  /**
   * @brief Hand a root-owned per-user directory back to its account.
   *
   * Walks without following symlinks and uses lchown, so a link planted inside
   * can only ever have its own ownership changed, never its target's. The
   * directory is root-owned at this point, which is what makes the walk safe to
   * start; entries below it may be anything.
   */
  bool hand_back_config_directory(const fs::path &directory, uid_t uid, gid_t gid, std::string &failure) {
    std::error_code ec;
    if (lchown(directory.c_str(), uid, gid) != 0) {
      failure = directory.string();
      return false;
    }
    // Ownership alone is not enough. The private-state guard refuses a group or
    // other writable directory just as firmly, and that is the more common
    // failure because directory creation honours the umask.
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) {
      failure = directory.string();
      return false;
    }
    auto walk = fs::recursive_directory_iterator(
      directory,
      fs::directory_options::skip_permission_denied,
      ec
    );
    if (ec) {
      failure = directory.string();
      return false;
    }
    for (const auto &entry : walk) {
      if (lchown(entry.path().c_str(), uid, gid) != 0) {
        failure = entry.path().string();
        return false;
      }
    }
    return true;
  }

  std::optional<headless_boot_account_t> resolve_headless_boot_account() {
    const auto user = platf::input_access::setup_host_target_user();
    if (user.empty() || user == "root") {
      BOOST_LOG(error) << "Headless boot applies to the account that streams, and root is not it. "
                          "Run this via [sudo -H] from that account so SUDO_USER identifies it."sv;
      return std::nullopt;
    }

    const auto *pw = getpwnam(user.c_str());
    if (!pw || !pw->pw_dir || pw->pw_dir[0] == '\0') {
      BOOST_LOG(error) << "Headless boot could not resolve a home directory for ["sv << user << ']';
      return std::nullopt;
    }

    return headless_boot_account_t {user, pw->pw_uid, pw->pw_gid, fs::path(pw->pw_dir)};
  }

  std::optional<fs::path> installed_user_unit_path() {
    for (const auto *candidate : {"/usr/lib/systemd/user/polaris.service", "/usr/local/lib/systemd/user/polaris.service", "/etc/systemd/user/polaris.service"}) {
      std::error_code ec;
      if (fs::exists(candidate, ec)) {
        return fs::path(candidate);
      }
    }
    return std::nullopt;
  }

  fs::path headless_boot_wants_link(const headless_boot_account_t &account) {
    return account.home / ".config/systemd/user/default.target.wants/polaris.service";
  }

  /**
   * @brief Start the account's Polaris user service at boot.
   *
   * Two pieces, both explicit: lingering, so the account's user manager runs
   * from boot without a login, and a default.target want for the packaged unit.
   * The want is the same symlink `systemctl --user add-wants` would create,
   * written directly because a root process cannot reliably talk to another
   * account's user manager. The packaged [Install] section stays pointed at
   * xdg-desktop-autostart.target on purpose: desktop hosts get the late,
   * environment-complete start they have today, and only hosts that opt in
   * here start at boot.
   */
  bool enable_headless_boot_for(const headless_boot_account_t &account) {
    const auto unit = installed_user_unit_path();
    if (!unit) {
      BOOST_LOG(error) << "Headless boot needs the packaged user unit (usr/lib/systemd/user/polaris.service), which was not found. Install the Polaris package first."sv;
      return false;
    }

    if (!run_host_command(std::format("enable lingering for {}", account.name), std::format("loginctl enable-linger {}", account.name))) {
      return false;
    }

    const auto link_path = headless_boot_wants_link(account);
    const auto wants_dir = link_path.parent_path();

    // Track which directories are about to be created so ownership can be
    // handed to the account; root-owned directories inside the home would
    // break the account's own later `systemctl --user` edits.
    std::vector<fs::path> created_dirs;
    std::error_code ec;
    for (auto dir = wants_dir; !fs::exists(dir, ec) && dir != account.home && !dir.empty(); dir = dir.parent_path()) {
      created_dirs.push_back(dir);
    }
    fs::create_directories(wants_dir, ec);
    if (ec) {
      BOOST_LOG(error) << "Headless boot could not create ["sv << wants_dir << "]: "sv << ec.message();
      return false;
    }
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) {
      if (chown(it->c_str(), account.uid, account.gid) != 0) {
        BOOST_LOG(warning) << "Headless boot could not hand ["sv << *it << "] to "sv << account.name;
      }
    }

    if (fs::is_symlink(link_path, ec)) {
      if (fs::read_symlink(link_path, ec) == *unit) {
        BOOST_LOG(info) << "Headless boot: the default.target want already points at ["sv << *unit << ']';
        return true;
      }
      fs::remove(link_path, ec);
    } else if (fs::exists(link_path, ec)) {
      BOOST_LOG(error) << "Headless boot: ["sv << link_path << "] exists and is not a symlink; refusing to replace it"sv;
      return false;
    }

    fs::create_symlink(*unit, link_path, ec);
    if (ec) {
      BOOST_LOG(error) << "Headless boot could not create ["sv << link_path << "]: "sv << ec.message();
      return false;
    }
    if (lchown(link_path.c_str(), account.uid, account.gid) != 0) {
      BOOST_LOG(warning) << "Headless boot could not hand ["sv << link_path << "] to "sv << account.name;
    }

    std::cout
      << "Headless boot enabled for "sv << account.name << '.' << std::endl
      << "Polaris now starts at boot with no monitor, desktop login, or Game Mode session."sv << std::endl
      << "Verify after the next reboot with: systemctl --user is-active polaris"sv << std::endl
      << "To start it right now without rebooting, run as "sv << account.name << ':' << std::endl
      << "  systemctl --user daemon-reload && systemctl --user start polaris"sv << std::endl
      << "Note: Private Stream and Gamescope Stream need no desktop. Streaming the visible"sv << std::endl
      << "desktop (Mirror Desktop, Host Virtual Display) still needs a desktop login, and"sv << std::endl
      << "Polaris must be restarted after that login to see it."sv << std::endl;
    return true;
  }

  bool disable_headless_boot_for(const headless_boot_account_t &account) {
    const auto link_path = headless_boot_wants_link(account);
    std::error_code ec;
    if (!fs::is_symlink(link_path, ec) && !fs::exists(link_path, ec)) {
      std::cout << "Headless boot was not enabled for "sv << account.name << "; nothing to remove."sv << std::endl;
      return true;
    }

    if (!fs::remove(link_path, ec) || ec) {
      BOOST_LOG(error) << "Headless boot could not remove ["sv << link_path << "]: "sv << ec.message();
      return false;
    }

    std::cout
      << "Headless boot disabled for "sv << account.name << "; the user service returns to starting with the desktop session."sv << std::endl
      << "Lingering was left enabled because other user services may rely on it. If nothing"sv << std::endl
      << "else needs it: loginctl disable-linger "sv << account.name << std::endl;
    return true;
  }

  void print_setup_host_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " --setup-host [--enable-kms] [--enable-headless-boot | --disable-headless-boot]"sv << std::endl
      << std::endl
      << "  Applies Linux host integration explicitly instead of relying on package scripts."sv << std::endl
      << "  Steps:"sv << std::endl
      << "    - install Polaris udev rules into /etc/udev/rules.d, unless the package already"sv << std::endl
      << "      provides them at "sv << POLARIS_UDEV_RULES_DIR << std::endl
      << "    - install Polaris modules-load config into /etc/modules-load.d, unless the package"sv << std::endl
      << "      already provides it at "sv << POLARIS_MODULES_LOAD_DIR << std::endl
      << "    - remove an /etc copy left by an older Polaris that now shadows the packaged file"sv << std::endl
      << "    - reload udev and trigger /dev/uinput and /dev/uhid"sv << std::endl
      << "    - load uinput and uhid now via modprobe"sv << std::endl
      << "    - report whether the calling account is in the input group, which seat isolation needs"sv << std::endl
      << "    - optionally apply cap_sys_admin for DRM/KMS capture"sv << std::endl
      << "    - optionally make the invoking account's Polaris user service start at boot,"sv << std::endl
      << "      with no monitor, desktop login, or Game Mode session required"sv << std::endl
      << std::endl
      << "  Options:"sv << std::endl
      << "    --enable-kms            Also run setcap cap_sys_admin+ep on the Polaris binary"sv << std::endl
      << "    --enable-headless-boot  Enable lingering for the invoking account and hook the"sv << std::endl
      << "                            Polaris user service into default.target, so it starts at"sv << std::endl
      << "                            boot before anyone logs in"sv << std::endl
      << "    --disable-headless-boot Remove the boot-start hook again; lingering is left as"sv << std::endl
      << "                            configured because other user services may rely on it"sv << std::endl
      << "    help                    Print this help"sv << std::endl;
  }
}  // namespace
#endif

void launch_ui(const std::optional<std::string> &path) {
  const auto local_host = config::sunshine.address_family == "ipv4" ? "127.0.0.1" : "localhost";
  std::string url = std::format("https://{}:{}", local_host, static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

bool is_shipped_host_asset_version(std::string_view file_name, std::string_view contents) {
  // Every version of the two files Polaris has shipped, by sha256. When either
  // file changes, add the new digest here; the entry handler test fails until then.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> shipped {{
    {"60-polaris.rules", "9c50dce1aaf5d326685fb83bd00880d09d368a1eece1e45de7297f7b7494c4ae"},
    {"60-polaris.rules", "a48ab8b22bda422bdaec1212eadbf38aa80a384e89f6b8bc7dca060f521e892d"},
    {"60-polaris.rules", "5e69cee0e39bf85d57c17c273d3ff71a3b5d187f8125e3f396a207dae2651490"},
    {"60-polaris.rules", "f34cde4f5d6b369ae2bab83c50b8bd0f6b0f783d7d9bbe0f6affd6947a1542b5"},
    {"60-polaris.rules", "1e1455ef4f19154d83fe5d5da6b9f530daad733d5debda101950bace0863ca33"},
    {"60-polaris.rules", "0f7e99b4ae07d7379f9416b2417eee6b7744c9bafc0547dfc503cb35b7b0bdf1"},
    {"60-polaris.conf", "11ed83ac0126b16abab10ae0ca1c8750207f2fcabe5f00fe736a562a2bd45884"},
    {"60-polaris.conf", "557eb7e447e6e55fd3eefcc392cd1690cfb905936683b3978583a36f91a0ab84"},
  }};
  if (contents.empty()) {
    return false;
  }
  const auto digest = crypto::hash(contents);
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    hex += std::format("{:02x}", byte);
  }
  return std::any_of(shipped.begin(), shipped.end(), [&](const auto &version) {
    return version.first == file_name && version.second == hex;
  });
}

config_ownership_action_e config_ownership_action(
  bool exists,
  bool is_directory,
  bool is_symlink,
  std::uint32_t owner_uid,
  std::uint32_t account_uid,
  std::uint32_t mode
) {
  if (!exists) {
    return config_ownership_action_e::nothing;
  }
  if (is_symlink || !is_directory) {
    return config_ownership_action_e::refuse;
  }
  // Group or other writable is refused by the private-state guard just as
  // firmly as the wrong owner is, and it is the more common way in: directory
  // creation honours the umask, so a umask of 002 produces 0775 unasked. A
  // directory the account already owns can be narrowed safely.
  const bool writable_by_others = (mode & 0022) != 0;
  if (owner_uid == account_uid) {
    return writable_by_others ? config_ownership_action_e::repair :
                                config_ownership_action_e::nothing;
  }
  if (owner_uid == 0) {
    return config_ownership_action_e::repair;
  }
  return config_ownership_action_e::refuse;
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    const auto status = http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);
    if (status != 0) {
      return status;
    }

    BOOST_LOG(info) << "Credentials file: "sv << config::sunshine.credentials_file;
    BOOST_LOG(info) << "Restart any running Polaris process before signing in with the new web credentials."sv;
#ifdef __linux__
    BOOST_LOG(info) << "If Polaris runs as a user service, run [systemctl --user restart polaris]."sv;
#endif

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

#ifdef __linux__
  int setup_host(const char *name, int argc, char *argv[]) {
    bool enable_kms = false;
    bool enable_headless_boot = false;
    bool disable_headless_boot = false;

    for (int i = 0; i < argc; ++i) {
      const auto arg = std::string_view(argv[i]);
      if (arg == "help"sv || arg == "--help"sv) {
        print_setup_host_help(name);
        return 0;
      }
      if (arg == "--enable-kms"sv || arg == "enable-kms"sv) {
        enable_kms = true;
        continue;
      }
      if (arg == "--enable-headless-boot"sv || arg == "enable-headless-boot"sv) {
        enable_headless_boot = true;
        continue;
      }
      if (arg == "--disable-headless-boot"sv || arg == "disable-headless-boot"sv) {
        disable_headless_boot = true;
        continue;
      }

      BOOST_LOG(error) << "Unknown --setup-host option: "sv << arg;
      print_setup_host_help(name);
      return 1;
    }

    if (enable_headless_boot && disable_headless_boot) {
      BOOST_LOG(error) << "--enable-headless-boot and --disable-headless-boot are mutually exclusive"sv;
      print_setup_host_help(name);
      return 1;
    }

    const auto exe_path = executable_path();
    if (!exe_path) {
      BOOST_LOG(error) << "Unable to resolve the running Polaris binary path for --setup-host"sv;
      return 1;
    }

    const auto udev_source = resolve_bundled_host_asset("udev/rules.d/60-polaris.rules");
    const auto modules_source = resolve_bundled_host_asset("modules-load.d/60-polaris.conf");

    const auto udev_packaged = fs::path(POLARIS_UDEV_RULES_DIR) / "60-polaris.rules";
    const auto modules_packaged = fs::path(POLARIS_MODULES_LOAD_DIR) / "60-polaris.conf";
    const bool udev_from_package = host_asset_provided_by_package(udev_source, udev_packaged, "udev rules");
    const bool modules_from_package = host_asset_provided_by_package(modules_source, modules_packaged, "modules-load config");

    // After a packaged install and a reboot there is nothing privileged left to
    // do, and asking for sudo to discover that is exactly the friction this
    // command should not have. Only claim it when the virtual input nodes are
    // actually usable, which is the thing the whole step exists to arrange.
    const bool etc_copies_absent = !fs::exists("/etc/udev/rules.d/60-polaris.rules") &&
                                   !fs::exists("/etc/modules-load.d/60-polaris.conf");
    const auto setup_target_user = platf::input_access::setup_host_target_user();
    const bool input_nodes_ready = platf::input_access::setup_host_target_can_access_input_nodes();
    // Membership is not something host setup can arrange — usermod would have to
    // pick a target account, and this command deliberately never guesses which
    // account streams. It reports it so a user preparing the host learns it here
    // rather than from a controller that silently never appears.
    const auto input_group_advice = platf::input_access::setup_host_input_group_advice();

    // A host that can boot into a Steam Game Mode session loses Polaris the
    // moment it leaves Desktop Mode unless the service starts at boot. Say so
    // here, where the person preparing the host is reading, instead of
    // leaving it to a client that reports the host offline. The advice is
    // about the account that streams, so like headless boot itself it stays
    // silent when a bare root login cannot name one.
    std::string game_mode_advice;
    std::string service_override_advice;
    if (const auto *target_pw = setup_target_user.empty() || setup_target_user == "root" ? nullptr : getpwnam(setup_target_user.c_str());
        target_pw && target_pw->pw_dir && target_pw->pw_dir[0] != '\0') {
      const auto game_mode = platf::game_mode_host::detect(platf::game_mode_host::default_probe(target_pw->pw_uid));
      const auto boot_before = platf::game_mode_host::boot_readiness(
        platf::game_mode_host::default_boot_paths(setup_target_user, target_pw->pw_dir)
      );
      using state_t = platf::game_mode_host::setup_host_state_t;
      const auto state = enable_headless_boot ? state_t::headless_boot_enabled_now :
                         disable_headless_boot ? state_t::headless_boot_disabled_now :
                         boot_before.independent() ? state_t::already_independent :
                                                     state_t::needs_headless_boot;
      game_mode_advice = platf::game_mode_host::setup_host_advice(game_mode, state, exe_path->string());

      // The Bazzite DRM/KMS recipe points the user service at a copy of the
      // binary through a drop-in. A copy removed without its drop-in leaves a
      // service that cannot exec, and a copy kept across package updates keeps
      // running the old version; both are invisible until someone reads
      // `systemctl --user cat polaris`. Say it here, where the person fixing
      // the host is reading.
      service_override_advice = platf::user_unit::setup_host_advice(
        platf::user_unit::effective_exec_override(fs::path(target_pw->pw_dir) / ".config/systemd/user/polaris.service.d"),
        setup_target_user,
        *exe_path
      );
    }

    const bool headless_boot_requested = enable_headless_boot || disable_headless_boot;
    if (!enable_kms && !headless_boot_requested && udev_from_package && modules_from_package && etc_copies_absent && input_nodes_ready) {
      if (game_mode_advice.empty()) {
        std::cout
          << "Linux host setup: nothing to do."sv << std::endl
          << "The package provides the udev rules and modules-load configuration, and /dev/uinput"sv << std::endl
          << "and /dev/uhid are already usable by ["sv << setup_target_user << "]. Re-run with --enable-kms only if you"sv << std::endl
          << "need DRM/KMS capture."sv << std::endl;
      } else {
        // "Nothing to do" would contradict the command that follows.
        std::cout
          << "Linux host setup: the package provides the udev rules and modules-load configuration, and /dev/uinput"sv << std::endl
          << "and /dev/uhid are already usable by ["sv << setup_target_user << "]."sv << std::endl
          << std::endl
          << game_mode_advice;
      }
      if (!service_override_advice.empty()) {
        std::cout << std::endl
                  << service_override_advice;
      }
      if (!input_group_advice.empty()) {
        std::cout << std::endl
                  << input_group_advice << std::endl;
      }
      return 0;
    }

    if (geteuid() != 0) {
      std::cout
        << "Polaris host setup requires root because it loads kernel modules, reloads udev"sv << std::endl
        << "and, on installs the package manager does not own, writes /etc."sv << std::endl
        << "Run:"sv << std::endl
        << "  sudo -H "sv << exe_path->string() << " --setup-host"sv;
      if (enable_kms) {
        std::cout << " --enable-kms"sv;
      }
      if (enable_headless_boot) {
        std::cout << " --enable-headless-boot"sv;
      }
      if (disable_headless_boot) {
        std::cout << " --disable-headless-boot"sv;
      }
      std::cout << std::endl;
      return 1;
    }

    bool ok = true;
    // A single earlier `sudo polaris` leaves this directory owned by root, and
    // from then on Polaris cannot save anything when it runs as the account
    // that streams. Setup already holds the privilege needed to undo it, so it
    // does, rather than leaving someone to infer it from a failed credential
    // save. Only the default location is checked: a session that moved it with
    // XDG_CONFIG_HOME is not visible from a root setup run, so nothing here
    // claims to have checked one.
    if (const auto *target_pw = setup_target_user.empty() || setup_target_user == "root" ? nullptr : getpwnam(setup_target_user.c_str());
        target_pw && target_pw->pw_dir && target_pw->pw_dir[0] != '\0') {
      const auto config_dir = fs::path(target_pw->pw_dir) / ".config" / "polaris";
      struct stat metadata {};
      const bool present = ::lstat(config_dir.c_str(), &metadata) == 0;
      switch (config_ownership_action(
        present,
        present && S_ISDIR(metadata.st_mode),
        present && S_ISLNK(metadata.st_mode),
        present ? static_cast<std::uint32_t>(metadata.st_uid) : 0,
        static_cast<std::uint32_t>(target_pw->pw_uid),
        present ? static_cast<std::uint32_t>(metadata.st_mode & 07777) : 0
      )) {
        case config_ownership_action_e::repair: {
          std::string failure;
          if (hand_back_config_directory(config_dir, target_pw->pw_uid, target_pw->pw_gid, failure)) {
            std::cout
              << "Polaris host setup: ["sv << config_dir.string() << "] could not hold private state,"sv << std::endl
              << "which is what stops Polaris saving its settings as "sv << setup_target_user
              << ". It now belongs to that"sv << std::endl
              << "account and is readable only by it."sv << std::endl;
          } else {
            BOOST_LOG(error)
              << "Polaris host setup could not hand ["sv << failure << "] back to "sv
              << setup_target_user << "; run: sudo chown -R "sv << setup_target_user << ' '
              << config_dir.string();
            ok = false;
          }
          break;
        }
        case config_ownership_action_e::refuse:
          BOOST_LOG(warning)
            << "Polaris host setup: ["sv << config_dir.string() << "] is not a directory owned by "sv
            << setup_target_user << " or by root, so setup will not change it. Polaris cannot save its "sv
            << "settings there until it belongs to "sv << setup_target_user << '.';
          break;
        case config_ownership_action_e::nothing:
          break;
      }
    }

    std::vector<fs::path> etc_overrides_left;
    if (udev_from_package) {
      if (!retire_shadowing_etc_asset("/etc/udev/rules.d/60-polaris.rules", udev_source, "udev rules")) {
        etc_overrides_left.emplace_back("/etc/udev/rules.d/60-polaris.rules");
      }
    } else {
      ok &= install_host_asset(udev_source, "/etc/udev/rules.d/60-polaris.rules", "udev rules");
    }
    if (modules_from_package) {
      if (!retire_shadowing_etc_asset("/etc/modules-load.d/60-polaris.conf", modules_source, "modules-load config")) {
        etc_overrides_left.emplace_back("/etc/modules-load.d/60-polaris.conf");
      }
    } else {
      ok &= install_host_asset(modules_source, "/etc/modules-load.d/60-polaris.conf", "modules-load config");
    }
    ok &= run_host_command("reload udev rules", "udevadm control --reload-rules", false);
    ok &= run_host_command("trigger /dev/uinput permissions", "udevadm trigger --property-match=DEVNAME=/dev/uinput", false);
    ok &= run_host_command("trigger /dev/uhid permissions", "udevadm trigger --property-match=DEVNAME=/dev/uhid", false);
    ok &= run_host_command("load uinput", "modprobe uinput", false);
    ok &= run_host_command("load uhid", "modprobe uhid", false);

    if (enable_kms) {
      ok &= run_host_command("enable DRM/KMS capability", std::format(R"(setcap cap_sys_admin+ep "{}")", exe_path->string()));
    } else {
      BOOST_LOG(info) << "Linux host setup: skipping cap_sys_admin. Re-run with --enable-kms only if you need DRM/KMS capture."sv;
    }

    if (headless_boot_requested) {
      const auto account = resolve_headless_boot_account();
      if (!account) {
        ok = false;
      } else if (enable_headless_boot) {
        ok &= enable_headless_boot_for(*account);
      } else {
        ok &= disable_headless_boot_for(*account);
      }
    }

    if (!ok) {
      BOOST_LOG(error) << "Linux host setup did not complete successfully"sv;
      return 1;
    }

    std::cout
      << "Linux host setup complete."sv << std::endl;
    if (udev_from_package && modules_from_package) {
      std::cout
        << "The udev rules and modules-load configuration came from the package, so nothing was written to /etc."sv << std::endl
        << "Everything this step applied is also applied automatically at boot; it is only needed to avoid a reboot after install."sv << std::endl;
    }
    for (const auto &override_path : etc_overrides_left) {
      // Repeated here because the warning scrolls away above the summary.
      std::cout
        << "Still overriding the packaged file: "sv << override_path.string() << ". See the warning above; unless you edited it yourself,"sv << std::endl
        << "remove it with: sudo rm "sv << override_path.string() << std::endl;
    }
    std::cout
      << "Existing virtual gamepad nodes keep their previous access policy until recreated; stop active streams and restart Polaris after changing client gamepad seat isolation."sv << std::endl
      << "Start Polaris directly with `polaris`, or opt into background autostart with `systemctl --user enable --now polaris`."sv << std::endl;
    if (!game_mode_advice.empty()) {
      std::cout << std::endl
                << game_mode_advice;
    } else {
      std::cout
        << "For a host that boots with no monitor or desktop login (Game Mode consoles, dedicated streaming boxes), re-run with --enable-headless-boot."sv << std::endl;
    }
    if (!service_override_advice.empty()) {
      std::cout << std::endl
                << service_override_advice;
    }
    if (!input_group_advice.empty()) {
      std::cout << std::endl
                << input_group_advice << std::endl;
    }
    return 0;
  }
#endif

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif
}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  namespace {
    // Recorded from signal handlers, so this holds the caller's string rather
    // than a copy: taking a lock or allocating there is not async-signal-safe.
    // Every reason is a literal, which is why the parameter is a const char *.
    std::atomic<const char *> recorded_shutdown_reason {nullptr};

    std::mutex shutdown_request_handler_mutex;
    shutdown_request_handler_t shutdown_request_handler;
    std::atomic<bool> restart_in_place {false};

    shutdown_request_handler_t shutdown_request_handler_snapshot() {
      std::lock_guard<std::mutex> lock(shutdown_request_handler_mutex);
      return shutdown_request_handler;
    }
  }  // namespace

  void set_shutdown_request_handler(shutdown_request_handler_t handler) {
    std::lock_guard<std::mutex> lock(shutdown_request_handler_mutex);
    shutdown_request_handler = std::move(handler);
  }

  bool restart_in_place_pending() {
    return restart_in_place.load();
  }

  void set_restart_in_place_pending(bool pending) {
    restart_in_place.store(pending);
  }

  std::string systemd_service_unit_from_cgroup(std::string_view cgroup_contents) {
    // Prefer the unified hierarchy line; a hybrid host lists the v1 controllers first.
    std::string_view chosen;
    size_t start = 0;
    while (start < cgroup_contents.size()) {
      auto end = cgroup_contents.find('\n', start);
      if (end == std::string_view::npos) {
        end = cgroup_contents.size();
      }
      const auto line = cgroup_contents.substr(start, end - start);
      start = end + 1;
      if (line.empty()) {
        continue;
      }
      if (line.rfind("0::", 0) == 0) {
        chosen = line;
        break;
      }
      if (chosen.empty()) {
        chosen = line;
      }
    }
    const auto path_at = chosen.rfind(':');
    if (path_at == std::string_view::npos) {
      return {};
    }
    auto path = chosen.substr(path_at + 1);
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ' || path.back() == '/')) {
      path.remove_suffix(1);
    }
    const auto leaf_at = path.rfind('/');
    const auto leaf = leaf_at == std::string_view::npos ? path : path.substr(leaf_at + 1);
    constexpr std::string_view suffix = ".service";
    if (leaf.size() <= suffix.size() || leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) != 0) {
      return {};
    }
    return std::string {leaf};
  }

  bool restart_via_service_manager(std::string_view unit, std::string_view environment_override) {
    if (environment_override == "1" || environment_override == "true") {
      return true;
    }
    if (environment_override == "0" || environment_override == "false") {
      return false;
    }
    return unit == "polaris.service";
  }

  bool restart_via_service_manager() {
    const char *override_value = std::getenv("POLARIS_SERVICE_RESTART");
    std::string unit;
#ifdef __linux__
    if (std::ifstream cgroup("/proc/self/cgroup"); cgroup) {
      std::string contents((std::istreambuf_iterator<char>(cgroup)), std::istreambuf_iterator<char>());
      unit = systemd_service_unit_from_cgroup(contents);
    }
#endif
    return restart_via_service_manager(unit, override_value ? override_value : "");
  }

#ifdef POLARIS_TESTS
  void reset_for_tests() {
    set_shutdown_request_handler({});
    recorded_shutdown_reason.store(nullptr);
    restart_in_place.store(false);
    desired_exit_code.store(0);
  }
#endif

  void note_shutdown_reason(const char *reason) {
    if (reason == nullptr || *reason == '\0') {
      return;
    }

    const char *unset = nullptr;
    if (!recorded_shutdown_reason.compare_exchange_strong(unset, reason)) {
      BOOST_LOG(debug) << "Additional shutdown request ["sv << reason << "] after ["sv << unset << ']';
      return;
    }

    BOOST_LOG(info) << "Shutdown requested: "sv << reason;
  }

  const char *shutdown_reason() {
    const char *reason = recorded_shutdown_reason.load();
    return reason == nullptr ? "unspecified" : reason;
  }

  void exit_sunshine(int exit_code, bool async, const char *reason) {
    note_shutdown_reason(reason);

    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // A registered handler begins the shutdown on this thread. Raising SIGINT
    // instead is unreliable: glibc's system() ignores SIGINT process-wide while
    // a command runs, and a thread-directed signal generated in that window is
    // discarded, which is how a restart from the console could simply vanish.
    if (const auto handler = shutdown_request_handler_snapshot()) {
      handler();
    } else {
      std::raise(SIGINT);
    }

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
    // If debug trap still doesn't work, abort
    abort();
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << POLARIS_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << POLARIS_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << POLARIS_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "ApolloService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << SERVICE_NAME " failed to start: "sv << status.dwWin32ExitCode;
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
