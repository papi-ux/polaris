/**
 * @file tests/unit/platform/test_multiseat_podman_host.cpp
 * @brief Live-host adapter tests for the rootless Podman worker backend.
 */
#include "src/platform/linux/multiseat_podman_host.h"

#ifdef __linux__

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
  class temporary_directory_t {
  public:
    temporary_directory_t() {
      auto pattern =
        (std::filesystem::temp_directory_path() / "polaris-podman-host-XXXXXX").string();
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {"temporary directory could not be created"};
      }
      path_ = created;
    }

    ~temporary_directory_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    temporary_directory_t(const temporary_directory_t &) = delete;
    temporary_directory_t &operator=(const temporary_directory_t &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  void write_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(out.good()) << path;
  }
}  // namespace

TEST(MultiseatPodmanHost, ReadsOwnedRegularFilesWithinTheBound) {
  temporary_directory_t root;
  multiseat::podman::local_host_t host;
  const std::string content {"{\"mounts\": []}\0tail", 20};
  write_file(root.path() / "config.json", content);
  write_file(root.path() / "empty.json", "");

  EXPECT_EQ(
    host.read_owned_regular_file(root.path() / "config.json", content.size()),
    content
  );
  EXPECT_EQ(
    host.read_owned_regular_file(root.path() / "config.json", 1U << 20U),
    content
  );
  EXPECT_FALSE(
    host.read_owned_regular_file(root.path() / "config.json", content.size() - 1)
  );
  EXPECT_EQ(host.read_owned_regular_file(root.path() / "empty.json", 16), std::string {});
}

TEST(MultiseatPodmanHost, RefusesAnythingButAnOwnedRegularFile) {
  temporary_directory_t root;
  multiseat::podman::local_host_t host;
  write_file(root.path() / "config.json", "{}");
  std::filesystem::create_symlink(root.path() / "config.json", root.path() / "link.json");
  ASSERT_EQ(::mkfifo((root.path() / "pipe").c_str(), 0600), 0);

  EXPECT_FALSE(host.read_owned_regular_file(root.path() / "link.json", 1024));
  EXPECT_FALSE(host.read_owned_regular_file(root.path(), 1024));
  EXPECT_FALSE(host.read_owned_regular_file(root.path() / "pipe", 1024));
  EXPECT_FALSE(host.read_owned_regular_file(root.path() / "missing.json", 1024));
  EXPECT_FALSE(host.read_owned_regular_file("/dev/null", 1024));
  if (::geteuid() != 0) {
    // Owned by root, readable by everyone, and still refused.
    EXPECT_FALSE(host.read_owned_regular_file("/proc/version", 1U << 16U));
  }
}

TEST(MultiseatPodmanHost, ReadsRunningProcessGroupsInsteadOfAccountMembership) {
  multiseat::podman::local_host_t host;
  const auto actual = host.supplementary_groups();
  ASSERT_TRUE(actual);
  const auto count = getgroups(0, nullptr);
  ASSERT_GE(count, 0);
  std::vector<gid_t> groups(std::max(count, 1));
  const auto received = getgroups(static_cast<int>(groups.size()), groups.data());
  ASSERT_GE(received, 0);
  std::vector<std::uint64_t> expected(groups.begin(), groups.begin() + received);
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  EXPECT_EQ(*actual, expected);
}

TEST(MultiseatPodmanHost, RefusesUserControlledOrIndirectRuntime) {
  temporary_directory_t root;
  multiseat::podman::local_host_t host;
  write_file(root.path() / "crun", "executable");
  ASSERT_EQ(chmod((root.path() / "crun").c_str(), 0755), 0);
  std::filesystem::create_directory(root.path() / "links");
  std::filesystem::create_symlink(root.path() / "crun", root.path() / "links/crun");
  std::filesystem::create_directory_symlink("/usr/bin", root.path() / "bin-link");
  EXPECT_FALSE(host.trusted_runtime_file(root.path() / "crun"));
  EXPECT_FALSE(host.trusted_runtime_file(root.path() / "links/crun"));
  EXPECT_FALSE(host.trusted_runtime_file(root.path() / "bin-link/crun"));
  EXPECT_FALSE(host.trusted_runtime_file("crun"));
  EXPECT_FALSE(host.trusted_runtime_file("/usr/bin/runc"));
  EXPECT_FALSE(host.trusted_runtime_file("/usr/bin/../bin/crun"));
}

#endif
