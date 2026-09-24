/**
 * @file src/platform/linux/pyrowave_upload.cpp
 * @brief Getting a captured frame onto the GPU the codec encodes on.
 */
#include "src/platform/linux/pyrowave_upload.h"

// standard includes
#include <cstring>

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

  }  // namespace

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
#undef POLARIS_VK_RESOLVE

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
      api.device_wait_idle(owner->device);
    }
    release_frame_resources();
    if (fence != VK_NULL_HANDLE) {
      api.destroy_fence(owner->device, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
      // Frees the command buffer with it.
      api.destroy_command_pool(owner->device, pool, nullptr);
    }
  }

  void upload_t::release_frame_resources() {
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
    if (image != VK_NULL_HANDLE) {
      api.destroy_image(owner->device, image, nullptr);
      image = VK_NULL_HANDLE;
    }
    if (image_memory != VK_NULL_HANDLE) {
      api.free_memory(owner->device, image_memory, nullptr);
      image_memory = VK_NULL_HANDLE;
    }
    staging_size = 0;
    image_stride = 0;
    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_format = VK_FORMAT_UNDEFINED;
    image_width = 0;
    image_height = 0;
  }

  bool upload_t::prepare(int width, int height, int stride, VkFormat format) {
    if (width <= 0 || height <= 0 || stride < width * 4) {
      return false;
    }
    if (image != VK_NULL_HANDLE && image_width == static_cast<uint32_t>(width) &&
        image_height == static_cast<uint32_t>(height) && image_format == format &&
        image_stride == static_cast<uint32_t>(stride)) {
      return true;
    }

    // Capture changed shape under us, which happens when a monitor mode changes mid session.
    api.device_wait_idle(owner->device);
    release_frame_resources();

    VkPhysicalDeviceMemoryProperties memory = {};
    api.get_memory_properties(owner->physical_device, &memory);

    // Sized for capture's rows rather than for tight ones, so the frame goes in with one call and
    // the copy below is told how wide a row really is. Four bytes a pixel for every format this path
    // accepts, packed 10 bit included.
    const VkDeviceSize wanted_bytes = static_cast<VkDeviceSize>(stride) * height;

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = wanted_bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.create_buffer(owner->device, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a "sv << wanted_bytes << " byte staging buffer"sv;
      release_frame_resources();
      return false;
    }

    VkMemoryRequirements buffer_needs = {};
    api.get_buffer_memory_requirements(owner->device, staging, &buffer_needs);
    const auto staging_type = staging_memory_type(memory, buffer_needs.memoryTypeBits);
    if (staging_type == UINT32_MAX) {
      BOOST_LOG(warning) << "PyroWave: this GPU has no memory the host can write and it can read"sv;
      release_frame_resources();
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
      release_frame_resources();
      return false;
    }

    void *mapped = nullptr;
    if (api.map_memory(owner->device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not map the staging buffer"sv;
      release_frame_resources();
      return false;
    }
    // Mapped once and left mapped. Unmapping between frames buys nothing and costs a round trip.
    staging_mapped = static_cast<uint8_t *>(mapped);
    staging_size = wanted_bytes;

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Sampled because the codec's scaler reads it through a sampler, and nothing else: it makes its
    // own intermediate planes and never writes back here.
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (api.create_image(owner->device, &image_info, nullptr, &image) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: this GPU will not make a "sv << width << 'x' << height
                         << " image in format "sv << static_cast<int>(format);
      release_frame_resources();
      return false;
    }

    VkMemoryRequirements image_needs = {};
    api.get_image_memory_requirements(owner->device, image, &image_needs);
    const auto image_type = image_memory_type(memory, image_needs.memoryTypeBits);
    if (image_type == UINT32_MAX) {
      BOOST_LOG(warning) << "PyroWave: this GPU reports no device local memory"sv;
      release_frame_resources();
      return false;
    }

    allocation.allocationSize = image_needs.size;
    allocation.memoryTypeIndex = image_type;
    if (api.allocate_memory(owner->device, &allocation, nullptr, &image_memory) != VK_SUCCESS ||
        api.bind_image_memory(owner->device, image, image_memory, 0) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not allocate "sv << image_needs.size
                         << " bytes for the captured frame"sv;
      release_frame_resources();
      return false;
    }

    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_format = format;
    image_width = static_cast<uint32_t>(width);
    image_height = static_cast<uint32_t>(height);
    image_stride = static_cast<uint32_t>(stride);
    return true;
  }

  bool upload_t::begin(const uint8_t *pixels, int width, int height, int stride, VkFormat format) {
    if (recording || !pixels || stride <= 0) {
      return false;
    }
    // Four bytes a pixel, so a row of pixels is a whole number of texels wide however capture padded
    // it. A stride that is not is a capture backend this path cannot read, and saying so beats
    // uploading a sheared picture.
    if (stride % 4 != 0) {
      BOOST_LOG(warning) << "PyroWave: a "sv << stride << " byte row is not a whole number of pixels"sv;
      return false;
    }
    if (!prepare(width, height, stride, format)) {
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

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = static_cast<uint32_t>(stride / 4);
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image_width, image_height, 1};
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

  bool upload_t::begin_retained() {
    if (recording || image == VK_NULL_HANDLE ||
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
      return false;
    }
    if (api.reset_fences(owner->device, 1, &fence) != VK_SUCCESS) {
      return false;
    }

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (api.queue_submit(owner->queue, 1, &submit, fence) != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: the queue refused a frame"sv;
      // Nothing ran, so the transitions this command buffer described did not happen either.
      image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      return false;
    }

    // Waiting here is the contract, not a shortcut. The codec's packetizing entry points read a
    // buffer this submission writes, and its own header says the caller submits and waits before
    // calling them when the command buffer is the caller's.
    const auto waited = api.wait_for_fences(owner->device, 1, &fence, VK_TRUE, fence_timeout_ns);
    if (waited != VK_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: the GPU did not finish a frame within a second (result "sv
                       << static_cast<int>(waited) << ')';
      image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    // Not submitted, so the picture never reached the image and the layout it claims is a lie. Say
    // so, and the next frame will transition from undefined rather than from a layout it is not in.
    api.reset_command_pool(owner->device, pool, 0);
    image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

}  // namespace pyrowave_encode
