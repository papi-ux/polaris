/**
 * @file src/platform/linux/vulkan_loader.h
 * @brief The Vulkan entry points Polaris calls itself, resolved from the loader by name.
 */
#pragma once

// Polaris is built with VK_NO_PROTOTYPES, so no translation unit can call a Vulkan entry point by
// its bare C name. The compute codec links volk in, and volk defines every entry point as a global
// function pointer variable under that same bare name. A call made through the ordinary prototype
// then binds, at link time, to volk's variable instead of to the loader, and the call lands on the
// address of a pointer rather than on code. Resolving by name, into pointers of Polaris's own that
// live in a namespace, leaves nothing for the linker to choose between.
#ifndef VK_NO_PROTOTYPES
  #error "Polaris resolves Vulkan entry points by name: build with VK_NO_PROTOTYPES (cmake/compile_definitions/linux.cmake)"
#endif

// standard includes
#include <string_view>

// lib includes
#include <vulkan/vulkan.h>

/**
 * Every core entry point the Vulkan Video encoder and the CUDA DMA-BUF interop call directly.
 * Extension entry points are not listed: those are already fetched through vkGetInstanceProcAddr
 * or vkGetDeviceProcAddr at their call sites, because the loader does not export them.
 */
#define POLARIS_VK_LOADER_FNS(X) \
  X(AllocateCommandBuffers) \
  X(AllocateDescriptorSets) \
  X(AllocateMemory) \
  X(BeginCommandBuffer) \
  X(BindBufferMemory) \
  X(BindImageMemory) \
  X(CmdBindDescriptorSets) \
  X(CmdBindPipeline) \
  X(CmdCopyBuffer) \
  X(CmdCopyBufferToImage) \
  X(CmdDispatch) \
  X(CmdPipelineBarrier) \
  X(CmdPushConstants) \
  X(CreateBuffer) \
  X(CreateCommandPool) \
  X(CreateComputePipelines) \
  X(CreateDescriptorPool) \
  X(CreateDescriptorSetLayout) \
  X(CreateDevice) \
  X(CreateFence) \
  X(CreateImage) \
  X(CreateImageView) \
  X(CreateInstance) \
  X(CreatePipelineLayout) \
  X(CreateSampler) \
  X(CreateShaderModule) \
  X(DestroyBuffer) \
  X(DestroyCommandPool) \
  X(DestroyDescriptorPool) \
  X(DestroyDescriptorSetLayout) \
  X(DestroyDevice) \
  X(DestroyFence) \
  X(DestroyImage) \
  X(DestroyImageView) \
  X(DestroyInstance) \
  X(DestroyPipeline) \
  X(DestroyPipelineLayout) \
  X(DestroySampler) \
  X(DestroyShaderModule) \
  X(DeviceWaitIdle) \
  X(EndCommandBuffer) \
  X(EnumerateDeviceExtensionProperties) \
  X(EnumeratePhysicalDevices) \
  X(FlushMappedMemoryRanges) \
  X(FreeMemory) \
  X(GetBufferMemoryRequirements) \
  X(GetDeviceProcAddr) \
  X(GetDeviceQueue) \
  X(GetImageMemoryRequirements) \
  X(GetImageSubresourceLayout) \
  X(GetInstanceProcAddr) \
  X(GetPhysicalDeviceExternalBufferProperties) \
  X(GetPhysicalDeviceFormatProperties) \
  X(GetPhysicalDeviceFormatProperties2) \
  X(GetPhysicalDeviceMemoryProperties) \
  X(GetPhysicalDeviceProperties) \
  X(GetPhysicalDeviceProperties2) \
  X(GetPhysicalDeviceQueueFamilyProperties) \
  X(MapMemory) \
  X(QueueSubmit) \
  X(ResetCommandBuffer) \
  X(ResetFences) \
  X(UnmapMemory) \
  X(UpdateDescriptorSets) \
  X(WaitForFences)

namespace platf::vulkan_loader {
  // One pointer per entry point, named exactly like the C function so call sites read as plain
  // Vulkan once they say `using namespace platf::vulkan_loader`. Each is resolved from the loader
  // library when Polaris starts, and is null when the loader is absent or does not export it.
#define POLARIS_VK_LOADER_DECLARE(name) extern PFN_vk##name vk##name;
  POLARIS_VK_LOADER_FNS(POLARIS_VK_LOADER_DECLARE)
#undef POLARIS_VK_LOADER_DECLARE

  /**
   * @brief The first entry point the Vulkan loader did not provide.
   * @return The entry point's name, or an empty view when every one resolved.
   */
  std::string_view missing_entry_point();
}  // namespace platf::vulkan_loader
