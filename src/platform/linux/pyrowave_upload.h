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
   * @brief Where a captured picture sits inside the image the codec reads.
   *
   * The codec's scaler fills its output with its input, so the only way to letterbox a stream is to
   * letterbox what the scaler is given: an image shaped like the stream, with the picture in the
   * middle of it and black around the edges. The bars are cleared once when the image is made, and
   * every frame after that copies into the same rectangle, so they cost one clear and nothing per
   * frame.
   */
  struct placement_t {
    /// The image to make, which has the stream's shape rather than capture's.
    int image_width = 0;
    int image_height = 0;

    /// Where the top left of the captured picture goes inside it.
    int offset_x = 0;
    int offset_y = 0;

    bool operator==(const placement_t &other) const {
      return image_width == other.image_width && image_height == other.image_height &&
             offset_x == other.offset_x && offset_y == other.offset_y;
    }

    /// Whether anything is left uncovered, which is the only case that needs clearing.
    bool has_bars() const {
      return offset_x != 0 || offset_y != 0;
    }
  };

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
     * @param where The image to copy into and the offset to copy to, for a stream whose shape is not
     *   capture's. Zero offsets into an image the size of the picture mean no bars.
     * @return false when the frame could not be staged; nothing is left open.
     */
    bool begin(const uint8_t *pixels, int width, int height, int stride, VkFormat format,
               const placement_t &where);

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
    bool prepare(int width, int height, int stride, VkFormat format, const placement_t &where);
    void release_frame_resources();

    /**
     * @brief Forget what the image was going to be, for a frame that never reached the GPU.
     *
     * begin() records the transitions and says what the image will be in once they run. When the
     * submission does not happen, none of it did: the image is in whatever layout it was already in,
     * and the bars this frame was going to paint are not painted. Claiming otherwise transitions from
     * a layout the image is not in, which leaves its contents undefined, and the bars of a letterboxed
     * stream would then be whatever was in that memory, for the life of the session.
     */
    void forget_gpu_state();

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

    /// What capture hands over, which is the part of the image that gets copied into.
    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
    placement_t placement;

    /// Set when the image is new, so the bars are painted once rather than every frame.
    bool needs_clearing = false;

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
      PFN_vkCmdClearColorImage cmd_clear_color_image = nullptr;
    } api;
  };

}  // namespace pyrowave_encode
