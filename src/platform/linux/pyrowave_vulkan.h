/**
 * @file src/platform/linux/pyrowave_vulkan.h
 * @brief The Vulkan device Polaris owns and PyroWave borrows.
 */
#pragma once

// PyroWave's header refuses to compile unless the Vulkan API is already declared, and says so with
// an #error. The system header, not the volk copy the vendored tree builds against; the note at the
// top of pyrowave_encode.cpp says why that distinction matters.
#include <vulkan/vulkan.h>

#include "pyrowave.h"

// standard includes
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pyrowave_encode {

  /**
   * @brief A Vulkan device Polaris creates and the codec borrows.
   *
   * PyroWave will make its own device, and while the host only hands it planes in system memory that
   * is the better trade: no interop to get wrong. It stops being enough the moment the picture
   * should stay on the GPU. `pyrowave_device_get_vk_device_handles` reports the instance, the
   * physical device and the device, and nothing else. No queue and no family index, so there is
   * nothing to record a copy, a barrier or a layout transition against. The codec's C API is built
   * for the other direction, taking handles somebody else made, and this is that somebody.
   *
   * Vulkan is resolved through dlopen rather than linked. Polaris links the Vulkan loader only when
   * CUDA or the FFmpeg Vulkan encoder asked for it, and the vendored codec is built against volk,
   * which spells every entry point as a function pointer variable rather than as a function. Two
   * spellings of vkCreateInstance in one binary is a call that lands on the address of a pointer
   * instead of on code. Resolving by name leaves nothing for the linker to choose between.
   *
   * The create infos are members on purpose. The C API says the pointers it is handed, and
   * everything they point at, have to outlive the device it makes from them.
   */
  struct vk_device_t {
    /**
     * @brief Stand up an instance, pick a GPU that can run the codec, and lend it to PyroWave.
     * @return false when no GPU on this host has what the encoder needs, with the reason logged.
     */
    bool create();

    /// Tears down the codec's device first, because it holds the Vulkan one.
    void destroy();

    ~vk_device_t() {
      destroy();
    }

    vk_device_t() = default;
    vk_device_t(const vk_device_t &) = delete;
    vk_device_t &operator=(const vk_device_t &) = delete;

    /**
     * @brief Resolve an instance level entry point by name, or nullptr.
     *
     * For the parts of Polaris that have to speak Vulkan themselves, since nothing here is linked.
     */
    PFN_vkVoidFunction instance_fn(const char *name) const;

    /**
     * @brief Resolve a device level entry point by name, or nullptr.
     *
     * Through vkGetDeviceProcAddr, so the pointer skips the loader's dispatch on every call.
     */
    PFN_vkVoidFunction device_fn(const char *name) const;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    /// Graphics and compute, because the codec asks for a graphics capable queue and records compute.
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = VK_QUEUE_FAMILY_IGNORED;

    /// The handle every other PyroWave entry point wants. Null until create() succeeds.
    pyrowave_device codec = nullptr;

    /// What the driver calls the GPU this landed on, for the log line that says which one it was.
    std::string gpu_name;

    /**
     * @brief Whether this device can take a captured frame as a dmabuf rather than as a copy.
     *
     * Four extensions have to be there: the fd import itself, the dmabuf handle type, the DRM format
     * modifier that says how the pixels are laid out, and the foreign queue family a buffer another
     * API filled has to be acquired from. A driver missing any of them is not a driver that can do
     * three quarters of it, so this is one answer rather than four.
     */
    bool can_import_dmabuf = false;

    /**
     * @brief The queue lock, and nothing else. A leaf: never held across a call into the codec.
     *
     * A VkQueue may only be submitted to from one thread at a time, and two sessions share this one.
     * The codec takes a pointer to this in its create info and locks it around its own submissions,
     * so the same lock covers both its queue work and Polaris's.
     *
     * **It must not be held across a codec entry point.** The codec calls this back from deep inside
     * Granite, which is already holding its own device mutex by then: every path to
     * `submit_batches` goes through `LOCK()` or `DRAIN_FRAME_LOCK()` first. So the codec's order is
     * its mutex and then this one. A caller that took this one and then entered the codec would be
     * asking for them in the other order, and two sessions doing both at once deadlock: one waiting
     * for Granite's mutex while holding this, the other waiting for this while holding Granite's.
     * Tearing a session down is enough to be the second thread, because destroying an encoder submits.
     *
     * Recursive because the callback can fire twice on one thread inside Granite, and because
     * Polaris's own queue work can sit inside a frame that already took it.
     */
    mutable std::recursive_mutex device_lock;

    /**
     * @brief Held for the length of a frame that lends the codec a command buffer.
     *
     * The command buffer is set on the device rather than on the encoder, so two sessions encoding at
     * once would each set their own and record into the other's. This is the outer lock: taken before
     * entering the codec and released after the frame is gathered, so the order is always this one,
     * then Granite's, then the queue lock. Nothing inside the codec asks for this one, which is what
     * keeps that order from inverting.
     */
    mutable std::mutex command_buffer_lock;

  private:
    VkApplicationInfo application_info = {};
    VkInstanceCreateInfo instance_info = {};
    VkDeviceCreateInfo device_info = {};
    VkDeviceQueueCreateInfo queue_info = {};
    VkPhysicalDeviceFeatures2 features = {};
    VkPhysicalDeviceVulkan11Features vulkan11 = {};
    VkPhysicalDeviceVulkan12Features vulkan12 = {};
    VkPhysicalDeviceVulkan13Features vulkan13 = {};
    std::vector<const char *> device_extensions;
    float queue_priority = 1.0f;
  };

}  // namespace pyrowave_encode
