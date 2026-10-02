#include <iostream>
#include <vulkan/vulkan.h>

extern "C" PFN_vkGetInstanceProcAddr polaris_test_loader();
extern "C" unsigned polaris_test_creates();
extern "C" unsigned polaris_test_destroys();
bool exercise_volk(PFN_vkGetInstanceProcAddr loader);

int main() {
  // These are ordinary function prototypes, as in Polaris's Vulkan/CUDA paths.
  // The only linked implementation is the fixture library; no GPU is consulted.
  VkInstance first = VK_NULL_HANDLE;
  if (vkCreateInstance(nullptr, nullptr, &first) != VK_SUCCESS || !first) return 1;
  if (!exercise_volk(polaris_test_loader())) return 2;
  vkDestroyInstance(first, nullptr);
  VkInstance second = VK_NULL_HANDLE;
  if (vkCreateInstance(nullptr, nullptr, &second) != VK_SUCCESS || !second) return 3;
  vkDestroyInstance(second, nullptr);
  if (polaris_test_creates() != 3 || polaris_test_destroys() != 3) {
    std::cerr << "Host and volk calls did not reach the same fake loader\n";
    return 4;
  }
  std::cout << "Host Vulkan functions and volk pointers coexist under LTO\n";
  return 0;
}
