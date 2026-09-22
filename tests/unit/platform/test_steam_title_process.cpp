/**
 * @file tests/unit/platform/test_steam_title_process.cpp
 * @brief Test how one Steam title's processes are found and which of them are asked to close.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/steam_title_process.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;
namespace st = platf::steam_title;

namespace {
  constexpr uid_t k_player = 1000;
  constexpr uid_t k_someone_else = 1001;

  std::string argv_bytes(std::initializer_list<std::string_view> words) {
    std::string cmdline;
    for (const auto word : words) {
      cmdline.append(word);
      cmdline.push_back('\0');
    }
    return cmdline;
  }

  st::process_t process(pid_t pid, pid_t parent, std::string comm, std::string cmdline, uid_t uid = k_player) {
    st::process_t row;
    row.pid = pid;
    row.parent = parent;
    row.uid = uid;
    row.start_ticks = static_cast<std::uint64_t>(pid) * 10;
    row.comm = std::move(comm);
    row.cmdline = std::move(cmdline);
    return row;
  }

  std::vector<pid_t> pids(const std::vector<st::process_t> &rows) {
    std::vector<pid_t> out;
    for (const auto &row : rows) {
      out.push_back(row.pid);
    }
    return out;
  }

  /// The tree a Proton title has on a Steam Deck in Game Mode, as read there, beside the Steam that
  /// runs the session and another title of the same player.
  std::vector<st::process_t> deck_table() {
    const auto reaper = argv_bytes({"~/.local/share/Steam/ubuntu12_32/reaper", "SteamLaunch", "AppId=813230", "--", "/steam/steam-launch-wrapper", "--", "Animal Well.exe"});
    return {
      process(1625, 1355, "bash", argv_bytes({"bash", "/usr/bin/steam-jupiter"})),
      process(1792, 1625, "steam", argv_bytes({"~/.local/share/Steam/ubuntu12_32/steam", "-gamepadui"})),
      process(2074, 1792, "steam-runtime-l", argv_bytes({"steam-runtime-launcher-service"})),
      process(2894, 1792, "reaper", argv_bytes({"/steam/reaper", "SteamLaunch", "AppId=3127633177", "--", "nova-deck"})),
      process(2895, 2894, "bwrap", argv_bytes({"bwrap", "--args", "27"})),
      process(2929, 2895, "nova-deck", argv_bytes({"nova-deck"})),
      process(16009, 1792, "reaper", reaper),
      process(16012, 16009, "srt-bwrap", argv_bytes({"srt-bwrap", "--args", "31"})),
      process(16106, 16012, "pv-adverb", argv_bytes({"pv-adverb", "--subreaper", "--", "Animal Well.exe"})),
      process(16241, 16106, "Animal Well.exe", argv_bytes({"Z:\\games\\Animal Well.exe"})),
      process(16300, 16106, "wineserver", argv_bytes({"wineserver"})),
    };
  }

  struct scratch_t {
    fs::path root;

    scratch_t() {
      auto pattern = (fs::temp_directory_path() / "polaris-steam-title-XXXXXX").string();
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

    void add(const std::string &name, const std::string &stat, const std::string &cmdline) const {
      fs::create_directories(root / name);
      std::ofstream {root / name / "stat", std::ios::binary} << stat;
      std::ofstream {root / name / "cmdline", std::ios::binary} << cmdline;
    }
  };
}  // namespace

TEST(SteamTitleProcess, ATitleIsRecognisedByTheWaySteamStartsIt) {
  const auto reaper = argv_bytes({"/steam/reaper", "SteamLaunch", "AppId=813230", "--", "game"});
  EXPECT_TRUE(st::launch_cmdline_matches_appid(reaper, "813230"));
  EXPECT_FALSE(st::launch_cmdline_matches_appid(reaper, "81323")) << "a shorter appid is another title";
  EXPECT_FALSE(st::launch_cmdline_matches_appid(reaper, "8132300"));
  EXPECT_FALSE(st::launch_cmdline_matches_appid(reaper, ""));
  EXPECT_FALSE(st::launch_cmdline_matches_appid(reaper, "813230x"));

  // A title that only mentions the words, apart or in its own arguments, is not Steam starting it.
  EXPECT_FALSE(st::launch_cmdline_matches_appid(argv_bytes({"game", "AppId=813230"}), "813230"));
  EXPECT_FALSE(st::launch_cmdline_matches_appid(argv_bytes({"game", "SteamLaunch", "--", "AppId=813230"}), "813230"));
  EXPECT_FALSE(st::launch_cmdline_matches_appid(argv_bytes({"game", "--note=SteamLaunch AppId=813230x"}), "813230"));
}

TEST(SteamTitleProcess, AShortcutIsFoundByTheIdTheReaperCarries) {
  // A non-Steam shortcut launches as steam://rungameid/<64-bit game id>, and the reaper names the
  // shortcut's 32-bit id. The Deck's shortcut to the Nova app is AppId=3127633177.
  const std::uint64_t shortcut = 3127633177ull;
  const auto game_id = std::to_string((shortcut << 32) | 0x02000000ull);
  EXPECT_EQ(st::launch_appid(game_id), "3127633177");
  EXPECT_TRUE(st::running(deck_table(), st::launch_appid(game_id), k_player));

  EXPECT_EQ(st::launch_appid("813230"), "813230") << "a Steam game's id is its appid";
  EXPECT_EQ(st::launch_appid("4294967295"), "4294967295") << "the largest 32-bit id is still an appid";
  EXPECT_EQ(st::launch_appid("abc"), "abc");
  EXPECT_EQ(st::launch_appid("99999999999999999999999"), "99999999999999999999999") << "too long for 64 bits";
}

TEST(SteamTitleProcess, RunningMeansThisAccountsSteamStartedThisTitle) {
  const auto table = deck_table();
  EXPECT_TRUE(st::running(table, "813230", k_player));
  EXPECT_TRUE(st::running(table, "3127633177", k_player));
  EXPECT_FALSE(st::running(table, "367520", k_player)) << "installed is not running";
  EXPECT_FALSE(st::running(table, "813230", k_someone_else)) << "another account's title is not this host's";
}

TEST(SteamTitleProcess, TheTitleIsAskedFromInsideItsContainerAndSteamIsNot) {
  const auto asked = pids(st::processes_to_ask(deck_table(), "813230", k_player));

  // The container's supervisor first, then what it supervises.
  EXPECT_EQ(asked, (std::vector<pid_t> {16106, 16241, 16300}));

  for (const pid_t left_alone : {1625, 1792, 2074}) {
    EXPECT_EQ(std::count(asked.begin(), asked.end(), left_alone), 0) << "Steam and the session are never asked: " << left_alone;
  }
  EXPECT_EQ(std::count(asked.begin(), asked.end(), 16009), 0) << "the reaper is what Steam waits on";
  EXPECT_EQ(std::count(asked.begin(), asked.end(), 16012), 0)
    << "a signalled bwrap exits at once and the kernel kills the title instead of letting it close";
  for (const pid_t other_title : {2894, 2895, 2929}) {
    EXPECT_EQ(std::count(asked.begin(), asked.end(), other_title), 0) << "another title of the same player: " << other_title;
  }
}

TEST(SteamTitleProcess, ATitleWithNoContainerIsAskedDirectly) {
  const std::vector<st::process_t> table {
    process(1792, 1625, "steam", argv_bytes({"steam", "-gamepadui"})),
    process(500, 1792, "reaper", argv_bytes({"/steam/reaper", "SteamLaunch", "AppId=367520", "--", "/games/hollow_knight.sh"})),
    process(501, 500, "hollow_knight.s", argv_bytes({"/bin/sh", "/games/hollow_knight.sh"})),
    process(502, 501, "hollow_knight.x", argv_bytes({"/games/hollow_knight.x86_64"})),
  };
  EXPECT_EQ(pids(st::processes_to_ask(table, "367520", k_player)), (std::vector<pid_t> {501, 502}))
    << "a launcher script does not pass the request on, so the title hears it too";
}

TEST(SteamTitleProcess, TheLaunchWrapperAboveTheReaperIsOneRootNotTwo) {
  // Some Steam builds keep steam-launch-wrapper as the parent of the reaper, and both command lines
  // name the title.
  const auto wrapped = argv_bytes({"/steam/steam-launch-wrapper", "--", "/steam/reaper", "SteamLaunch", "AppId=813230", "--", "game"});
  const std::vector<st::process_t> table {
    process(700, 1792, "steam-launch-wr", wrapped),
    process(701, 700, "reaper", argv_bytes({"/steam/reaper", "SteamLaunch", "AppId=813230", "--", "game"})),
    process(702, 701, "game", argv_bytes({"game"})),
  };
  EXPECT_EQ(pids(st::processes_to_ask(table, "813230", k_player)), (std::vector<pid_t> {702}));
}

TEST(SteamTitleProcess, AShellThatOnlyMentionsTheLaunchIsNotTheTitle) {
  // A pgrep for the title, or a shell script that names it, has the same words in its command line.
  auto table = deck_table();
  table.push_back(process(18000, 1, "bash", argv_bytes({"bash", "-c", "pgrep -f 'SteamLaunch AppId=813230'"})));
  table.push_back(process(18001, 18000, "pgrep", argv_bytes({"pgrep", "-f", "SteamLaunch AppId=813230"})));

  EXPECT_EQ(pids(st::processes_to_ask(table, "813230", k_player)), (std::vector<pid_t> {16106, 16241, 16300}))
    << "the title is still found, and the shell is not a second launch that would make it ambiguous";

  std::vector<st::process_t> only_the_shell {
    process(18000, 1, "bash", argv_bytes({"bash", "-c", "echo SteamLaunch AppId=367520"})),
    process(18001, 18000, "sleep", argv_bytes({"sleep", "60"})),
  };
  EXPECT_FALSE(st::running(only_the_shell, "367520", k_player));
  EXPECT_TRUE(st::processes_to_ask(only_the_shell, "367520", k_player).empty())
    << "and nothing under it is ever asked to close";
}

TEST(SteamTitleProcess, NothingIsAskedWhenItIsNotClearWhichLaunchIsMeant) {
  auto table = deck_table();
  EXPECT_TRUE(st::processes_to_ask(table, "367520", k_player).empty()) << "not running";
  EXPECT_TRUE(st::processes_to_ask(table, "813230", k_someone_else).empty());

  // A second, separate launch of the same appid: nothing here can say which one this host started.
  table.push_back(process(17000, 1792, "reaper", argv_bytes({"/steam/reaper", "SteamLaunch", "AppId=813230", "--", "game"})));
  table.push_back(process(17001, 17000, "game", argv_bytes({"game"})));
  EXPECT_TRUE(st::processes_to_ask(table, "813230", k_player).empty());
  EXPECT_TRUE(st::running(table, "813230", k_player)) << "it is still reported as running, so the host can say so";
}

TEST(SteamTitleProcess, TheProcessTableIsReadFromProcAndBrokenRowsAreLeftOut) {
  const scratch_t scratch;
  // A name with a space and a bracket in it, which is why the name ends at the LAST bracket.
  scratch.add("4242", "4242 (Animal (Well) x) S 16106 4242 4242 0 -1 4194304 1 2 3 4 5 6 7 8 20 0 12 0 987654 1000 200", argv_bytes({"Z:\\Animal Well.exe"}));
  scratch.add("16106", "16106 (pv-adverb) S 16012 16106 16106 0 -1 4194304 1 2 3 4 5 6 7 8 20 0 1 0 555 1000 200", argv_bytes({"pv-adverb", "--subreaper"}));
  scratch.add("77", "77 (cut-short) S 1 77", argv_bytes({"cut-short"}));
  scratch.add("1", "1 (systemd) S 0 1 1 0 -1 4194304 1 2 3 4 5 6 7 8 20 0 1 0 1 1000 200", argv_bytes({"systemd"}));
  scratch.add("self", "not a pid", "");
  fs::create_directories(scratch.root / "99");  // a process that went away between the listing and the read

  const auto table = st::read_process_table(scratch.root);
  ASSERT_EQ(pids(table), (std::vector<pid_t> {4242, 16106}));
  EXPECT_EQ(table[0].comm, "Animal (Well) x");
  EXPECT_EQ(table[0].parent, 16106);
  EXPECT_EQ(table[0].start_ticks, 987654u);
  EXPECT_EQ(table[0].cmdline, argv_bytes({"Z:\\Animal Well.exe"}));
  EXPECT_EQ(table[0].uid, getuid());
  EXPECT_EQ(table[1].start_ticks, 555u);
}

TEST(SteamTitleProcess, AskingATitleThatIsNotRunningDoesNothing) {
  const auto result = st::ask_to_close("4294967295");
  EXPECT_FALSE(result.was_running);
  EXPECT_EQ(result.asked, 0);
  EXPECT_EQ(result.gone, 0);
}

#endif
