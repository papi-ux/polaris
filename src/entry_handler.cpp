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
      << "    - install Polaris udev rules into /etc/udev/rules.d"sv << std::endl
      << "    - install Polaris modules-load config into /etc/modules-load.d"sv << std::endl
      << "    - reload udev and trigger /dev/uinput and /dev/uhid"sv << std::endl
      << "    - load uinput and uhid now via modprobe"sv << std::endl
      << "    - optionally apply cap_sys_admin for DRM/KMS capture"sv << std::endl
      << std::endl
      << "  Options:"sv << std::endl
      << "    --enable-kms  Also run setcap cap_sys_admin+ep on the Polaris binary"sv << std::endl
      << "    help          Print this help"sv << std::endl;
  }
}  // namespace
#endif

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
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

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

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

    if (geteuid() != 0) {
      std::cout
        << "Polaris host setup requires root because it writes /etc and reloads udev."sv << std::endl
        << "Run:"sv << std::endl
        << "  sudo "sv << exe_path->string() << " --setup-host"sv;
      if (enable_kms) {
        std::cout << " --enable-kms"sv;
      }
      std::cout << std::endl;
      return 1;
    }

    bool ok = true;
    ok &= install_host_asset(udev_source, "/etc/udev/rules.d/60-polaris.rules", "udev rules");
    ok &= install_host_asset(modules_source, "/etc/modules-load.d/60-polaris.conf", "modules-load config");
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
      << "Linux host setup complete."sv << std::endl
      << "Start Polaris directly with `polaris`, or opt into background autostart with `systemctl --user enable --now polaris`."sv << std::endl;
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

  void exit_sunshine(int exit_code, bool async) {
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
