/**
 * @file tests/unit/test_labwc_render_device_source.cpp
 * @brief Source guard: labwc must pin its wlroots render device to the
 *        configured adapter_name (issue #354).
 *
 * The behaviour depends on a real, present /dev/dri render node, which the
 * GitHub CI runners do not have (no GPU), so it can't be exercised from a unit
 * test. This guards the invariant at the source level — the same approach as
 * test_kmsgrab_logging_source.cpp: the private compositor's renderer must be
 * pinned to config::video.adapter_name, and an adapter_name that can't be used
 * must be reported rather than silently ignored.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_cage_router_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/cage_display_router.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(LabwcRenderDeviceSource, PinsWlrootsRenderDeviceToConfiguredAdapter) {
  const auto source = read_cage_router_source();
  ASSERT_FALSE(source.empty()) << "could not read cage_display_router.cpp via POLARIS_SOURCE_DIR";

  // The render device the wlroots backend uses must come from adapter_name.
  EXPECT_NE(source.find("WLR_RENDER_DRM_DEVICE"), std::string::npos)
    << "labwc must set WLR_RENDER_DRM_DEVICE so it does not auto-pick a GPU";
  EXPECT_NE(source.find("config::video.adapter_name"), std::string::npos)
    << "the render device must be sourced from the configured adapter_name";

  // The pin is headless-only. On the windowed (wayland-backend) path labwc is a
  // client of the host compositor and negotiates its device from the parent's
  // dmabuf feedback, so forcing WLR_RENDER_DRM_DEVICE there could mismatch the
  // parent and break buffer sharing on a multi-GPU host — it must be gated on the
  // headless flag (issue #354).
  EXPECT_NE(source.find("headless && key == \"WLR_RENDER_DRM_DEVICE\""), std::string::npos)
    << "WLR_RENDER_DRM_DEVICE must be pinned only for the headless backend, "
       "never on the windowed path that inherits its device from the parent";

  // WLR_RENDER_DRM_DEVICE must be in the set of keys actually pushed into the
  // labwc process environment, not merely referenced.
  const auto set_env_pos = source.find("set_labwc_process_environment");
  ASSERT_NE(set_env_pos, std::string::npos);
  const auto set_env_body = source.substr(set_env_pos, 1200);
  EXPECT_NE(set_env_body.find("\"WLR_RENDER_DRM_DEVICE\"sv"), std::string::npos)
    << "WLR_RENDER_DRM_DEVICE must be exported to labwc, not just computed";

  // Only a present /dev/dri device is used; a bad path is left to auto-select,
  // never forced (which would make wlroots fail instead of falling back).
  EXPECT_NE(source.find("/dev/dri/"), std::string::npos)
    << "adapter_name must be validated as a /dev/dri device path";
  EXPECT_NE(source.find("access(adapter.c_str(), F_OK)"), std::string::npos)
    << "adapter_name must be checked for existence before being forced on wlroots";

  // A configured-but-unusable adapter_name must be surfaced, since the bug it
  // fixes is otherwise invisible (wlroots silently grabs another card).
  EXPECT_NE(source.find("wlroots will auto-select a GPU"), std::string::npos)
    << "an ignored adapter_name must be logged, not swallowed";
}
