/**
 * @file tests/unit/test_preallocated_gamepad_source.cpp
 * @brief Source guard: the pad created before an app launches must carry the controller type
 *        the client last declared, and an arrival that arrives too late must not be discarded
 *        in silence.
 *
 * The pad has to exist before the app starts, or a game does not see a controller at startup.
 * The client's own SS_CONTROLLER_ARRIVAL only comes once the stream is up, which is later. For
 * a long time preallocate_gamepad() closed that gap by passing an empty arrival, which made
 * every auto-detect branch of the type ladder in inputtino_gamepad.cpp unreachable and silently
 * emulated an Xbox pad for every client, including the ones asking for a DualSense. Under strict
 * isolation the pad cannot be swapped afterwards either, because bubblewrap binds the device node
 * list at exec time, so remembering the last declared type is the only route to the right pad.
 *
 * Creating a uinput device is not something a unit test can do on every runner, so the invariant
 * is guarded at the source level, the same way test_video_hdr_probe_guard_source.cpp guards its
 * non-fatal-probe contract.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_input_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/input.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(PreallocatedGamepadSource, PreallocationDoesNotPassAnEmptyArrival) {
  const auto source = read_input_source();
  ASSERT_FALSE(source.empty()) << "could not read src/input.cpp via POLARIS_SOURCE_DIR";

  const auto preallocate = source.find("void preallocate_gamepad(");
  ASSERT_NE(preallocate, std::string::npos);
  const auto body_end = source.find("\n  }", preallocate);
  ASSERT_NE(body_end, std::string::npos);
  const auto body = source.substr(preallocate, body_end - preallocate);

  // An empty brace here is the whole defect: type 0 is LI_CTYPE_UNKNOWN and capabilities 0, so
  // the ladder falls through every auto branch to the Xbox default.
  EXPECT_EQ(body.find("platf_input, {id, static_cast<std::uint8_t>(controller_number)}, {},"), std::string::npos)
    << "preallocate_gamepad is passing an empty gamepad_arrival_t again";
  EXPECT_NE(body.find("client_controller_type"), std::string::npos)
    << "preallocate_gamepad no longer uses the remembered controller type";
}

TEST(PreallocatedGamepadSource, ALateArrivalIsRecordedRatherThanOnlyDropped) {
  const auto source = read_input_source();
  ASSERT_FALSE(source.empty());

  const auto already = source.find("ControllerNumber already allocated");
  ASSERT_NE(already, std::string::npos);

  // The record has to happen before the early return, or the client's declared type is lost for
  // the next session too and the user never gets the pad they asked for.
  const auto window_start = already > 700 ? already - 700 : 0;
  const auto window = source.substr(window_start, already - window_start);
  EXPECT_NE(window.find("update_client_declared_controller_type"), std::string::npos)
    << "the dropped controller arrival is no longer recorded for the next launch";
}
