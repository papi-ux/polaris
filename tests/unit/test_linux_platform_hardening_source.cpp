/**
 * @file tests/unit/test_linux_platform_hardening_source.cpp
 * @brief Source contracts for Linux backends omitted by some build variants.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

  std::string read_source(const std::filesystem::path &relative_path) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path);
    EXPECT_TRUE(input.good()) << relative_path;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

}  // namespace

TEST(LinuxPlatformHardeningSource, CageCapturePublishesMetadataAtomicallyAndJoins) {
  const auto source = read_source("src/platform/linux/cage_screencopy.cpp");
  EXPECT_NE(source.find("published_frame_t"), std::string::npos);
  EXPECT_NE(source.find("::poll(&display_poll"), std::string::npos);
  EXPECT_NE(source.find("sc_thread.join()"), std::string::npos);
  EXPECT_EQ(source.find("sc_thread.detach()"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, TimedOutWlrCaptureCancelsItsRequest) {
  const auto header = read_source("src/platform/linux/wayland.h");
  const auto capture = read_source("src/platform/linux/wlgrab.cpp");
  EXPECT_NE(header.find("pending_frame"), std::string::npos);
  EXPECT_NE(header.find("pending_buffer_create"), std::string::npos);
  EXPECT_NE(capture.find("dmabuf.cancel()"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, CudaFormatTransitionReleasesCompleteDestination) {
  const auto source = read_source("src/platform/linux/cuda.cpp");
  const auto transition = source.find("source_fourcc != descriptor.sd.fourcc");
  ASSERT_NE(transition, std::string::npos);
  EXPECT_NE(source.find("release_destination();", transition), std::string::npos);

  const auto release = source.find("void release_destination()");
  ASSERT_NE(release, std::string::npos);
  EXPECT_NE(source.find("vkDestroyBuffer", release), std::string::npos);
  EXPECT_NE(source.find("vkFreeMemory", release), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, NetlinkReadsAlwaysStartAtBufferBase) {
  const auto source = read_source("src/platform/linux/misc.cpp");
  EXPECT_NE(source.find("::recv(fd, buffer, sizeof(buffer), 0)"), std::string::npos);
  EXPECT_EQ(source.find("recv(fd, nlMsg"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, KscreenConfigurationNeverPassesThroughShell) {
  const auto topology = read_source("src/platform/linux/display_topology.cpp");
  EXPECT_EQ(topology.find("std::system"), std::string::npos);
  EXPECT_NE(topology.find("platf::run_process_argv(argv)"), std::string::npos);

  const auto virtual_display = read_source("src/platform/linux/virtual_display.cpp");
  const auto kscreen = virtual_display.find("namespace kscreen");
  ASSERT_NE(kscreen, std::string::npos);
  EXPECT_NE(virtual_display.find("platf::run_process_argv(args)", kscreen), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, VaapiValidatesExportedDescriptorBeforeIndexing) {
  const auto source = read_source("src/platform/linux/vaapi.cpp");
  EXPECT_NE(source.find("DRMPRIMESurfaceDescriptor prime {}"), std::string::npos);
  EXPECT_NE(source.find("prime.num_objects > max_objects"), std::string::npos);
  EXPECT_NE(source.find("layer.num_planes > max_planes"), std::string::npos);
  EXPECT_NE(source.find("layer.object_index[x] >= prime.num_objects"), std::string::npos);
  EXPECT_NE(source.find("vaSetInfoCallback"), std::string::npos);
  EXPECT_NE(source.find("bytes > 0"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, X11ShmRefreshStaysOnCaptureThread) {
  const auto source = read_source("src/platform/linux/x11grab.cpp");
  EXPECT_EQ(source.find("task_pool.pushDelayed"), std::string::npos);
  EXPECT_EQ(source.find("task_pool.cancel"), std::string::npos);
  EXPECT_NE(source.find("now >= next_refresh"), std::string::npos);
  EXPECT_NE(source.find("if (!x_img)"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, EglRejectsMissingDriverMetadata) {
  const auto source = read_source("src/platform/linux/graphics.cpp");
  EXPECT_NE(source.find("if (!extension_st)"), std::string::npos);
  EXPECT_NE(source.find("count == 0 || !conf"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, NvfbcOutputsStartInitialized) {
  const auto source = read_source("src/platform/linux/cuda.cpp");
  EXPECT_NE(source.find("NVFBC_SESSION_HANDLE handle {0}"), std::string::npos);
  EXPECT_NE(source.find("CUdeviceptr device_ptr {}"), std::string::npos);
  EXPECT_NE(source.find("NVFBC_FRAME_GRAB_INFO info {}"), std::string::npos);
}
