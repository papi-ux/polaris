/**
 * @file src/host_setup_facts.h
 * @brief What the first-run setup says about this host's GPUs, encoders and networks.
 *
 * The routes collect facts from sysfs, VA-API and getifaddrs; everything here is pure so the
 * advice a new user reads can be tested without the hardware it describes.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <format>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace host_setup {

  /**
   * @brief The NVENC API version the pinned prepared FFmpeg is built against.
   * @details Follows POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION in
   *          cmake/dependencies/prepared_ffmpeg.cmake; a test pins the two together.
   */
  inline constexpr std::string_view nvenc_api_version = "13.0";

  /**
   * @brief The oldest Linux NVIDIA driver that NVENC API version runs on.
   * @details libavcodec refuses any driver older than the API it was built against, so this is
   *          the driver floor for NVENC on every Polaris package.
   */
  inline constexpr std::string_view nvenc_min_linux_driver = "570";

  /**
   * @brief RPM Fusion's free repository package for the running Fedora release.
   */
  inline constexpr std::string_view rpmfusion_free_release =
    "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm";

  /**
   * @brief What a render node's VA-API driver can encode.
   */
  struct vaapi_facts_t {
    bool driver_loaded = false;  ///< A VA driver loaded and initialized for the node
    std::string driver_vendor;  ///< vaQueryVendorString, which names the driver and its version
    bool h264 = false;  ///< H.264 Main encode, which Polaris requires before it uses VA-API
    bool hevc = false;  ///< HEVC Main encode
    bool av1 = false;  ///< AV1 Profile 0 encode
  };

  /**
   * @brief One GPU as the setup step describes it.
   */
  struct gpu_facts_t {
    std::string render_node;  ///< /dev/dri/renderD* path
    std::string driver;  ///< Kernel driver: amdgpu, i915, xe, nvidia, nouveau
    std::string pci_vendor;  ///< sysfs vendor id, e.g. 0x1002
    std::string model;  ///< Marketing or PCI database name; empty when unknown
    std::string driver_version;  ///< NVIDIA kernel module version; empty elsewhere
    std::optional<vaapi_facts_t> vaapi;  ///< Present when the node was probed
  };

  /**
   * @brief Everything the GPU step's report is built from.
   */
  struct hardware_inputs_t {
    std::vector<gpu_facts_t> gpus;
    std::string selected_render_node;  ///< The node the encoder runs on
    std::string distro_id;  ///< os-release ID
    bool image_based_os = false;  ///< /run/ostree-booted exists
    bool build_cuda = false;
    bool build_vaapi = false;
    std::string configured_encoder;  ///< encoder from the configuration; empty means Automatic
    std::string policy;  ///< The Automatic policy, or explicit
    std::string planned_encoder;  ///< What the policy prefers before any stream
    std::string active_encoder;  ///< What the last encoder probe chose; empty before the first stream
  };

  namespace detail {
    inline std::string lower(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (const char character : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
      }
      return out;
    }

    inline std::string_view trim(std::string_view text) {
      const auto not_space = [](char character) {
        return !std::isspace(static_cast<unsigned char>(character));
      };
      const auto begin = std::find_if(text.begin(), text.end(), not_space);
      const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
      return begin < end ? std::string_view {begin, end} : std::string_view {};
    }

    inline std::string pci_id_digits(std::string_view id) {
      id = trim(id);
      if (id.size() > 2 && id[0] == '0' && (id[1] == 'x' || id[1] == 'X')) {
        id.remove_prefix(2);
      }
      return lower(id);
    }
  }  // namespace detail

  /**
   * @brief The GPU vendor from its PCI vendor id, or from the kernel driver when sysfs has no id.
   * @return amd, intel, nvidia or unknown.
   */
  inline std::string gpu_vendor(std::string_view pci_vendor, std::string_view driver) {
    const auto id = detail::pci_id_digits(pci_vendor);
    if (id == "1002" || driver == "amdgpu" || driver == "radeon") {
      return "amd";
    }
    if (id == "10de" || driver == "nvidia" || driver == "nouveau") {
      return "nvidia";
    }
    if (id == "8086" || driver == "i915" || driver == "xe") {
      return "intel";
    }
    return "unknown";
  }

  /**
   * @brief Look a device up in a pci.ids database.
   * @param ids The database, read from the start.
   * @param vendor Vendor id as sysfs prints it (0x1002) or bare (1002).
   * @param device Device id in the same forms.
   * @return The device name, or empty when either id is absent.
   */
  inline std::string pci_ids_device_name(std::istream &ids, std::string_view vendor, std::string_view device) {
    const auto vendor_id = detail::pci_id_digits(vendor);
    const auto device_id = detail::pci_id_digits(device);
    if (vendor_id.size() != 4 || device_id.size() != 4) {
      return {};
    }

    std::string line;
    bool in_vendor = false;
    while (std::getline(ids, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      if (line.front() != '\t') {
        if (in_vendor) {
          return {};
        }
        in_vendor = line.size() > 5 && line[4] == ' ' && detail::lower(std::string_view {line}.substr(0, 4)) == vendor_id;
        continue;
      }
      // One tab is a device; two tabs are a subsystem of the device above.
      if (!in_vendor || line.size() < 7 || line[1] == '\t' || line[5] != ' ') {
        continue;
      }
      if (detail::lower(std::string_view {line}.substr(1, 4)) == device_id) {
        return std::string {detail::trim(std::string_view {line}.substr(6))};
      }
    }
    return {};
  }

  /**
   * @brief The model line of /proc/driver/nvidia/gpus/<slot>/information.
   */
  inline std::string nvidia_information_model(std::string_view information) {
    while (!information.empty()) {
      const auto end = information.find('\n');
      const auto line = information.substr(0, end);
      if (line.starts_with("Model:")) {
        return std::string {detail::trim(line.substr(6))};
      }
      if (end == std::string_view::npos) {
        break;
      }
      information.remove_prefix(end + 1);
    }
    return {};
  }

  /**
   * @brief A driver version shaped like 580.82.09, or nothing.
   */
  inline std::string parse_driver_version(std::string_view reported) {
    const auto version = detail::trim(reported);
    if (version.empty() || version.size() > 24 || version.front() == '.' || version.back() == '.') {
      return {};
    }
    for (std::size_t index = 0; index < version.size(); ++index) {
      const char character = version[index];
      if (character == '.') {
        if (version[index - 1] == '.') {
          return {};
        }
      } else if (character < '0' || character > '9') {
        return {};
      }
    }
    return std::string {version};
  }

  /**
   * @brief Whether a driver version reaches a floor, compared part by part.
   * @return Nothing when either version is unreadable.
   */
  inline std::optional<bool> driver_at_least(std::string_view version, std::string_view floor) {
    const auto parts = [](std::string_view text) -> std::optional<std::vector<unsigned long>> {
      if (parse_driver_version(text).empty()) {
        return std::nullopt;
      }
      std::vector<unsigned long> out;
      unsigned long value = 0;
      for (const char character : detail::trim(text)) {
        if (character == '.') {
          out.push_back(value);
          value = 0;
        } else {
          value = value * 10 + static_cast<unsigned long>(character - '0');
        }
      }
      out.push_back(value);
      return out;
    };
    const auto have = parts(version);
    const auto need = parts(floor);
    if (!have || !need) {
      return std::nullopt;
    }
    return !std::lexicographical_compare(have->begin(), have->end(), need->begin(), need->end());
  }

  namespace detail {
    inline nlohmann::json advice(
      std::string_view code,
      std::string_view severity,
      std::string_view render_node = {},
      std::vector<std::string> commands = {},
      nlohmann::json params = nlohmann::json::object()
    ) {
      return {
        {"code", code},
        {"severity", severity},
        {"render_node", render_node},
        {"commands", std::move(commands)},
        {"params", std::move(params)},
      };
    }

    inline const gpu_facts_t *selected_gpu(const hardware_inputs_t &in) {
      const auto found = std::find_if(in.gpus.begin(), in.gpus.end(), [&](const gpu_facts_t &gpu) {
        return gpu.render_node == in.selected_render_node;
      });
      return found == in.gpus.end() ? nullptr : &*found;
    }

    inline bool nvenc_can_run(const gpu_facts_t &gpu) {
      return gpu.driver == "nvidia" && driver_at_least(gpu.driver_version, nvenc_min_linux_driver) != std::optional<bool> {false};
    }

    inline bool vaapi_can_run(const gpu_facts_t &gpu) {
      return gpu.vaapi && gpu.vaapi->driver_loaded && gpu.vaapi->h264;
    }
  }  // namespace detail

  /**
   * @brief The hardware encoders the facts say can start on this host, best first.
   * @details Vulkan Video and software encoding are left to Settings: Vulkan is experimental
   *          and cannot be checked without a live frame, and Automatic already falls back to
   *          software.
   */
  inline std::vector<std::string> working_encoders(const hardware_inputs_t &in) {
    std::vector<std::string> out;
    if (std::any_of(in.gpus.begin(), in.gpus.end(), detail::nvenc_can_run)) {
      out.emplace_back("nvenc");
    }
    if (in.build_vaapi && std::any_of(in.gpus.begin(), in.gpus.end(), detail::vaapi_can_run)) {
      out.emplace_back("vaapi");
    }
    return out;
  }

  /**
   * @brief The encoder a new user should expect streams to use.
   * @details The last probe's choice when there is one. Before the first stream, Automatic's
   *          preference when the facts say it can start, otherwise the next encoder that can.
   *          Vulkan Video cannot be checked here; its drivers ship with the same codec build
   *          options as VA-API, so it is expected only where VA-API can encode.
   */
  inline std::string expected_encoder(const hardware_inputs_t &in, const std::vector<std::string> &working) {
    if (!in.active_encoder.empty()) {
      return in.active_encoder;
    }
    if (!in.configured_encoder.empty()) {
      return in.configured_encoder;
    }
    const auto works = [&](std::string_view encoder) {
      return std::find(working.begin(), working.end(), encoder) != working.end();
    };
    if (in.planned_encoder == "vulkan") {
      return works("vaapi") ? "vulkan" : "software";
    }
    if (works(in.planned_encoder)) {
      return in.planned_encoder;
    }
    return working.empty() ? "software" : working.front();
  }

  /**
   * @brief What the new user should do about encoding on this host, most serious first.
   */
  inline nlohmann::json hardware_advice(const hardware_inputs_t &in, const std::vector<std::string> &working) {
    auto out = nlohmann::json::array();
    const auto *selected = detail::selected_gpu(in);
    const bool fedora = in.distro_id == "fedora";

    if (in.gpus.empty()) {
      out.push_back(detail::advice("no_gpu", "fail"));
    } else if (!selected) {
      out.push_back(detail::advice("encoder_gpu_not_found", "warning", in.selected_render_node));
    }

    if (selected) {
      const auto vendor = gpu_vendor(selected->pci_vendor, selected->driver);
      const auto &node = selected->render_node;
      if (selected->driver == "nouveau") {
        out.push_back(detail::advice("nouveau_cannot_encode", "fail", node));
      } else if (selected->driver == "nvidia") {
        if (driver_at_least(selected->driver_version, nvenc_min_linux_driver) == std::optional<bool> {false}) {
          out.push_back(detail::advice(
            "nvidia_driver_below_floor",
            "fail",
            node,
            {},
            {{"version", selected->driver_version}, {"floor", nvenc_min_linux_driver}}
          ));
        }
        if (!in.build_cuda) {
          out.push_back(detail::advice("nvidia_build_without_cuda", "warning", node));
        }
        if (!in.active_encoder.empty() && in.active_encoder != "nvenc" && in.configured_encoder.empty()) {
          out.push_back(detail::advice("nvenc_not_active", "warning", node, {}, {{"encoder", in.active_encoder}}));
        }
      } else if (vendor == "amd" || vendor == "intel") {
        if (!in.build_vaapi) {
          out.push_back(detail::advice("vaapi_not_built", "fail", node));
        } else if (selected->vaapi && !selected->vaapi->driver_loaded) {
          if (vendor == "amd" && fedora && in.image_based_os) {
            out.push_back(detail::advice(
              "amd_vaapi_driver_missing_fedora_atomic",
              "fail",
              node,
              {
                std::format("rpm-ostree install {}", rpmfusion_free_release),
                "systemctl reboot",
                "rpm-ostree install mesa-va-drivers-freeworld",
                "systemctl reboot",
              }
            ));
          } else if (vendor == "amd" && fedora) {
            out.push_back(detail::advice(
              "amd_vaapi_driver_missing_fedora",
              "fail",
              node,
              {
                std::format("sudo dnf install {}", rpmfusion_free_release),
                "sudo dnf install mesa-va-drivers-freeworld",
              }
            ));
          } else {
            out.push_back(detail::advice("vaapi_driver_missing", "fail", node, {}, {{"vendor", vendor}}));
          }
        } else if (selected->vaapi && !selected->vaapi->h264) {
          if (vendor == "amd" && fedora && in.image_based_os) {
            out.push_back(detail::advice(
              "amd_vaapi_encode_missing_fedora_atomic",
              "fail",
              node,
              {
                std::format("rpm-ostree install {}", rpmfusion_free_release),
                "systemctl reboot",
                "rpm-ostree override remove mesa-va-drivers --install mesa-va-drivers-freeworld",
                "systemctl reboot",
              }
            ));
          } else if (vendor == "amd" && fedora) {
            out.push_back(detail::advice(
              "amd_vaapi_encode_missing_fedora",
              "fail",
              node,
              {
                std::format("sudo dnf install {}", rpmfusion_free_release),
                "sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld",
              }
            ));
          } else if (vendor == "amd") {
            out.push_back(detail::advice("amd_vaapi_encode_missing", "fail", node));
          } else {
            out.push_back(detail::advice("intel_vaapi_encode_missing", "fail", node));
          }
          if (selected->vaapi->av1) {
            out.push_back(detail::advice("vaapi_av1_only", "info", node));
          }
        } else if (selected->vaapi && !selected->vaapi->hevc) {
          out.push_back(detail::advice("vaapi_hevc_missing", "info", node));
        }
      }
    }

    if (
      (in.configured_encoder == "nvenc" || in.configured_encoder == "vaapi") &&
      std::find(working.begin(), working.end(), in.configured_encoder) == working.end()
    ) {
      out.push_back(detail::advice("configured_encoder_cannot_run", "warning", {}, {}, {{"encoder", in.configured_encoder}}));
    }

    if (in.active_encoder.empty()) {
      out.push_back(detail::advice("encoder_confirmed_at_first_stream", "info"));
    }
    return out;
  }

  /**
   * @brief The whole GPU step report: each GPU, the encoder and why, the choices and the advice.
   */
  inline nlohmann::json hardware_report(const hardware_inputs_t &in) {
    const auto working = working_encoders(in);
    auto gpus = nlohmann::json::array();
    for (const auto &gpu : in.gpus) {
      nlohmann::json vaapi = nullptr;
      if (gpu.vaapi) {
        vaapi = {
          {"driver_loaded", gpu.vaapi->driver_loaded},
          {"driver_vendor", gpu.vaapi->driver_vendor},
          {"h264", gpu.vaapi->h264},
          {"hevc", gpu.vaapi->hevc},
          {"av1", gpu.vaapi->av1},
        };
      }
      gpus.push_back({
        {"render_node", gpu.render_node},
        {"vendor", gpu_vendor(gpu.pci_vendor, gpu.driver)},
        {"model", gpu.model},
        {"driver", gpu.driver},
        {"driver_version", gpu.driver_version},
        {"selected", gpu.render_node == in.selected_render_node},
        {"vaapi", std::move(vaapi)},
      });
    }

    return {
      {"gpus", std::move(gpus)},
      {"build", {{"cuda", in.build_cuda}, {"vaapi", in.build_vaapi}}},
      {"encoder",
       {
         {"configured", in.configured_encoder},
         {"policy", in.policy},
         {"planned", in.planned_encoder},
         {"active", in.active_encoder},
         {"expected", expected_encoder(in, working)},
       }},
      {"encoder_choices", working},
      {"nvenc_min_driver", nvenc_min_linux_driver},
      {"advice", hardware_advice(in, working)},
    };
  }

  /**
   * @brief One IPv4 address on one interface, with the facts that say whether it is a LAN.
   */
  struct interface_address_t {
    std::string name;
    std::uint32_t address = 0;  ///< Host byte order
    std::uint32_t netmask = 0;  ///< Host byte order
    bool up = false;
    bool loopback = false;
    bool point_to_point = false;  ///< VPN tunnels (WireGuard, tun) are point-to-point
    bool hardware_backed = false;  ///< Has a device, or is a bridge, bond or VLAN over one
  };

  /**
   * @brief Interface names that belong to containers, virtual machines and VPNs.
   * @details The sysfs device check already drops most of these; the names also cover a
   *          virtual interface whose lower link reaches real hardware, like a Docker macvlan.
   */
  inline bool virtual_interface_name(std::string_view name) {
    static constexpr std::array prefixes {
      std::string_view {"docker"},
      std::string_view {"br-"},
      std::string_view {"veth"},
      std::string_view {"virbr"},
      std::string_view {"vnet"},
      std::string_view {"podman"},
      std::string_view {"cni"},
      std::string_view {"flannel"},
      std::string_view {"cali"},
      std::string_view {"weave"},
      std::string_view {"lxcbr"},
      std::string_view {"lxdbr"},
      std::string_view {"incusbr"},
      std::string_view {"macvlan"},
      std::string_view {"macvtap"},
      std::string_view {"tun"},
      std::string_view {"tap"},
      std::string_view {"wg"},
      std::string_view {"tailscale"},
      std::string_view {"zt"},
      std::string_view {"vboxnet"},
      std::string_view {"vmnet"},
      std::string_view {"nordlynx"},
      std::string_view {"proton"},
      std::string_view {"mullvad"},
      std::string_view {"waydroid"},
      std::string_view {"kube"},
      std::string_view {"ppp"},
      std::string_view {"dummy"},
    };
    return std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view prefix) {
      return name.starts_with(prefix);
    });
  }

  /**
   * @brief Dotted IPv4 text for a host byte order address.
   */
  inline std::string ipv4_text(std::uint32_t address) {
    return std::format("{}.{}.{}.{}", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
  }

  /**
   * @brief The private LAN an interface address sits on, as a CIDR.
   * @return Nothing for loopback, down, VPN, container and virtual machine interfaces, and for
   *         anything outside the RFC 1918 private ranges, since trusting a network lets every
   *         device on it pair without a PIN.
   */
  inline std::optional<std::string> lan_cidr(const interface_address_t &entry) {
    if (!entry.up || entry.loopback || entry.point_to_point || !entry.hardware_backed || virtual_interface_name(entry.name)) {
      return std::nullopt;
    }
    // A netmask is a run of ones; anything else is not a network.
    if (entry.netmask != 0 && (~entry.netmask & (~entry.netmask + 1)) != 0) {
      return std::nullopt;
    }
    const int prefix = std::popcount(entry.netmask);
    if (prefix < 8 || prefix > 30) {
      return std::nullopt;
    }
    const std::uint32_t network = entry.address & entry.netmask;
    const bool private_range =
      ((network & 0xFF000000u) == 0x0A000000u && prefix >= 8) ||
      ((network & 0xFFF00000u) == 0xAC100000u && prefix >= 12) ||
      ((network & 0xFFFF0000u) == 0xC0A80000u && prefix >= 16);
    if (!private_range) {
      return std::nullopt;
    }
    return std::format("{}/{}", ipv4_text(network), prefix);
  }

  /**
   * @brief The LAN networks a user can trust with one click, one entry per CIDR.
   */
  inline nlohmann::json lan_networks(const std::vector<interface_address_t> &addresses) {
    auto out = nlohmann::json::array();
    std::vector<std::string> cidrs;
    for (const auto &entry : addresses) {
      const auto cidr = lan_cidr(entry);
      if (!cidr) {
        continue;
      }
      const auto existing = std::find(cidrs.begin(), cidrs.end(), *cidr);
      if (existing != cidrs.end()) {
        auto &interfaces = out.at(static_cast<std::size_t>(existing - cidrs.begin())).at("interfaces");
        const auto names = interfaces.get<std::vector<std::string>>();
        if (std::find(names.begin(), names.end(), entry.name) == names.end()) {
          interfaces.push_back(entry.name);
        }
        continue;
      }
      cidrs.push_back(*cidr);
      out.push_back({
        {"interface", entry.name},
        {"interfaces", nlohmann::json::array({entry.name})},
        {"cidr", *cidr},
        {"address", ipv4_text(entry.address)},
      });
    }
    return out;
  }

}  // namespace host_setup
