/**
 * @file tests/unit/platform/test_absolute_input_source.cpp
 * @brief Source guard: every capture that places absolute input on a desktop of several monitors
 *        does it through the functions the layout tests cover, and the Linux absolute mouse adds
 *        the captured screen's place on the desktop.
 *
 * The placement is tested where it lives: output_layout.h, wl::capture_geometry(),
 * video::make_port() and platf::point_on_desktop(). What those tests cannot reach is each call.
 * wlgrab's init needs a Wayland compositor, KMS capture a DRM card and CAP_SYS_ADMIN, and
 * abs_mouse() a uinput device or a compositor to write to, so a call site put back to placing
 * input by hand would leave every one of those tests green. The calls are guarded at the source
 * level instead, the way test_capture_backend_fallback_source.cpp guards its own.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

  std::string read_source(std::string_view relative) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative;
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  /**
   * @brief The source from the first `begin` up to the first `end` after it, or nothing when either
   *        is missing, so a renamed anchor fails the test rather than widening it.
   */
  std::string between(const std::string &source, std::string_view begin, std::string_view end) {
    const auto start = source.find(begin);
    if (start == std::string::npos) {
      return {};
    }
    const auto stop = source.find(end, start);
    if (stop == std::string::npos) {
      return {};
    }
    return source.substr(start, stop - start);
  }

  bool has(const std::string &body, std::string_view text) {
    return body.find(text) != std::string::npos;
  }

}  // namespace

// wlgrab measures the desktop, the frame and the screen from the monitors it has just enumerated,
// in one call the Wayland layout tests cover.
TEST(AbsoluteInputSource, WlgrabPlacesItsCaptureThroughCaptureGeometry) {
  const auto source = read_source("src/platform/linux/wlgrab.cpp");
  ASSERT_FALSE(source.empty()) << "could not read wlgrab.cpp via POLARIS_SOURCE_DIR";

  const auto init = between(source, "int init(platf::mem_type_e hwdevice_type, const std::string &display_name", "int dummy_img(");
  ASSERT_FALSE(init.empty()) << "wlgrab's first init() or the dummy_img() after it was renamed";

  EXPECT_TRUE(has(init, "wl::capture_geometry(interface.monitors, *monitor_index).apply_to(*this);"))
    << "wlgrab's init no longer places its capture through wl::capture_geometry()";
  EXPECT_FALSE(has(init, "= monitor->viewport."))
    << "wlgrab's init places its capture by hand from the monitor's viewport again";
  EXPECT_FALSE(has(init, "env_width ="))
    << "wlgrab's init sets the desktop's size by hand again";
}

// The Linux absolute mouse is given a point counted from the captured screen's corner, and puts
// the screen's place on the desktop in before any backend sees it.
TEST(AbsoluteInputSource, LinuxAbsMouseAddsTheScreensPlaceOnTheDesktop) {
  const auto source = read_source("src/platform/linux/input/inputtino.cpp");
  ASSERT_FALSE(source.empty()) << "could not read inputtino.cpp via POLARIS_SOURCE_DIR";

  const auto abs_mouse = between(source, "void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y)", "void button_mouse(");
  ASSERT_FALSE(abs_mouse.empty()) << "abs_mouse() or the button_mouse() after it was renamed";

  EXPECT_TRUE(has(abs_mouse, "point_on_desktop(touch_port, x, y)"))
    << "abs_mouse() no longer adds the captured screen's place on the desktop";
  EXPECT_TRUE(has(abs_mouse, "platf::mouse::move_abs(raw, touch_port, desktop_x, desktop_y);"))
    << "abs_mouse() hands the backends something other than the point on the desktop";
  EXPECT_FALSE(has(abs_mouse, "move_abs(raw, touch_port, x, y)"))
    << "abs_mouse() hands the backends the point on the captured screen again";
}

// KMS capture places the CRTC it streams through crtc_input_rect() on either branch, including the
// one for a CRTC it could not tie to a monitor, and takes the desktop's size from the same place.
TEST(AbsoluteInputSource, KmsPlacesTheStreamedCrtcThroughCrtcInputRect) {
  const auto source = read_source("src/platform/linux/kmsgrab.cpp");
  ASSERT_FALSE(source.empty()) << "could not read kmsgrab.cpp via POLARIS_SOURCE_DIR";

  const auto placement = between(source, "Found monitor for DRM screencasting", "plane_id = plane->plane_id;");
  ASSERT_FALSE(placement.empty()) << "the KMS capture's placement moved";

  EXPECT_TRUE(has(placement, "this->env_width = ::platf::kms::desktop.rect.width;"));
  EXPECT_TRUE(has(placement, "this->env_height = ::platf::kms::desktop.rect.height;"));
  EXPECT_TRUE(has(placement, "streamed = kms::crtc_output(monitor->second);"))
    << "a CRTC tied to a monitor is no longer placed by what the desktop measured it by";
  EXPECT_TRUE(has(placement, "const auto screen = output_layout::crtc_input_rect(streamed, kms::desktop, width, height);"))
    << "KMS capture no longer places the streamed CRTC through crtc_input_rect()";
  for (const auto *field : {"offset_x = screen.x;", "offset_y = screen.y;", "input_width = screen.width;", "input_height = screen.height;"}) {
    EXPECT_TRUE(has(placement, field)) << "KMS capture no longer sets " << field;
  }
  // The leading space keeps img_offset_x, the crop into the framebuffer, out of it.
  EXPECT_FALSE(has(placement, " offset_x = crtc->x"))
    << "the branch for an untied CRTC counts its offset from zero again, not from the desktop's corner";
  EXPECT_FALSE(has(placement, " offset_x = viewport.offset_x"))
    << "KMS capture places the streamed CRTC by hand from its viewport again";
}

// KMS capture measures the desktop from every active CRTC on the cards it opened, including one no
// connector names, through measure_crtc_desktop(), which keeps Wayland's rectangles and CRTC
// rectangles apart.
TEST(AbsoluteInputSource, KmsMeasuresTheDesktopThroughMeasureCrtcDesktop) {
  const auto source = read_source("src/platform/linux/kmsgrab.cpp");
  ASSERT_FALSE(source.empty()) << "could not read kmsgrab.cpp via POLARIS_SOURCE_DIR";

  const auto names = between(source, "std::vector<std::string> kms_display_names(mem_type_e hwdevice_type) {", "kms::card_descriptors = std::move(cds);");
  ASSERT_FALSE(names.empty()) << "kms_display_names() moved";

  EXPECT_TRUE(has(names, "unnamed_crtcs.insert(plane->crtc_id);"))
    << "an active CRTC no connector names is no longer counted";
  EXPECT_TRUE(has(names, "active_crtcs.emplace_back(kms::crtc_output(monitor_descriptor));"));
  EXPECT_TRUE(has(names, "active_crtcs.resize(active_crtcs.size() + unnamed_active_crtcs);"))
    << "the unnamed CRTCs no longer keep the desktop in CRTC rectangles";
  EXPECT_TRUE(has(names, "kms::desktop = output_layout::measure_crtc_desktop(active_crtcs);"))
    << "KMS capture no longer measures the desktop through measure_crtc_desktop()";
  EXPECT_FALSE(has(names, "output_layout::bounds("))
    << "KMS capture measures the desktop by hand again, where Wayland and CRTC rectangles can mix";
}
