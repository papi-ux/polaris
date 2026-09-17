#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_kmsgrab_source() {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/kmsgrab.cpp";
  std::ifstream file {path};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(KmsgrabLoggingSource, AutoProbeSetcapGuidanceIsNotFatalOnWayland) {
  const auto source = read_kmsgrab_source();

  EXPECT_NE(source.find("config::video.capture == \"kms\" ? fatal"), std::string::npos);
  EXPECT_EQ(source.find("window_system != window_system_e::X11 || config::video.capture == \"kms\""), std::string::npos);
  EXPECT_NE(source.find("KMS display capture requires CAP_SYS_ADMIN"), std::string::npos);
  EXPECT_NE(source.find("KMS probe could not access DRM framebuffer handles; continuing with non-KMS capture backends"), std::string::npos);
}

TEST(KmsgrabLoggingSource, VirtualDisplayCardsDoNotWarnAboutRenderNodesOrNvenc) {
  const auto source = read_kmsgrab_source();

  EXPECT_NE(source.find("virtual_display_driver = ver && ver->name && platf::is_virtual_display_driver(ver->name);"), std::string::npos);
  EXPECT_NE(source.find("BOOST_LOG(virtual_display_driver ? debug : warning) << \"No render device name for: \"sv"), std::string::npos);
  const auto nvenc = source.find("Using NVENC with your display connected to a different GPU may not work properly!");
  ASSERT_NE(nvenc, std::string::npos);
  const auto guard = source.rfind("if (card.virtual_display_driver) {", nvenc);
  ASSERT_NE(guard, std::string::npos);
  EXPECT_LT(nvenc - guard, 300u);
}

TEST(KmsgrabLoggingSource, MissingCapabilityGuidanceSaysUpdatesDropIt) {
  const auto source = read_kmsgrab_source();

  EXPECT_NE(source.find("[sudo -H polaris --setup-host --enable-kms] after each install or update"), std::string::npos);
  EXPECT_EQ(source.find("sudo setcap cap_sys_admin+ep $(readlink -f $(which polaris))"), std::string::npos);
}
