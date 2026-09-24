/**
 * @file src/platform/linux/pyrowave_upload.cpp
 * @brief Getting a captured frame onto the GPU the codec encodes on.
 */
#include "src/platform/linux/pyrowave_upload.h"

// standard includes
#include <unistd.h>

#include <drm_fourcc.h>

#include <cstring>
#include <mutex>

// local includes
#include "src/logging.h"

using namespace std::literals;

namespace pyrowave_encode {

  namespace {

    /// A second is not a budget, it is a diagnosis. A frame that takes one has hung.
    constexpr uint64_t fence_timeout_ns = 1'000'000'000ULL;

    /**
     * The memory to stage through.
     *
     * Host visible and coherent, so the copy needs no flush and the queue submit makes it visible.
     * Uncached first, which on every desktop driver is write combined: the CPU only ever writes this
     * memory and the GPU only ever reads it, and write combining is what that pattern wants. Cached
     * memory is the fallback rather than the goal, because reading back is not what this is for.
     */
    uint32_t staging_memory_type(const VkPhysicalDeviceMemoryProperties &memory, uint32_t allowed) {
      constexpr VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      for (const bool uncached : {true, false}) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
          if ((allowed & (1u << i)) == 0) {
            continue;
          }
          const auto flags = memory.memoryTypes[i].propertyFlags;
          if ((flags & wanted) != wanted) {
            continue;
          }
          if (uncached && (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            continue;
          }
          return i;
        }
      }
      return UINT32_MAX;
    }

    /// Where the picture itself lives, which is on the GPU or nowhere worth the trouble.
    uint32_t image_memory_type(const VkPhysicalDeviceMemoryProperties &memory, uint32_t allowed) {
      for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((allowed & (1u << i)) == 0) {
          continue;
        }
        if ((memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
          return i;
        }
      }
      return UINT32_MAX;
    }

