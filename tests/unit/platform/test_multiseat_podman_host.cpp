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

#endif
