#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>

namespace {
unsigned creates = 0, destroys = 0;
}

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
  const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *instance
) {
  ++creates;
  *instance = reinterpret_cast<VkInstance>(uintptr_t{0x1234});
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *) {
  if (instance == reinterpret_cast<VkInstance>(uintptr_t{0x1234})) ++destroys;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *name) {
  if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
  if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
  return nullptr;
}

PFN_vkGetInstanceProcAddr polaris_test_loader() { return &vkGetInstanceProcAddr; }
unsigned polaris_test_creates() { return creates; }
unsigned polaris_test_destroys() { return destroys; }
}
