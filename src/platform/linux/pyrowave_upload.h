/**
 * @file src/platform/linux/pyrowave_upload.h
 * @brief Getting a captured frame onto the GPU the codec encodes on.
 */
#pragma once

// local includes
#include "src/platform/linux/pyrowave_encode.h"
#include "src/platform/linux/pyrowave_vulkan.h"

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

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
   * @brief The layouts this device can import a captured frame in, for one DRM format.
   *
   * Asked of Vulkan rather than assumed, and asked about the exact image the import creates rather
   * than about the format in the abstract: the modifier has to be one the driver will accept for a
   * sampled, transfer-source image built from an imported dmabuf, with one memory plane.
   *
   * It matters more here than it would elsewhere. A frame that arrives as a dmabuf has no copy in
   * host memory behind it, so a layout that turns out not to import is a lost frame rather than a
   * slow one. Offering only what has been checked is what keeps that from being possible.
   *
   * @param owner The device that will import.
   * @param fourcc What capture would be asked to produce.
   * @return The modifiers to offer, best left in the order the driver gave them. Empty when this
   *   format cannot be imported at all, which is the honest answer for a format the codec cannot read.
   */
  std::vector<std::uint64_t> importable_modifiers(const vk_device_t &owner, std::uint32_t fourcc);

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
     * @brief Take a frame that is already on the GPU, copying nothing across the bus at all.
     *
     * The whole cost of the other path is the copy into memory the GPU can read, and a capture
     * backend that can hand over a dmabuf has already put the frame there. What is left is to
     * describe it to Vulkan and to take it off the queue family that filled it.
     *
     * A stream that needs bars still costs one copy, but a copy between two images on the GPU rather
     * than one across the bus, which is roughly a tenth of the price and does not touch the host.
     *
     * @param buffer What capture handed over. The descriptors stay the caller's: this duplicates the
     *   one it uses and never closes theirs.
     * @param where Where the picture sits in what the codec reads, exactly as for the copying path.
     * @param ten_bit What this stream carries, which the frame has to agree with. Nothing downstream
     *   would notice if it did not: the colour space is decided by the stream and the samples are
     *   read as whatever the format says, so an eight bit frame in a ten bit stream is Rec. 709
     *   values sent as BT.2020 PQ, which is the dark oversaturated picture people report as broken.
     *   Channel order is not checked, because a described order is honoured and either is correct.
     * @return false when this frame cannot be imported, which is a frame lost rather than a session.
     */
    bool begin_imported(const dmabuf_t &buffer, const placement_t &where, bool ten_bit);

    /// Whether this path can import at all, which is a property of the device rather than the frame.
    bool can_import() const;

    /**
     * @brief How many of capture's buffers this path has had to describe to Vulkan.
     *
     * One per buffer in capture's pool once a stream is running, because a description is kept and
     * reused. A count that keeps climbing with the frame count means the buffers are not being
     * recognised, and every frame is paying for an image and an allocation it did not need to.
     */
    unsigned buffers_described() const {
      return described_buffers;
    }

    /**
     * @brief Open a frame that is nothing but black, for when there is no picture to send yet.
     *
     * Polaris primes an encoder by converting a dummy image before the first real frame arrives, so
     * that a session which times out waiting still has something to encode. On this path that image
     * carries no pixels at all: capture hands over a descriptor with no file descriptors and no host
     * buffer, because the frames that follow it will live on the GPU.
     *
     * So the picture it stands for is made here instead of read from it.
     *
     * @param width The stream's width, since there is no source to take one from.
     * @param height The stream's height.
     * @param format What the frames that follow will be, so the image does not have to be remade.
     */
    bool begin_blank(int width, int height, VkFormat format);

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

    /**
     * @brief Whether this path has given up, which it does when a submission never finished.
     *
     * A frame that does not complete inside a second is a lost GPU, not a slow one, and its
     * submission is still executing with no way to know when it stops. Everything this object would
     * do next touches something that submission is reading: the staging buffer, the command pool, the
     * fence. So it stops, the session it belongs to fails, and the stream tears down, which is the
     * outcome anyway once a device is lost.
     */
    bool is_wedged() const {
      return wedged;
    }

  private:
    /// Defined below, beside the slots it describes.
    struct import_t;

    upload_t() = default;

    bool resolve(const vk_device_t &owner);
    /**
     * @brief The slot holding this buffer's import, made on first sight and kept.
     *
     * @return nullptr when this frame cannot be imported, which is a frame lost rather than a session.
     */
    import_t *import_for(const dmabuf_t &buffer, bool ten_bit);
    void release_import(import_t &entry);
    void release_imports();

    /**
     * @brief Make the image the codec reads, sized and shaped for this stream.
     *
     * Both paths need it. The copying path writes into it from the staging buffer and the importing
     * path copies into it on the GPU, and either way it is what the codec is handed: an image of the
     * stream's shape, holding the picture where the placement says, and holding black wherever the
     * picture does not reach.
     */
    bool prepare_image(int width, int height, VkFormat format, const placement_t &where);

    /**
     * @brief Make the host visible buffer a frame is copied through, which only one path needs.
     *
     * Sized from capture's stride rather than from the picture, so a padded frame goes in with one
     * call. An imported frame never touches this, which is the point of importing it.
     */
    bool prepare_staging(int width, int height, int stride);

    /// Free what each half owns, separately, because the two paths need different halves.
    void release_staging_resources();
    void release_image_resources();

    /// Wait for the GPU to finish everything before freeing what it might be reading.
    bool drain();
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

    /**
     * @brief One of capture's buffers, described to Vulkan.
     *
     * Never read by the codec directly, and never read outside the frame it arrived for. Capture
     * takes the buffer back as soon as the frame is released and the compositor may be drawing into
     * it by the time the next one is asked for, so what the codec reads is always the copy made from
     * it while it was ours. That copy is between two images on the GPU, which costs a fraction of a
     * millisecond and none of the host's time, and it is what makes repeating a frame possible at all.
     *
     * The description outlives the frame even though the pixels do not. Nothing here reads the
     * buffer's contents: an image and an imported allocation say where the pixels are and how they
     * are arranged, and that stays true every time capture hands the same buffer back.
     */
    struct import_t {
      /// Zero means no slot, which is also what an unstamped frame gets: it is never matched.
      std::uint64_t buffer_key = 0;
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkFormat format = VK_FORMAT_UNDEFINED;
      uint32_t width = 0;
      uint32_t height = 0;
      /// Kept so a buffer reused under a new shape is rebuilt rather than read through a stale one.
      std::uint64_t modifier = 0;
      uint32_t pitch = 0;
      uint32_t offset = 0;
    };

    /**
     * Four, because capture's pool is smaller than that on every backend here and a fifth buffer
     * would only cost the import it saves. A key that is not in these is built into the next slot in
     * turn, so a pool that grows or is rebuilt cycles the old ones out rather than growing this.
     */
    std::array<import_t, 4> imports {};
    std::size_t next_import = 0;

    /// The slot this frame reads, owned by imports above. Null between frames.
    import_t *current_import = nullptr;

    /// How many buffers have had to be described, which is the cost this cache exists to pay once.
    unsigned described_buffers = 0;

    /// What capture's rows measured last time, which is what the staging buffer was sized for.
    uint32_t image_stride = 0;

    bool recording = false;

    /// Set once, never cleared. See is_wedged().
    bool wedged = false;

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
      PFN_vkCmdCopyImage cmd_copy_image = nullptr;
      PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
    } api;
  };

}  // namespace pyrowave_encode
