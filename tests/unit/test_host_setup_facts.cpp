/**
 * @file tests/unit/test_host_setup_facts.cpp
 * @brief What the first-run GPU and network steps say about a host, from fixed facts.
 */
#include <src/host_setup_facts.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
  using host_setup::gpu_facts_t;
  using host_setup::hardware_inputs_t;
  using host_setup::interface_address_t;
  using host_setup::vaapi_facts_t;

  gpu_facts_t nvidia_card(std::string driver_version = "580.82.09") {
    return {
      .render_node = "/dev/dri/renderD128",
      .driver = "nvidia",
      .pci_vendor = "0x10de",
      .model = "NVIDIA GeForce RTX 4090",
      .driver_version = std::move(driver_version),
    };
  }

  gpu_facts_t amd_card(vaapi_facts_t vaapi, std::string node = "/dev/dri/renderD128") {
    return {
      .render_node = std::move(node),
      .driver = "amdgpu",
      .pci_vendor = "0x1002",
      .model = "Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]",
      .vaapi = std::move(vaapi),
    };
  }

  // Fedora's own Mesa is built without the patented codecs: the driver loads, AV1 encodes, and
  // H.264 and HEVC do not.
  vaapi_facts_t fedora_stock_mesa() {
    return {
      .driver_loaded = true,
      .driver_vendor = "Mesa Gallium driver 25.1.9 for AMD Radeon RX 7900 XTX (radeonsi, navi31)",
      .av1 = true,
    };
  }

  vaapi_facts_t full_mesa() {
    return {
      .driver_loaded = true,
      .driver_vendor = "Mesa Gallium driver 25.1.9 for AMD Radeon RX 7900 XTX (radeonsi, navi31)",
      .h264 = true,
      .hevc = true,
      .av1 = true,
    };
  }

  hardware_inputs_t host_with(std::vector<gpu_facts_t> gpus, std::string selected = "/dev/dri/renderD128") {
    hardware_inputs_t in;
    in.gpus = std::move(gpus);
    in.selected_render_node = std::move(selected);
    in.distro_id = "fedora";
    in.build_cuda = true;
    in.build_vaapi = true;
    return in;
  }

  std::vector<std::string> codes(const nlohmann::json &report) {
    std::vector<std::string> out;
    for (const auto &entry : report.at("advice")) {
      out.push_back(entry.at("code").get<std::string>());
    }
    return out;
  }

  bool has_code(const nlohmann::json &report, std::string_view code) {
    const auto found = codes(report);
    return std::find(found.begin(), found.end(), code) != found.end();
  }

  const nlohmann::json &advice(const nlohmann::json &report, std::string_view code) {
    for (const auto &entry : report.at("advice")) {
      if (entry.at("code").get<std::string>() == code) {
        return entry;
      }
    }
    static const nlohmann::json missing;
    ADD_FAILURE() << "no advice " << code << " in " << report.at("advice").dump();
    return missing;
  }

  interface_address_t lan(std::string name, std::string_view address, int prefix) {
    std::uint32_t value = 0;
    std::istringstream in {std::string {address}};
    for (int part = 0; part < 4; ++part) {
      unsigned octet = 0;
      in >> octet;
      in.ignore(1);
      value = (value << 8) | octet;
    }
    return {
      .name = std::move(name),
      .address = value,
      .netmask = prefix == 0 ? 0u : static_cast<std::uint32_t>(0xFFFFFFFFull << (32 - prefix)),
      .up = true,
      .hardware_backed = true,
    };
  }
}  // namespace

TEST(HostSetupFactsTests, NamesTheVendorFromThePciIdOrTheDriver) {
  EXPECT_EQ(host_setup::gpu_vendor("0x1002", ""), "amd");
  EXPECT_EQ(host_setup::gpu_vendor("0x10DE", ""), "nvidia");
  EXPECT_EQ(host_setup::gpu_vendor("0x8086", ""), "intel");
  EXPECT_EQ(host_setup::gpu_vendor("", "xe"), "intel");
  EXPECT_EQ(host_setup::gpu_vendor("", "nouveau"), "nvidia");
  EXPECT_EQ(host_setup::gpu_vendor("0x1af4", "virtio_gpu"), "unknown");
}

