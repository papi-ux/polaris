/**
 * @file src/platform/linux/vulkan_encode.cpp
 * @brief Vulkan-native encoder: DMA-BUF -> Vulkan compute (RGB->YUV) -> Vulkan Video encode.
 *        No EGL/GL dependency — all GPU work stays in a single Vulkan queue.
 */
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <drm_fourcc.h>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#if defined(__FreeBSD__)
  #include <sys/types.h>
#else
  #include <sys/sysmacros.h>
#endif
#include <vector>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video_colorspace.h"
#include "vulkan_encode.h"

// SPIR-V data generated at build time
static const std::vector<uint32_t> rgb2yuv_comp_spv_data
#include "shaders/rgb2yuv.spv.inc"
  ;
static const size_t rgb2yuv_comp_spv_size = rgb2yuv_comp_spv_data.size() * sizeof(uint32_t);

using namespace std::literals;

namespace vk {

  // Match a DRI device node path to a Vulkan device index via VK_EXT_physical_device_drm.
  // Returns the index as a string (e.g. "1"), or empty string if no match.
  static std::string find_vulkan_index_for_render_node(const char *render_path) {
    struct stat node_stat;
    if (stat(render_path, &node_stat) < 0) {
      return {};
    }

    auto target_major = major(node_stat.st_rdev);
    auto target_minor = minor(node_stat.st_rdev);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
      return {};
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(inst, &count, devs.data());

    std::string result;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t extension_count = 0;
      if (vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &extension_count, nullptr) != VK_SUCCESS) {
        continue;
      }
      std::vector<VkExtensionProperties> extensions(extension_count);
      if (vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &extension_count, extensions.data()) != VK_SUCCESS) {
        continue;
      }
      const auto has_drm_properties = std::ranges::any_of(extensions, [](const auto &extension) {
        return std::string_view {extension.extensionName} == VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
      });
      if (!has_drm_properties) {
        continue;
      }

      VkPhysicalDeviceDrmPropertiesEXT drm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &drm;
      vkGetPhysicalDeviceProperties2(devs[i], &props2);
      const bool render_match = drm.hasRender && drm.renderMajor == (int64_t) target_major && drm.renderMinor == (int64_t) target_minor;
      const bool primary_match = drm.hasPrimary && drm.primaryMajor == (int64_t) target_major && drm.primaryMinor == (int64_t) target_minor;
      if (render_match || primary_match) {
        result = std::to_string(i);
        break;
      }
    }
    vkDestroyInstance(inst, nullptr);
    return result;
  }

  static int create_vulkan_hwdevice(AVBufferRef **hw_device_buf, std::string_view capture_render_path = {}) {
    // KMS passes the capture card's exact render node. Using a different physical
    // device can make DMA-BUF import fail or silently add a cross-GPU copy.
    const bool require_exact_device = !capture_render_path.empty() || !config::video.adapter_name.empty();
    const std::string render_path = !capture_render_path.empty() ?
                                      std::string {capture_render_path} :
                                      (config::video.adapter_name.empty() ? platf::default_render_device() : config::video.adapter_name);
    if (!render_path.empty() && render_path.front() == '/') {
      const auto idx = find_vulkan_index_for_render_node(render_path.c_str());
      if (!idx.empty()) {
        if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, idx.c_str(), nullptr, 0) >= 0) {
          BOOST_LOG(info) << "Vulkan Video matched render node [" << render_path << "] to physical device [" << idx << ']';
          return 0;
        }
      }
      if (require_exact_device) {
        BOOST_LOG(error) << "Vulkan Video could not create the physical device matching render node [" << render_path << ']';
        return -1;
      }
    } else if (!render_path.empty()) {
      // Non-path: treat as device name substring or numeric index
      if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, render_path.c_str(), nullptr, 0) >= 0) {
        return 0;
      }
      if (require_exact_device) {
        BOOST_LOG(error) << "Vulkan Video could not create configured device [" << render_path << ']';
        return -1;
      }
    }
    // Final fallback: let FFmpeg pick default
    if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) >= 0) {
      return 0;
    }
    return -1;
  }

  /**
   * @brief Vulkan shader constants used by the conversion pass.
   */
  struct PushConstants {
    std::array<float, 4> color_vec_y;  ///< Color vec y.
    std::array<float, 4> color_vec_u;  ///< Color vec u.
    std::array<float, 4> color_vec_v;  ///< Color vec v.
    std::array<float, 2> range_y;  ///< Range y.
    std::array<float, 2> range_uv;  ///< Range uv.
    std::array<int32_t, 2> src_offset;  ///< Src offset.
    std::array<int32_t, 2> src_size;  ///< Src size.
    std::array<int32_t, 2> dst_offset;  ///< Dst offset.
    std::array<int32_t, 2> dst_size;  ///< Dst size.
    std::array<int32_t, 2> dst_full_size;  ///< Dst full size.
    std::array<int32_t, 2> cursor_pos;  ///< Cursor pos.
    std::array<int32_t, 2> cursor_size;  ///< Cursor size.
    int32_t y_invert;  ///< Y invert.
  };

// Helper to check VkResult
/**
 * @def VK_CHECK(expr)
 * @brief Macro for VK CHECK.
 */
#define VK_CHECK(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return -1; \
    } \
  } while (0)
/**
 * @def VK_CHECK_BOOL(expr)
 * @brief Macro for VK CHECK BOOL.
 */