    /**
     * What a DRM fourcc means to Vulkan, for the handful this codec can read.
     *
     * Packed RGB only, which is what the portal is asked for and all the codec's scaled entry point
     * takes. Anything else is refused by name rather than guessed at, because a wrong guess here is
     * not a wrong colour, it is noise.
     */
    VkFormat format_for_fourcc(uint32_t fourcc) {
      switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
          return VK_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
          return VK_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
          return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
          return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        default:
          return VK_FORMAT_UNDEFINED;
      }
    }

  }  // namespace

  std::vector<std::uint64_t> importable_modifiers(const vk_device_t &owner, std::uint32_t fourcc) {
    std::vector<std::uint64_t> usable;

    const auto format = format_for_fourcc(fourcc);
    if (format == VK_FORMAT_UNDEFINED || !owner.can_import_dmabuf ||
        owner.physical_device == VK_NULL_HANDLE) {
      return usable;
    }

    const auto list_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
      owner.instance_fn("vkGetPhysicalDeviceFormatProperties2"));
    const auto describe_image = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
      owner.instance_fn("vkGetPhysicalDeviceImageFormatProperties2"));
    if (!list_formats || !describe_image) {
      return usable;
    }

    // Two calls, which is how Vulkan hands over a list: once for the count, once for the contents.
    VkDrmFormatModifierPropertiesListEXT modifiers = {};
    modifiers.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

    VkFormatProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &modifiers;
    list_formats(owner.physical_device, format, &properties);
    if (modifiers.drmFormatModifierCount == 0) {
      return usable;
    }

    std::vector<VkDrmFormatModifierPropertiesEXT> described(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = described.data();
    list_formats(owner.physical_device, format, &properties);

    for (const auto &candidate : described) {
      // One memory plane, because that is what the import binds. A layout that spreads a packed RGB
      // image across several planes, as some compression schemes do, would have the rest read from
      // inside the first one's allocation.
      if (candidate.drmFormatModifierPlaneCount != 1) {
        continue;
      }
      constexpr VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
      if ((candidate.drmFormatModifierTilingFeatures & needed) != needed) {
        continue;
      }

      // And then the exact image, because a format that supports something in general may not support
      // it for an image created this way. This is the same create info the import uses.
      VkPhysicalDeviceExternalImageFormatInfo external = {};
      external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
      external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

      VkPhysicalDeviceImageDrmFormatModifierInfoEXT layout = {};
      layout.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
      layout.drmFormatModifier = candidate.drmFormatModifier;
      layout.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      layout.pNext = &external;

      VkPhysicalDeviceImageFormatInfo2 wanted = {};
      wanted.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
      wanted.format = format;
      wanted.type = VK_IMAGE_TYPE_2D;
      wanted.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      wanted.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      wanted.pNext = &layout;

      VkExternalImageFormatProperties external_properties = {};
      external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

      VkImageFormatProperties2 answer = {};
      answer.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
      answer.pNext = &external_properties;

      if (describe_image(owner.physical_device, &wanted, &answer) != VK_SUCCESS) {
        continue;
      }
      const auto features = external_properties.externalMemoryProperties.externalMemoryFeatures;
      if ((features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        continue;
      }

      usable.push_back(candidate.drmFormatModifier);
    }

    return usable;
  }

  std::unique_ptr<upload_t> upload_t::make(const vk_device_t &owner) {
    if (owner.device == VK_NULL_HANDLE || owner.queue == VK_NULL_HANDLE) {
      return nullptr;
    }

    std::unique_ptr<upload_t> staging {new upload_t()};
    staging->owner = &owner;
    if (!staging->resolve(owner)) {
      return nullptr;
    }

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = owner.queue_family;
    if (staging->api.create_command_pool(owner.device, &pool_info, nullptr, &staging->pool) !=
        VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a command pool to upload frames with"sv;
      return nullptr;
    }

    VkCommandBufferAllocateInfo cmd_info = {};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = staging->pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (staging->api.allocate_command_buffers(owner.device, &cmd_info, &staging->cmd) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not allocate a command buffer"sv;
      return nullptr;
    }

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (staging->api.create_fence(owner.device, &fence_info, nullptr, &staging->fence) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a fence"sv;
      return nullptr;
    }

    return staging;
  }

  bool upload_t::resolve(const vk_device_t &from) {
#define POLARIS_VK_RESOLVE(member, name)                                            \
  api.member = reinterpret_cast<PFN_vk##name>(from.device_fn("vk" #name));          \
  if (!api.member) {                                                                \
    BOOST_LOG(warning) << "PyroWave: this driver has no vk" #name ""sv;             \
    return false;                                                                   \
  }

    POLARIS_VK_RESOLVE(create_command_pool, CreateCommandPool)
    POLARIS_VK_RESOLVE(destroy_command_pool, DestroyCommandPool)
    POLARIS_VK_RESOLVE(allocate_command_buffers, AllocateCommandBuffers)
    POLARIS_VK_RESOLVE(begin_command_buffer, BeginCommandBuffer)
    POLARIS_VK_RESOLVE(end_command_buffer, EndCommandBuffer)
    POLARIS_VK_RESOLVE(reset_command_pool, ResetCommandPool)
    POLARIS_VK_RESOLVE(create_fence, CreateFence)
    POLARIS_VK_RESOLVE(destroy_fence, DestroyFence)
    POLARIS_VK_RESOLVE(wait_for_fences, WaitForFences)
    POLARIS_VK_RESOLVE(reset_fences, ResetFences)
    POLARIS_VK_RESOLVE(queue_submit, QueueSubmit)
    POLARIS_VK_RESOLVE(device_wait_idle, DeviceWaitIdle)
    POLARIS_VK_RESOLVE(create_buffer, CreateBuffer)
    POLARIS_VK_RESOLVE(destroy_buffer, DestroyBuffer)
    POLARIS_VK_RESOLVE(get_buffer_memory_requirements, GetBufferMemoryRequirements)
    POLARIS_VK_RESOLVE(bind_buffer_memory, BindBufferMemory)
    POLARIS_VK_RESOLVE(create_image, CreateImage)
    POLARIS_VK_RESOLVE(destroy_image, DestroyImage)
    POLARIS_VK_RESOLVE(get_image_memory_requirements, GetImageMemoryRequirements)
    POLARIS_VK_RESOLVE(bind_image_memory, BindImageMemory)
    POLARIS_VK_RESOLVE(allocate_memory, AllocateMemory)
    POLARIS_VK_RESOLVE(free_memory, FreeMemory)
    POLARIS_VK_RESOLVE(map_memory, MapMemory)
    POLARIS_VK_RESOLVE(unmap_memory, UnmapMemory)
    POLARIS_VK_RESOLVE(cmd_pipeline_barrier, CmdPipelineBarrier)
    POLARIS_VK_RESOLVE(cmd_copy_buffer_to_image, CmdCopyBufferToImage)
    POLARIS_VK_RESOLVE(cmd_clear_color_image, CmdClearColorImage)
    POLARIS_VK_RESOLVE(cmd_copy_image, CmdCopyImage)
#undef POLARIS_VK_RESOLVE

    // Optional, because a device without the dmabuf extensions does not have it and still works:
    // that host copies its frames instead.
    api.get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      from.device_fn("vkGetMemoryFdPropertiesKHR"));

    // The only one that belongs to the instance rather than to the device.
    api.get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      from.instance_fn("vkGetPhysicalDeviceMemoryProperties"));
    if (!api.get_memory_properties) {
      BOOST_LOG(warning) << "PyroWave: this driver has no vkGetPhysicalDeviceMemoryProperties"sv;
      return false;
    }

    return true;
  }

  upload_t::~upload_t() {
    if (owner && owner->device != VK_NULL_HANDLE && api.device_wait_idle) {
      // Under the queue lock, because vkDeviceWaitIdle wants every queue on the device synchronised
      // against host access, and another session is very likely submitting on this one right now:
      // a session being destroyed while another streams is the ordinary case, not the exotic one.
      const std::lock_guard<std::recursive_mutex> lock {owner->device_lock};
      const auto waited = api.device_wait_idle(owner->device);
      if (waited != VK_SUCCESS) {
        BOOST_LOG(warning) << "PyroWave: the device did not drain before teardown (result "sv
                           << static_cast<int>(waited) << ')';
      }
    }
    release_imports();
    release_frame_resources();
    if (fence != VK_NULL_HANDLE) {
      api.destroy_fence(owner->device, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
      // Frees the command buffer with it.
      api.destroy_command_pool(owner->device, pool, nullptr);
    }
  }

  void upload_t::release_staging_resources() {
    if (!owner || owner->device == VK_NULL_HANDLE) {
      return;
    }
    if (staging_mapped) {
      api.unmap_memory(owner->device, staging_memory);
      staging_mapped = nullptr;
    }
    if (staging != VK_NULL_HANDLE) {
      api.destroy_buffer(owner->device, staging, nullptr);
      staging = VK_NULL_HANDLE;
    }
    if (staging_memory != VK_NULL_HANDLE) {
      api.free_memory(owner->device, staging_memory, nullptr);
      staging_memory = VK_NULL_HANDLE;
    }
    staging_size = 0;
    image_stride = 0;
  }

  void upload_t::release_image_resources() {
    if (!owner || owner->device == VK_NULL_HANDLE) {
      return;
    }
    if (image != VK_NULL_HANDLE) {
      api.destroy_image(owner->device, image, nullptr);
      image = VK_NULL_HANDLE;
    }
    if (image_memory != VK_NULL_HANDLE) {
      api.free_memory(owner->device, image_memory, nullptr);
      image_memory = VK_NULL_HANDLE;
    }
    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_format = VK_FORMAT_UNDEFINED;
    image_width = 0;
    image_height = 0;
    picture_width = 0;
    picture_height = 0;
    placement = {};
    needs_clearing = false;
  }

  void upload_t::release_frame_resources() {
    release_staging_resources();
    release_image_resources();
  }

  void upload_t::forget_gpu_state() {
    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    needs_clearing = placement.has_bars();
  }

  /**
   * Wait for everything in flight, because what is about to be freed may be in it.
   *
   * Under the queue lock, for the reason the destructor's drain is: another session is very likely
   * submitting on this queue right now.
   */
  bool upload_t::drain() {
    const std::lock_guard<std::recursive_mutex> lock {owner->device_lock};
    const auto waited = api.device_wait_idle(owner->device);
    if (waited != VK_SUCCESS) {
      // Freeing an image a submission is still reading is worse than refusing the frame.
      BOOST_LOG(error) << "PyroWave: the device would not drain (result "sv
                       << static_cast<int>(waited) << ')';
      wedged = true;
      return false;
    }
    return true;
  }

  bool upload_t::prepare_image(int width, int height, VkFormat format, const placement_t &where) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (where.image_width < width + where.offset_x || where.image_height < height + where.offset_y ||
        where.offset_x < 0 || where.offset_y < 0) {
      BOOST_LOG(error) << "PyroWave: a "sv << width << 'x' << height << " picture does not fit at "sv
                       << where.offset_x << ',' << where.offset_y << " of a "sv << where.image_width
                       << 'x' << where.image_height << " image"sv;
      return false;
    }
    if (image != VK_NULL_HANDLE && picture_width == static_cast<uint32_t>(width) &&
        picture_height == static_cast<uint32_t>(height) && image_format == format &&
        placement == where) {
      return true;
    }

    // Capture changed shape under us, which happens when a monitor mode changes mid session.
    if (!drain()) {
      return false;
    }
    release_image_resources();

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {static_cast<uint32_t>(where.image_width),
                         static_cast<uint32_t>(where.image_height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Sampled because the codec's scaler reads it through a sampler, and a transfer destination
    // because both paths write into it: one from a host buffer, one from the imported frame.
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (api.create_image(owner->device, &image_info, nullptr, &image) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: this GPU will not make a "sv << where.image_width << 'x'
                         << where.image_height << " image in format "sv << static_cast<int>(format);
      image = VK_NULL_HANDLE;
      release_image_resources();
      return false;
    }

    VkPhysicalDeviceMemoryProperties memory = {};
    api.get_memory_properties(owner->physical_device, &memory);

    VkMemoryRequirements image_needs = {};
    api.get_image_memory_requirements(owner->device, image, &image_needs);
    const auto image_type = image_memory_type(memory, image_needs.memoryTypeBits);
    if (image_type == UINT32_MAX) {
      BOOST_LOG(warning) << "PyroWave: this GPU reports no device local memory"sv;
      release_image_resources();
      return false;
    }

    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = image_needs.size;
    allocation.memoryTypeIndex = image_type;
    if (api.allocate_memory(owner->device, &allocation, nullptr, &image_memory) != VK_SUCCESS ||
        api.bind_image_memory(owner->device, image, image_memory, 0) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not allocate "sv << image_needs.size
                         << " bytes for the captured frame"sv;
      release_image_resources();
      return false;
    }

    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_format = format;
    image_width = static_cast<uint32_t>(where.image_width);
    image_height = static_cast<uint32_t>(where.image_height);
    picture_width = static_cast<uint32_t>(width);
    picture_height = static_cast<uint32_t>(height);
    placement = where;
    // Only when something is left uncovered. A picture that fills the image is written over in full
    // every frame, and clearing it first would be work with no effect.
    needs_clearing = where.has_bars();
    return true;
  }

  bool upload_t::prepare_staging(int width, int height, int stride) {
    if (width <= 0 || height <= 0 || stride < width * 4) {
      return false;
    }
    if (staging != VK_NULL_HANDLE && image_stride == static_cast<uint32_t>(stride) &&
        staging_size == static_cast<VkDeviceSize>(stride) * height) {
      return true;
    }

    if (!drain()) {
      return false;
    }
    release_staging_resources();

    VkPhysicalDeviceMemoryProperties memory = {};
    api.get_memory_properties(owner->physical_device, &memory);

    // Sized for capture's rows rather than for tight ones, so the frame goes in with one call and
    // the copy is told how wide a row really is. Four bytes a pixel for every format this path
    // accepts, packed ten bit included.
    const VkDeviceSize wanted_bytes = static_cast<VkDeviceSize>(stride) * height;

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = wanted_bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.create_buffer(owner->device, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a "sv << wanted_bytes << " byte staging buffer"sv;
      release_staging_resources();
      return false;
    }

    VkMemoryRequirements buffer_needs = {};
    api.get_buffer_memory_requirements(owner->device, staging, &buffer_needs);
    const auto staging_type = staging_memory_type(memory, buffer_needs.memoryTypeBits);
    if (staging_type == UINT32_MAX) {
      BOOST_LOG(warning) << "PyroWave: this GPU has no memory the host can write and it can read"sv;
      release_staging_resources();
      return false;
    }

    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = buffer_needs.size;
    allocation.memoryTypeIndex = staging_type;
    if (api.allocate_memory(owner->device, &allocation, nullptr, &staging_memory) != VK_SUCCESS ||
        api.bind_buffer_memory(owner->device, staging, staging_memory, 0) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not allocate "sv << buffer_needs.size
                         << " bytes to stage frames through"sv;
      release_staging_resources();
      return false;
    }

    void *mapped = nullptr;
    if (api.map_memory(owner->device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not map the staging buffer"sv;
      release_staging_resources();
      return false;
    }
    // Mapped once and left mapped. Unmapping between frames buys nothing and costs a round trip.
    staging_mapped = static_cast<uint8_t *>(mapped);
    staging_size = wanted_bytes;
    image_stride = static_cast<uint32_t>(stride);
    return true;
  }


  bool upload_t::can_import() const {
    return owner && owner->can_import_dmabuf && api.get_memory_fd_properties != nullptr;
  }

  void upload_t::release_import(import_t &entry) {
    if (!owner || owner->device == VK_NULL_HANDLE) {
      return;
    }
    if (entry.image != VK_NULL_HANDLE) {
      api.destroy_image(owner->device, entry.image, nullptr);
    }
    if (entry.memory != VK_NULL_HANDLE) {
      api.free_memory(owner->device, entry.memory, nullptr);
    }
    if (current_import == &entry) {
      current_import = nullptr;
    }
    entry = {};
  }

  void upload_t::release_imports() {
    for (auto &entry : imports) {
      release_import(entry);
    }
    current_import = nullptr;
  }

  /**
   * Describe a dmabuf to Vulkan and bind its memory, once per buffer.
   *
   * It used to be once per frame, and the comment here said to measure before caching because an
   * image creation is not obviously expensive. Measured, 300 frames to 1080p on a 4090, per frame:
   *
   *     source       per frame   per buffer
   *     1920x1080    0.508 ms    0.306 ms
   *     3840x2160    0.829 ms    0.374 ms
   *     7680x2160    1.505 ms    0.594 ms
   *
   * Nearly all of it is in vkAllocateMemory, where the kernel attaches the buffer to the device, so it
   * scales with the buffer rather than with the stream: at the ultrawide the import alone cost more
   * than the whole frame does now.
   *
   * The worry in that comment was the right one and it is what decides the key. A file descriptor can
   * be closed and the same integer handed back for something else, so a cache keyed on one reads the
   * wrong picture and says nothing. Capture stamps each buffer in its pool with a number from a
   * counter that only goes up, which is what is used here: a number that is not in the table means a
   * buffer nobody has described yet, always, including after a pool is thrown away and rebuilt.
   *
   * What is kept is a description, not pixels. An image and an imported allocation say where the
   * pixels are and how they are laid out, and that stays true every time capture hands the same
   * buffer back. The shape is checked all the same, so a buffer reused under a new geometry is
   * rebuilt rather than read through a stale description.
   *
   * This is what cuda.cpp does for the same reason, four slots and all.
   */
  upload_t::import_t *upload_t::import_for(const dmabuf_t &buffer, bool ten_bit) {
    current_import = nullptr;

    if (!can_import()) {
      BOOST_LOG(error) << "PyroWave: asked to import a frame on a device that cannot"sv;
      return nullptr;
    }
    if (buffer.fds[0] < 0 || buffer.width <= 0 || buffer.height <= 0) {
      BOOST_LOG(error) << "PyroWave: capture described a "sv << buffer.width << 'x' << buffer.height
                       << " dmabuf with descriptor "sv << buffer.fds[0];
      return nullptr;
    }

    const auto format = format_for_fourcc(buffer.fourcc);
    if (format == VK_FORMAT_UNDEFINED) {
      BOOST_LOG(warning) << "PyroWave: capture handed over a dmabuf in a format this codec cannot "sv
                         << "read (fourcc "sv << buffer.fourcc << ')';
      return nullptr;
    }

    // The depth this stream carries, checked here because nothing after this point would notice. The
    // portal offers eight and ten bit formats in one list and lets the compositor choose, so a stream
    // can be told it is getting one and handed the other.
    const bool frame_is_ten_bit = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                                  format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (frame_is_ten_bit != ten_bit) {
      BOOST_LOG(error) << "PyroWave: capture handed over "sv
                       << (frame_is_ten_bit ? "ten"sv : "eight"sv) << " bit pixels and this stream is "sv
                       << (ten_bit ? "ten"sv : "eight"sv) << " bit"sv;
      return nullptr;
    }

    // A buffer this path has already described, handed back by the pool. Everything the description
    // depends on is compared, not just the number, so a pool that reuses a buffer for a different
    // shape gets a new description rather than a wrong one.
    //
    // An unstamped frame matches nothing: zero is what a backend that does not track its buffers
    // leaves behind, and treating two of those as the same buffer is exactly the mistake this key
    // exists to avoid.
    if (buffer.buffer_key != 0) {
      for (auto &entry : imports) {
        if (entry.buffer_key != buffer.buffer_key) {
          continue;
        }
        if (entry.format == format &&
            entry.width == static_cast<uint32_t>(buffer.width) &&
            entry.height == static_cast<uint32_t>(buffer.height) &&
            entry.modifier == buffer.modifier &&
            entry.pitch == buffer.pitches[0] &&
            entry.offset == buffer.offsets[0]) {
          return &entry;
        }
        release_import(entry);
        break;
      }
    }

    // Duplicated, because Vulkan takes ownership of the descriptor it is given and capture closes the
    // one it kept. Two owners of one descriptor is a double close, which is somebody else's bug.
    //
    // Holding the duplicate is also what makes keeping the description safe: the buffer cannot be
    // freed underneath a slot that still names it, however the other side manages its own pool.
    const int fd = ::dup(buffer.fds[0]);
    if (fd < 0) {
      BOOST_LOG(warning) << "PyroWave: could not duplicate the captured frame's descriptor"sv;
      return nullptr;
    }

    VkMemoryFdPropertiesKHR fd_properties = {};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (api.get_memory_fd_properties(owner->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     fd, &fd_properties) != VK_SUCCESS ||
        fd_properties.memoryTypeBits == 0) {
      BOOST_LOG(warning) << "PyroWave: this driver will not describe the captured frame's memory"sv;
      ::close(fd);
      return nullptr;
    }

    // The slot this one is built into, taken in turn. Whatever was here is given back first, which is
    // how a pool larger than the table cycles rather than leaks.
    auto &entry = imports[next_import];
    next_import = (next_import + 1) % imports.size();
    release_import(entry);

    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // One plane, and a modifier that says how it is laid out. Both are refused rather than guessed
    // at, and the refusal is the honest part:
    //
    // Only the first descriptor is imported, so a buffer whose modifier yields more than one memory
    // plane would have the rest read from inside the first one's allocation. AMD's DCC and Intel's
    // CCS do exactly that on packed RGB.
    //
    // And an invalid modifier means nobody said how the pixels are arranged. Assuming linear reads a
    // tiled buffer as noise, and even when it is linear, Vulkan then picks its own row pitch and a
    // padded capture pitch shears the picture.
    //
    // Neither happens today: Polaris asks the portal for one plane of linear, and PipeWire rewrites
    // an invalid modifier to linear before it gets here. They are refused because that is a sentence
    // in a log rather than a picture nobody can explain.
    uint32_t planes = 0;
    for (int i = 0; i < 4 && buffer.fds[i] >= 0; i++) {
      planes++;
    }
    if (planes != 1) {
      BOOST_LOG(warning) << "PyroWave: capture handed over a "sv << planes
                         << " plane dmabuf, and this path imports one"sv;
      ::close(fd);
      return nullptr;
    }
    if (buffer.modifier == DRM_FORMAT_MOD_INVALID) {
      BOOST_LOG(warning) << "PyroWave: capture handed over a dmabuf with no layout modifier"sv;
      ::close(fd);
      return nullptr;
    }

    std::array<VkSubresourceLayout, 4> layouts = {};
    layouts[0].offset = buffer.offsets[0];
    layouts[0].rowPitch = buffer.pitches[0];

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier_info.drmFormatModifier = buffer.modifier;
    modifier_info.drmFormatModifierPlaneCount = 1;
    modifier_info.pPlaneLayouts = layouts.data();
    external.pNext = &modifier_info;
    const VkImageTiling tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {static_cast<uint32_t>(buffer.width), static_cast<uint32_t>(buffer.height), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = tiling;
    // Sampled for the codec to read, and a transfer source for the letterbox copy, which is the only
    // other thing that ever touches it.
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (api.create_image(owner->device, &info, nullptr, &entry.image) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: this driver will not make an image for a "sv << buffer.width
                         << 'x' << buffer.height << " dmabuf with modifier "sv << buffer.modifier;
      entry.image = VK_NULL_HANDLE;
      ::close(fd);
      return nullptr;
    }

    VkMemoryRequirements needs = {};
    api.get_image_memory_requirements(owner->device, entry.image, &needs);
    const auto usable = needs.memoryTypeBits & fd_properties.memoryTypeBits;
    if (usable == 0) {
      BOOST_LOG(warning) << "PyroWave: the captured frame's memory suits no type this image can use"sv;
      ::close(fd);
      release_import(entry);
      return nullptr;
    }

    uint32_t chosen = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties memory = {};
    api.get_memory_properties(owner->physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
      if ((usable & (1u << i)) == 0) {
        continue;
      }
      chosen = i;
      if ((memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        break;
      }
    }
    if (chosen == UINT32_MAX) {
      BOOST_LOG(error) << "PyroWave: no memory type suits the captured frame"sv;
      ::close(fd);
      release_import(entry);
      return nullptr;
    }

    // The descriptor goes with this structure: a successful import takes it, and a failed one leaves
    // it to be closed here.
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = fd;

    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = entry.image;
    import.pNext = &dedicated;

    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &import;
    allocation.allocationSize = needs.size;
    allocation.memoryTypeIndex = chosen;

    if (api.allocate_memory(owner->device, &allocation, nullptr, &entry.memory) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: the captured frame's memory could not be imported"sv;
      entry.memory = VK_NULL_HANDLE;
      ::close(fd);
      release_import(entry);
      return nullptr;
    }
    if (api.bind_image_memory(owner->device, entry.image, entry.memory, 0) != VK_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: the imported memory would not bind to the image"sv;
      release_import(entry);
      return nullptr;
    }

    entry.buffer_key = buffer.buffer_key;
    entry.format = format;
    entry.width = static_cast<uint32_t>(buffer.width);
    entry.height = static_cast<uint32_t>(buffer.height);
    entry.modifier = buffer.modifier;
    entry.pitch = buffer.pitches[0];
    entry.offset = buffer.offsets[0];
    ++described_buffers;
    return &entry;
  }

  bool upload_t::begin_imported(const dmabuf_t &buffer, const placement_t &where, bool ten_bit) {
    if (wedged || recording) {
      BOOST_LOG(error) << "PyroWave: asked for a frame while "sv
                       << (wedged ? "this path has given up"sv : "one is already open"sv);
      return false;
    }
    auto *imported = import_for(buffer, ten_bit);
    if (!imported) {
      return false;
    }

    // Into an image this path owns, always, even when the shapes match and the codec could have read
    // the imported frame directly. Two reasons, and the second is the one that decides it:
    //
    // Capture takes the buffer back as soon as this frame is released, and the compositor may be
    // drawing into it by the time anything wants to read it again. Reading it later is reading
    // whatever is in it now.
    //
    // And a host repeats a frame when capture has nothing new, which means encoding the last picture
    // again with no frame in hand. That is only possible from an image that is still ours.
    //
    // The copy is between two images on the GPU: about a tenth of a millisecond at 4K against the
    // four this path exists to remove, and none of it the host's.
    // A failure from here on leaves the slot alone. It is a good description of a buffer capture is
    // going to hand back, and throwing it away would only mean building it again.
    if (!prepare_image(buffer.width, buffer.height, imported->format, where)) {
      return false;
    }

    if (api.reset_command_pool(owner->device, pool, 0) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (api.begin_command_buffer(cmd, &begin_info) != VK_SUCCESS) {
      return false;
    }
    recording = true;
    current_import = imported;

    // Taken off the queue family that filled it. A buffer another API wrote is owned by a family
    // Vulkan calls foreign, and reading it without acquiring it first is undefined: the contents are
    // whatever the driver felt like leaving visible.
    //
    // Every frame, including the frames where this image was described long ago. The writer that
    // filled it this time was the compositor again, so the layout it is coming from is undefined
    // again, and what the pixels mean is carried by the modifier rather than by a Vulkan layout.
    VkImageMemoryBarrier acquire = {};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    acquire.dstQueueFamilyIndex = owner->queue_family;
    acquire.image = imported->image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = image_layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const bool first = image_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.srcAccessMask = first ? 0 : VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(
      cmd, first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

    if (needs_clearing) {
      const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
      const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      api.cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);

      VkImageMemoryBarrier cleared = to_transfer;
      cleared.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      cleared.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, nullptr, 0, nullptr, 1, &cleared);
      needs_clearing = false;
    }

    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, 0, 0};
    region.dstOffset = {placement.offset_x, placement.offset_y, 0};
    region.extent = {imported->width, imported->height, 1};
    api.cmd_copy_image(cmd, imported->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read = to_transfer;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_read);

    image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
  }

  bool upload_t::begin(const uint8_t *pixels, int width, int height, int stride, VkFormat format,
                       const placement_t &where) {
    if (wedged || recording || !pixels || stride <= 0) {
      return false;
    }
    // Four bytes a pixel, so a row of pixels is a whole number of texels wide however capture padded
    // it. A stride that is not is a capture backend this path cannot read, and saying so beats
    // uploading a sheared picture.
    if (stride % 4 != 0) {
      BOOST_LOG(warning) << "PyroWave: a "sv << stride << " byte row is not a whole number of pixels"sv;
      return false;
    }
    if (!prepare_image(width, height, format, where) || !prepare_staging(width, height, stride)) {
      return false;
    }

    // Everything but the padding after the last row. A backend is entitled to end its buffer at the
    // last pixel rather than at the end of the row it sits in, and reading the difference is reading
    // off the end of a mapping.
    const auto row_bytes = static_cast<std::size_t>(stride);
    const auto last_row = static_cast<std::size_t>(width) * 4;
    const auto frame_bytes = row_bytes * (static_cast<std::size_t>(height) - 1) + last_row;
    std::memcpy(staging_mapped, pixels, frame_bytes);

    if (api.reset_command_pool(owner->device, pool, 0) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (api.begin_command_buffer(cmd, &begin_info) != VK_SUCCESS) {
      return false;
    }
    recording = true;

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = image_layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    // The first frame has nothing to wait for. Every one after it waits for the encode that read the
    // picture this one is about to overwrite.
    const bool first = image_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.srcAccessMask = first ? 0 : VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(
      cmd, first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

    if (needs_clearing) {
      // Black, once, and the bars stay black for the life of the image because every frame after
      // this copies into the same rectangle and never touches the rest.
      const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
      const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      api.cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);

      // The clear covers the whole image and the copy below covers part of it, so one has to finish
      // before the other starts.
      VkImageMemoryBarrier cleared = to_transfer;
      cleared.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      cleared.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, nullptr, 0, nullptr, 1, &cleared);
      needs_clearing = false;
    }

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = static_cast<uint32_t>(stride / 4);
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {placement.offset_x, placement.offset_y, 0};
    region.imageExtent = {picture_width, picture_height, 1};
    api.cmd_copy_buffer_to_image(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);

    VkImageMemoryBarrier to_read = to_transfer;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_read);

    image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
  }

  bool upload_t::begin_blank(int width, int height, VkFormat format) {
    if (wedged || recording || width <= 0 || height <= 0) {
      return false;
    }

    // The whole image is the picture, so there are no bars, and the clear below covers all of it.
    const placement_t whole {width, height, 0, 0};
    if (!prepare_image(width, height, format, whole)) {
      return false;
    }

    if (api.reset_command_pool(owner->device, pool, 0) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (api.begin_command_buffer(cmd, &begin_info) != VK_SUCCESS) {
      return false;
    }
    recording = true;

    VkImageMemoryBarrier to_transfer = {};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = image_layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const bool first = image_layout == VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.srcAccessMask = first ? 0 : VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(
      cmd, first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

    const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    api.cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                              &whole_image);
    // The whole image was just written, so whatever the bars were is gone with it.
    needs_clearing = false;

    VkImageMemoryBarrier to_read = to_transfer;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    api.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_read);

    image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
  }

  bool upload_t::begin_retained() {
    if (wedged || recording || image == VK_NULL_HANDLE ||
        image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      return false;
    }
    if (api.reset_command_pool(owner->device, pool, 0) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (api.begin_command_buffer(cmd, &begin_info) != VK_SUCCESS) {
      return false;
    }
    // Nothing to record. The picture is where it was and in the layout it was read in, and the only
    // hazard would be a write, which is what this call exists to avoid.
    recording = true;
    return true;
  }

  pyrowave_image_view upload_t::view() const {
    pyrowave_image_view view = {};
    // Always the image this path owns. An imported frame is copied into it while capture still has
    // it lent to us, and a repeat encodes what is in it, so this is the one thing that is still true
    // after capture has taken its buffer back.
    view.image = image;
    view.width = image_width;
    view.height = image_height;
    view.image_format = image_format;
    view.view_format = image_format;
    view.mip_level = 0;
    view.layer = 0;
    view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return view;
  }

  bool upload_t::end() {
    if (!recording) {
      return false;
    }
    recording = false;

    if (api.end_command_buffer(cmd) != VK_SUCCESS) {
      forget_gpu_state();
      return false;
    }
    if (api.reset_fences(owner->device, 1, &fence) != VK_SUCCESS) {
      forget_gpu_state();
      return false;
    }

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    // The queue lock covers the submit and nothing more. Waiting on the fence below touches no queue,
    // and holding a shared lock across a wait would stall every other session for as long as this
    // frame takes.
    VkResult submitted = VK_SUCCESS;
    {
      const std::lock_guard<std::recursive_mutex> lock {owner->device_lock};
      submitted = api.queue_submit(owner->queue, 1, &submit, fence);
    }
    if (submitted != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: the queue refused a frame"sv;
      forget_gpu_state();
      return false;
    }

    // Waiting here is the contract, not a shortcut. The codec's packetizing entry points read a
    // buffer this submission writes, and its own header says the caller submits and waits before
    // calling them when the command buffer is the caller's.
    const auto waited = api.wait_for_fences(owner->device, 1, &fence, VK_TRUE, fence_timeout_ns);
    if (waited != VK_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: the GPU did not finish a frame within a second (result "sv
                       << static_cast<int>(waited) << ')';
      // That submission is still executing and there is no telling when it stops. Reusing anything
      // here would overwrite the staging buffer it is reading, reset the pool its command buffer is
      // pending in, and resubmit a one time command buffer. So the path is done: the session fails
      // and the stream tears down, which is what a lost GPU means anyway.
      wedged = true;
      forget_gpu_state();
      return false;
    }
    return true;
  }

  void upload_t::abandon() {
    if (!recording) {
      return;
    }
    recording = false;
    api.end_command_buffer(cmd);
    // Not submitted, so the picture never reached the image and the layout it claims is a lie.
    api.reset_command_pool(owner->device, pool, 0);
    forget_gpu_state();
  }

}  // namespace pyrowave_encode
