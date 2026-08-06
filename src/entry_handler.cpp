/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <array>
#include <csignal>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <thread>

// local includes
#include "config.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#ifdef __linux__
  #include "platform/linux/input/input_group_access.h"
#endif

extern "C" {
#ifdef __linux__
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
   * and then ignored. A copy that still matches what Polaris ships is Polaris'
   * own leftover and is removed; anything else is a local edit and is kept, with
   * a warning that it is the file in effect.
   */
  void retire_shadowing_etc_asset(const fs::path &etc_target, const fs::path &packaged_source, std::string_view label) {
    std::error_code ec;
    if (!fs::exists(etc_target, ec)) {
      return;
    }

    if (file_handler::read_file(etc_target.c_str()) != file_handler::read_file(packaged_source.c_str())) {
      // Either a local edit worth keeping or a copy from an older Polaris. The
      // two are indistinguishable from content alone, and deleting somebody's
      // edit is worse than leaving a stale file, so this only reports it — with
      // the command to run, because the shadowed file is the packaged one.
      BOOST_LOG(warning) << "Linux host setup: ["sv << etc_target << "] differs from the "sv << label
                         << " this Polaris ships and overrides it. If you did not edit it yourself it is "sv
                         << "a copy from an older install; remove it with [sudo rm "sv << etc_target
                         << "] so the packaged file applies."sv;
      return;
    }

    if (fs::remove(etc_target, ec) && !ec) {
      BOOST_LOG(info) << "Linux host setup: removed the superseded "sv << label << " copy at ["sv << etc_target
                      << "]; the packaged file applies from now on"sv;
      return;
    }

    BOOST_LOG(warning) << "Linux host setup: could not remove the superseded "sv << label << " copy at ["sv
                       << etc_target << "]: "sv << ec.message();
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

  void print_setup_host_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " --setup-host [--enable-kms]"sv << std::endl
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
      << std::endl
      << "  Options:"sv << std::endl
      << "    --enable-kms  Also run setcap cap_sys_admin+ep on the Polaris binary"sv << std::endl
      << "    help          Print this help"sv << std::endl;
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

      BOOST_LOG(error) << "Unknown --setup-host option: "sv << arg;
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
    const bool input_nodes_ready = access("/dev/uinput", R_OK | W_OK) == 0 &&
                                   access("/dev/uhid", R_OK | W_OK) == 0;
    // Membership is not something host setup can arrange — usermod would have to
    // pick a target account, and this command deliberately never guesses which
    // account streams. It reports it so a user preparing the host learns it here
    // rather than from a controller that silently never appears.
    const auto input_group_advice = platf::input_access::setup_host_input_group_advice();

    if (!enable_kms && udev_from_package && modules_from_package && etc_copies_absent && input_nodes_ready) {
      std::cout
        << "Linux host setup: nothing to do."sv << std::endl
        << "The package provides the udev rules and modules-load configuration, and /dev/uinput"sv << std::endl
        << "and /dev/uhid are already usable by this account. Re-run with --enable-kms only if you"sv << std::endl
        << "need DRM/KMS capture."sv << std::endl;
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
      std::cout << std::endl;
      return 1;
    }

    bool ok = true;
    if (udev_from_package) {
      retire_shadowing_etc_asset("/etc/udev/rules.d/60-polaris.rules", udev_source, "udev rules");
    } else {
      ok &= install_host_asset(udev_source, "/etc/udev/rules.d/60-polaris.rules", "udev rules");
    }
    if (modules_from_package) {
      retire_shadowing_etc_asset("/etc/modules-load.d/60-polaris.conf", modules_source, "modules-load config");
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
    std::cout
      << "Existing virtual gamepad nodes keep their previous access policy until recreated; stop active streams and restart Polaris after changing client gamepad seat isolation."sv << std::endl
      << "Start Polaris directly with `polaris`, or opt into background autostart with `systemctl --user enable --now polaris`."sv << std::endl;
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
  }  // namespace

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

    // Raise SIGINT to start termination
    std::raise(SIGINT);

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
