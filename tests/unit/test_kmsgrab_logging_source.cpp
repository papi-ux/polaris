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
