#include "src/platform/linux/spaces_gpu_nodes.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <unistd.h>

#ifdef __linux__
namespace {
  using namespace multiseat;
  using namespace multiseat::spaces;
  namespace fs = std::filesystem;
  bool same(const production_controller_gpu_t &a, const production_controller_gpu_t &b) {
    return a.logical_gpu_id == b.logical_gpu_id && a.render_node == b.render_node && a.devices == b.devices &&
      a.max_seats == b.max_seats && a.max_encoder_sessions == b.max_encoder_sessions;
  }
  // A fake /sys/bus/pci/devices: each device directory holds its DRM minors under drm/.
  class SpacesGpuNodes : public ::testing::Test {
  protected:
    fs::path root, pci;
    void SetUp() override {
      char pattern[] = "/tmp/polaris-spaces-gpu-nodes-XXXXXX";
      const auto created = ::mkdtemp(pattern);
      ASSERT_NE(created, nullptr);
      root = created; pci = root / "bus/pci/devices";
    }
    void TearDown() override { fs::remove_all(root); }
    fs::path node(const fs::path &device, const std::string &name, const std::string &dev) {
      fs::create_directories(device / "drm" / name);
      std::ofstream(device / "drm" / name / "dev") << dev << '\n';
      return device / "drm" / name;
    }
    // What guided setup saved for an NVIDIA card before this boot.
    static production_controller_gpu_t saved() {
      return {"pci-0000_01_00.0", "/dev/dri/renderD128",
        {"/dev/dri/renderD128", "/dev/dri/card2", "/dev/nvidia0", "/dev/nvidiactl", "/dev/nvidia-modeset", "/dev/nvidia-uvm"}, 1, 1};
    }
  };

  TEST_F(SpacesGpuNodes, PciAddressComesOnlyFromTheSavedIdForm) {
    EXPECT_EQ(pci_address_of("pci-0000_01_00.0"), "0000:01:00.0");
    EXPECT_EQ(pci_address_of("pci-0000_c3_1f.7"), "0000:c3:1f.7");
    for (const auto *id : {"gpu-0", "pci-0000:01:00.0", "pci-0000_01_00.8", "pci-0000_01_00", "pci-0000_0A_00.0",
                           "pci-0000_01_00.0/../x", "pci-00000_01_00.0", "pci-0000_01_00.0 ", "", "pci-"})
      EXPECT_FALSE(pci_address_of(id)) << id;
    EXPECT_FALSE(resolve_drm_nodes("../0000:01:00.0", pci));
    EXPECT_FALSE(resolve_drm_nodes("0000_01_00.0", pci));
  }

  TEST_F(SpacesGpuNodes, RenumberedNodesFollowThePciDevice) {
    const auto gpu = pci / "0000:01:00.0";
    node(gpu, "card1", "226:1");
    node(gpu, "renderD129", "226:129");
    fs::create_directory_symlink("card1", gpu / "drm/controlD65");
    fs::create_directories(gpu / "drm/card1/card1-DP-1");
    const auto nodes = resolve_drm_nodes("0000:01:00.0", pci);
    ASSERT_TRUE(nodes);
    EXPECT_EQ(nodes->card, (drm_node_t {"/dev/dri/card1", 226, 1}));
    EXPECT_EQ(nodes->render, (drm_node_t {"/dev/dri/renderD129", 226, 129}));

    auto entry = saved();
    const auto refresh = refresh_drm_nodes(entry, *nodes);
    ASSERT_TRUE(refresh.ok);
    EXPECT_EQ(entry.render_node, "/dev/dri/renderD129");
    // Order is kept and the NVIDIA character devices are left exactly as saved.
    EXPECT_EQ(entry.devices, (std::vector<fs::path> {"/dev/dri/renderD129", "/dev/dri/card1", "/dev/nvidia0",
      "/dev/nvidiactl", "/dev/nvidia-modeset", "/dev/nvidia-uvm"}));
    EXPECT_EQ(entry.logical_gpu_id, "pci-0000_01_00.0");
    EXPECT_EQ(refresh.moved, (std::vector<std::pair<fs::path, fs::path>> {
      {"/dev/dri/renderD128", "/dev/dri/renderD129"}, {"/dev/dri/card2", "/dev/dri/card1"}}));

    // Unchanged numbers are a no-op.
    const auto again = refresh_drm_nodes(entry, *nodes);
    EXPECT_TRUE(again.ok);
    EXPECT_TRUE(again.moved.empty());
  }

