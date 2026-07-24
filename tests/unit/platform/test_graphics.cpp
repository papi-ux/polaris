/**
 * @file tests/unit/platform/test_graphics.cpp
 * @brief Test Linux OpenGL helper behavior used by DMA-BUF capture.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <glad/gl.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/vaapi.h>

  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <string>

namespace {
  GLenum fake_gl_error_once() {
    static int calls = 0;
    return calls++ == 0 ? GL_INVALID_OPERATION : GL_NO_ERROR;
  }

  GLenum fake_gl_no_error() {
    return GL_NO_ERROR;
  }

  std::string read_source_for_contract(const char *relative_path) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative_path;
    std::ifstream in(path);
    if (!in) {
      return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::size_t count_occurrences(const std::string &haystack, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = haystack.find(needle, offset)) != std::string::npos; offset += needle.size()) {
      ++count;
    }
    return count;
  }
}  // namespace

TEST(GraphicsTests, DrainErrorsReportsWhetherAnyErrorWasDrained) {
  const auto original_get_error = gl::ctx.GetError;

  gl::ctx.GetError = fake_gl_no_error;
  EXPECT_FALSE(gl::drain_errors("test"));

  gl::ctx.GetError = fake_gl_error_once;
  EXPECT_TRUE(gl::drain_errors("test"));

  gl::ctx.GetError = original_get_error;
}

TEST(VaapiSourceContractTests, ConverterRetainsSurfacesAndPropagatesConversionFailure) {
  const auto source = read_source_for_contract("src/platform/linux/vaapi.cpp");
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("egl::rgb_t blank_rgb;"), std::string::npos);
  EXPECT_NE(
    source.find("std::array<egl::rgb_t, dmabuf_surface_cache_policy_t::capacity> rgb_cache;"),
    std::string::npos
  );
  EXPECT_EQ(count_occurrences(source, "cache_policy.plan("), 1u);
  EXPECT_EQ(count_occurrences(source, "cache_policy.commit_live("), 2u);
  EXPECT_EQ(count_occurrences(source, "descriptor.reset();"), 2u);
  EXPECT_NE(source.find("slot = std::move(*rgb_opt);"), std::string::npos);
  EXPECT_NE(source.find("return sws.convert(nv12->buf);"), std::string::npos);
}

TEST(VaapiSourceContractTests, ConversionFailuresRemainWiredToLinuxFallbackHandler) {
  const auto source = read_source_for_contract("src/video.cpp");
  ASSERT_FALSE(source.empty());

  EXPECT_EQ(count_occurrences(source, "handle_linux_gpu_native_conversion_failure("), 3u);

  EXPECT_NE(source.find("update_windowed_gpu_native_probe_result(false);"), std::string::npos);
  EXPECT_NE(
    source.find("if (handle_linux_gpu_native_conversion_failure(frame)) {\n              reinit_request_event.raise(true);"),
    std::string::npos
  );
  EXPECT_NE(
    source.find("if (handle_linux_gpu_native_conversion_failure(frame)) {\n              ec = platf::capture_e::reinit;"),
    std::string::npos
  );
}

TEST(VaapiDmabufSurfaceCachePolicyTests, PreservesBlankSurfaceAcrossDummyFrames) {
  va::dmabuf_surface_cache_policy_t policy;
  const std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  auto first = policy.plan(0, 0, valid_slots);
  EXPECT_EQ(first.action, va::dmabuf_surface_action_e::blank);
  policy.commit_blank();

  auto repeated = policy.plan(0, 0, valid_slots);
  EXPECT_EQ(repeated.action, va::dmabuf_surface_action_e::blank);
  EXPECT_FALSE(repeated.slot.has_value());
}

TEST(VaapiDmabufSurfaceCachePolicyTests, ReusesImportedSurfaceByBufferKey) {
  va::dmabuf_surface_cache_policy_t policy;
  std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  auto first = policy.plan(1, 41, valid_slots);
  ASSERT_EQ(first.action, va::dmabuf_surface_action_e::import);
  ASSERT_EQ(first.slot, 0);
  policy.commit_live(1, 41, *first.slot);
  valid_slots[0] = true;

  auto reused = policy.plan(2, 41, valid_slots);
  EXPECT_EQ(reused.action, va::dmabuf_surface_action_e::reuse);
  EXPECT_EQ(reused.slot, 0);
  policy.commit_live(2, 41, *reused.slot);

  auto repeated = policy.plan(2, 41, valid_slots);
  EXPECT_EQ(repeated.action, va::dmabuf_surface_action_e::reuse);
  EXPECT_EQ(repeated.slot, 0);
}

TEST(VaapiDmabufSurfaceCachePolicyTests, RotatesImportsWithoutDestroyingActiveSurfaceImmediately) {
  va::dmabuf_surface_cache_policy_t policy;
  std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  auto first = policy.plan(1, 10, valid_slots);
  ASSERT_EQ(first.slot, 0);
  policy.commit_live(1, 10, *first.slot);
  valid_slots[0] = true;

  auto second = policy.plan(2, 20, valid_slots);
  ASSERT_EQ(second.action, va::dmabuf_surface_action_e::import);
  ASSERT_EQ(second.slot, 1);
  policy.commit_live(2, 20, *second.slot);
  valid_slots[1] = true;

  auto third = policy.plan(3, 30, valid_slots);
  EXPECT_EQ(third.action, va::dmabuf_surface_action_e::import);
  EXPECT_EQ(third.slot, 0);
}

TEST(VaapiDmabufSurfaceCachePolicyTests, ZeroBufferKeyNeverAliasesANewerFrame) {
  va::dmabuf_surface_cache_policy_t policy;
  std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  auto first = policy.plan(1, 0, valid_slots);
  ASSERT_EQ(first.action, va::dmabuf_surface_action_e::import);
  policy.commit_live(1, 0, *first.slot);
  valid_slots[*first.slot] = true;

  auto second = policy.plan(2, 0, valid_slots);
  EXPECT_EQ(second.action, va::dmabuf_surface_action_e::import);
  EXPECT_EQ(second.slot, 1);
}

TEST(VaapiDmabufSurfaceCachePolicyTests, FailedImportDoesNotMutateSelectionState) {
  va::dmabuf_surface_cache_policy_t policy;
  const std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  const auto first = policy.plan(1, 55, valid_slots);
  const auto retry = policy.plan(1, 55, valid_slots);

  EXPECT_EQ(first.action, va::dmabuf_surface_action_e::import);
  EXPECT_EQ(retry.action, va::dmabuf_surface_action_e::import);
  EXPECT_EQ(first.slot, retry.slot);
}

TEST(VaapiDmabufSurfaceCachePolicyTests, MissingActiveSurfaceFailsClosed) {
  va::dmabuf_surface_cache_policy_t policy;
  std::array<bool, va::dmabuf_surface_cache_policy_t::capacity> valid_slots {};

  auto imported = policy.plan(1, 77, valid_slots);
  policy.commit_live(1, 77, *imported.slot);

  auto repeated = policy.plan(1, 77, valid_slots);
  EXPECT_EQ(repeated.action, va::dmabuf_surface_action_e::unavailable);
  EXPECT_FALSE(repeated.slot.has_value());
}
#else
TEST(GraphicsTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only test";
}
#endif
