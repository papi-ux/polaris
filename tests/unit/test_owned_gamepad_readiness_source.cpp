/**
 * @file tests/unit/test_owned_gamepad_readiness_source.cpp
 * @brief Source guard: a virtual DualSense must have its own input nodes before Polaris registers
 *        them for a private launch, and a pad whose nodes never appear must not be registered empty.
 *
 * inputtino's PS5Joypad is a UHID device. create() returns once UHID_CREATE2 is written; the kernel
 * then binds hid-playstation, which asks inputtino's own UHID thread for its pairing and firmware
 * reports before it registers the gamepad, motion and touchpad inputs. PS5Joypad::get_nodes() is a
 * single sysfs enumeration, so reading it as create() returns can see none of those inputs. The
 * registry keeps that first answer, and strict bubblewrap isolation later binds exactly the
 * registered list at exec time, so a game launched that way sees no controller at all.
 *
 * The Xbox and Switch pads are uinput devices whose nodes exist when create() returns, so they keep
 * registering directly. Creating a UHID device is not something a unit test can do on every runner,
 * so this invariant is guarded at the source level, like test_preallocated_gamepad_source.cpp; the
 * readiness behaviour itself is covered in tests/unit/platform/test_gamepad_node_readiness.cpp.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_gamepad_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/input/inputtino_gamepad.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  /// The body of one `case X:` in alloc()'s controller-type switch, up to the next case or the end of the switch.
  std::string case_body(const std::string &source, const std::string &label) {
    const auto start = source.find("case " + label + ":");
    if (start == std::string::npos) {
      return {};
    }
    auto end = source.find("\n      case ", start + 1);
    if (end == std::string::npos) {
      end = source.find("\n    return -1;", start);
    }
    return end == std::string::npos ? std::string {} : source.substr(start, end - start);
  }

}  // namespace

TEST(OwnedGamepadReadinessSource, DualSenseDoesNotRegisterAOneShotNodeSnapshot) {
  const auto source = read_gamepad_source();
  ASSERT_FALSE(source.empty()) << "could not read inputtino_gamepad.cpp via POLARIS_SOURCE_DIR";

  const auto body = case_body(source, "DualSenseWired");
  ASSERT_FALSE(body.empty()) << "could not find the DualSenseWired allocation";

  // The defect: the asynchronous UHID pad registered whatever get_nodes() returned at once.
  EXPECT_EQ(body.find("register_created_joypad_nodes("), std::string::npos)
    << "the DualSense allocation registers an immediate get_nodes() snapshot again";
  EXPECT_NE(body.find("wait_for_owned_ds5_nodes("), std::string::npos)
    << "the DualSense allocation no longer waits, bounded, for its own nodes";
  EXPECT_NE(body.find("register_virtual_gamepad_nodes(id.globalIndex, ready.nodes)"), std::string::npos)
    << "the DualSense allocation must register the nodes its readiness wait returned";
}

TEST(OwnedGamepadReadinessSource, DualSenseWithNoOwnNodesFailsInsteadOfRegisteringNone) {
  const auto source = read_gamepad_source();
  ASSERT_FALSE(source.empty());

  const auto body = case_body(source, "DualSenseWired");
  ASSERT_FALSE(body.empty());

  const auto unusable = body.find("!ready.usable()");
  ASSERT_NE(unusable, std::string::npos)
    << "a DualSense whose own nodes never appeared must be detected before registration";
  const auto failed = body.find("return -1;", unusable);
  const auto registered = body.find("register_virtual_gamepad_nodes(", unusable);
  ASSERT_NE(failed, std::string::npos) << "an unusable DualSense must fail the allocation";
  EXPECT_TRUE(registered == std::string::npos || failed < registered)
    << "an unusable DualSense must fail before anything is registered";
}

TEST(OwnedGamepadReadinessSource, ReadinessIsBoundedAndUsesOnlyTheOwnedDevice) {
  const auto source = read_gamepad_source();
  ASSERT_FALSE(source.empty());

  const auto helper = source.find("wait_for_owned_ds5_nodes(const inputtino::PS5Joypad &");
  ASSERT_NE(helper, std::string::npos) << "the bounded DualSense readiness wait is missing";
  const auto helper_end = source.find("\n  }", helper);
  ASSERT_NE(helper_end, std::string::npos);
  const auto helper_body = source.substr(helper, helper_end - helper);

  EXPECT_NE(helper_body.find("readiness::wait_for_owned_nodes("), std::string::npos);
  EXPECT_NE(helper_body.find("readiness::policy_t"), std::string::npos)
    << "the wait must use the bounded readiness policy";
  // Only the pad's own enumeration (matched by its MAC/uniq inside inputtino) may supply nodes;
  // scanning /dev/input would admit unrelated host controllers into the private session.
  EXPECT_NE(helper_body.find("joypad.get_nodes()"), std::string::npos);
  EXPECT_EQ(helper_body.find("/dev/input"), std::string::npos)
    << "the readiness wait must not enumerate /dev/input itself";
  EXPECT_EQ(helper_body.find("directory_iterator"), std::string::npos)
    << "the readiness wait must not scan devices beyond the owned pad";
}

TEST(OwnedGamepadReadinessSource, UinputPadsKeepRegisteringTheirSynchronousNodes) {
  const auto source = read_gamepad_source();
  ASSERT_FALSE(source.empty());

  for (const auto *label : {"XboxOneWired", "SwitchProWired"}) {
    const auto body = case_body(source, label);
    ASSERT_FALSE(body.empty()) << label;
    EXPECT_NE(body.find("register_created_joypad_nodes(id.globalIndex, *gamepad->joypad)"), std::string::npos)
      << label << " is a uinput pad whose nodes exist on return and keeps registering them directly";
  }
}
