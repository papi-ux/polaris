/**
 * @file src/platform/linux/pyrowave_upload.h
 * @brief Getting a captured frame onto the GPU the codec encodes on.
 */
#pragma once

// local includes
#include "src/platform/linux/pyrowave_vulkan.h"

// standard includes
#include <cstdint>
#include <memory>

namespace pyrowave_encode {

  /**
   * @brief One frame's journey from the pointer capture handed over to an image the codec can read.
   *
   * A staging buffer, an image, a command buffer and a fence, all made once and reused, because the
   * geometry only changes when the session does. The copy is the whole cost of this path and it
   * replaces a colour conversion that cost several times more, so even a path that still touches
   * every pixel on the CPU comes out ahead. A capture backend that can hand over a dmabuf would
   * skip it entirely, and this is the shape that work would slot into.
   *
   * The command buffer is deliberately left open between begin() and end() so the codec can record
   * its encode into the same one. That is what its C API asks for when a command buffer is set on
   * the device: one submission carries the upload, the colour conversion, the wavelet transform and
   * the entropy coding, and one fence says when all of it is done.
   */
  class upload_t {
  public:
    /**
     * @brief Build the staging path, or nullptr when this device cannot offer one.
     * @param owner The device to allocate against. Must outlive the returned object.
     */
    static std::unique_ptr<upload_t> make(const vk_device_t &owner);

    ~upload_t();

    upload_t(const upload_t &) = delete;
    upload_t &operator=(const upload_t &) = delete;

    /**
     * @brief Copy a captured frame in and record the upload, leaving the command buffer open.
     * @param pixels First byte of the top left pixel.
     * @param width Width of what capture handed over.
     * @param height Height of the same.
     * @param stride Bytes per row, which capture rarely makes equal to width times four.
     * @param format What those bytes mean. The image is remade when it changes.
     * @return false when the frame could not be staged; nothing is left open.
     */
    bool begin(const uint8_t *pixels, int width, int height, int stride, VkFormat format);

    /**
     * @brief Open a command buffer over the picture already on the GPU, copying nothing.
     *
     * For the frames a host repeats when capture has nothing new. The image is already where the
     * codec reads from and already in the layout it reads in, so there is nothing to record but the
     * encode the caller is about to add.
     */
    bool begin_retained();

    /// Where the codec should record its encode. Valid between a successful begin and end().
    VkCommandBuffer command_buffer() const {
      return cmd;
    }

    /// What the codec should read the picture from. Valid once a begin has succeeded.
    pyrowave_image_view view() const;

    /// Close the command buffer, submit it, and wait for the GPU to finish the lot.
    bool end();

    /// Close and drop an open command buffer, for when the codec refused the frame.
    void abandon();

    /// Whether a picture has been staged at all, which is what makes a repeat possible.
    bool has_picture() const {
      return image != VK_NULL_HANDLE;
    }

  private:
    upload_t() = default;

    bool resolve(const vk_device_t &owner);
    bool prepare(int width, int height, int stride, VkFormat format);
    void release_frame_resources();

    const vk_device_t *owner = nullptr;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    uint8_t *staging_mapped = nullptr;
    VkDeviceSize staging_size = 0;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat image_format = VK_FORMAT_UNDEFINED;
    uint32_t image_width = 0;
    uint32_t image_height = 0;

    /// What capture's rows measured last time, which is what the staging buffer was sized for.
    uint32_t image_stride = 0;

    bool recording = false;

    /// Resolved through the device rather than linked, for the reason pyrowave_vulkan.h gives.
    struct api_t {
      PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
      PFN_vkCreateCommandPool create_command_pool = nullptr;
      PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
      PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
      PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
      PFN_vkEndCommandBuffer end_command_buffer = nullptr;
      PFN_vkResetCommandPool reset_command_pool = nullptr;
      PFN_vkCreateFence create_fence = nullptr;
      PFN_vkDestroyFence destroy_fence = nullptr;
      PFN_vkWaitForFences wait_for_fences = nullptr;
      PFN_vkResetFences reset_fences = nullptr;
      PFN_vkQueueSubmit queue_submit = nullptr;
      PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
      PFN_vkCreateBuffer create_buffer = nullptr;
      PFN_vkDestroyBuffer destroy_buffer = nullptr;
      PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
      PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
      PFN_vkCreateImage create_image = nullptr;
      PFN_vkDestroyImage destroy_image = nullptr;
      PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
      PFN_vkBindImageMemory bind_image_memory = nullptr;
      PFN_vkAllocateMemory allocate_memory = nullptr;
      PFN_vkFreeMemory free_memory = nullptr;
      PFN_vkMapMemory map_memory = nullptr;
      PFN_vkUnmapMemory unmap_memory = nullptr;
      PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
      PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    } api;
  };

}  // namespace pyrowave_encode
