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

  // Only a host set to KMS capture is failing when the probe finds no CAP_SYS_ADMIN. Every other
  // host never enabled KMS, and the probe runs at each capture evaluation, so it says so once, as info.
  const auto probe = source.find("note_kms_capture_refused_for_capability();");
  ASSERT_NE(probe, std::string::npos);
  const auto chosen = source.find("if (config::video.capture == \"kms\") {", probe);
  const auto fatal_line = source.find("BOOST_LOG(fatal)", probe);
  const auto once = source.find("std::call_once(said,", probe);
  const auto quiet = source.find("BOOST_LOG(info) << \"KMS capture is off on this host, so Polaris captures another way.", probe);
  ASSERT_NE(chosen, std::string::npos);
  ASSERT_NE(fatal_line, std::string::npos);
  ASSERT_NE(once, std::string::npos);
  ASSERT_NE(quiet, std::string::npos);
  EXPECT_LT(chosen, fatal_line);
  EXPECT_LT(fatal_line, once);
  EXPECT_LT(once, quiet);
  EXPECT_EQ(source.find("window_system != window_system_e::X11 || config::video.capture == \"kms\""), std::string::npos);
  EXPECT_NE(source.find("KMS display capture requires CAP_SYS_ADMIN"), std::string::npos);
  // A binary that holds no CAP_SYS_ADMIN is not asked to raise it, so that is not an error either.
  EXPECT_NE(source.find("cap_get_flag(caps, CAP_SYS_ADMIN, CAP_PERMITTED, &permitted)"), std::string::npos);
  // A binary that does hold it is not told to run --enable-kms, and is not recorded as refused for it.
  const auto held = source.find("if (kms::sys_admin_permitted()) {");
  ASSERT_NE(held, std::string::npos);
  EXPECT_LT(held, probe);
  EXPECT_LT(probe - held, 700u);
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