TEST(HostSetupFactsTests, LooksDevicesUpInThePciDatabaseWithoutReadingSubsystems) {
  const std::string ids =
    "# comment\n"
    "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
    "\t73bf  Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]\n"
    "\t\t744c 0e3b  A subsystem that must never match\n"
    "\t744c  Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]\n"
    "10de  NVIDIA Corporation\n"
    "\t2684  AD102 [GeForce RTX 4090]\n"
    "C 03  Display controller\n";

  std::istringstream amd {ids};
  EXPECT_EQ(host_setup::pci_ids_device_name(amd, "0x1002", "0x744c"), "Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]");
  std::istringstream nvidia {ids};
  EXPECT_EQ(host_setup::pci_ids_device_name(nvidia, "10de", "2684"), "AD102 [GeForce RTX 4090]");
  // The device id exists under NVIDIA but not under AMD: the lookup stops at the next vendor.
  std::istringstream wrong_vendor {ids};
  EXPECT_EQ(host_setup::pci_ids_device_name(wrong_vendor, "0x1002", "0x2684"), "");
  std::istringstream malformed {ids};
  EXPECT_EQ(host_setup::pci_ids_device_name(malformed, "0x10", "0x2684"), "");
}

TEST(HostSetupFactsTests, ReadsTheModelFromTheNvidiaInformationFile) {
  EXPECT_EQ(
    host_setup::nvidia_information_model("Model: \t\t NVIDIA GeForce RTX 4090\nIRQ:   \t\t 180\nGPU UUID: \t GPU-1\n"),
    "NVIDIA GeForce RTX 4090"
  );
  EXPECT_EQ(host_setup::nvidia_information_model("IRQ: 180\n"), "");
}

TEST(HostSetupFactsTests, ComparesDriverVersionsPartByPart) {
  EXPECT_EQ(host_setup::parse_driver_version("580.82.09\n"), "580.82.09");
  EXPECT_EQ(host_setup::parse_driver_version("NVIDIA-SMI has failed"), "");
  EXPECT_EQ(host_setup::parse_driver_version("580..1"), "");
  EXPECT_EQ(host_setup::driver_at_least("580.82.09", "570"), std::optional<bool> {true});
  EXPECT_EQ(host_setup::driver_at_least("570", "570"), std::optional<bool> {true});
  EXPECT_EQ(host_setup::driver_at_least("565.77", "570"), std::optional<bool> {false});
  EXPECT_EQ(host_setup::driver_at_least("1000.1", "570"), std::optional<bool> {true});
  EXPECT_EQ(host_setup::driver_at_least("", "570"), std::nullopt);
}

