#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
  std::string read_source(const std::filesystem::path &relative) {
    std::ifstream input(std::filesystem::path(POLARIS_SOURCE_DIR) / relative);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
  }
}

TEST(LinuxStreamContractTests, ReconnectRetiresOldPipeWireGenerationBeforeReplacement) {
  const auto portal = read_source("src/platform/linux/portal_grab.cpp");
  const auto pipewire = read_source("src/platform/linux/pipewire_capture.cpp");
  ASSERT_FALSE(portal.empty());
  ASSERT_FALSE(pipewire.empty());

  const auto ensure = portal.find("ensure_global_capture(int width");
  const auto ensure_end = portal.find("// -----------------------------------------------------------------------", ensure);
  ASSERT_NE(ensure, std::string::npos);
  ASSERT_NE(ensure_end, std::string::npos);
  const auto body = portal.substr(ensure, ensure_end - ensure);
  const auto retire = body.find("retired_capture = std::move(g_media.capture)");
  const auto shutdown = body.find("retired_capture->shutdown()", retire);
  const auto replacement = body.find("g_media.capture = std::move(local)", shutdown);
  EXPECT_NE(body.find("g_capture_transition_mu"), std::string::npos);
  ASSERT_NE(retire, std::string::npos);
  ASSERT_NE(shutdown, std::string::npos);
  ASSERT_NE(replacement, std::string::npos);
  EXPECT_LT(retire, shutdown);
  EXPECT_LT(shutdown, replacement);

  EXPECT_NE(pipewire.find("void capture_t::shutdown()"), std::string::npos);
  EXPECT_NE(pipewire.find("active_dmabuf_leases_ == 0"), std::string::npos);
}

TEST(LinuxStreamContractTests, DmaBufIdentityTracksPipeWireBufferAllocationLifecycle) {
  const auto source = read_source("src/platform/linux/pipewire_capture.cpp");
  ASSERT_FALSE(source.empty());
  EXPECT_NE(source.find(".add_buffer = capture_t::on_add_buffer"), std::string::npos);
  EXPECT_NE(source.find(".remove_buffer = capture_t::on_remove_buffer"), std::string::npos);
  EXPECT_NE(source.find("allocate_buffer_key()"), std::string::npos);
  EXPECT_NE(source.find("descriptor->dmabuf_buffer_key = front_dmabuf_buffer_key_"), std::string::npos);
  EXPECT_EQ(source.find("reinterpret_cast<std::uintptr_t>(front_dmabuf_buffer_->buffer)"), std::string::npos);
  EXPECT_NE(source.find("front_info_.spa_format != raw_info.format"), std::string::npos);
  EXPECT_NE(source.find("front_info_.modifier != negotiated_modifier"), std::string::npos);
}

TEST(LinuxStreamContractTests, GamescopeOwnershipTransitionsUseOneCrossProcessLock) {
  const auto shell = read_source("nix/modules/polaris-gamescope-runtime-lib.sh");
  const auto runtime = read_source("src/platform/linux/stream_runtime_gamescope.cpp");
  ASSERT_FALSE(shell.empty());
  ASSERT_FALSE(runtime.empty());
  EXPECT_NE(shell.find("polaris-gamescope.lock"), std::string::npos);
  EXPECT_NE(shell.find("flock"), std::string::npos);
  EXPECT_NE(shell.find("POLARIS_MARKER_EXECUTABLE"), std::string::npos);
  EXPECT_NE(shell.find("gamescope|.gamescope-wrapped"), std::string::npos);
  EXPECT_NE(runtime.find("owner_transition_lock_t"), std::string::npos);
  EXPECT_NE(runtime.find("polaris-gamescope.lock"), std::string::npos);
  EXPECT_NE(runtime.find("POLARIS_GAMESCOPE_EXECUTABLE"), std::string::npos);
}

TEST(LinuxStreamContractTests, RootMediaFenceSurvivesThroughCompositorTermination) {
  const auto media = read_source("src/platform/linux/session_media.cpp");
  const auto process = read_source("src/process.cpp");
  ASSERT_FALSE(media.empty());
  ASSERT_FALSE(process.empty());

  EXPECT_NE(media.find("wait_for_other_teardowns(teardown)"), std::string::npos);
  EXPECT_NE(media.find("return teardown;"), std::string::npos);
  EXPECT_EQ(media.find("teardown.reset()"), std::string::npos);

  const auto terminate = process.find("void proc_t::terminate_impl(");
  ASSERT_NE(terminate, std::string::npos);
  const auto body = process.substr(terminate, 4200);
  const auto fence = body.find("media_stop_fence = session_media::prepare_for_stop()");
  const auto compositor = body.find("terminate_isolated_session_generation()");
  ASSERT_NE(fence, std::string::npos);
  ASSERT_NE(compositor, std::string::npos);
  EXPECT_LT(fence, compositor);
}
