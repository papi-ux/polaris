#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_source(const char *relative) {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative;
  std::ifstream file {path};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string read_kmsgrab_source() {
  return read_source("src/platform/linux/kmsgrab.cpp");
}

}  // namespace

TEST(KmsgrabLoggingSource, AutoProbeSetcapGuidanceIsNotFatalOnWayland) {
  const auto source = read_kmsgrab_source();

  // Only a host set to KMS capture is failing when the probe finds no CAP_SYS_ADMIN. Every other
  // host never enabled KMS, and the probe runs at each capture evaluation, so it says so once, as info.
  const auto probe = source.find("note_kms_capture_refused_for_capability();");
  ASSERT_NE(probe, std::string::npos);
  const auto chosen = source.find(
    "if (stream_display_policy::canonical_capture_backend(config::video.capture) == \"kms\") {",
    probe
  );
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

TEST(KmsgrabLoggingSource, AHostSetToDrmIsReadAsTheKmsHostItIs) {
  // Dispatch, the capture evaluation and the refusal record all read drm as kms. The probe's own
  // lines compared the literal, so a drm host refused the capability was recorded as having asked
  // for KMS and told in the same breath that it needs KMS only if it wants it.
  const auto source = read_kmsgrab_source();
  EXPECT_EQ(source.find("config::video.capture == \"kms\""), std::string::npos);
  EXPECT_EQ(source.find("config::video.capture != \"kms\""), std::string::npos);
  EXPECT_NE(
    source.find("stream_display_policy::canonical_capture_backend(config::video.capture) != \"kms\""),
    std::string::npos
  ) << "the X11 fallback for a driver without atomic mode setting reads drm as no KMS choice again";
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

TEST(KmsgrabLoggingSource, MissingCapabilityGuidanceNamesTheHelperNotEveryUpdate) {
  const auto source = read_kmsgrab_source();

  // Since 1.4.13 the capability is on the polaris-kms package's helper, which updates keep. Telling
  // someone to rerun --enable-kms after every update sends them to redo what the package already
  // keeps, and while a drop-in is parked until a login, a rerun alone only parks it again.
  EXPECT_EQ(source.find("after each install or update"), std::string::npos);
  EXPECT_EQ(source.find("replaces the binary without it"), std::string::npos);
  EXPECT_NE(source.find("DRM/KMS helper in the polaris-kms package, which updates \"sv"), std::string::npos);
  EXPECT_NE(source.find("run [sudo -H polaris --setup-host --enable-kms] once"), std::string::npos);
  EXPECT_NE(source.find("since it may ask for a new login first"), std::string::npos);
  EXPECT_EQ(source.find("sudo setcap cap_sys_admin+ep $(readlink -f $(which polaris))"), std::string::npos);
}

TEST(KmsgrabLoggingSource, EveryOtherKmsSetupAdviceNamesTheHelperNotEveryUpdate) {
  // kmsgrab no longer gives the rerun-after-every-update advice. The launch refusal and the
  // command line help still said it, and both reach people who already have the helper.
  const auto video = read_source("src/video.cpp");
  EXPECT_EQ(video.find("Every Polaris install or update needs this again."), std::string::npos);
  EXPECT_NE(video.find("run sudo -H polaris --setup-host --enable-kms once and do what it prints"), std::string::npos);
  EXPECT_NE(video.find("The polaris-kms package keeps the capability across updates."), std::string::npos);

  const auto logging = read_source("src/logging.cpp");
  EXPECT_EQ(logging.find("every install or update removes it again"), std::string::npos);
  EXPECT_EQ(logging.find("also grant this binary cap_sys_admin"), std::string::npos);
  EXPECT_NE(logging.find("point the user service at the polaris-kms helper"), std::string::npos);

  const auto troubleshooting = read_source("docs/troubleshooting.md");
  EXPECT_EQ(troubleshooting.find("run the step again after every install or update"), std::string::npos);
  EXPECT_NE(troubleshooting.find("helper of its own, which updates keep"), std::string::npos);
}
