/**
 * @file tests/unit/platform/test_vulkan_loader.cpp
 * @brief Tests that Polaris's Vulkan calls land in the Vulkan loader, not in volk's variables.
 */
// test includes
#include "../../tests_common.h"

// VK_NO_PROTOTYPES is defined exactly when the build uses the Vulkan loader (CUDA interop or the
// Vulkan Video encoder), which is also when vulkan_loader.cpp is compiled in.
#if defined(__linux__) && defined(VK_NO_PROTOTYPES)

  // standard includes
  #include <dlfcn.h>
  #include <string>
  #include <utility>
  #include <vector>

  // local includes
  #include "src/platform/linux/vulkan_loader.h"

namespace {
  /** Anything that lives in this test executable, to name the executable's own mapping. */
  void executable_marker() {}
}  // namespace

TEST(VulkanLoaderTests, EveryEntryPointResolvesWhenTheLoaderIsPresent) {
  if (!platf::vulkan_loader::vkGetInstanceProcAddr) {
    GTEST_SKIP() << "No Vulkan loader on this host";
  }

  EXPECT_TRUE(platf::vulkan_loader::missing_entry_point().empty())
    << "The loader does not export " << platf::vulkan_loader::missing_entry_point();
}

TEST(VulkanLoaderTests, EveryEntryPointLandsInTheLoaderLibraryNotTheExecutable) {
  if (!platf::vulkan_loader::vkGetInstanceProcAddr) {
    GTEST_SKIP() << "No Vulkan loader on this host";
  }

  // The regression this guards: with the compute codec linked in, volk defines every entry point as
  // a global variable in the executable, and a call made through the ordinary prototype bound to
  // that variable, so it jumped into data. Every pointer Polaris calls through must instead sit in
  // the loader's own mapping.
  Dl_info executable {};
  ASSERT_NE(dladdr(reinterpret_cast<void *>(&executable_marker), &executable), 0);

  std::vector<std::pair<std::string, void *>> entry_points;
  #define POLARIS_VK_LOADER_COLLECT(name) \
    entry_points.emplace_back("vk" #name, reinterpret_cast<void *>(platf::vulkan_loader::vk##name));
  POLARIS_VK_LOADER_FNS(POLARIS_VK_LOADER_COLLECT)
  #undef POLARIS_VK_LOADER_COLLECT

  for (const auto &[name, address] : entry_points) {
    if (!address) {
      continue;  // Reported by the resolution test above.
    }
    Dl_info info {};
    ASSERT_NE(dladdr(address, &info), 0) << name << " points outside every loaded object";
    ASSERT_NE(info.dli_fname, nullptr) << name;
    EXPECT_STRNE(info.dli_fname, executable.dli_fname) << name << " resolved into the executable itself";
    EXPECT_NE(std::string {info.dli_fname}.find("libvulkan"), std::string::npos)
      << name << " resolved into " << info.dli_fname << " rather than the Vulkan loader";
  }
}

#endif
