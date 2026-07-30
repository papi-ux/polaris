#include "../tests_common.h"

#include <cstdlib>
#include <fstream>
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
