/**
 * @file src/platform/linux/private_session_input.cpp
 * @brief Definitions for private labwc session input configuration.
 */
#include "private_session_input.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <system_error>

#include "src/logging.h"

using namespace std::literals;

namespace platf::private_session_input {

  namespace fs = std::filesystem;

  namespace {
    /**
     * Names Polaris gives the virtual devices it creates for clients. inputtino
     * appends " (absolute)" to the second mouse node, so prefix matching is used
     * rather than equality.
     */
    constexpr std::array virtual_device_prefixes {
      "Polaris Mouse passthrough"sv,
      "Polaris Keyboard passthrough"sv,
      "Touch passthrough"sv,
      "Pen passthrough"sv,
      "Polaris X-Box One (virtual) pad"sv,
      "Polaris Nintendo (virtual) pad"sv,
      "Polaris PS5 (virtual) pad"sv,
      "Polaris gamepad (virtual) motion sensors"sv,
      "Sunshine X-Box One (virtual) pad"sv,
      "Sunshine Nintendo (virtual) pad"sv,
      "Sunshine PS5 (virtual) pad"sv,
      "Sunshine gamepad (virtual) motion sensors"sv,
    };

    std::string xml_escape(std::string_view value) {
      std::string escaped;
      escaped.reserve(value.size());
      for (const char c : value) {
        switch (c) {
          case '&':
            escaped += "&amp;";
            break;
          case '<':
            escaped += "&lt;";
            break;
          case '>':
            escaped += "&gt;";
            break;
          case '"':
            escaped += "&quot;";
            break;
          case '\'':
            escaped += "&apos;";
            break;
          default:
            escaped += c;
            break;
        }
      }
      return escaped;
    }

    std::string read_first_line(const fs::path &path) {
      std::ifstream input {path};
      std::string line;
      std::getline(input, line);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }
      return line;
    }
  }  // namespace

  bool is_polaris_virtual_device(std::string_view name) {
    return std::any_of(
      virtual_device_prefixes.begin(),
      virtual_device_prefixes.end(),
      [name](std::string_view prefix) {
        return name.starts_with(prefix);
      }
    );
  }

  std::vector<input_device_t> enumerate_host_input_devices() {
    std::vector<input_device_t> devices;

    std::error_code ec;
    fs::directory_iterator entries {"/sys/class/input", ec};
    if (ec) {
      BOOST_LOG(debug) << "private_session_input: cannot read /sys/class/input: "sv << ec.message();
      return devices;
    }

    for (const auto &entry : entries) {
      const auto node = entry.path().filename().string();
      if (!node.starts_with("event")) {
        continue;
      }

      const auto name_path = entry.path() / "device" / "name";
      if (!fs::exists(name_path, ec)) {
        continue;
      }

      auto name = read_first_line(name_path);
      if (name.empty()) {
        continue;
      }

      devices.push_back({std::move(name), entry.path().string()});
    }

    std::sort(devices.begin(), devices.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.name < rhs.name;
    });

    return devices;
  }

  std::string build_libinput_isolation_block(const std::vector<input_device_t> &devices) {
    std::vector<std::string> ignored;
    for (const auto &device : devices) {
      if (is_polaris_virtual_device(device.name)) {
        continue;
      }
      if (std::find(ignored.begin(), ignored.end(), device.name) == ignored.end()) {
        ignored.push_back(device.name);
      }
    }

    if (ignored.empty()) {
      return {};
    }

    std::ostringstream block;
    block << "  <!-- Physical devices on the host. The private session ignores them so the\n"
             "       keyboard and mouse on the desk keep driving the host desktop only. The\n"
             "       virtual devices Polaris creates for the client are deliberately absent\n"
             "       from this list. -->\n";
    block << "  <libinput>\n";
    for (const auto &name : ignored) {
      block << "    <device category=\"" << xml_escape(name) << "\">\n"
            << "      <sendEventsMode>no</sendEventsMode>\n"
            << "    </device>\n";
    }
    block << "  </libinput>\n";

    return block.str();
  }

  std::string build_rc_xml(const std::vector<input_device_t> &devices) {
    std::ostringstream rc;
    rc << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << generated_marker << "\n"
       << "     Put your own rc.xml here to take over: a file without the line above is\n"
       << "     left alone, and Polaris then stops managing input isolation for this\n"
       << "     session. -->\n"
       << "<labwc_config>\n"
       << "  <core>\n"
       << "    <xwaylandPersistence>yes</xwaylandPersistence>\n"
       << "  </core>\n"
       << "  <focus>\n"
       << "    <followMouse>no</followMouse>\n"
       << "    <raiseOnFocus>no</raiseOnFocus>\n"
       << "  </focus>\n"
       << "\n"
       << "  <!-- Kiosk presentation: no decorations, nothing in a taskbar. -->\n"
       << "  <windowRules>\n"
       << "    <windowRule identifier=\"*\">\n"
       << "      <serverDecoration>no</serverDecoration>\n"
       << "      <skipTaskbar>yes</skipTaskbar>\n"
       << "    </windowRule>\n"
       << "  </windowRules>\n"
       << "\n"
       << "  <theme>\n"
       << "    <cornerRadius>0</cornerRadius>\n"
       << "  </theme>\n"
       << "\n"
       << "  <keyboard>\n"
       << "    <default />\n"
       << "  </keyboard>\n";

    const auto libinput_block = build_libinput_isolation_block(devices);
    if (!libinput_block.empty()) {
      rc << "\n" << libinput_block;
    }

    rc << "</labwc_config>\n";
    return rc.str();
  }

  bool ensure_generated_rc_xml(
    const fs::path &config_dir,
    const std::vector<input_device_t> &devices,
    std::string &status_out
  ) {
    const auto rc_path = config_dir / "rc.xml";

    const auto contents = build_rc_xml(devices);

    std::error_code ec;
    if (fs::exists(rc_path, ec)) {
      std::ifstream existing {rc_path};
      std::ostringstream buffer;
      buffer << existing.rdbuf();
      const auto current = buffer.str();
      if (current.find(generated_marker) == std::string::npos) {
        status_out = "keeping " + rc_path.string() + "; it was not generated by Polaris, so this "
                     "session's input isolation is whatever that file says. Delete it to hand the "
                     "file back to Polaris";
        return false;
      }
      if (current == contents) {
        // Same hardware as last time. Leaving the file untouched keeps its
        // mtime stable for anyone watching the config directory.
        status_out = rc_path.string() + " is already current";
        return true;
      }
    }

    fs::create_directories(config_dir, ec);
    if (ec) {
      status_out = "could not create " + config_dir.string() + ": " + ec.message();
      return false;
    }

    // Write through a temporary file: labwc reads rc.xml as it starts, and a
    // half-written config makes it fall back to defaults with no input rules.
    const auto tmp_path = rc_path.string() + ".tmp";
    {
      std::ofstream out {tmp_path, std::ios::trunc};
      if (!out) {
        status_out = "could not write " + tmp_path;
        return false;
      }
      out << contents;
      if (!out) {
        status_out = "could not write " + tmp_path;
        return false;
      }
    }

    fs::rename(tmp_path, rc_path, ec);
    if (ec) {
      fs::remove(tmp_path);
      status_out = "could not install " + rc_path.string() + ": " + ec.message();
      return false;
    }

    const auto ignored = std::count_if(devices.begin(), devices.end(), [](const auto &device) {
      return !is_polaris_virtual_device(device.name);
    });
    status_out = "generated " + rc_path.string() + " ignoring " + std::to_string(ignored) + " host input device(s)";
    return true;
  }

}  // namespace platf::private_session_input
