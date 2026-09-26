/**
 * @file src/platform/linux/vulkan_loader.cpp
 * @brief The Vulkan entry points Polaris calls itself, resolved from the loader by name.
 */
// standard includes
#include <dlfcn.h>

// local includes
#include "vulkan_loader.h"

namespace platf::vulkan_loader {
  namespace {
    /**
     * The loader library, opened once. The soname first, because that is what a machine that can run
     * Vulkan has; the unversioned name is a development symlink and may well be absent. Opening it is
     * cheap when the process already maps it, and nothing here logs, because this runs while Polaris
     * is still starting up and logging is not ready yet.
     */
    void *loader_library() {
      static void *const library = [] {
        for (const auto *candidate : {"libvulkan.so.1", "libvulkan.so"}) {
          if (auto *opened = dlopen(candidate, RTLD_NOW | RTLD_LOCAL)) {
            return opened;
          }
        }
        return static_cast<void *>(nullptr);
      }();
      return library;
    }

    /**
     * The loader's own exported symbol, the same function a direct link to libvulkan would have
     * called. dlsym on the loader's handle searches only the loader and what it depends on, never
     * the executable, so volk's variables of the same name are out of reach.
     */
    template<class T>
    T resolve(const char *name) {
      auto *library = loader_library();
      return library ? reinterpret_cast<T>(dlsym(library, name)) : nullptr;
    }
  }  // namespace

#define POLARIS_VK_LOADER_DEFINE(name) PFN_vk##name vk##name = resolve<PFN_vk##name>("vk" #name);
  POLARIS_VK_LOADER_FNS(POLARIS_VK_LOADER_DEFINE)
#undef POLARIS_VK_LOADER_DEFINE

  std::string_view missing_entry_point() {
#define POLARIS_VK_LOADER_CHECK(name) \
  if (!vk##name) { \
    return "vk" #name; \
  }
    POLARIS_VK_LOADER_FNS(POLARIS_VK_LOADER_CHECK)
#undef POLARIS_VK_LOADER_CHECK
    return {};
  }
}  // namespace platf::vulkan_loader
