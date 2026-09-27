#include <volk.h>

bool exercise_volk(PFN_vkGetInstanceProcAddr loader) {
  // Use the pinned implementation and the same unqualified names as Granite.
  volkInitializeCustom(loader);
  if (!vkCreateInstance) return false;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(nullptr, nullptr, &instance) != VK_SUCCESS || !instance) return false;
  VolkInstanceTable table{};
  volkLoadInstanceTable(&table, instance);
  if (!table.vkDestroyInstance) return false;
  table.vkDestroyInstance(instance, nullptr);
  volkFinalize();
  return true;
}
