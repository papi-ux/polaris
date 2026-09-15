/**
 * @file tests/unit/platform/test_capture_backend_fallback_source.cpp
 * @brief Source guard: a configured capture backend that can capture nothing must fall back
 *        rather than leave Polaris with no capture at all.
 *
 * Capture backends are not interchangeable across compositors. wlr capture needs
 * zwlr_export_dmabuf_manager_v1, which only wlroots compositors have, and the stream mode decides
 * which compositor gets enumerated: a cage mode enumerates Polaris' own labwc and always works,
 * while every non-cage mode has to enumerate the host desktop. On KDE or GNOME that finds nothing,
 * and before this fallback existed the result was zero capture sources, no encoder probe, a
 * "Fatal: Unable to find display or encoder" line, and a host that carried on serving H.264 as
 * the only advertised codec (issue #677).
 *
 * Reproducing that needs a specific compositor, so the invariant is guarded at the source level,
 * the same way test_preallocated_gamepad_source.cpp guards its own.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_misc_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/misc.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(CaptureBackendFallbackSource, AConfiguredBackendThatFindsNothingFallsBack) {
  const auto source = read_misc_source();
  ASSERT_FALSE(source.empty()) << "could not read misc.cpp via POLARIS_SOURCE_DIR";

  const auto entry = source.find("void reevaluate_capture_sources()");
  ASSERT_NE(entry, std::string::npos);
  const auto body = source.substr(entry);

  // The retry is the whole fix: evaluate again as if no backend were configured, which is what
  // auto-selection would have done and what would have worked on this host all along.
  EXPECT_NE(body.find("capture_backend_override = std::string {}"), std::string::npos)
    << "reevaluate_capture_sources no longer retries with auto-selection";
  EXPECT_NE(body.find("verified_action::confirm"), std::string::npos)
    << "a substituted capture backend is no longer recorded as a silent substitution";
}

TEST(CaptureBackendFallbackSource, TheEvaluationConsultsTheOverrideRatherThanTheConfig) {
  const auto source = read_misc_source();
  ASSERT_FALSE(source.empty());

  const auto entry = source.find("void evaluate_capture_sources()");
  ASSERT_NE(entry, std::string::npos);
  const auto end = source.find("void reevaluate_capture_sources()", entry);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(entry, end - entry);

  // Reading config::video.capture directly here would make the retry a no-op, silently.
  EXPECT_EQ(body.find("config::video.capture"), std::string::npos)
    << "evaluate_capture_sources reads the configuration directly again, so the retry cannot work";
  EXPECT_NE(body.find("requested_capture()"), std::string::npos);
}