TEST(HostSetupFactsTests, TheNvencDriverFloorFollowsThePreparedFfmpegCeiling) {
  // Raising the NVENC API version the packages ship raises the driver every host needs; the
  // setup step must say the new floor in the same change.
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "cmake/dependencies/prepared_ffmpeg.cmake");
  ASSERT_TRUE(in);
  const std::string cmake((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(
    cmake.find("set(POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION \"" + std::string {host_setup::nvenc_api_version} + "\""),
    std::string::npos
  );
  EXPECT_NE(
    cmake.find(std::string {host_setup::nvenc_api_version} + " needs driver " + std::string {host_setup::nvenc_min_linux_driver} + " or newer"),
    std::string::npos
  );
}

TEST(HostSetupFactsTests, AnNvidiaHostOnACudaBuildIsReadyForNvenc) {
  auto in = host_with({nvidia_card()});
  in.policy = "nvidia_nvenc";
  in.planned_encoder = "nvenc";
  const auto report = host_setup::hardware_report(in);

  EXPECT_EQ(report.at("encoder_choices"), nlohmann::json::array({"nvenc"}));
  EXPECT_EQ(report.at("encoder").at("expected"), "nvenc");
  EXPECT_EQ(report.at("nvenc_min_driver"), "570");
  EXPECT_EQ(codes(report), (std::vector<std::string> {"encoder_confirmed_at_first_stream"}));
  const auto &gpu = report.at("gpus").at(0);
  EXPECT_EQ(gpu.at("vendor"), "nvidia");
  EXPECT_EQ(gpu.at("model"), "NVIDIA GeForce RTX 4090");
  EXPECT_EQ(gpu.at("driver_version"), "580.82.09");
  EXPECT_TRUE(gpu.at("selected").get<bool>());
  EXPECT_TRUE(gpu.at("vaapi").is_null());
}

TEST(HostSetupFactsTests, AnNvidiaHostOnABuildWithoutCudaIsToldFramesCopyThroughMemory) {
  auto in = host_with({nvidia_card()});
  in.build_cuda = false;
  in.planned_encoder = "nvenc";
  const auto report = host_setup::hardware_report(in);

  EXPECT_EQ(advice(report, "nvidia_build_without_cuda").at("severity"), "warning");
  EXPECT_EQ(report.at("encoder_choices"), nlohmann::json::array({"nvenc"}));
}

TEST(HostSetupFactsTests, AnNvidiaDriverBelowTheFloorCannotRunNvenc) {
  auto in = host_with({nvidia_card("550.144.03")});
  in.planned_encoder = "nvenc";
  const auto report = host_setup::hardware_report(in);

  const auto &below = advice(report, "nvidia_driver_below_floor");
  EXPECT_EQ(below.at("severity"), "fail");
  EXPECT_EQ(below.at("params").at("version"), "550.144.03");
  EXPECT_EQ(below.at("params").at("floor"), "570");
  EXPECT_TRUE(report.at("encoder_choices").empty());
  EXPECT_EQ(report.at("encoder").at("expected"), "software");
}

TEST(HostSetupFactsTests, AnAmdCardOnStockFedoraMesaGetsTheRpmFusionSwap) {
  auto in = host_with({amd_card(fedora_stock_mesa())});
  in.policy = "amd_established_desktop";
  in.planned_encoder = "vaapi";
  const auto report = host_setup::hardware_report(in);

  const auto &missing = advice(report, "amd_vaapi_encode_missing_fedora");
  EXPECT_EQ(missing.at("severity"), "fail");
  EXPECT_EQ(missing.at("render_node"), "/dev/dri/renderD128");
  EXPECT_EQ(
    missing.at("commands"),
    nlohmann::json::array({
      "sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm",
      "sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld",
    })
  );
  EXPECT_EQ(advice(report, "vaapi_av1_only").at("severity"), "info");
  EXPECT_TRUE(report.at("encoder_choices").empty());
  EXPECT_EQ(report.at("encoder").at("expected"), "software");
  const auto &vaapi = report.at("gpus").at(0).at("vaapi");
  EXPECT_TRUE(vaapi.at("driver_loaded").get<bool>());
  EXPECT_FALSE(vaapi.at("h264").get<bool>());
  EXPECT_TRUE(vaapi.at("av1").get<bool>());
}

TEST(HostSetupFactsTests, AnImageBasedFedoraLayersTheDriverWithRpmOstree) {
  auto in = host_with({amd_card(fedora_stock_mesa())});
  in.image_based_os = true;
  const auto report = host_setup::hardware_report(in);

  const auto &missing = advice(report, "amd_vaapi_encode_missing_fedora_atomic");
  const auto commands = missing.at("commands").get<std::vector<std::string>>();
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_EQ(commands[0].rfind("rpm-ostree install https://mirrors.rpmfusion.org/free/fedora/", 0), 0u);
  EXPECT_EQ(commands[2], "rpm-ostree override remove mesa-va-drivers --install mesa-va-drivers-freeworld");
  EXPECT_EQ(std::count(commands.begin(), commands.end(), "systemctl reboot"), 2);
  EXPECT_FALSE(has_code(report, "amd_vaapi_encode_missing_fedora"));
}

TEST(HostSetupFactsTests, OtherDistributionsGetTheFindingWithoutAFedoraCommand) {
  auto in = host_with({amd_card(fedora_stock_mesa())});
  in.distro_id = "cachyos";
  const auto report = host_setup::hardware_report(in);

  EXPECT_TRUE(advice(report, "amd_vaapi_encode_missing").at("commands").empty());
  // A Fedora derivative is not Fedora: its repositories and packages differ.
  in.distro_id = "nobara";
  EXPECT_TRUE(advice(host_setup::hardware_report(in), "amd_vaapi_encode_missing").at("commands").empty());
}

TEST(HostSetupFactsTests, AMissingVaDriverIsNamedApartFromMissingCodecs) {
  auto in = host_with({amd_card(vaapi_facts_t {})});
  const auto fedora = host_setup::hardware_report(in);
  EXPECT_EQ(
    advice(fedora, "amd_vaapi_driver_missing_fedora").at("commands").back(),
    "sudo dnf install mesa-va-drivers-freeworld"
  );

  in.distro_id = "arch";
  const auto arch = host_setup::hardware_report(in);
  EXPECT_EQ(advice(arch, "vaapi_driver_missing").at("params").at("vendor"), "amd");
}

TEST(HostSetupFactsTests, IntelWithoutH264EncodeGetsItsOwnFinding) {
  auto intel = gpu_facts_t {
    .render_node = "/dev/dri/renderD128",
    .driver = "i915",
    .pci_vendor = "0x8086",
    .vaapi = vaapi_facts_t {.driver_loaded = true, .driver_vendor = "Intel iHD driver"},
  };
  const auto report = host_setup::hardware_report(host_with({intel}));
  EXPECT_EQ(advice(report, "intel_vaapi_encode_missing").at("severity"), "fail");
  EXPECT_FALSE(has_code(report, "vaapi_av1_only"));
}

TEST(HostSetupFactsTests, AnAmdCardWithFullMesaOffersVaapi) {
  auto in = host_with({amd_card(full_mesa())});
  in.planned_encoder = "vaapi";
  auto report = host_setup::hardware_report(in);
  EXPECT_EQ(report.at("encoder_choices"), nlohmann::json::array({"vaapi"}));
  EXPECT_EQ(report.at("encoder").at("expected"), "vaapi");
  EXPECT_EQ(codes(report), (std::vector<std::string> {"encoder_confirmed_at_first_stream"}));

  in.planned_encoder = "vulkan";
  EXPECT_EQ(host_setup::hardware_report(in).at("encoder").at("expected"), "vulkan");

  auto no_hevc = full_mesa();
  no_hevc.hevc = false;
  in.gpus = {amd_card(no_hevc)};
  EXPECT_EQ(advice(host_setup::hardware_report(in), "vaapi_hevc_missing").at("severity"), "info");

  in.build_vaapi = false;
  report = host_setup::hardware_report(in);
  EXPECT_EQ(advice(report, "vaapi_not_built").at("severity"), "fail");
  EXPECT_TRUE(report.at("encoder_choices").empty());
}

TEST(HostSetupFactsTests, AnIntegratedGpuWithoutCodecsDoesNotWarnWhenNvidiaEncodes) {
  auto in = host_with({amd_card(fedora_stock_mesa(), "/dev/dri/renderD129"), nvidia_card()});
  in.planned_encoder = "nvenc";
  const auto report = host_setup::hardware_report(in);

  EXPECT_EQ(codes(report), (std::vector<std::string> {"encoder_confirmed_at_first_stream"}));
  EXPECT_EQ(report.at("encoder_choices"), nlohmann::json::array({"nvenc"}));
  EXPECT_FALSE(report.at("gpus").at(0).at("selected").get<bool>());
  EXPECT_TRUE(report.at("gpus").at(1).at("selected").get<bool>());
}

TEST(HostSetupFactsTests, AHostWithoutAGpuEncodesInSoftware) {
  const auto report = host_setup::hardware_report(host_with({}, ""));
  EXPECT_EQ(advice(report, "no_gpu").at("severity"), "fail");
  EXPECT_EQ(report.at("encoder").at("expected"), "software");
  EXPECT_TRUE(report.at("gpus").empty());
}

TEST(HostSetupFactsTests, NamesAForcedEncoderThisHostCannotRun) {
  auto in = host_with({nvidia_card()});
  in.configured_encoder = "vaapi";
  in.policy = "explicit";
  const auto report = host_setup::hardware_report(in);
  EXPECT_EQ(advice(report, "configured_encoder_cannot_run").at("params").at("encoder"), "vaapi");
  EXPECT_EQ(report.at("encoder").at("configured"), "vaapi");
  EXPECT_EQ(report.at("encoder").at("expected"), "vaapi");
}

TEST(HostSetupFactsTests, SaysWhenNvencDidNotStartAfterAProbe) {
  auto in = host_with({nvidia_card()});
  in.planned_encoder = "nvenc";
  in.active_encoder = "software";
  const auto report = host_setup::hardware_report(in);
  EXPECT_EQ(advice(report, "nvenc_not_active").at("params").at("encoder"), "software");
  EXPECT_EQ(report.at("encoder").at("expected"), "software");
  EXPECT_FALSE(has_code(report, "encoder_confirmed_at_first_stream"));
}

TEST(HostSetupFactsTests, NouveauAndAMissingNodeAreNamed) {
  auto nouveau = nvidia_card("");
  nouveau.driver = "nouveau";
  EXPECT_EQ(advice(host_setup::hardware_report(host_with({nouveau})), "nouveau_cannot_encode").at("severity"), "fail");

  const auto missing_node = host_setup::hardware_report(host_with({nvidia_card()}, "/dev/dri/renderD200"));
  EXPECT_EQ(advice(missing_node, "encoder_gpu_not_found").at("render_node"), "/dev/dri/renderD200");
}

TEST(HostSetupFactsTests, TrustsOnlyPrivateLanNetworksOnRealInterfaces) {
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "10.0.0.232", 24)), "10.0.0.0/24");
  EXPECT_EQ(host_setup::lan_cidr(lan("wlp4s0", "192.168.1.20", 24)), "192.168.1.0/24");
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "172.20.4.9", 16)), "172.20.0.0/16");
  EXPECT_EQ(host_setup::lan_cidr(lan("br0", "10.0.0.232", 8)), "10.0.0.0/8");

  auto loopback = lan("lo", "127.0.0.1", 8);
  loopback.loopback = true;
  EXPECT_EQ(host_setup::lan_cidr(loopback), std::nullopt);
  auto down = lan("enp5s0", "10.0.0.232", 24);
  down.up = false;
  EXPECT_EQ(host_setup::lan_cidr(down), std::nullopt);
  auto wireguard = lan("home", "10.8.0.2", 24);
  wireguard.point_to_point = true;
  EXPECT_EQ(host_setup::lan_cidr(wireguard), std::nullopt);
  auto container_bridge = lan("docker0", "172.17.0.1", 16);
  container_bridge.hardware_backed = false;
  EXPECT_EQ(host_setup::lan_cidr(container_bridge), std::nullopt);
  // Hardware underneath does not make a container network a LAN.
  EXPECT_EQ(host_setup::lan_cidr(lan("macvlan0", "192.168.1.50", 24)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("podman0", "10.88.0.1", 16)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("tailscale0", "100.101.102.103", 32)), std::nullopt);

  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "203.0.113.7", 24)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "100.64.3.1", 16)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "169.254.10.1", 16)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "192.168.1.20", 31)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "192.168.1.20", 32)), std::nullopt);
  EXPECT_EQ(host_setup::lan_cidr(lan("enp5s0", "192.168.1.20", 12)), std::nullopt);
  auto split_mask = lan("enp5s0", "192.168.1.20", 24);
  split_mask.netmask = 0xFF00FF00u;
  EXPECT_EQ(host_setup::lan_cidr(split_mask), std::nullopt);
}

TEST(HostSetupFactsTests, ListsEachNetworkOnceWithEveryInterfaceOnIt) {
  auto container_bridge = lan("docker0", "172.17.0.1", 16);
  container_bridge.hardware_backed = false;
  const auto networks = host_setup::lan_networks({
    lan("enp5s0", "192.168.1.20", 24),
    container_bridge,
    lan("wlp4s0", "192.168.1.21", 24),
    lan("enp6s0", "10.0.0.5", 24),
  });

  ASSERT_EQ(networks.size(), 2u);
  EXPECT_EQ(networks.at(0).at("cidr"), "192.168.1.0/24");
  EXPECT_EQ(networks.at(0).at("interface"), "enp5s0");
  EXPECT_EQ(networks.at(0).at("interfaces"), nlohmann::json::array({"enp5s0", "wlp4s0"}));
  EXPECT_EQ(networks.at(0).at("address"), "192.168.1.20");
  EXPECT_EQ(networks.at(1).at("cidr"), "10.0.0.0/24");
}
