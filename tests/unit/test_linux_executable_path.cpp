#include "../tests_common.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "src/platform/linux/executable_path.h"

TEST(LinuxExecutablePath, FindsExecutableWithoutConstructingEnvironmentLocale) {
  const std::string dir = "/tmp/polaris-path-search-" + std::to_string(getpid());
  ASSERT_EQ(0, mkdir(dir.c_str(), 0700));
  const std::string executable = dir + "/xdg-open";
  {
    std::ofstream file(executable);
    file << "#!/bin/sh\nexit 0\n";
  }
  ASSERT_EQ(0, chmod(executable.c_str(), 0700));

  const char *lang_value = std::getenv("LANG");
  const char *lc_all_value = std::getenv("LC_ALL");
  const std::string old_lang = lang_value ? lang_value : "";
  const std::string old_lc_all = lc_all_value ? lc_all_value : "";
  const bool had_lang = lang_value != nullptr;
  const bool had_lc_all = lc_all_value != nullptr;
  setenv("LANG", "zz_ZZ.UTF-8", 1);
  setenv("LC_ALL", "zz_ZZ.UTF-8", 1);
  EXPECT_EQ(executable, platf::linux_util::find_executable_in_path("xdg-open", dir.c_str()));
  EXPECT_EQ(executable, platf::linux_util::find_executable_in_path(executable, ""));
  EXPECT_TRUE(platf::linux_util::find_executable_in_path("missing", dir.c_str()).empty());

  unlink(executable.c_str());
  rmdir(dir.c_str());
  if (had_lang) {
    setenv("LANG", old_lang.c_str(), 1);
  } else {
    unsetenv("LANG");
  }
  if (had_lc_all) {
    setenv("LC_ALL", old_lc_all.c_str(), 1);
  } else {
    unsetenv("LC_ALL");
  }
}

TEST(LinuxExecutablePath, RejectsDirectoriesAndNonExecutableFiles) {
  const std::string dir = "/tmp/polaris-path-types-" + std::to_string(getpid());
  ASSERT_EQ(0, mkdir(dir.c_str(), 0700));
  const std::string plain = dir + "/plain";
  {
    std::ofstream file(plain);
    file << "not executable";
  }
  EXPECT_TRUE(platf::linux_util::find_executable_in_path("plain", dir.c_str()).empty());
  EXPECT_TRUE(platf::linux_util::find_executable_in_path(dir, "").empty());
  unlink(plain.c_str());
  rmdir(dir.c_str());
}

TEST(LinuxExecutablePath, SteamCoverSynthesisUsesLocaleIndependentExecutableLookup) {
  std::ifstream source(std::string {POLARIS_SOURCE_DIR} + "/src/confighttp.cpp");
  ASSERT_TRUE(source.is_open());
  std::ostringstream contents;
  contents << source.rdbuf();
  const auto source_text = contents.str();

  const auto function_start = source_text.find("bool synthesize_steam_cover_from_header(");
  const auto function_end = source_text.find("std::optional<std::string> download_best_steam_cover(", function_start);
  ASSERT_NE(function_start, std::string::npos);
  ASSERT_NE(function_end, std::string::npos);
  const auto function_text = source_text.substr(function_start, function_end - function_start);

  const auto linux_guard = function_text.find("#ifdef __linux__");
  const auto linux_lookup = function_text.find(
    "platf::linux_util::find_executable_in_path(\"magick\")",
    linux_guard
  );
  const auto fallback_guard = function_text.find("#else", linux_lookup);
  const auto fallback_lookup = function_text.find(
    "boost::process::v1::search_path(\"magick\")",
    fallback_guard
  );
  const auto guard_end = function_text.find("#endif", fallback_lookup);
  ASSERT_NE(linux_guard, std::string::npos);
  ASSERT_NE(linux_lookup, std::string::npos);
  ASSERT_NE(fallback_guard, std::string::npos);
  ASSERT_NE(fallback_lookup, std::string::npos);
  ASSERT_NE(guard_end, std::string::npos);
  EXPECT_LT(linux_guard, linux_lookup);
  EXPECT_LT(linux_lookup, fallback_guard);
  EXPECT_LT(fallback_guard, fallback_lookup);
  EXPECT_LT(fallback_lookup, guard_end);
}