  TEST_F(SpacesGpuNodes, MissingDeviceOrDriverFailsClosed) {
    EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci));
    // Present on the bus but no DRM driver bound yet.
    fs::create_directories(pci / "0000:01:00.0");
    EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci));
    fs::create_directories(pci / "0000:01:00.0/drm");
    EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci));
    auto entry = saved();
    const auto before = entry;
    const auto refresh = refresh_drm_nodes(entry, {});
    EXPECT_FALSE(refresh.ok);
    EXPECT_EQ(refresh.unresolved, "/dev/dri/renderD128");
    EXPECT_TRUE(same(entry, before));
  }

  TEST_F(SpacesGpuNodes, RenderOnlyDeviceKeepsItsShape) {
    const auto gpu = pci / "0000:03:00.0";
    node(gpu, "renderD130", "226:130");
    const auto nodes = resolve_drm_nodes("0000:03:00.0", pci);
    ASSERT_TRUE(nodes);
    EXPECT_FALSE(nodes->card);
    EXPECT_EQ(nodes->render, (drm_node_t {"/dev/dri/renderD130", 226, 130}));

    production_controller_gpu_t compute {"pci-0000_03_00.0", "/dev/dri/renderD128", {"/dev/dri/renderD128"}, 1, 1};
    const auto refresh = refresh_drm_nodes(compute, *nodes);
    ASSERT_TRUE(refresh.ok);
    EXPECT_EQ(compute.devices, (std::vector<fs::path> {"/dev/dri/renderD130"}));
    EXPECT_EQ(compute.render_node, "/dev/dri/renderD130");

    // A saved card node is never dropped or invented: the device lost a kind it had.
    auto entry = saved();
    const auto before = entry;
    const auto lost = refresh_drm_nodes(entry, *nodes);
    EXPECT_FALSE(lost.ok);
    EXPECT_EQ(lost.unresolved, "/dev/dri/card2");
    EXPECT_TRUE(same(entry, before));
  }

  TEST_F(SpacesGpuNodes, UnrelatedDeviceWithTheSavedCardNumberIsNeverChosen) {
    // After the reboot EVDI and a second GPU probed first: card2 now exists but is not this GPU.
    node(root / "devices/platform/evdi.0", "card0", "226:0");
    const auto other = pci / "0000:02:00.0";
    node(other, "card2", "226:2");
    node(other, "renderD128", "226:128");
    const auto gpu = pci / "0000:01:00.0";
    node(gpu, "card1", "226:1");
    node(gpu, "renderD129", "226:129");

    const auto nodes = resolve_drm_nodes("0000:01:00.0", pci);
    ASSERT_TRUE(nodes);
    EXPECT_EQ(nodes->card->path, "/dev/dri/card1");
    EXPECT_EQ(nodes->render->path, "/dev/dri/renderD129");
    auto entry = saved();
    ASSERT_TRUE(refresh_drm_nodes(entry, *nodes).ok);
    EXPECT_EQ(std::count(entry.devices.begin(), entry.devices.end(), fs::path("/dev/dri/card2")), 0);
    EXPECT_EQ(std::count(entry.devices.begin(), entry.devices.end(), fs::path("/dev/dri/renderD128")), 0);
    // An absent address never borrows another device's nodes.
    EXPECT_FALSE(resolve_drm_nodes("0000:04:00.0", pci));
  }

  TEST_F(SpacesGpuNodes, AmbiguousOrForgedLayoutsFailClosed) {
    const auto gpu = pci / "0000:01:00.0";
    const auto card = node(gpu, "card1", "226:1");
    node(gpu, "renderD128", "226:128");
    ASSERT_TRUE(resolve_drm_nodes("0000:01:00.0", pci));
    for (const auto *dev : {"", "226", "226:", ":1", "226:1:0", "-1:1", "226:x", "99999999999:1"}) {
      std::ofstream(card / "dev", std::ios::trunc) << dev << '\n';
      EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci)) << dev;
    }
    std::ofstream(card / "dev", std::ios::trunc) << "226:1\n";
    const auto second = node(gpu, "card3", "226:3");
    EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci));
    fs::remove_all(second);
    // A card entry that is a link points at some other device's minor.
    node(pci / "0000:02:00.0", "card3", "226:3");
    fs::create_directory_symlink(pci / "0000:02:00.0/drm/card3", gpu / "drm/card3");
    EXPECT_FALSE(resolve_drm_nodes("0000:01:00.0", pci));
    fs::remove(gpu / "drm/card3");
    ASSERT_TRUE(resolve_drm_nodes("0000:01:00.0", pci));

    // Saved entries that are not DRM nodes of this shape are refused rather than guessed at.
    const auto nodes = *resolve_drm_nodes("0000:01:00.0", pci);
    auto wrong_render = saved(); wrong_render.render_node = "/dev/dri/card2";
    EXPECT_FALSE(refresh_drm_nodes(wrong_render, nodes).ok);
    auto two_cards = saved(); two_cards.devices.push_back("/dev/dri/card5");
    const auto collapsed = refresh_drm_nodes(two_cards, nodes);
    EXPECT_FALSE(collapsed.ok);
    EXPECT_TRUE(collapsed.unresolved.empty());
    auto untouched = saved(); untouched.devices.push_back("/dev/dri/card5");
    EXPECT_TRUE(same(two_cards, untouched));
  }
}
#endif
