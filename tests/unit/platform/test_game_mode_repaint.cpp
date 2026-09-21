/**
 * @file tests/unit/platform/test_game_mode_repaint.cpp
 * @brief Test how a still Steam Game Mode screen is asked for the first frame of a stream.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/game_mode_repaint.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;
namespace gm = platf::game_mode_host;

namespace {
  struct scratch_t {
    fs::path root;

    scratch_t() {
      auto pattern = (fs::temp_directory_path() / "polaris-game-mode-repaint-XXXXXX").string();
      const char *made = mkdtemp(pattern.data());
      if (made == nullptr) {
        throw std::runtime_error("mkdtemp failed");
      }
      root = made;
    }

    ~scratch_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    void touch(const std::string &name) const {
      std::ofstream {root / name} << "";
    }
  };

  std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int value : values) {
      out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
  }
}  // namespace

TEST(GameModeRepaint, TheFocusDisplayIsReadFromTheFirstWordGamescopeWrites) {
  // What a Steam Deck really publishes: ":1" and its terminator in the first word, then two words
  // of whatever followed the string in gamescope's memory.
  const auto deck = bytes({':', '1', 0, 0, 0, 0, 0, 0, 80, 0, 0, 0});
  EXPECT_EQ(gm::focus_display_from_property(deck), std::optional<std::string> {":1"});

  const auto two_digits = bytes({':', '1', '2', 0});
  EXPECT_EQ(gm::focus_display_from_property(two_digits), std::optional<std::string> {":12"});
}

TEST(GameModeRepaint, AFocusDisplayThatIsNotAPlainLocalDisplayIsRefused) {
  // The answer goes straight to a connect call, so nothing but ":<number>" gets through.
  EXPECT_EQ(gm::focus_display_from_property(bytes({})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({0, 0, 0, 0})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({':', 0, 0, 0})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({':', 'a', 0, 0})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({'h', ':', '0', 0})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({':', '-', '1', 0})), std::nullopt);

  // A name too long for the first word lost its tail on the way, so what is left cannot be trusted.
  EXPECT_EQ(gm::focus_display_from_property(bytes({':', '1', '0', '0', 0, 0, 0, 0})), std::nullopt);
  EXPECT_EQ(gm::focus_display_from_property(bytes({':', '1'})), std::nullopt);
}

TEST(GameModeRepaint, LocalDisplaysComeFromTheSocketsLowestFirst) {
  const scratch_t scratch;
  scratch.touch("X1");
  scratch.touch("X0");
  scratch.touch("X10");
  scratch.touch("X");
  scratch.touch("Xfoo");
  scratch.touch("X-1");
  scratch.touch("X1234");
  scratch.touch("lock");

  EXPECT_EQ(gm::local_x_displays(scratch.root), (std::vector<std::string> {":0", ":1", ":10"}));
  EXPECT_TRUE(gm::local_x_displays(scratch.root / "absent").empty());
}

TEST(GameModeRepaint, AStaleSocketDirectoryCannotTurnOneRepaintIntoALongWalk) {
  const scratch_t scratch;
  for (int number = 0; number < 40; ++number) {
    scratch.touch("X" + std::to_string(number));
  }

  const auto displays = gm::local_x_displays(scratch.root);
  ASSERT_EQ(displays.size(), 8u);
  EXPECT_EQ(displays.front(), ":0");
  EXPECT_EQ(displays.back(), ":7");
}

TEST(GameModeRepaint, AGameModeCaptureAsksUntilTheFirstFrameIsIn) {
  gm::first_frame_t first_frame {true};

  EXPECT_TRUE(first_frame.ask()) << "as the capture starts";
  EXPECT_TRUE(first_frame.ask()) << "after a wait that ended with no frame";

  first_frame.frame_arrived();
  EXPECT_FALSE(first_frame.ask()) << "a still screen after the first frame is the picture the stream already has";
}

TEST(GameModeRepaint, AScreenThatNeverAnswersIsNotAskedForever) {
  gm::first_frame_t first_frame {true};

  int asked = 0;
  for (int wait = 0; wait < 100; ++wait) {
    asked += first_frame.ask() ? 1 : 0;
  }
  EXPECT_EQ(asked, gm::first_frame_t::k_attempts);
}

TEST(GameModeRepaint, ACaptureOutsideGameModeNeverAsks) {
  gm::first_frame_t first_frame {false};
  EXPECT_FALSE(first_frame.ask());
  EXPECT_FALSE(first_frame.ask());
}

#endif