#define VK_CHECK_BOOL(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return false; \
    } \
  } while (0)

  /**
   * @brief Vulkan encode device that keeps converted frames in GPU memory.
   */
  class vk_vram_t: public platf::avcodec_encode_device_t {
  public:
    ~vk_vram_t() override {
      cleanup_pipeline();
    }

    /**
     * @brief Initialize Vulkan encode device and conversion resources.
     *
     * @param in_width In width.
     * @param in_height In height.
     * @param in_offset_x In offset x.
     * @param in_offset_y In offset y.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(
      int in_width,
      int in_height,
      int in_offset_x,
      int in_offset_y,
      std::string in_render_device,
      bool in_ram_input = false
    ) {
      if (in_width <= 0 || in_height <= 0) {
        BOOST_LOG(error) << "Invalid Vulkan capture dimensions ["sv
                         << in_width << 'x' << in_height << ']';
        return -1;
      }
      width = in_width;
      height = in_height;
      offset_x = in_offset_x;
      offset_y = in_offset_y;
      render_device = std::move(in_render_device);
      ram_input = in_ram_input;
      this->data = (void *) &init_hw_device;
      return 0;
    }

    /**
     * @brief Initialize codec options.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param options Request options or socket options to apply.
     */
    void init_codec_options(AVCodecContext *ctx, AVDictionary **options) override {
      // When VBR mode is selected (rc_mode=4), don't pin rc_min_rate to the target bitrate.
      // Having rc_min_rate == rc_max_rate == bit_rate in VBR mode prevents the encoder from
      // undershooting on simple frames, which builds up headroom that causes large overshoots
      if (config::video.vk.rc_mode == 4) {
        ctx->rc_min_rate = 0;
      }
    }

    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param new_frame Frame to attach.
     * @param hw_frames_ctx_buf Hardware frames context buffer.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(new_frame);
      this->frame = new_frame;
      this->hw_frames_ctx = hw_frames_ctx_buf;

      auto *frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      auto *dev_ctx = (AVHWDeviceContext *) frames_ctx->device_ref->data;
      vk_dev.ctx = (AVVulkanDeviceContext *) dev_ctx->hwctx;
      vk_dev.dev = vk_dev.ctx->act_dev;
      vk_dev.phys_dev = vk_dev.ctx->phys_dev;
      is_10bit = (frames_ctx->sw_format == AV_PIX_FMT_P010);

      {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(vk_dev.phys_dev, &p);
        BOOST_LOG(info) << "Vulkan encode using GPU: " << p.deviceName;
      }

      // Find a compute-capable queue family from FFmpeg's context
      vk_dev.compute_qf = -1;
      for (int i = 0; i < vk_dev.ctx->nb_qf; i++) {
        if (vk_dev.ctx->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
          vk_dev.compute_qf = vk_dev.ctx->qf[i].idx;
          break;
        }
      }
      if (vk_dev.compute_qf < 0) {
        BOOST_LOG(error) << "No compute queue family in Vulkan device"sv;
        return -1;
      }

      vkGetDeviceQueue(vk_dev.dev, vk_dev.compute_qf, 0, &vk_dev.compute_queue);

      // Load extension functions
      vk_dev.getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(vk_dev.dev, "vkGetMemoryFdPropertiesKHR");

      if (!create_compute_pipeline()) {
        return -1;
      }
      if (!create_command_resources()) {
        return -1;
      }
      if (ram_input && !create_ram_upload_resources()) {
        return -1;
      }

      return 0;
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    void apply_colorspace() override {
      auto *colors = video::color_vectors_from_colorspace(colorspace);
      if (colors) {
        memcpy(push.color_vec_y.data(), colors->color_vec_y, sizeof(push.color_vec_y));
        memcpy(push.color_vec_u.data(), colors->color_vec_u, sizeof(push.color_vec_u));
        memcpy(push.color_vec_v.data(), colors->color_vec_v, sizeof(push.color_vec_v));
        memcpy(push.range_y.data(), colors->range_y, sizeof(push.range_y));
        memcpy(push.range_uv.data(), colors->range_uv, sizeof(push.range_uv));
      }
    }

    /**
     * @brief Configure FFmpeg Vulkan hardware frames for video encode input.
     *
     * @param frames FFmpeg hardware frames context to initialize.
     */
    void init_hwframes(AVHWFramesContext *frames) override {
      frames->initial_pool_size = 4;
      auto *vk_frames = (AVVulkanFramesContext *) frames->hwctx;
      vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
      // FFmpeg creates a profile-independent pool before the selected codec's
      // video profile is known. EXTENDED_USAGE lets storage-only plane views
      // coexist with the profile-compatible video encode view without making
      // unrelated transfer/sampling usage part of the video-format contract.
      vk_frames->img_flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
                              VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      vk_frames->usage = (VkImageUsageFlagBits) (VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
    }

    /**
     * @brief Convert a captured frame into a Vulkan hardware frame.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      auto *descriptor = ram_input ? nullptr : dynamic_cast<egl::img_descriptor_t *>(&img);
      if (!ram_input && !descriptor) {
        BOOST_LOG(error) << "Vulkan DMA-BUF conversion received a non-DMA-BUF frame"sv;
        return -1;
      }

      // Get encoder target frame
      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0) < 0) {
          BOOST_LOG(error) << "Failed to get Vulkan frame"sv;
          return -1;
        }
      }

      // Setup Y/UV image views for the encoder target (once)
      if (!target.views_created) {
        if (!create_target_views()) {
          return -1;
        }
        target.views_created = true;
      }

      // Encoder probes use a dummy capture frame. Submit an explicit encoded
      // black frame instead of returning an uninitialized Vulkan allocation.
      if (descriptor && descriptor->sequence == 0) {
        push.src_offset = {0, 0};
        push.src_size = {1, 1};
        push.dst_offset = {0, 0};
        push.dst_size = {0, 0};
        push.dst_full_size = {frame->width, frame->height};
        push.cursor_size = {0, 0};
        push.y_invert = 0;
        return dispatch_compute(true);
      }

      // Import new DMA-BUF as VkImage when capture sequence changes
      if (descriptor && descriptor->sequence > sequence) {
        if (!import_dmabuf(descriptor->sd)) {
          BOOST_LOG(error) << "Failed to import DMA-BUF"sv;
          return -1;
        }
        sequence = descriptor->sequence;
      }

      if (!ram_input && src.image == VK_NULL_HANDLE) {
        return -1;
      }

      if (descriptor && descriptor->data && descriptor->serial != cursor_serial) {
        if (!create_cursor_image(descriptor->src_w, descriptor->src_h, descriptor->data)) {
          return -1;
        }
        cursor_serial = descriptor->serial;
      }

      // Preserve aspect ratio: fit src into dst, center with black bars.
      // UV plane is subsampled 2x, so keep effective size and offset even.
      float scalar = std::min((float) frame->width / width, (float) frame->height / height);
      int32_t eff_w = std::min<int32_t>(((int32_t) (width * scalar)) & ~1, frame->width & ~1);
      int32_t eff_h = std::min<int32_t>(((int32_t) (height * scalar)) & ~1, frame->height & ~1);
      int32_t dst_off_x = ((frame->width - eff_w) / 2) & ~1;
      int32_t dst_off_y = ((frame->height - eff_h) / 2) & ~1;
      eff_w = std::min(eff_w, (frame->width - dst_off_x) & ~1);
      eff_h = std::min(eff_h, (frame->height - dst_off_y) & ~1);

      // Fill push constants
      push.src_offset[0] = offset_x;
      push.src_offset[1] = offset_y;
      push.src_size[0] = width;
      push.src_size[1] = height;
      push.dst_offset[0] = dst_off_x;
      push.dst_offset[1] = dst_off_y;
      push.dst_size[0] = eff_w;
      push.dst_size[1] = eff_h;
      push.dst_full_size[0] = frame->width;
      push.dst_full_size[1] = frame->height;
      push.y_invert = descriptor && descriptor->y_invert ? 1 : 0;

      if (descriptor && descriptor->data) {
        float scale_x = (float) eff_w / width;
        float scale_y = (float) eff_h / height;
        push.cursor_pos[0] = (int32_t) ((descriptor->x - offset_x) * scale_x) + dst_off_x;
        push.cursor_pos[1] = (int32_t) ((descriptor->y - offset_y) * scale_y) + dst_off_y;
        // The cursor buffer uses src_w/src_h, while width/height are the KMS
        // plane's displayed dimensions after compositor scaling.
        push.cursor_size[0] = (int32_t) (descriptor->width * scale_x);
        push.cursor_size[1] = (int32_t) (descriptor->height * scale_y);
      } else {
        push.cursor_size[0] = 0;
      }

      // Record and submit compute dispatch
      return dispatch_compute(false, ram_input ? &img : nullptr);
    }

  private:
    struct image_resource_t {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
    };

    struct upload_resource_t {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      void *mapped = nullptr;
      bool coherent = false;
    };

    // Three submissions can remain in flight. A slot's fence is waited only
    // when the ring wraps, preserving pipelining while making command-buffer,
    // descriptor-set, and imported-image lifetimes explicit.
    static constexpr std::size_t CMD_RING_SIZE = 3;

    struct cmd_slot_t {
      VkCommandBuffer buffer = VK_NULL_HANDLE;
      VkFence fence = VK_NULL_HANDLE;
      VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
      bool submitted = false;
      std::vector<image_resource_t> retired_images;
      image_resource_t ram_source;
      upload_resource_t ram_upload;
      bool ram_source_initialized = false;
    };

    bool create_compute_pipeline() {
      // Shader module
      VkShaderModuleCreateInfo shader_ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shader_ci.codeSize = rgb2yuv_comp_spv_size;
      shader_ci.pCode = rgb2yuv_comp_spv_data.data();
      VK_CHECK_BOOL(vkCreateShaderModule(vk_dev.dev, &shader_ci, nullptr, &compute.shader_module));

      // Descriptor set layout: binding 0=sampler, 1=Y storage, 2=UV storage, 3=cursor sampler
      std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
      bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

      VkDescriptorSetLayoutCreateInfo ds_layout_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      ds_layout_ci.bindingCount = bindings.size();
      ds_layout_ci.pBindings = bindings.data();
      VK_CHECK_BOOL(vkCreateDescriptorSetLayout(vk_dev.dev, &ds_layout_ci, nullptr, &compute.ds_layout));

      // Push constant range
      VkPushConstantRange pc_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};

      VkPipelineLayoutCreateInfo pl_ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl_ci.setLayoutCount = 1;
      pl_ci.pSetLayouts = &compute.ds_layout;
      pl_ci.pushConstantRangeCount = 1;
      pl_ci.pPushConstantRanges = &pc_range;
      VK_CHECK_BOOL(vkCreatePipelineLayout(vk_dev.dev, &pl_ci, nullptr, &compute.pipeline_layout));

      // Compute pipeline
      VkComputePipelineCreateInfo comp_ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      comp_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      comp_ci.stage.module = compute.shader_module;
      comp_ci.stage.pName = "main";
      comp_ci.layout = compute.pipeline_layout;
      VK_CHECK_BOOL(vkCreateComputePipelines(vk_dev.dev, VK_NULL_HANDLE, 1, &comp_ci, nullptr, &compute.pipeline));

      // Descriptor pool
      std::array<VkDescriptorPoolSize, 2> pool_sizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * CMD_RING_SIZE},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * CMD_RING_SIZE},
      }};
      VkDescriptorPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool_ci.maxSets = CMD_RING_SIZE;
      pool_ci.poolSizeCount = pool_sizes.size();
      pool_ci.pPoolSizes = pool_sizes.data();
      VK_CHECK_BOOL(vkCreateDescriptorPool(vk_dev.dev, &pool_ci, nullptr, &compute.desc_pool));

      std::array<VkDescriptorSetLayout, CMD_RING_SIZE> layouts;
      layouts.fill(compute.ds_layout);
      std::array<VkDescriptorSet, CMD_RING_SIZE> descriptor_sets = {};
      VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc_info.descriptorPool = compute.desc_pool;
      alloc_info.descriptorSetCount = descriptor_sets.size();
      alloc_info.pSetLayouts = layouts.data();
      VK_CHECK_BOOL(vkAllocateDescriptorSets(vk_dev.dev, &alloc_info, descriptor_sets.data()));
      for (std::size_t i = 0; i < CMD_RING_SIZE; ++i) {
        cmd.slots[i].descriptor_set = descriptor_sets[i];
      }

      // Sampler for source image
      VkSamplerCreateInfo sampler_ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler_ci.magFilter = VK_FILTER_LINEAR;
      sampler_ci.minFilter = VK_FILTER_LINEAR;
      sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      VK_CHECK_BOOL(vkCreateSampler(vk_dev.dev, &sampler_ci, nullptr, &compute.sampler));

      const std::array<uint8_t, 4> transparent_pixel = {};
      if (!create_cursor_image(1, 1, transparent_pixel.data())) {
        return false;
      }

      return true;
    }

    bool create_command_resources() {
      VkCommandPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_ci.queueFamilyIndex = vk_dev.compute_qf;
      pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      VK_CHECK_BOOL(vkCreateCommandPool(vk_dev.dev, &pool_ci, nullptr, &cmd.pool));

      VkCommandBufferAllocateInfo alloc_ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc_ci.commandPool = cmd.pool;
      alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc_ci.commandBufferCount = CMD_RING_SIZE;
      std::array<VkCommandBuffer, CMD_RING_SIZE> command_buffers = {};
      VK_CHECK_BOOL(vkAllocateCommandBuffers(vk_dev.dev, &alloc_ci, command_buffers.data()));

      VkFenceCreateInfo fence_ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      for (std::size_t i = 0; i < CMD_RING_SIZE; ++i) {
        cmd.slots[i].buffer = command_buffers[i];
        VK_CHECK_BOOL(vkCreateFence(vk_dev.dev, &fence_ci, nullptr, &cmd.slots[i].fence));
      }

      return true;
    }

    bool create_ram_upload_resources() {
      VkFormatProperties format_properties {};
      vkGetPhysicalDeviceFormatProperties(
        vk_dev.phys_dev,
        VK_FORMAT_B8G8R8A8_UNORM,
        &format_properties
      );
      constexpr auto required_format_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((format_properties.optimalTilingFeatures & required_format_features) != required_format_features) {
        BOOST_LOG(error) << "Vulkan device cannot sample from an uploaded BGRA capture image"sv;
        return false;
      }

      const auto upload_size = static_cast<VkDeviceSize>(width) *
                               static_cast<VkDeviceSize>(height) * 4;
      for (auto &slot : cmd.slots) {
        VkImageCreateInfo image_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_ci.imageType = VK_IMAGE_TYPE_2D;
        image_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_ci.extent = {
          static_cast<std::uint32_t>(width),
          static_cast<std::uint32_t>(height),
          1
        };
        image_ci.mipLevels = 1;
        image_ci.arrayLayers = 1;
        image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(vk_dev.dev, &image_ci, nullptr, &slot.ram_source.image) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to create Vulkan RAM-upload source image"sv;
          return false;
        }

        VkMemoryRequirements image_requirements {};
        vkGetImageMemoryRequirements(vk_dev.dev, slot.ram_source.image, &image_requirements);
        auto image_memory_type = find_memory_type(
          image_requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );
        if (!image_memory_type) {
          image_memory_type = find_memory_type(image_requirements.memoryTypeBits, 0);
        }
        if (!image_memory_type) {
          BOOST_LOG(error) << "Could not select Vulkan memory for RAM-upload source image"sv;
          return false;
        }

        VkMemoryAllocateInfo image_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        image_alloc.allocationSize = image_requirements.size;
        image_alloc.memoryTypeIndex = *image_memory_type;
        if (vkAllocateMemory(vk_dev.dev, &image_alloc, nullptr, &slot.ram_source.mem) != VK_SUCCESS ||
            vkBindImageMemory(vk_dev.dev, slot.ram_source.image, slot.ram_source.mem, 0) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to allocate or bind Vulkan RAM-upload source image"sv;
          return false;
        }

        VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image = slot.ram_source.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &slot.ram_source.view) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to create Vulkan RAM-upload source image view"sv;
          return false;
        }

        VkBufferCreateInfo buffer_ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size = upload_size;
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk_dev.dev, &buffer_ci, nullptr, &slot.ram_upload.buffer) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to create Vulkan RAM-upload staging buffer"sv;
          return false;
        }

        VkMemoryRequirements buffer_requirements {};
        vkGetBufferMemoryRequirements(vk_dev.dev, slot.ram_upload.buffer, &buffer_requirements);
        auto upload_memory_type = find_memory_type(
          buffer_requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        slot.ram_upload.coherent = upload_memory_type.has_value();
        if (!upload_memory_type) {
          upload_memory_type = find_memory_type(
            buffer_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
          );
        }
        if (!upload_memory_type) {
          BOOST_LOG(error) << "Could not select host-visible Vulkan memory for RAM upload"sv;
          return false;
        }

        VkMemoryAllocateInfo buffer_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        buffer_alloc.allocationSize = buffer_requirements.size;
        buffer_alloc.memoryTypeIndex = *upload_memory_type;
        if (vkAllocateMemory(vk_dev.dev, &buffer_alloc, nullptr, &slot.ram_upload.mem) != VK_SUCCESS ||
            vkBindBufferMemory(vk_dev.dev, slot.ram_upload.buffer, slot.ram_upload.mem, 0) != VK_SUCCESS ||
            vkMapMemory(vk_dev.dev, slot.ram_upload.mem, 0, VK_WHOLE_SIZE, 0, &slot.ram_upload.mapped) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to allocate, bind, or map Vulkan RAM-upload staging memory"sv;
          return false;
        }
      }

      BOOST_LOG(info) << "Vulkan Video RAM upload path initialized with "sv
                      << CMD_RING_SIZE << " in-flight staging slots"sv;
      return true;
    }

    bool stage_ram_frame(cmd_slot_t &slot, const platf::img_t &img) {
      if (!slot.ram_upload.mapped) {
        return false;
      }
      if (img.data &&
          (img.pixel_pitch != 4 || img.row_pitch < std::max(img.width, 0) * 4)) {
        BOOST_LOG(error) << "Vulkan RAM upload requires packed 4-byte BGRA rows"sv;
        return false;
      }

      auto *dst = static_cast<std::uint8_t *>(slot.ram_upload.mapped);
      const auto tight_row_size = static_cast<std::size_t>(width) * 4;
      const auto tight_frame_size = tight_row_size * static_cast<std::size_t>(height);
      if (!img.data) {
        std::memset(dst, 0, tight_frame_size);
      } else if (img.width == width &&
                 img.height == height &&
                 img.pixel_pitch == 4 &&
                 img.row_pitch == static_cast<int>(tight_row_size)) {
        std::memcpy(dst, img.data, tight_frame_size);
      } else {
        std::memset(dst, 0, tight_frame_size);
        const auto copy_height = std::max(0, std::min(height, img.height));
        const auto copy_width = std::max(0, std::min(width, img.width));
        const auto copy_row_size = static_cast<std::size_t>(copy_width) * 4;
        for (int row = 0; row < copy_height; ++row) {
          std::memcpy(
            dst + static_cast<std::size_t>(row) * tight_row_size,
            img.data + static_cast<std::size_t>(row) * img.row_pitch,
            copy_row_size
          );
        }
      }

      if (!slot.ram_upload.coherent) {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = slot.ram_upload.mem;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(vk_dev.dev, 1, &range) != VK_SUCCESS) {
          BOOST_LOG(error) << "Failed to flush Vulkan RAM-upload staging memory"sv;
          return false;
        }
      }
      return true;
    }

    struct drm_format_info {
      VkFormat format;
      VkComponentMapping swizzle;
    };

    static drm_format_info drm_fourcc_to_vk_format(uint32_t fourcc) {
      static constexpr VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
      };
      static constexpr VkComponentMapping bgr_swap = {
        VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_A,
      };

      switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
          return {VK_FORMAT_R8G8B8A8_UNORM, identity};
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
          return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
          return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_ABGR16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, identity};
        case DRM_FORMAT_XRGB16161616:
        case DRM_FORMAT_ARGB16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, bgr_swap};
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, identity};
        case DRM_FORMAT_XRGB16161616F:
        case DRM_FORMAT_ARGB16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, bgr_swap};
        default:
          BOOST_LOG(warning) << "Unknown DRM fourcc 0x" << std::hex << fourcc << std::dec << ", assuming B8G8R8A8";
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
      }
    }

    /**
     * @brief Query the driver-expected plane count for a format+modifier pair.
     * @return Expected plane count, or 0 if unknown.
     */
    int query_modifier_plane_count(VkFormat format, uint64_t modifier) {
      VkDrmFormatModifierPropertiesListEXT mod_list = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 fmt_props2 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      fmt_props2.pNext = &mod_list;
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(mod_list.drmFormatModifierCount);
      mod_list.pDrmFormatModifierProperties = mod_props.data();
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      for (const auto &mp : mod_props) {
        if (mp.drmFormatModifier == modifier) {
          return mp.drmFormatModifierPlaneCount;
        }
      }
      return 0;
    }

    void destroy_image_resource(image_resource_t &resource) {
      if (resource.view) {
        vkDestroyImageView(vk_dev.dev, resource.view, nullptr);
      }
      if (resource.image) {
        vkDestroyImage(vk_dev.dev, resource.image, nullptr);
      }
      if (resource.mem) {
        vkFreeMemory(vk_dev.dev, resource.mem, nullptr);
      }
      resource = {};
    }

    void destroy_upload_resource(upload_resource_t &resource) {
      if (resource.mapped && resource.mem) {
        vkUnmapMemory(vk_dev.dev, resource.mem);
      }
      if (resource.buffer) {
        vkDestroyBuffer(vk_dev.dev, resource.buffer, nullptr);
      }
      if (resource.mem) {
        vkFreeMemory(vk_dev.dev, resource.mem, nullptr);
      }
      resource = {};
    }

    void retire_image_resource(image_resource_t &resource, std::optional<std::size_t> &last_used_slot) {
      if (!resource.image && !resource.view && !resource.mem) {
        last_used_slot.reset();
        return;
      }

      if (last_used_slot && cmd.slots[*last_used_slot].submitted) {
        cmd.slots[*last_used_slot].retired_images.push_back(resource);
        resource = {};
      } else {
        destroy_image_resource(resource);
      }
      last_used_slot.reset();
    }

    void destroy_retired_images(cmd_slot_t &slot) {
      for (auto &resource : slot.retired_images) {
        destroy_image_resource(resource);
      }
      slot.retired_images.clear();
    }

    bool import_dmabuf(const egl::surface_descriptor_t &sd) {
      int fd = dup(sd.fds[0]);
      if (fd < 0) {
        BOOST_LOG(error) << "Could not duplicate DMA-BUF descriptor: "sv << strerror(errno);
        return false;
      }

      // Query memory requirements for this DMA-BUF
      VkMemoryFdPropertiesKHR fd_props = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      if (!vk_dev.getMemoryFdProperties) {
        BOOST_LOG(error) << "Vulkan device does not expose vkGetMemoryFdPropertiesKHR for DMA-BUF import"sv;
        close(fd);
        return false;
      }
      auto res = vk_dev.getMemoryFdProperties(vk_dev.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);
      if (res != VK_SUCCESS || fd_props.memoryTypeBits == 0) {
        BOOST_LOG(error) << "vkGetMemoryFdPropertiesKHR failed for DMA-BUF: "sv << res;
        close(fd);
        return false;
      }

      // Create VkImage for the DMA-BUF
      VkExternalMemoryImageCreateInfo ext_ci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

      std::array<VkSubresourceLayout, 4> drm_layouts = {};
      VkImageDrmFormatModifierExplicitCreateInfoEXT drm_ci = {
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT
      };
      VkImageTiling tiling;

      auto [vk_format, vk_swizzle] = drm_fourcc_to_vk_format(sd.fourcc);

      if (sd.modifier != DRM_FORMAT_MOD_INVALID) {
        int dmabuf_planes = 0;
        for (int i = 0; i < 4 && sd.fds[i] >= 0; ++i) {
          dmabuf_planes++;
        }

        // Query driver for the expected plane count for this format+modifier.
        // DMA-BUF exports may include extra metadata planes (e.g. AMD DCC).
        int expected = query_modifier_plane_count(vk_format, sd.modifier);
        int plane_count = (expected > 0 && expected <= dmabuf_planes) ? expected : dmabuf_planes;

        for (int i = 0; i < plane_count; ++i) {
          drm_layouts[i].offset = sd.offsets[i];
          drm_layouts[i].rowPitch = sd.pitches[i];
        }
        drm_ci.drmFormatModifier = sd.modifier;
        drm_ci.drmFormatModifierPlaneCount = plane_count;
        drm_ci.pPlaneLayouts = drm_layouts.data();
        ext_ci.pNext = &drm_ci;
        tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      } else {
        tiling = VK_IMAGE_TILING_LINEAR;
      }

      VkImageCreateInfo img_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.pNext = &ext_ci;
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = vk_format;
      img_ci.extent = {(uint32_t) sd.width, (uint32_t) sd.height, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = tiling;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      image_resource_t next_src;
      res = vkCreateImage(vk_dev.dev, &img_ci, nullptr, &next_src.image);
      if (res != VK_SUCCESS) {
        close(fd);
        BOOST_LOG(error) << "vkCreateImage for DMA-BUF failed: " << res
                         << " (modifier=0x" << std::hex << sd.modifier << std::dec
                         << ", pitch=" << sd.pitches[0] << ", offset=" << sd.offsets[0] << ")";
        return false;
      }

      // Bind imported DMA-BUF memory
      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, next_src.image, &mem_req);

      const auto compatible_type_bits = fd_props.memoryTypeBits & mem_req.memoryTypeBits;
      if (compatible_type_bits == 0) {
        BOOST_LOG(error) << "DMA-BUF has no Vulkan memory type compatible with the imported image"sv;
        close(fd);
        destroy_image_resource(next_src);
        return false;
      }

      auto memory_type = find_memory_type(compatible_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!memory_type) {
        memory_type = find_memory_type(compatible_type_bits, 0);
        if (memory_type) {
          BOOST_LOG(warning) << "DMA-BUF import has no device-local Vulkan memory type; using compatible type ["sv << *memory_type << ']';
        }
      }
      if (!memory_type) {
        BOOST_LOG(error) << "Could not select a compatible Vulkan memory type for DMA-BUF import"sv;
        close(fd);
        destroy_image_resource(next_src);
        return false;
      }

      VkImportMemoryFdInfoKHR import_fd = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      import_fd.fd = fd;  // Vulkan takes ownership

      VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.pNext = &import_fd;
      alloc_info.allocationSize = mem_req.size;
      alloc_info.memoryTypeIndex = *memory_type;

      res = vkAllocateMemory(vk_dev.dev, &alloc_info, nullptr, &next_src.mem);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkAllocateMemory for DMA-BUF failed: " << res;
        close(fd);
        destroy_image_resource(next_src);
        return false;
      }
      // A successful import transfers ownership of the duplicated fd to Vulkan.
      fd = -1;

      res = vkBindImageMemory(vk_dev.dev, next_src.image, next_src.mem, 0);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkBindImageMemory for DMA-BUF failed: "sv << res;
        destroy_image_resource(next_src);
        return false;
      }

      // Create image view
      VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = next_src.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = vk_format;
      view_ci.components = vk_swizzle;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      res = vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &next_src.view);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImageView for DMA-BUF failed: "sv << res;
        destroy_image_resource(next_src);
        return false;
      }

      retire_image_resource(src, src_last_used_slot);
      src = next_src;
      return true;
    }

    bool create_cursor_image(int w, int h, const uint8_t *pixels) {
      if (w <= 0 || h <= 0) {
        BOOST_LOG(error) << "Invalid Vulkan cursor dimensions ["sv << w << 'x' << h << ']';
        return false;
      }

      VkImageCreateInfo img_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      img_ci.extent = {(uint32_t) w, (uint32_t) h, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = VK_IMAGE_TILING_LINEAR;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      image_resource_t next_cursor;
      auto res = vkCreateImage(vk_dev.dev, &img_ci, nullptr, &next_cursor.image);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImage for cursor failed: "sv << res;
        return false;
      }

      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, next_cursor.image, &mem_req);
      const auto memory_type = find_memory_type(
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
      );
      if (!memory_type) {
        BOOST_LOG(error) << "Could not select host-visible coherent Vulkan memory for the cursor"sv;
        destroy_image_resource(next_cursor);
        return false;
      }

      VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.allocationSize = mem_req.size;
      alloc.memoryTypeIndex = *memory_type;
      res = vkAllocateMemory(vk_dev.dev, &alloc, nullptr, &next_cursor.mem);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkAllocateMemory for cursor failed: "sv << res;
        destroy_image_resource(next_cursor);
        return false;
      }
      res = vkBindImageMemory(vk_dev.dev, next_cursor.image, next_cursor.mem, 0);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkBindImageMemory for cursor failed: "sv << res;
        destroy_image_resource(next_cursor);
        return false;
      }

      if (pixels) {
        void *mapped = nullptr;
        res = vkMapMemory(vk_dev.dev, next_cursor.mem, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (res != VK_SUCCESS) {
          BOOST_LOG(error) << "vkMapMemory for cursor failed: "sv << res;
          destroy_image_resource(next_cursor);
          return false;
        }
        VkImageSubresource subres = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(vk_dev.dev, next_cursor.image, &subres, &layout);
        for (int y = 0; y < h; y++) {
          memcpy((uint8_t *) mapped + layout.offset + y * layout.rowPitch, pixels + y * w * 4, w * 4);
        }
        vkUnmapMemory(vk_dev.dev, next_cursor.mem);
      }

      VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = next_cursor.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      res = vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &next_cursor.view);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImageView for cursor failed: "sv << res;
        destroy_image_resource(next_cursor);
        return false;
      }

      retire_image_resource(cursor.resource, cursor_last_used_slot);
      cursor.resource = next_cursor;
      cursor.needs_transition = true;
      return true;
    }

    bool create_target_views() {
      auto *vk_frame = (AVVkFrame *) frame->data[0];
      if (!vk_frame) {
        return false;
      }

      auto y_fmt = is_10bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
      auto uv_fmt = is_10bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;

      // Detect multiplane vs multi-image layout
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }

      VkImageViewUsageCreateInfo storage_usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      storage_usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;

      if (num_imgs == 1) {
        // Single multiplane image — create plane views
        VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.pNext = &storage_usage;
        view_ci.image = vk_frame->img[0];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;

        // Y plane
        view_ci.format = y_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        // UV plane
        view_ci.format = uv_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      } else {
        // Separate images per plane
        VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.pNext = &storage_usage;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        view_ci.image = vk_frame->img[0];
        view_ci.format = y_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        view_ci.image = vk_frame->img[1];
        view_ci.format = uv_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      }
      return true;
    }

    void update_descriptors(VkDescriptorSet descriptor_set, VkImageView source_view) {
      VkDescriptorImageInfo src_info = {compute.sampler, source_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo y_info = {VK_NULL_HANDLE, target.y_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo uv_info = {VK_NULL_HANDLE, target.uv_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo cursor_info = {compute.sampler, cursor.resource.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

      std::array<VkWriteDescriptorSet, 4> writes = {};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_info, nullptr, nullptr};
      writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &y_info, nullptr, nullptr};
      writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &uv_info, nullptr, nullptr};
      writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cursor_info, nullptr, nullptr};
      vkUpdateDescriptorSets(vk_dev.dev, writes.size(), writes.data(), 0, nullptr);
    }

    bool prepare_command_slot(cmd_slot_t &slot) {
      if (slot.submitted) {
        auto res = vkWaitForFences(vk_dev.dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        if (res != VK_SUCCESS) {
          BOOST_LOG(error) << "vkWaitForFences for Vulkan conversion slot failed: "sv << res;
          return false;
        }

        destroy_retired_images(slot);
        res = vkResetFences(vk_dev.dev, 1, &slot.fence);
        if (res != VK_SUCCESS) {
          BOOST_LOG(error) << "vkResetFences for Vulkan conversion slot failed: "sv << res;
          return false;
        }
        slot.submitted = false;
      } else {
        destroy_retired_images(slot);
      }

      const auto res = vkResetCommandBuffer(slot.buffer, 0);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkResetCommandBuffer for Vulkan conversion slot failed: "sv << res;
        return false;
      }
      return true;
    }

    int dispatch_compute(bool blank, const platf::img_t *ram_img = nullptr) {
      auto *vk_frame = (AVVkFrame *) frame->data[0];
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }

      const auto slot_index = cmd.ring_idx;
      auto &slot = cmd.slots[slot_index];
      if (!prepare_command_slot(slot)) {
        return -1;
      }
      if (ram_img && !stage_ram_frame(slot, *ram_img)) {
        return -1;
      }
      auto cmd_buf = slot.buffer;

      // Each in-flight submission owns its descriptor set. Updating a shared
      // set here would race a prior frame still executing on the GPU.
      const auto source_view = blank ?
                                 cursor.resource.view :
                                 (ram_img ? slot.ram_source.view : src.view);
      update_descriptors(slot.descriptor_set, source_view);

      VkCommandBufferBeginInfo begin_ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_CHECK(vkBeginCommandBuffer(cmd_buf, &begin_ci));

      if (ram_img && !blank) {
        VkImageMemoryBarrier upload_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        upload_barrier.srcAccessMask = slot.ram_source_initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        upload_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        upload_barrier.oldLayout = slot.ram_source_initialized ?
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                     VK_IMAGE_LAYOUT_UNDEFINED;
        upload_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        upload_barrier.image = slot.ram_source.image;
        upload_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        upload_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        upload_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(
          cmd_buf,
          slot.ram_source_initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          0,
          0,
          nullptr,
          0,
          nullptr,
          1,
          &upload_barrier
        );

        VkBufferImageCopy copy_region {};
        copy_region.bufferOffset = 0;
        copy_region.bufferRowLength = static_cast<std::uint32_t>(width);
        copy_region.bufferImageHeight = static_cast<std::uint32_t>(height);
        copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_region.imageOffset = {0, 0, 0};
        copy_region.imageExtent = {
          static_cast<std::uint32_t>(width),
          static_cast<std::uint32_t>(height),
          1
        };
        vkCmdCopyBufferToImage(
          cmd_buf,
          slot.ram_upload.buffer,
          slot.ram_source.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          1,
          &copy_region
        );

        VkImageMemoryBarrier sample_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sample_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sample_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sample_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sample_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sample_barrier.image = slot.ram_source.image;
        sample_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        sample_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sample_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(
          cmd_buf,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0,
          0,
          nullptr,
          0,
          nullptr,
          1,
          &sample_barrier
        );
      } else if (!blank) {
        // Transition the external capture image to SHADER_READ_ONLY. Blank
        // probe frames bind the local transparent image and never sample it.
        VkImageMemoryBarrier src_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        src_barrier.srcAccessMask = 0;
        src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        src_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src_barrier.image = src.image;
        src_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        src_barrier.dstQueueFamilyIndex = vk_dev.compute_qf;

        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &src_barrier);
      }

      // Transition cursor image if needed
      const bool cursor_needs_transition = cursor.needs_transition;
      if (cursor_needs_transition) {
        VkImageMemoryBarrier cursor_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        cursor_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        cursor_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        cursor_barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        cursor_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cursor_barrier.image = cursor.resource.image;
        cursor_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cursor_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cursor_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cursor_barrier);
      }

      // Transition target planes to GENERAL for storage writes
      std::array<VkImageMemoryBarrier, 2> dst_barriers = {};
      int num_dst_barriers = (num_imgs == 1) ? 1 : 2;
      for (int i = 0; i < num_dst_barriers; i++) {
        dst_barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        // The timeline semaphore makes prior writes available. Access flags
        // from FFmpeg's video queue are not meaningful on this compute queue.
        dst_barriers[i].srcAccessMask = 0;
        dst_barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        dst_barriers[i].oldLayout = vk_frame->layout[num_imgs == 1 ? 0 : i];
        dst_barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst_barriers[i].image = vk_frame->img[num_imgs == 1 ? 0 : i];
        dst_barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dst_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dst_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      }

      vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, num_dst_barriers, dst_barriers.data());

      // Bind pipeline and dispatch
      vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline);
      vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline_layout, 0, 1, &slot.descriptor_set, 0, nullptr);
      vkCmdPushConstants(cmd_buf, compute.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &push);

      uint32_t gx = (frame->width + 15) / 16;
      uint32_t gy = (frame->height + 15) / 16;
      vkCmdDispatch(cmd_buf, gx, gy, 1);

      VK_CHECK(vkEndCommandBuffer(cmd_buf));

      // Submit with timeline semaphore signaling for FFmpeg
      VkTimelineSemaphoreSubmitInfo timeline_info = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> wait_sems = {};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> signal_sems = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> wait_vals = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> signal_vals = {};
      std::array<VkPipelineStageFlags, AV_NUM_DATA_POINTERS> wait_stages = {};
      int sem_count = 0;

      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->sem[i]; i++) {
        wait_sems[sem_count] = vk_frame->sem[i];
        wait_vals[sem_count] = vk_frame->sem_value[i];
        wait_stages[sem_count] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        signal_sems[sem_count] = vk_frame->sem[i];
        signal_vals[sem_count] = vk_frame->sem_value[i] + 1;
        sem_count++;
      }

      timeline_info.waitSemaphoreValueCount = sem_count;
      timeline_info.pWaitSemaphoreValues = wait_vals.data();
      timeline_info.signalSemaphoreValueCount = sem_count;
      timeline_info.pSignalSemaphoreValues = signal_vals.data();

      VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.pNext = &timeline_info;
      submit.waitSemaphoreCount = sem_count;
      submit.pWaitSemaphores = wait_sems.data();
      submit.pWaitDstStageMask = wait_stages.data();
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd_buf;
      submit.signalSemaphoreCount = sem_count;
      submit.pSignalSemaphores = signal_sems.data();

      auto res = vkQueueSubmit(vk_dev.compute_queue, 1, &submit, slot.fence);

      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkQueueSubmit failed: " << res;
        return -1;
      }

      for (int i = 0; i < sem_count; ++i) {
        vk_frame->sem_value[i]++;
      }
      slot.submitted = true;
      if (ram_img && !blank) {
        slot.ram_source_initialized = true;
      } else if (!blank) {
        src_last_used_slot = slot_index;
      }
      cursor_last_used_slot = slot_index;
      if (cursor_needs_transition) {
        cursor.needs_transition = false;
      }
      cmd.ring_idx = (cmd.ring_idx + 1) % CMD_RING_SIZE;

      // Update frame layouts for FFmpeg
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        vk_frame->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
        // The timeline semaphore makes the compute writes available to the
        // video queue. Shader access flags are invalid on a video-only queue,
        // so do not carry them into FFmpeg's encode-side barrier.
        vk_frame->access[i] = static_cast<VkAccessFlagBits>(0);
        vk_frame->queue_family[i] = vk_dev.compute_qf;
      }

      target.initialized = true;

      return 0;
    }

    std::optional<uint32_t> find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
      VkPhysicalDeviceMemoryProperties mem_props;
      vkGetPhysicalDeviceMemoryProperties(vk_dev.phys_dev, &mem_props);
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
          return i;
        }
      }
      return std::nullopt;
    }

    void cleanup_pipeline() {
      if (!vk_dev.dev || resources_released) {
        return;
      }

      const bool report_teardown = sequence > 0;
      if (report_teardown) {
        BOOST_LOG(info) << "Vulkan converter teardown: waiting for device idle"sv;
      }
      const auto idle_status = vkDeviceWaitIdle(vk_dev.dev);
      if (report_teardown) {
        BOOST_LOG(info) << "Vulkan converter teardown: device idle status="sv << idle_status;
      }

      destroy_image_resource(src);
      destroy_image_resource(cursor.resource);
      for (auto &slot : cmd.slots) {
        destroy_retired_images(slot);
        destroy_image_resource(slot.ram_source);
        destroy_upload_resource(slot.ram_upload);
        slot.ram_source_initialized = false;
        if (slot.fence) {
          vkDestroyFence(vk_dev.dev, slot.fence, nullptr);
          slot.fence = VK_NULL_HANDLE;
        }
      }
      if (target.y_view) {
        vkDestroyImageView(vk_dev.dev, target.y_view, nullptr);
        target.y_view = VK_NULL_HANDLE;
      }
      if (target.uv_view) {
        vkDestroyImageView(vk_dev.dev, target.uv_view, nullptr);
        target.uv_view = VK_NULL_HANDLE;
      }
      if (cmd.pool) {
        vkDestroyCommandPool(vk_dev.dev, cmd.pool, nullptr);
        cmd.pool = VK_NULL_HANDLE;
      }
      if (compute.sampler) {
        vkDestroySampler(vk_dev.dev, compute.sampler, nullptr);
        compute.sampler = VK_NULL_HANDLE;
      }
      if (compute.desc_pool) {
        vkDestroyDescriptorPool(vk_dev.dev, compute.desc_pool, nullptr);
        compute.desc_pool = VK_NULL_HANDLE;
      }
      if (compute.pipeline) {
        vkDestroyPipeline(vk_dev.dev, compute.pipeline, nullptr);
        compute.pipeline = VK_NULL_HANDLE;
      }
      if (compute.pipeline_layout) {
        vkDestroyPipelineLayout(vk_dev.dev, compute.pipeline_layout, nullptr);
        compute.pipeline_layout = VK_NULL_HANDLE;
      }
      if (compute.ds_layout) {
        vkDestroyDescriptorSetLayout(vk_dev.dev, compute.ds_layout, nullptr);
        compute.ds_layout = VK_NULL_HANDLE;
      }
      if (compute.shader_module) {
        vkDestroyShaderModule(vk_dev.dev, compute.shader_module, nullptr);
        compute.shader_module = VK_NULL_HANDLE;
      }
      target.views_created = false;
      target.initialized = false;
      if (report_teardown) {
        BOOST_LOG(info) << "Vulkan converter teardown: conversion resources released"sv;
      }

      // This AVFrame keeps FFmpeg's Vulkan hardware device alive while the
      // platform-owned conversion resources above are destroyed. The owning
      // encode session closes AVCodecContext first so codec-owned picture views
      // and synchronization objects are gone before this final reference drops.
      hwframe.reset();
      frame = nullptr;
      hw_frames_ctx = nullptr;
      resources_released = true;

      if (report_teardown) {
        BOOST_LOG(info) << "Vulkan converter teardown: complete"sv;
      }
    }

    static int init_hw_device(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf) {
      const auto *self = static_cast<const vk_vram_t *>(encode_device);
      return create_vulkan_hwdevice(hw_device_buf, self->render_device);
    }

    // Dimensions
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    std::string render_device;
    bool ram_input = false;
    bool is_10bit = false;
    AVBufferRef *hw_frames_ctx = nullptr;
    frame_t hwframe;
    std::uint64_t sequence = 0;

    // Vulkan device (from FFmpeg)
    struct vk_device_t {
      VkDevice dev = VK_NULL_HANDLE;
      VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
      AVVulkanDeviceContext *ctx = nullptr;
      int compute_qf = -1;
      VkQueue compute_queue = VK_NULL_HANDLE;
      PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
    };

    vk_device_t vk_dev = {};
    bool resources_released = false;

    // Compute pipeline
    struct compute_pipeline_t {
      VkShaderModule shader_module = VK_NULL_HANDLE;
      VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
      VkDescriptorPool desc_pool = VK_NULL_HANDLE;
      VkSampler sampler = VK_NULL_HANDLE;
    };

    compute_pipeline_t compute = {};

    struct cmd_submission_t {
      VkCommandPool pool = VK_NULL_HANDLE;
      std::array<cmd_slot_t, CMD_RING_SIZE> slots = {};
      std::size_t ring_idx = 0;
    };

    cmd_submission_t cmd = {};

    image_resource_t src = {};
    std::optional<std::size_t> src_last_used_slot;

    // Target NV12 plane views
    struct target_state_t {
      VkImageView y_view = VK_NULL_HANDLE;
      VkImageView uv_view = VK_NULL_HANDLE;
      bool views_created = false;
      bool initialized = false;
    };

    target_state_t target = {};

    // Cursor image
    struct {
      image_resource_t resource;
      bool needs_transition = false;
    } cursor = {};
    std::optional<std::size_t> cursor_last_used_slot;

    unsigned long cursor_serial = 0;

    // Push constants (color matrix)
    PushConstants push = {};
  };

  // Free functions

  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *, AVBufferRef **hw_device_buf) {
    return create_vulkan_hwdevice(hw_device_buf);
  }

  bool validate() {
    if (!avcodec_find_encoder_by_name("h264_vulkan") &&
        !avcodec_find_encoder_by_name("hevc_vulkan")) {
      return false;
    }
    AVBufferRef *dev = nullptr;
    if (create_vulkan_hwdevice(&dev) < 0) {
      return false;
    }
    av_buffer_unref(&dev);
    return true;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(
    int w,
    int h,
    int offset_x,
    int offset_y,
    std::string render_device
  ) {
    auto dev = std::make_unique<vk_vram_t>();
    if (dev->init(w, h, offset_x, offset_y, std::move(render_device)) < 0) {
      return nullptr;
    }
    return dev;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(
    int w,
    int h,
    std::string render_device
  ) {
    auto dev = std::make_unique<vk_vram_t>();
    if (dev->init(w, h, 0, 0, std::move(render_device), true) < 0) {
      return nullptr;
    }
    return dev;
  }

}  // namespace vk
