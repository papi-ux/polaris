/**
 * @file src/confighttp_setup_reports.cpp
 * @brief The first-run setup reports: the host's GPUs, encoder and private networks.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "host_setup_facts.h"
#include "stream_stats.h"
#include "update_status.h"
#include "video.h"

#ifdef __linux__
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>

  #include "platform/linux/misc.h"
  #ifdef POLARIS_BUILD_VAAPI
    #include "platform/linux/vaapi.h"
  #endif
#endif

namespace confighttp {
  namespace fs = std::filesystem;

#ifdef __linux__
  namespace {
  namespace setup_facts {
    std::string read_text(const fs::path &path, std::size_t max_bytes = 64 * 1024) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return {};
      }
      std::string text(max_bytes, '\0');
      in.read(text.data(), static_cast<std::streamsize>(max_bytes));
      text.resize(static_cast<std::size_t>(in.gcount()));
      return text;
    }

    std::string first_line(const fs::path &path) {
      auto text = read_text(path, 256);
      text = text.substr(0, text.find('\n'));
      boost::algorithm::trim(text);
      return text;
    }

    std::string pci_ids_model(std::string_view vendor, std::string_view device) {
      for (const auto *path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"}) {
        std::ifstream ids(path);
        if (ids) {
          return host_setup::pci_ids_device_name(ids, vendor, device);
        }
      }
      return {};
    }

    host_setup::gpu_facts_t gpu_facts(const platf::render_device_candidate_t &candidate) {
      host_setup::gpu_facts_t gpu;
      gpu.render_node = candidate.path;
      gpu.driver = candidate.driver;
      const auto device_dir = fs::path("/sys/class/drm") / fs::path(candidate.path).filename() / "device";
      gpu.pci_vendor = first_line(device_dir / "vendor");
      if (gpu.driver == "nvidia") {
        gpu.driver_version = host_setup::parse_driver_version(first_line("/sys/module/nvidia/version"));
        std::error_code ec;
        const auto slot = fs::canonical(device_dir, ec).filename().string();
        // A PCI slot reads 0000:01:00.0; anything else would not name a directory here.
        if (!ec && !slot.empty() && slot.find_first_not_of("0123456789abcdefABCDEF:.") == std::string::npos) {
          gpu.model = host_setup::nvidia_information_model(read_text(fs::path("/proc/driver/nvidia/gpus") / slot / "information"));
        }
      }
      if (gpu.model.empty()) {
        gpu.model = pci_ids_model(gpu.pci_vendor, first_line(device_dir / "device"));
      }
  #ifdef POLARIS_BUILD_VAAPI
      // NVIDIA nodes have no VA encode driver; loading the decode-only one would say nothing.
      if (gpu.driver != "nvidia" && gpu.driver != "nouveau") {
        const auto support = va::encode_support(candidate.path);
        gpu.vaapi = host_setup::vaapi_facts_t {
          .driver_loaded = support.driver_loaded,
          .driver_vendor = support.driver_vendor,
          .h264 = support.h264,
          .hevc = support.hevc,
          .av1 = support.av1,
        };
      }
  #endif
      return gpu;
    }

    /**
     * @brief True for a real network card, or a bridge, bond or VLAN over one.
     */
    bool interface_hardware_backed(const std::string &name, int depth = 0) {
      if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
        return false;
      }
      const auto base = fs::path("/sys/class/net") / name;
      std::error_code ec;
      if (fs::exists(base / "device", ec)) {
        return true;
      }
      if (depth >= 2) {
        return false;
      }
      try {
        for (const auto &entry : fs::directory_iterator(base, ec)) {
          const auto file = entry.path().filename().string();
          if (file.rfind("lower_", 0) == 0 && interface_hardware_backed(file.substr(6), depth + 1)) {
            return true;
          }
        }
      } catch (const std::exception &) {
        return false;
      }
      return false;
    }
  }  // namespace setup_facts
  }  // namespace
#endif

  nlohmann::json setup_hardware_report() {
    host_setup::hardware_inputs_t inputs;
    inputs.build_cuda = stream_stats::build_has_cuda();
#ifdef POLARIS_BUILD_VAAPI
    inputs.build_vaapi = true;
#endif
    inputs.configured_encoder = config::video.encoder;
    const auto selection = video::active_encoder_selection_info();
    inputs.policy = selection.policy;
    inputs.planned_encoder = selection.preferred_encoder;
    inputs.active_encoder = video::active_encoder_name();
#ifdef __linux__
    inputs.selected_render_node = platf::effective_encoder_render_device();
    inputs.distro_id = update_status::detect_host_distro().id;
    std::error_code ec;
    inputs.image_based_os = fs::exists("/run/ostree-booted", ec);
    for (const auto &candidate : platf::render_devices()) {
      inputs.gpus.push_back(setup_facts::gpu_facts(candidate));
    }
#endif
    auto report = host_setup::hardware_report(inputs);
    report["status"] = true;
    report["platform"] = POLARIS_PLATFORM;
    return report;
  }

  nlohmann::json setup_networks_report() {
    nlohmann::json report {
      {"status", true},
      {"platform", POLARIS_PLATFORM},
      {"supported", false},
      {"networks", nlohmann::json::array()},
    };
#ifdef __linux__
    std::vector<host_setup::interface_address_t> addresses;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
      for (auto *entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_netmask || !entry->ifa_name) {
          continue;
        }
        host_setup::interface_address_t address;
        address.name = entry->ifa_name;
        address.address = ntohl(((sockaddr_in *) entry->ifa_addr)->sin_addr.s_addr);
        address.netmask = ntohl(((sockaddr_in *) entry->ifa_netmask)->sin_addr.s_addr);
        address.up = (entry->ifa_flags & IFF_UP) != 0;
        address.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        address.point_to_point = (entry->ifa_flags & IFF_POINTOPOINT) != 0;
        address.hardware_backed = setup_facts::interface_hardware_backed(address.name);
        addresses.push_back(std::move(address));
      }
      freeifaddrs(list);
      report["supported"] = true;
    }
    report["networks"] = host_setup::lan_networks(addresses);
#endif
    return report;
  }
}  // namespace confighttp
