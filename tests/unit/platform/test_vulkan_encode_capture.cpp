/**
 * @file tests/unit/platform/test_vulkan_encode_capture.cpp
 * @brief Tests the Vulkan Video encoder's live capture path: a real DMA-BUF and cursor into an encoder frame.
 */
// test includes
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_VULKAN)

  // standard includes
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <cstdlib>
  #include <cstring>
  #include <functional>
  #include <iostream>
  #include <limits>
  #include <memory>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <utility>
  #include <vector>

  // platform includes
  #include <drm_fourcc.h>
  #include <fcntl.h>
  #include <gbm.h>
  #include <unistd.h>

// lib includes
extern "C" {
  #include <libavutil/frame.h>
  #include <libavutil/hwcontext.h>
  #include <libavutil/hwcontext_vulkan.h>
}

  // lib includes
  #include <boost/core/null_deleter.hpp>
  #include <boost/log/core.hpp>
  #include <boost/log/sinks/sync_frontend.hpp>
  #include <boost/log/sinks/text_ostream_backend.hpp>
  #include <boost/smart_ptr/make_shared_object.hpp>
  #include <boost/smart_ptr/shared_ptr.hpp>

  // local includes
  #include "src/logging.h"
  #include "src/platform/linux/graphics.h"
  #include "src/platform/linux/vulkan_encode.h"
  #include "src/platform/linux/vulkan_loader.h"
  #ifdef POLARIS_BUILD_PORTAL
    #include "../../fixtures/pipewire_producer_graph.h"
    #include "src/video.h"
  #endif

namespace {
  class vulkan_discovery_environment_t final: public testing::Environment {
    void TearDown() override {
      vk::shutdown_discovery();
    }
  };

  [[maybe_unused]] const auto *discovery_environment = testing::AddGlobalTestEnvironment(new vulkan_discovery_environment_t);

  constexpr int frame_width = 256;
  constexpr int frame_height = 144;
  constexpr const char *render_node = "/dev/dri/renderD128";

  struct buffer_unref_t {
    void operator()(AVBufferRef *buffer) const {
      av_buffer_unref(&buffer);
    }
  };

  using buffer_t = std::unique_ptr<AVBufferRef, buffer_unref_t>;

  /**
   * A linear XRGB8888 buffer the GPU owns, filled with one grey level or four grey quadrants.
   * Linear because it is the only layout a test can write by hand, and an explicit linear modifier because that is the shape a
   * portal buffer arrives in, so the import takes its modifier path rather than assuming a tiling.
   */
  struct grey_dmabuf_t {
    explicit grey_dmabuf_t(std::uint8_t level, int width = frame_width, int height = frame_height):
        grey_dmabuf_t {std::array<std::uint8_t, 4> {level, level, level, level}, width, height} {
    }

    grey_dmabuf_t(const std::array<std::uint8_t, 4> &levels, int width, int height):
        capture_width {width},
        capture_height {height} {
      node = ::open(render_node, O_RDWR | O_CLOEXEC);
      if (node < 0) {
        return;
      }
      device = gbm_create_device(node);
      if (!device) {
        return;
      }
      const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
      bo = gbm_bo_create_with_modifiers(device, width, height, GBM_FORMAT_XRGB8888, &linear, 1);
      if (!bo) {
        return;
      }

      void *map_data = nullptr;
      std::uint32_t map_stride = 0;
      auto *mapped = static_cast<std::uint8_t *>(
        gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data)
      );
      if (!mapped) {
        return;
      }
      for (int row = 0; row < height; ++row) {
        const auto quadrant_row = row < height / 2 ? 0 : 2;
        auto *pixels = mapped + static_cast<std::size_t>(row) * map_stride;
        std::memset(pixels, levels[quadrant_row], static_cast<std::size_t>(width / 2) * 4);
        std::memset(pixels + (width / 2) * 4, levels[quadrant_row + 1], static_cast<std::size_t>(width - width / 2) * 4);
      }
      gbm_bo_unmap(bo, map_data);
      ok = true;
    }

    ~grey_dmabuf_t() {
      if (bo) {
        gbm_bo_destroy(bo);
      }
      if (device) {
        gbm_device_destroy(device);
      }
      if (node >= 0) {
        ::close(node);
      }
    }

    grey_dmabuf_t(const grey_dmabuf_t &) = delete;
    grey_dmabuf_t &operator=(const grey_dmabuf_t &) = delete;

    /**
     * Describe the buffer the way capture does. The descriptor owns the exported descriptor and
     * closes it, as it does for a captured frame.
     */
    void describe(egl::img_descriptor_t &img, std::uint64_t sequence) const {
      img.reset();
      img.sd.width = capture_width;
      img.sd.height = capture_height;
      img.sd.fds[0] = gbm_bo_get_fd(bo);
      img.sd.fourcc = DRM_FORMAT_XRGB8888;
      img.sd.modifier = gbm_bo_get_modifier(bo);
      img.sd.pitches[0] = gbm_bo_get_stride(bo);
      img.sd.offsets[0] = gbm_bo_get_offset(bo, 0);
      img.sequence = sequence;
    }

    bool ok = false;

    bool overwrite(std::uint8_t level) {
      void *map_data = nullptr;
      std::uint32_t stride = 0;
      auto *mapped = static_cast<std::uint8_t *>(gbm_bo_map(bo, 0, 0, capture_width, capture_height, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
      if (!mapped) return false;
      for (int row = 0; row < capture_height; ++row) std::memset(mapped + row * stride, level, capture_width * 4);
      gbm_bo_unmap(bo, map_data);
      return true;
    }

    int export_fd() const { return gbm_bo_get_fd(bo); }
    std::uint32_t stride() const { return gbm_bo_get_stride(bo); }

  private:
    int capture_width;
    int capture_height;
    int node = -1;
    gbm_device *device = nullptr;
    gbm_bo *bo = nullptr;
  };

  /**
   * An encoder frame and the device that converts into it, set up the way video.cpp sets them up for
   * a session. The hardware device and the frames context are declared before the converting device
   * so they outlive it: its teardown still uses the Vulkan device, and it keeps a plain pointer to the
   * frames context.
   */
  struct encoder_frame_t {
    buffer_t hw_device;
    buffer_t frames_ref;
    std::unique_ptr<platf::avcodec_encode_device_t> device;
    AVFrame *frame = nullptr;  // The device owns it once set_frame() takes it.

    bool open(std::unique_ptr<platf::avcodec_encode_device_t> made, int width = frame_width, int height = frame_height) {
      device = std::move(made);
      if (!device || !device->data) {
        return false;
      }
      using init_hw_device_fn = int (*)(platf::avcodec_encode_device_t *, AVBufferRef **);
      AVBufferRef *created_device = nullptr;
      if (reinterpret_cast<init_hw_device_fn>(device->data)(device.get(), &created_device) != 0) {
        return false;
      }
      hw_device.reset(created_device);

      frames_ref.reset(av_hwframe_ctx_alloc(hw_device.get()));
      if (!frames_ref) {
        return false;
      }
      auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_ref->data);
      frames->format = AV_PIX_FMT_VULKAN;
      frames->sw_format = AV_PIX_FMT_NV12;
      frames->width = width;
      frames->height = height;
      frames->initial_pool_size = 0;
      device->init_hwframes(frames);
      if (av_hwframe_ctx_init(frames_ref.get()) < 0) {
        return false;
      }

      frame = av_frame_alloc();
      if (!frame || device->set_frame(frame, frames_ref.get()) != 0) {
        return false;
      }
      device->colorspace = {video::colorspace_e::rec709, false, 8};
      device->apply_colorspace();
      return true;
    }
  };

  // Observe Polaris's actual submission boundary while leaving FFmpeg's Vulkan
  // entry points alone. Faults are injected at the call boundary, not into the
  // driver. Every function pointer is restored after the owned device closes.
  struct capture_completion_boundary_t {
    inline static capture_completion_boundary_t *active = nullptr;
    PFN_vkWaitForFences original_wait = platf::vulkan_loader::vkWaitForFences;
    PFN_vkQueueSubmit original_submit = platf::vulkan_loader::vkQueueSubmit;
    PFN_vkDeviceWaitIdle original_idle = platf::vulkan_loader::vkDeviceWaitIdle;
    PFN_vkDestroyImage original_destroy_image = platf::vulkan_loader::vkDestroyImage;
    std::weak_ptr<platf::img_t> lease;
    VkFence submitted_fence = VK_NULL_HANDLE;
    std::vector<std::uint64_t> waits;
    std::vector<VkResult> idle_results;
    std::size_t idle_calls = 0;
    int image_destructions = 0;
    bool image_destroyed_before_retirement = false;
    int submissions = 0;
    bool timeout = false;
    VkResult submit_error = VK_SUCCESS;
    bool completion_observed = false;
    bool idle_observed = false;
    bool lease_retained_at_wait = true;
    bool lease_retained_at_idle = false;
    bool returned_before_completion = false;
    int returned = 0;
    std::function<void()> on_wait;

    void install() {
      EXPECT_EQ(active, nullptr);
      active = this;
      platf::vulkan_loader::vkWaitForFences = wait;
      platf::vulkan_loader::vkQueueSubmit = submit;
      platf::vulkan_loader::vkDeviceWaitIdle = idle;
      platf::vulkan_loader::vkDestroyImage = destroy_image;
    }

    ~capture_completion_boundary_t() {
      if (active == this) {
        platf::vulkan_loader::vkWaitForFences = original_wait;
        platf::vulkan_loader::vkQueueSubmit = original_submit;
        platf::vulkan_loader::vkDeviceWaitIdle = original_idle;
        platf::vulkan_loader::vkDestroyImage = original_destroy_image;
        active = nullptr;
      }
    }

    void publish(egl::img_descriptor_t &img) {
      completion_observed = false;
      img.dmabuf_buffer_key = 1;
      img.requires_producer_lease = true;
      img.capture_owner = std::shared_ptr<platf::img_t>(new platf::img_t, [this](platf::img_t *owner) {
        returned_before_completion |= !completion_observed && !idle_observed;
        ++returned;
        delete owner;
      });
      lease = img.capture_owner;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice device, std::uint32_t count, const VkFence *fences, VkBool32 all, std::uint64_t timeout_ns) {
      auto &self = *active;
      self.waits.push_back(timeout_ns);
      if (timeout_ns != UINT64_MAX) {
        // One owner is still held by the caller's descriptor; GPU submission
        // must hold another even if that descriptor is released on return.
        self.lease_retained_at_wait &= self.lease.use_count() >= 2;
      }
      if (self.timeout) {
        return VK_TIMEOUT;
      }
      if (self.on_wait) {
        auto action = std::move(self.on_wait);
        action();
      }
      const auto result = self.original_wait(device, count, fences, all, timeout_ns);
      if (result == VK_SUCCESS && count == 1 && fences[0] == self.submitted_fence) {
        self.completion_observed = true;
      }
      return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
      auto &self = *active;
      ++self.submissions;
      if (self.submit_error != VK_SUCCESS) {
        return self.submit_error;
      }
      const auto result = self.original_submit(queue, count, submits, fence);
      if (result == VK_SUCCESS) {
        self.submitted_fence = fence;
      }
      return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice device) {
      auto &self = *active;
      self.lease_retained_at_idle = !self.lease.expired();
      const auto attempt = self.idle_calls++;
      if (attempt < self.idle_results.size() && self.idle_results[attempt] != VK_ERROR_DEVICE_LOST) {
        // Failed waits do not establish completion. Leave actual GPU work and
        // the producer lease alone until a subsequent terminal observation.
        return self.idle_results[attempt];
      }
      const auto result = self.original_idle(device);
      // Complete the real fixture's GPU work before reporting injected loss;
      // this tests Polaris's error handling, not a fault inside the driver.
      const auto reported = result == VK_SUCCESS && attempt < self.idle_results.size() ? self.idle_results[attempt] : result;
      self.idle_observed = reported == VK_SUCCESS || reported == VK_ERROR_DEVICE_LOST;
      return reported;
    }

    static VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
      auto &self = *active;
      ++self.image_destructions;
      self.image_destroyed_before_retirement |= self.idle_calls > 0 && !self.idle_observed;
      self.original_destroy_image(device, image, allocator);
    }
  };

  struct capture_queue_boundary_t {
    inline static capture_queue_boundary_t *active = nullptr;
    PFN_vkCmdPipelineBarrier original_barrier = platf::vulkan_loader::vkCmdPipelineBarrier;
    PFN_vkCmdDispatch original_dispatch = platf::vulkan_loader::vkCmdDispatch;
    std::vector<VkImageMemoryBarrier> transfers;
    std::vector<char> order;

    void install() {
      ASSERT_EQ(active, nullptr);
      active = this;
      platf::vulkan_loader::vkCmdPipelineBarrier = barrier;
      platf::vulkan_loader::vkCmdDispatch = dispatch;
    }

    ~capture_queue_boundary_t() {
      if (active == this) {
        platf::vulkan_loader::vkCmdPipelineBarrier = original_barrier;
        platf::vulkan_loader::vkCmdDispatch = original_dispatch;
        active = nullptr;
      }
    }

    static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                             VkDependencyFlags flags, std::uint32_t memory_count, const VkMemoryBarrier *memory,
                                             std::uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
                                             std::uint32_t image_count, const VkImageMemoryBarrier *images) {
      auto &self = *active;
      for (std::uint32_t i = 0; i < image_count; ++i) {
        const auto &image = images[i];
        if (image.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL || image.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) {
          self.transfers.push_back(image);
          self.order.push_back('A');
          EXPECT_EQ(destination, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        } else if (image.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) {
          self.transfers.push_back(image);
          self.order.push_back('R');
          EXPECT_EQ(source, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
      }
      self.original_barrier(command, source, destination, flags, memory_count, memory, buffer_count, buffers, image_count, images);
    }

    static VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer command, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
      active->order.push_back('D');
      active->original_dispatch(command, x, y, z);
    }
  };

  // Observe and fault only Polaris's own capability/import calls. FFmpeg and
  // actual Vulkan allocation, conversion and readback still use the driver.
  struct capture_admission_boundary_t {
    inline static capture_admission_boundary_t *active = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 original_query = platf::vulkan_loader::vkGetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties2 original_formats = platf::vulkan_loader::vkGetPhysicalDeviceFormatProperties2;
    PFN_vkAllocateMemory original_allocate = platf::vulkan_loader::vkAllocateMemory;
    bool refuse_query = false;
    bool no_import = false;
    bool no_linear_sampling = false;
    bool small_extent = false;
    bool dedicated_only = false;
    int queries = 0;
    int imports = 0;
    bool dedicated_import = false;
    bool exact_query = false;
    VkResult import_error = VK_SUCCESS;

    void install() {
      ASSERT_EQ(active, nullptr);
      active = this;
      platf::vulkan_loader::vkGetPhysicalDeviceImageFormatProperties2 = query;
      platf::vulkan_loader::vkGetPhysicalDeviceFormatProperties2 = formats;
      platf::vulkan_loader::vkAllocateMemory = allocate;
    }

    ~capture_admission_boundary_t() {
      if (active == this) {
        platf::vulkan_loader::vkGetPhysicalDeviceImageFormatProperties2 = original_query;
        platf::vulkan_loader::vkGetPhysicalDeviceFormatProperties2 = original_formats;
        platf::vulkan_loader::vkAllocateMemory = original_allocate;
        active = nullptr;
      }
    }

    static VKAPI_ATTR VkResult VKAPI_CALL query(VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
                                               VkImageFormatProperties2 *properties) {
      auto &self = *active;
      ++self.queries;
      const auto *external = static_cast<const VkPhysicalDeviceExternalImageFormatInfo *>(info->pNext);
      const auto *modifier = external ? static_cast<const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *>(external->pNext) : nullptr;
      self.exact_query = info->format == VK_FORMAT_B8G8R8A8_UNORM && info->type == VK_IMAGE_TYPE_2D &&
                         info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && info->usage == VK_IMAGE_USAGE_SAMPLED_BIT &&
                         info->flags == 0 && external && external->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
                         external->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && modifier &&
                         modifier->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT &&
                         modifier->drmFormatModifier == DRM_FORMAT_MOD_LINEAR && modifier->sharingMode == VK_SHARING_MODE_EXCLUSIVE;
      if (self.refuse_query) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      const auto result = self.original_query(physical, info, properties);
      if (result == VK_SUCCESS) {
        auto *external_properties = static_cast<VkExternalImageFormatProperties *>(properties->pNext);
        if (self.no_import) {
          external_properties->externalMemoryProperties.externalMemoryFeatures &= ~VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        }
        if (self.dedicated_only) {
          external_properties->externalMemoryProperties.externalMemoryFeatures |= VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
        }
        if (self.small_extent) {
          properties->imageFormatProperties.maxExtent.width = frame_width - 1;
        }
      }
      return result;
    }

    static VKAPI_ATTR void VKAPI_CALL formats(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties) {
      auto &self = *active;
      self.original_formats(physical, format, properties);
      auto *modifiers = static_cast<VkDrmFormatModifierPropertiesListEXT *>(properties->pNext);
      if (self.no_linear_sampling && modifiers && modifiers->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT &&
          modifiers->pDrmFormatModifierProperties) {
        for (std::uint32_t i = 0; i < modifiers->drmFormatModifierCount; ++i) {
          modifiers->pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures &= ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
      }
    }

    static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice device, const VkMemoryAllocateInfo *info,
                                                  const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory) {
      auto &self = *active;
      const VkMemoryDedicatedAllocateInfo *dedicated = nullptr;
      bool imported = false;
      for (auto *next = static_cast<const VkBaseInStructure *>(info->pNext); next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
          dedicated = reinterpret_cast<const VkMemoryDedicatedAllocateInfo *>(next);
        } else if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
          imported = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(next)->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        }
      }
      if (imported) {
        ++self.imports;
        self.dedicated_import = dedicated && dedicated->image != VK_NULL_HANDLE && dedicated->buffer == VK_NULL_HANDLE;
        if (self.import_error != VK_SUCCESS) return self.import_error;
      }
      return self.original_allocate(device, info, callbacks, memory);
    }
  };

  /// A frame in host memory, the way portal shared memory capture hands one over.
  struct host_frame_t: platf::img_t {
    explicit host_frame_t(std::uint8_t level, int w = frame_width, int h = frame_height):
        pixels(static_cast<std::size_t>(w) * h * 4, level) {
      data = pixels.data();
      width = w;
      height = h;
      pixel_pitch = 4;
      row_pitch = w * 4;
    }

    std::vector<std::uint8_t> pixels;
  };

  /// The lines the host logs while it is alive, as an operator reads them.
  class log_capture_t {
  public:
    log_capture_t():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~log_capture_t() {
      boost::log::core::get()->remove_sink(sink_);
    }

    log_capture_t(const log_capture_t &) = delete;
    log_capture_t &operator=(const log_capture_t &) = delete;

    /// Every captured line that holds needle.
    [[nodiscard]] std::vector<std::string> lines_with(std::string_view needle) const {
      std::istringstream input {stream_->str()};
      std::vector<std::string> out;
      for (std::string line; std::getline(input, line);) {
        if (line.find(needle) != std::string::npos) {
          out.push_back(line);
        }
      }
      return out;
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
  };

  /**
   * Luma samples from the encoder's frame, read back through FFmpeg, which waits on the timeline
   * semaphores the conversion signalled. Returns the centre and the top-left corner.
   */
  std::pair<int, int> centre_and_corner_luma(AVFrame *hw_frame) {
    std::unique_ptr<AVFrame, void (*)(AVFrame *)> sw_frame {av_frame_alloc(), [](AVFrame *frame) {
                                                              av_frame_free(&frame);
                                                            }};
    sw_frame->format = AV_PIX_FMT_NV12;
    if (av_hwframe_transfer_data(sw_frame.get(), hw_frame, 0) < 0) {
      return {-1, -1};
    }
    const auto *luma = sw_frame->data[0];
    const auto pitch = static_cast<std::size_t>(sw_frame->linesize[0]);
    return {luma[(sw_frame->height / 2) * pitch + sw_frame->width / 2], luma[4 * pitch + 4]};
  }

  /// Check every visible luma/chroma byte, including the last row and column.
  /// Padding belongs to FFmpeg's allocation and is deliberately not graded.
  testing::AssertionResult has_grey_quadrants(AVFrame *hw_frame, int width, int height, const std::array<int, 4> &luma_levels) {
    std::unique_ptr<AVFrame, void (*)(AVFrame *)> sw_frame {av_frame_alloc(), [](AVFrame *frame) {
                                                              av_frame_free(&frame);
                                                            }};
    if (!sw_frame) {
      return testing::AssertionFailure() << "could not allocate readback frame";
    }
    sw_frame->format = AV_PIX_FMT_NV12;
    if (av_hwframe_transfer_data(sw_frame.get(), hw_frame, 0) < 0) {
      return testing::AssertionFailure() << "NV12 readback failed";
    }
    if (sw_frame->width != width || sw_frame->height != height || sw_frame->format != AV_PIX_FMT_NV12 ||
        !sw_frame->data[0] || !sw_frame->data[1] || sw_frame->linesize[0] < width || sw_frame->linesize[1] < width) {
      return testing::AssertionFailure() << "readback geometry or planes do not match " << width << 'x' << height;
    }
    std::size_t wrong_luma = 0, wrong_chroma = 0;
    for (int y = 0; y < height; ++y) {
      const auto *row = sw_frame->data[0] + static_cast<std::size_t>(y) * sw_frame->linesize[0];
      for (int x = 0; x < width; ++x) {
        const auto quadrant = (y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1);
        wrong_luma += std::abs(static_cast<int>(row[x]) - luma_levels[quadrant]) > 3;
      }
    }
    for (int y = 0; y < height / 2; ++y) {
      const auto *row = sw_frame->data[1] + static_cast<std::size_t>(y) * sw_frame->linesize[1];
      for (int x = 0; x < width; ++x) {
        wrong_chroma += std::abs(static_cast<int>(row[x]) - 128) > 3;
      }
    }
    if (wrong_luma || wrong_chroma) {
      return testing::AssertionFailure() << "incorrect visible NV12 bytes at " << width << 'x' << height
                                         << ": luma=" << wrong_luma << ", chroma=" << wrong_chroma;
    }
    return testing::AssertionSuccess();
  }
}  // namespace

TEST(VulkanEncodeCaptureTests, ImportsADmabufAndCursorIntoTheEncoderFrame) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  const grey_dmabuf_t black {0x00};
  if (!white.ok || !black.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }

  // Declared before the device so they outlive it: its teardown still uses the Vulkan device, and it
  // keeps a plain pointer to the frames context.
  buffer_t hw_device;
  buffer_t frames_ref;

  // The same sequence video.cpp runs for a VRAM capture session.
  auto device = vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node);
  ASSERT_TRUE(device);
  ASSERT_NE(device->data, nullptr);
  using init_hw_device_fn = int (*)(platf::avcodec_encode_device_t *, AVBufferRef **);
  AVBufferRef *created_device = nullptr;
  ASSERT_EQ(reinterpret_cast<init_hw_device_fn>(device->data)(device.get(), &created_device), 0);
  hw_device.reset(created_device);

  frames_ref.reset(av_hwframe_ctx_alloc(hw_device.get()));
  ASSERT_TRUE(frames_ref);
  auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_ref->data);
  frames->format = AV_PIX_FMT_VULKAN;
  frames->sw_format = AV_PIX_FMT_NV12;
  frames->width = frame_width;
  frames->height = frame_height;
  frames->initial_pool_size = 0;
  device->init_hwframes(frames);
  ASSERT_GE(av_hwframe_ctx_init(frames_ref.get()), 0);

  auto *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(device->set_frame(frame, frames_ref.get()), 0);  // The device owns the frame from here.
  device->colorspace = {video::colorspace_e::rec709, false, 8};
  device->apply_colorspace();

  // An opaque white cursor in the top-left corner, clear of the centre sample.
  std::vector<std::uint8_t> cursor_pixels(16 * 16 * 4, 0xFF);
  egl::img_descriptor_t img;
  img.data = cursor_pixels.data();
  img.src_w = img.width = 16;
  img.src_h = img.height = 16;
  img.x = img.y = 0;
  img.serial = 1;
  img.y_invert = false;

  white.describe(img, 1);
  ASSERT_EQ(device->convert(img), 0);
  const auto [white_centre, white_corner] = centre_and_corner_luma(frame);

  // A new capture sequence imports the next buffer and retires the previous one; the cursor, whose
  // serial has not changed, is drawn again from the image the first conversion uploaded.
  black.describe(img, 2);
  ASSERT_EQ(device->convert(img), 0);
  const auto [black_centre, black_corner] = centre_and_corner_luma(frame);

  // Rec. 709 limited range: white is 235 and black is 16. The margins only absorb rounding.
  EXPECT_GE(white_centre, 225) << "the white capture did not reach the encoder frame";
  EXPECT_LE(black_centre, 26) << "the black capture did not reach the encoder frame";
  EXPECT_GE(black_corner, 225) << "the cursor was not drawn over the capture";
  EXPECT_GE(white_corner, 225);
}

/**
 * The refusal belongs to the system memory upload alone.
 *
 * VRAM capture hands over exactly the frame that upload refuses whenever no cursor is drawn: a
 * DMA-BUF with nothing in host memory, since KMS capture sets the cursor pixels to null when none is
 * visible. Refused here, it would end every KMS or wlroots Vulkan Video stream on its first frame
 * without a cursor.
 */
TEST(VulkanEncodeCaptureTests, AVramFrameWithNoCursorIsImportedNotRefused) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));

  egl::img_descriptor_t img;
  white.describe(img, 1);
  ASSERT_EQ(img.data, nullptr) << "a cursor was drawn, so this is not the frame the refusal matches";
  ASSERT_TRUE(vk::ram_upload_cannot_read(img)) << "this is not the frame the system memory upload refuses";

  EXPECT_EQ(encoder.device->convert(img), 0) << "a VRAM frame with no cursor was refused, which ends the stream";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the DMA-BUF did not reach the encoder frame";
}

TEST(VulkanCaptureCompletionTests, UnleasedCaptureKeepsSubmissionPipelined) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);

  // wlroots also publishes buffer keys, and cloning retains its image owner.
  // Neither fact is a PipeWire producer-return lease.
  img.dmabuf_buffer_key = 123;
  img.capture_owner = std::make_shared<platf::img_t>();

  EXPECT_EQ(encoder.device->convert(img), 0);
  EXPECT_EQ(boundary.submissions, 1);
  EXPECT_TRUE(boundary.waits.empty());
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

TEST(VulkanCaptureCompletionTests, PublishedLeaseIsHeldUntilTheSubmittedFenceCompletes) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);

  EXPECT_EQ(encoder.device->convert(img), 0);
  EXPECT_EQ(boundary.waits.size(), 1);
  if (!boundary.waits.empty()) {
    EXPECT_LE(boundary.waits.front(), 500'000'000ULL);
  }
  EXPECT_TRUE(boundary.completion_observed);
  EXPECT_TRUE(boundary.lease_retained_at_wait);
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_FALSE(boundary.returned_before_completion);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

class VulkanCaptureGenerationTests: public testing::TestWithParam<std::uint64_t> {};

TEST_P(VulkanCaptureGenerationTests, ReplacementBufferIsReadWhenItsSequenceRestartsOrRepeats) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF}, black {0x00};
  if (!white.ok || !black.ok) {
    GTEST_SKIP() << "Could not allocate two linear DMA-BUFs on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  white.describe(img, 5);
  boundary.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);

  // PipeWire buffer keys are process-wide identities. A new capture instance
  // starts its own sequence, which alone cannot identify the imported image.
  black.describe(img, GetParam());
  boundary.publish(img);
  img.dmabuf_buffer_key = 2;
  ASSERT_EQ(encoder.device->convert(img), 0);
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 2);
  EXPECT_FALSE(boundary.returned_before_completion);
  const auto [centre, corner] = centre_and_corner_luma(encoder.frame);
  EXPECT_LE(centre, 25);
  EXPECT_LE(corner, 25);
  EXPECT_EQ(boundary.submissions, 2);
}

INSTANTIATE_TEST_SUITE_P(RestartAndRepeat, VulkanCaptureGenerationTests, testing::Values(1ULL, 5ULL));

class VulkanCaptureGeometryTests: public testing::TestWithParam<std::pair<int, int>> {};

TEST_P(VulkanCaptureGeometryTests, LeasedBufferReplacementConvertsEveryVisibleNv12Pixel) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const auto [width, height] = GetParam();
  const grey_dmabuf_t first {std::array<std::uint8_t, 4> {0, 85, 170, 255}, width, height};
  const grey_dmabuf_t second {std::array<std::uint8_t, 4> {255, 170, 85, 0}, width, height};
  if (!first.ok || !second.ok) {
    GTEST_SKIP() << "Could not allocate two " << width << 'x' << height << " linear DMA-BUFs on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(width, height, 0, 0, render_node), width, height));
  boundary.install();

  egl::img_descriptor_t img;
  first.describe(img, 5);
  boundary.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 1);
  // Independent expected Rec. 709 limited-range grey values; no shader-derived oracle.
  ASSERT_TRUE(has_grey_quadrants(encoder.frame, width, height, {16, 89, 162, 235}));

  second.describe(img, 1);
  boundary.publish(img);
  img.dmabuf_buffer_key = 2;
  ASSERT_EQ(encoder.device->convert(img), 0);
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 2);
  ASSERT_TRUE(has_grey_quadrants(encoder.frame, width, height, {235, 162, 89, 16}));
  EXPECT_EQ(boundary.submissions, 2);
  EXPECT_EQ(boundary.waits.size(), 2);
  EXPECT_TRUE(boundary.lease_retained_at_wait);
  EXPECT_FALSE(boundary.returned_before_completion);
}

INSTANTIATE_TEST_SUITE_P(FullFrame, VulkanCaptureGeometryTests, testing::Values(std::pair {1920, 1080}, std::pair {3840, 2160}));

class VulkanCaptureResizeTests: public testing::TestWithParam<std::pair<int, int>> {};

TEST_P(VulkanCaptureResizeTests, SmallerReplacementEndsBeforeImportOrSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const auto [new_width, new_height] = GetParam();
  const grey_dmabuf_t first {0xFF};
  const grey_dmabuf_t smaller {0x00, new_width, new_height};
  if (!first.ok || !smaller.ok) {
    GTEST_SKIP() << "Could not allocate two linear DMA-BUFs on " << render_node;
  }
  capture_completion_boundary_t completion;
  capture_admission_boundary_t admission;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  completion.install();
  admission.install();

  egl::img_descriptor_t img;
  first.describe(img, 1);
  completion.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  img.capture_owner.reset();
  ASSERT_EQ(admission.imports, 1);
  ASSERT_EQ(completion.submissions, 1);
  const auto queries_before_resize = admission.queries;

  smaller.describe(img, 2);
  completion.publish(img);
  img.dmabuf_buffer_key = 2;
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(admission.queries, queries_before_resize);
  EXPECT_EQ(admission.imports, 1);
  EXPECT_EQ(completion.submissions, 1);
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 2);
  EXPECT_TRUE(completion.lease.expired());

  // A refused live frame ends this converter's generation. A later valid
  // buffer must not silently revive it or be submitted under stale geometry.
  first.describe(img, 3);
  completion.publish(img);
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(admission.queries, queries_before_resize);
  EXPECT_EQ(admission.imports, 1);
  EXPECT_EQ(completion.submissions, 1);
}

INSTANTIATE_TEST_SUITE_P(SmallerDimensions, VulkanCaptureResizeTests, testing::Values(std::pair {128, 144}, std::pair {256, 72}));

class VulkanCaptureRegionTests: public testing::TestWithParam<std::pair<int, int>> {};

TEST_P(VulkanCaptureRegionTests, OutOfBoundsOffsetEndsBeforeImportOrSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  const auto [offset_x, offset_y] = GetParam();
  capture_completion_boundary_t completion;
  capture_admission_boundary_t admission;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, offset_x, offset_y, render_node)));
  completion.install();
  admission.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  completion.publish(img);

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(admission.queries, 0);
  EXPECT_EQ(admission.imports, 0);
  EXPECT_EQ(completion.submissions, 0);
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 1);
  EXPECT_TRUE(completion.lease.expired());
}

INSTANTIATE_TEST_SUITE_P(InvalidOffsets, VulkanCaptureRegionTests, testing::Values(std::pair {-1, 0}, std::pair {0, -1}, std::pair {std::numeric_limits<int>::max(), 0}, std::pair {0, std::numeric_limits<int>::max()}));

class VulkanCaptureCropTests: public testing::TestWithParam<std::pair<int, int>> {};

TEST_P(VulkanCaptureCropTests, RegionInsideALargerBufferConvertsTheRequestedPixels) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t quadrants {std::array<std::uint8_t, 4> {0, 85, 170, 255}, frame_width * 2, frame_height * 2};
  if (!quadrants.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  const auto [offset_x, offset_y] = GetParam();
  capture_completion_boundary_t completion;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, offset_x, offset_y, render_node)));
  completion.install();
  egl::img_descriptor_t img;
  quadrants.describe(img, 1);
  completion.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  const auto expected_luma = offset_x == 0 ? 16 : 235;
  ASSERT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {expected_luma, expected_luma, expected_luma, expected_luma}));
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 1);
  EXPECT_TRUE(completion.lease_retained_at_wait);
  EXPECT_FALSE(completion.returned_before_completion);
}

INSTANTIATE_TEST_SUITE_P(FirstAndLastQuadrants, VulkanCaptureCropTests, testing::Values(std::pair {0, 0}, std::pair {frame_width, frame_height}));

TEST(VulkanCaptureCompletionTests, CallerDescriptorCanRetireDuringTheSubmittedReadWait) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);
  bool descriptor_retired = false;
  boundary.on_wait = [&] {
    img.reset();
    descriptor_retired = true;
    EXPECT_FALSE(boundary.lease.expired());
    // A conversion argument may also hold a temporary shared_ptr. Require a
    // surviving producer lease rather than prescribing its exact copy count.
    EXPECT_GT(boundary.lease.use_count(), 0);
    EXPECT_EQ(boundary.returned, 0);
  };

  EXPECT_EQ(encoder.device->convert(img), 0);
  EXPECT_TRUE(descriptor_retired);
  EXPECT_TRUE(boundary.completion_observed);
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_TRUE(boundary.lease.expired());
  EXPECT_FALSE(boundary.returned_before_completion);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

TEST(VulkanCaptureCompletionTests, AOneBufferProducerCanSubmitBeyondTheCommandRing) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  for (int generation = 1; generation <= 7; ++generation) {
    // A producer can reuse its sole buffer only after the previous lease was
    // returned. Holding leases until ring wrap would stop after the first one.
    ASSERT_EQ(boundary.returned, generation - 1);
    white.describe(img, generation);
    boundary.publish(img);
    EXPECT_EQ(encoder.device->convert(img), 0);
    img.capture_owner.reset();
    EXPECT_EQ(boundary.returned, generation);
    EXPECT_FALSE(boundary.returned_before_completion);
  }
  EXPECT_EQ(std::count_if(boundary.waits.begin(), boundary.waits.end(), [](auto timeout_ns) {
              return timeout_ns <= 500'000'000ULL;
            }),
            7);
  EXPECT_TRUE(boundary.lease_retained_at_wait);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

TEST(VulkanCaptureCompletionTests, TimeoutRetainsTheLeaseUntilIdleAndStopsFurtherSubmissions) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  boundary.timeout = true;
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.waits.size(), 1);
  EXPECT_TRUE(boundary.lease_retained_at_wait);
  img.capture_owner.reset();
  EXPECT_FALSE(boundary.lease.expired());
  EXPECT_EQ(boundary.returned, 0);
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.submissions, 1);
  encoder.device.reset();
  EXPECT_TRUE(boundary.lease_retained_at_idle);
  EXPECT_TRUE(boundary.idle_observed);
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_FALSE(boundary.returned_before_completion);
}

TEST(VulkanCaptureCompletionTests, UnknownSubmissionFailureRetainsTheLeaseAndEndsConversionOnce) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  boundary.submit_error = VK_ERROR_UNKNOWN;
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  img.capture_owner.reset();
  EXPECT_FALSE(boundary.lease.expired());
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.submissions, 1);
  EXPECT_TRUE(boundary.waits.empty());
  encoder.device.reset();
  EXPECT_TRUE(boundary.lease_retained_at_idle);
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_FALSE(boundary.returned_before_completion);
}

class VulkanCaptureTeardownRetryTests: public testing::TestWithParam<VkResult> {};

TEST_P(VulkanCaptureTeardownRetryTests, FailedIdleWaitsKeepImportedResourcesAndLeaseUntilConfirmedRetirement) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  boundary.timeout = true;
  boundary.idle_results = {GetParam(), GetParam()};
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);
  ASSERT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  img.reset();
  ASSERT_EQ(boundary.returned, 0);
  ASSERT_FALSE(boundary.lease.expired());

  encoder.device.reset();

  EXPECT_EQ(boundary.idle_calls, 3);
  EXPECT_TRUE(boundary.idle_observed);
  EXPECT_TRUE(boundary.lease_retained_at_idle);
  EXPECT_GT(boundary.image_destructions, 0);
  EXPECT_FALSE(boundary.image_destroyed_before_retirement);
  EXPECT_FALSE(boundary.returned_before_completion);
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_TRUE(boundary.lease.expired());
  EXPECT_EQ(boundary.submissions, 1);
}

INSTANTIATE_TEST_SUITE_P(NonTerminalErrors, VulkanCaptureTeardownRetryTests, testing::Values(VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN));

TEST(VulkanCaptureCompletionTests, DeviceLossRetiresImportedResourcesAndLeaseWithoutRetry) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  boundary.timeout = true;
  boundary.idle_results = {VK_ERROR_DEVICE_LOST};
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);
  ASSERT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  img.reset();
  ASSERT_EQ(boundary.returned, 0);

  encoder.device.reset();

  EXPECT_EQ(boundary.idle_calls, 1);
  EXPECT_TRUE(boundary.idle_observed);
  EXPECT_TRUE(boundary.lease_retained_at_idle);
  EXPECT_GT(boundary.image_destructions, 0);
  EXPECT_FALSE(boundary.image_destroyed_before_retirement);
  EXPECT_FALSE(boundary.returned_before_completion);
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_TRUE(boundary.lease.expired());
}

class VulkanCaptureSubmissionRejectionTests: public testing::TestWithParam<VkResult> {};

TEST_P(VulkanCaptureSubmissionRejectionTests, UnaffectedRejectedSubmitReturnsItsUnusedLeaseBeforeTeardown) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  boundary.submit_error = GetParam();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  boundary.publish(img);

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  img.capture_owner.reset();
  EXPECT_TRUE(boundary.lease.expired()) << "the rejected queue submission never acquired a GPU read";
  EXPECT_EQ(boundary.returned, 1);
  EXPECT_TRUE(boundary.returned_before_completion);
  // No real GPU submission, completion wait, device idle or destructor is
  // needed to return storage after these two specified rejection results.
  EXPECT_TRUE(boundary.waits.empty());
  EXPECT_FALSE(boundary.idle_observed);
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.submissions, 1) << "a refused generation must not retry submission";
  encoder.device.reset();
  EXPECT_FALSE(boundary.lease_retained_at_idle);
  EXPECT_EQ(boundary.returned, 1);
}

INSTANTIATE_TEST_SUITE_P(
  UnaffectedResources, VulkanCaptureSubmissionRejectionTests,
  testing::Values(VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY));

TEST(VulkanCaptureCompletionTests, ALeasedBufferWithoutItsPublishedOwnerIsRefusedBeforeSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  img.dmabuf_buffer_key = 1;
  img.requires_producer_lease = true;

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.submissions, 0);
  EXPECT_TRUE(boundary.waits.empty());
}

TEST(VulkanCaptureCompletionTests, UnsupportedProducerFenceEndsBeforeGpuSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install();
  egl::img_descriptor_t img;
  img.sd.width = frame_width;
  img.sd.height = frame_height;
  img.sd.fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(img.sd.fds[0], 0);
  img.sd.fourcc = DRM_FORMAT_XRGB8888;
  img.sd.modifier = DRM_FORMAT_MOD_LINEAR;
  img.sd.pitches[0] = frame_width * 4;
  img.sequence = 1;
  boundary.publish(img);

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(boundary.submissions, 0);
  EXPECT_TRUE(boundary.waits.empty());
  // No GPU read was submitted; the caller can release its own lease immediately.
  boundary.completion_observed = true;
  img.capture_owner.reset();
  EXPECT_EQ(boundary.returned, 1);
}

namespace {
  enum class admission_fault_e {
    unknown_fourcc, unknown_modifier, extra_plane, different_allocation,
    no_modifier, short_pitch, zero_width, no_import, failed_query, device_limit, no_linear_sampling
  };

  class VulkanCaptureAdmissionBoundaryTests: public testing::TestWithParam<admission_fault_e> {};
}

TEST_P(VulkanCaptureAdmissionBoundaryTests, UnsupportedLayoutsEndBeforeImportAndSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  const grey_dmabuf_t black {0x00};
  if (!white.ok || !black.ok) {
    GTEST_SKIP() << "Could not allocate linear DMA-BUFs on " << render_node;
  }
  capture_completion_boundary_t completion;
  capture_admission_boundary_t admission;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  completion.install();
  admission.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  completion.publish(img);
  switch (GetParam()) {
    case admission_fault_e::unknown_fourcc: img.sd.fourcc = DRM_FORMAT_NV12; break;
    case admission_fault_e::unknown_modifier: img.sd.modifier = UINT64_MAX - 17; break;
    case admission_fault_e::extra_plane:
      img.sd.fds[1] = fcntl(img.sd.fds[0], F_DUPFD_CLOEXEC, 0);
      ASSERT_GE(img.sd.fds[1], 0);
      img.sd.pitches[1] = 256;
      break;
    case admission_fault_e::different_allocation: {
      egl::img_descriptor_t other;
      black.describe(other, 2);
      img.sd.fds[1] = std::exchange(other.sd.fds[0], -1);
      img.sd.pitches[1] = 1024;
      break;
    }
    case admission_fault_e::no_modifier: img.sd.modifier = DRM_FORMAT_MOD_INVALID; break;
    case admission_fault_e::short_pitch: img.sd.pitches[0] = 16; break;
    case admission_fault_e::zero_width: img.sd.width = 0; break;
    case admission_fault_e::no_import: admission.no_import = true; break;
    case admission_fault_e::failed_query: admission.refuse_query = true; break;
    case admission_fault_e::device_limit: admission.small_extent = true; break;
    case admission_fault_e::no_linear_sampling: admission.no_linear_sampling = true; break;
  }

  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(encoder.device->convert(img), platf::convert_capture_unreadable);
  EXPECT_EQ(admission.imports, 0);
  EXPECT_EQ(completion.submissions, 0);
  EXPECT_TRUE(completion.waits.empty());
  EXPECT_LE(admission.queries, 1);
  completion.completion_observed = true;  // No GPU read can own this lease.
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 1);
}

INSTANTIATE_TEST_SUITE_P(LayoutAndDevice, VulkanCaptureAdmissionBoundaryTests,
                        testing::Values(admission_fault_e::unknown_fourcc, admission_fault_e::unknown_modifier,
                                        admission_fault_e::extra_plane, admission_fault_e::different_allocation,
                                        admission_fault_e::no_modifier, admission_fault_e::short_pitch,
                                        admission_fault_e::zero_width, admission_fault_e::no_import,
                                        admission_fault_e::failed_query, admission_fault_e::device_limit,
                                        admission_fault_e::no_linear_sampling));

TEST(VulkanCaptureAdmissionTests, ExactDeviceQueryAndDedicatedImportPreserveRealPixels) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t completion;
  capture_admission_boundary_t admission;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  completion.install();
  admission.install();
  admission.dedicated_only = true;
  egl::img_descriptor_t img;
  white.describe(img, 1);
  completion.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  EXPECT_EQ(admission.queries, 1);
  EXPECT_TRUE(admission.exact_query);
  EXPECT_EQ(admission.imports, 1);
  EXPECT_TRUE(admission.dedicated_import);
  EXPECT_TRUE(completion.completion_observed);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

TEST(VulkanCaptureOwnershipTests, AcquireReadAndReleaseCompleteBeforeTheProducerLeaseReturns) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t completion;
  capture_queue_boundary_t queue;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  completion.install();
  queue.install();
  egl::img_descriptor_t img;
  white.describe(img, 1);
  completion.publish(img);
  ASSERT_EQ(encoder.device->convert(img), 0);
  EXPECT_EQ(queue.order, (std::vector<char> {'A', 'D', 'R'}));
  ASSERT_EQ(queue.transfers.size(), 2);
  const auto &acquire = queue.transfers[0];
  const auto &release = queue.transfers[1];
  EXPECT_EQ(acquire.srcQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(acquire.dstQueueFamilyIndex, release.srcQueueFamilyIndex);
  EXPECT_EQ(release.dstQueueFamilyIndex, VK_QUEUE_FAMILY_FOREIGN_EXT);
  EXPECT_EQ(acquire.oldLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(acquire.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(acquire.dstAccessMask, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_EQ(release.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(release.newLayout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(release.srcAccessMask, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_EQ(release.dstAccessMask, 0);
  EXPECT_EQ(acquire.image, release.image);
  EXPECT_TRUE(completion.completion_observed);
  EXPECT_TRUE(completion.lease_retained_at_wait);
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 1);
  EXPECT_FALSE(completion.returned_before_completion);
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225);
}

TEST(VulkanCaptureOwnershipTests, MissingEnabledForeignOwnershipEndsBeforeSubmission) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  capture_completion_boundary_t completion;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  completion.install();
  auto *device = reinterpret_cast<AVHWDeviceContext *>(encoder.hw_device->data);
  auto *context = static_cast<AVVulkanDeviceContext *>(device->hwctx);
  auto *original_extensions = context->enabled_dev_extensions;
  const int original_count = context->nb_enabled_dev_extensions;
  std::vector<const char *> without_foreign;
  for (int i = 0; i < original_count; ++i) {
    if (std::string_view {original_extensions[i]} != VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) {
      without_foreign.push_back(original_extensions[i]);
    }
  }
  egl::img_descriptor_t img;
  white.describe(img, 1);
  completion.publish(img);
  context->enabled_dev_extensions = without_foreign.data();
  context->nb_enabled_dev_extensions = static_cast<int>(without_foreign.size());
  const auto first = encoder.device->convert(img);
  const auto second = encoder.device->convert(img);
  // Restore the context's actual allocation before FFmpeg tears it down.
  context->enabled_dev_extensions = original_extensions;
  context->nb_enabled_dev_extensions = original_count;
  EXPECT_EQ(first, platf::convert_capture_unreadable);
  EXPECT_EQ(second, platf::convert_capture_unreadable);
  EXPECT_EQ(completion.submissions, 0);
  EXPECT_TRUE(completion.waits.empty());
  completion.completion_observed = true;
  img.capture_owner.reset();
  EXPECT_EQ(completion.returned, 1);
}

/**
 * The system memory upload refuses a live DMA-BUF frame and keeps the primer.
 *
 * It reads pixels from host memory and read a frame with none as black. A portal that negotiated
 * DMA-BUF hands over two such frames: the primer, which has no DMA-BUF either and is rightly black,
 * and every live frame after it, which has a DMA-BUF this route cannot read. Encoding those as black
 * streamed a black picture at full frame rate with nothing in any log (#635).
 */
TEST(VulkanRamUploadTests, OnlyALiveDmabufFrameIsRefused) {
  // The primer, exactly as the portal makes it once DMA-BUF is negotiated: alloc_img() leaves the
  // descriptors closed and the pixels unset, and dummy_img() sets the sequence to zero.
  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.sequence = 0;
  EXPECT_FALSE(vk::ram_upload_cannot_read(primer)) << "the primer is refused, so no session ever starts";

  const host_frame_t shared_memory {0x80};
  EXPECT_FALSE(vk::ram_upload_cannot_read(shared_memory)) << "shared memory is the frame this route exists for";

  // A live frame as portal capture fills one in: a DMA-BUF, no pixels in host memory.
  egl::img_descriptor_t live;
  live.width = frame_width;
  live.height = frame_height;
  live.sd.fds[0] = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  // The descriptor closes it.
  ASSERT_GE(live.sd.fds[0], 0);
  live.sequence = 1;
  EXPECT_TRUE(vk::ram_upload_cannot_read(live)) << "a DMA-BUF frame is read as black";
}

TEST(VulkanRamUploadTests, ThePrimerStillEncodesBlack) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.sequence = 0;
  ASSERT_EQ(encoder.device->convert(primer), 0) << "the frame every session is primed with was refused";

  const auto [centre, corner] = centre_and_corner_luma(encoder.frame);
  EXPECT_LE(centre, 26) << "the primer is not black";
  EXPECT_LE(corner, 26);
}

TEST(VulkanRamUploadTests, ALiveDmabufFrameEndsTheStreamInsteadOfEncodingBlack) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t captured {0x00};
  if (!captured.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  // A shared memory frame first, which this route reads, so a black frame after it can only be the
  // refused frame encoded anyway.
  host_frame_t white {0xFF};
  ASSERT_EQ(encoder.device->convert(white), 0);
  ASSERT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the shared memory frame did not reach the encoder";

  egl::img_descriptor_t live;
  captured.describe(live, 1);
  live.width = frame_width;
  live.height = frame_height;
  live.pixel_pitch = 4;
  live.row_pitch = frame_width * 4;
  const log_capture_t log;
  EXPECT_EQ(encoder.device->convert(live), platf::convert_capture_unreadable)
    << "the DMA-BUF frame was taken, so the stream goes on without a picture";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the refused frame was encoded as black";

  // A capture that hands this route a DMA-BUF hands it one for every frame, and the log says so once,
  // at error, naming the route and that Vulkan Video on the portal takes shared memory only.
  EXPECT_EQ(encoder.device->convert(live), platf::convert_capture_unreadable);
  const auto said = log.lines_with("DMA-BUF frame reached the system memory upload route");
  ASSERT_EQ(said.size(), 1u) << "the refusal was logged " << said.size() << " times for two frames";
  EXPECT_NE(said.front().find("Error: "), std::string::npos) << said.front();
  EXPECT_NE(said.front().find("Vulkan Video on the portal takes shared memory only"), std::string::npos) << said.front();
}


/**
 * The system memory upload refuses a packed 10-bit frame and reads an 8-bit one.
 *
 * The portal hands an HDR stream packed 10-bit frames in shared memory, at the same four bytes a
 * pixel as BGRA, and the upload copied them into an 8-bit BGRA image. What it encoded was noise, with
 * nothing in any log (#635).
 */
TEST(VulkanRamUploadTests, OnlyAFrameCaptureTaggedTenBitIsRefusedForItsDepth) {
  host_frame_t ten_bit {0x80};
  ten_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::p010};
  EXPECT_TRUE(vk::ram_upload_frame_is_ten_bit(ten_bit)) << "a 10-bit frame is read as 8-bit BGRA";

  host_frame_t eight_bit {0x80};
  eight_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8};
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(eight_bit)) << "shared memory BGRA is the frame this route exists for";

  const host_frame_t untagged {0x80};
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(untagged)) << "a backend that tags nothing hands over BGRA";

  // The primer carries no pixels to misread; a live frame with none is the DMA-BUF refusal's.
  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.frame_metadata.format = platf::frame_format_e::p010;
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(primer));
}

TEST(VulkanRamUploadTests, ATenBitFrameEndsTheStreamInsteadOfEncodingNoise) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  // An 8-bit frame first, which this route reads, so a black frame after it can only be the refused
  // frame encoded anyway.
  host_frame_t white {0xFF};
  white.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8};
  ASSERT_EQ(encoder.device->convert(white), 0);
  ASSERT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the shared memory frame did not reach the encoder";

  // Zero words, which read as 8-bit BGRA are black.
  host_frame_t ten_bit {0x00};
  ten_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::p010};
  const log_capture_t log;
  EXPECT_EQ(encoder.device->convert(ten_bit), platf::convert_capture_unreadable)
    << "the 10-bit frame was read as 8-bit BGRA";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the refused frame was encoded";

  // The portal fixes the format for the whole capture, and the log says so once, at error.
  EXPECT_EQ(encoder.device->convert(ten_bit), platf::convert_capture_unreadable);
  const auto said = log.lines_with("10-bit frame reached the system memory upload route");
  ASSERT_EQ(said.size(), 1u) << "the refusal was logged " << said.size() << " times for two frames";
  EXPECT_NE(said.front().find("Error: "), std::string::npos) << said.front();
  EXPECT_NE(said.front().find("Vulkan Video on the portal streams SDR only"), std::string::npos) << said.front();
}

TEST(VulkanDeviceLifecycleTests, RepeatedProbesAndSessionsKeepTheRequestedDeviceAvailable) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  for (int session = 1; session <= 12; ++session) {
    ASSERT_TRUE(vk::validate()) << "probe lost the encoder before session " << session;
    encoder_frame_t encoder;
    ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)))
      << "could not reopen the requested GPU for session " << session;
    egl::img_descriptor_t img;
    white.describe(img, 1);
    ASSERT_EQ(encoder.device->convert(img), 0) << "conversion failed in session " << session;
    ASSERT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "capture did not reach session " << session;
    // The image, converter, frame pool and hardware device close here. The next
    // iteration performs discovery again in the same long-lived host process.
  }
}

#ifdef POLARIS_BUILD_PORTAL
namespace {
  struct graph_dmabuf_pool_t {
    std::map<pw_buffer *, std::unique_ptr<grey_dmabuf_t>> storage;
    int width = frame_width, height = frame_height;

    bool allocate(pw_buffer *buffer) {
      auto made = std::make_unique<grey_dmabuf_t>(0, width, height);
      if (!made->ok || made->stride() != width * 4) return false;
      auto &data = buffer->buffer->datas[0];
      if (!(data.type & (1 << SPA_DATA_DmaBuf))) return false;
      data.type = SPA_DATA_DmaBuf;
      data.flags = SPA_DATA_FLAG_READWRITE;
      data.fd = made->export_fd();
      data.maxsize = width * height * 4;
      data.mapoffset = 0;
      data.data = nullptr;
      if (data.fd < 0) return false;
      storage.emplace(buffer, std::move(made));
      return true;
    }
    bool write(pw_buffer *buffer, std::uint8_t marker) { return storage.at(buffer)->overwrite(marker); }
    void remove(pw_buffer *buffer) {
      ::close(buffer->buffer->datas[0].fd);
      storage.erase(buffer);
    }
    test_utils::pipewire_producer_graph_t graph() {
      return {true, width, height,
        [this](pw_buffer *b) { return allocate(b); },
        [this](pw_buffer *b, std::uint8_t m) { return write(b, m); },
        [this](pw_buffer *b) { remove(b); }};
    }
  };

  std::shared_ptr<platf::img_t> publish_graph_frame(test_utils::pipewire_producer_graph_t &graph) {
    if (graph.capture->wait_for_frame(std::chrono::seconds(1)) != pipewire_capture::wait_result_e::frame) return {};
    std::shared_ptr<platf::img_t> image = std::make_shared<egl::img_descriptor_t>();
    return graph.capture->fill_frame(image) ? image : nullptr;
  }

  /**
   * One locked reading of the real graph into the retained test output, on
   * success as well as on failure: states and counts only, never pixels.
   */
  void print_graph_snapshot(const char *label, test_utils::pipewire_producer_graph_t &graph) {
    std::cout << "[ graph    ] " << label << ": " << graph.snapshot() << std::endl;
  }

  struct forwarding_graph_device_t: platf::avcodec_encode_device_t {
    forwarding_graph_device_t(platf::avcodec_encode_device_t &device, std::shared_ptr<platf::img_t> &clone): device(device), clone(clone) {}
    int convert(platf::img_t &image) override {
      clone = image.shared_from_this();
      return device.convert(image);
    }
    platf::avcodec_encode_device_t &device;
    std::shared_ptr<platf::img_t> &clone;
  };

  int convert_graph_frame(encoder_frame_t &encoder, std::shared_ptr<platf::img_t> image, std::shared_ptr<platf::img_t> &clone) {
    video::frame_t frame {std::move(image)};
    // This calls the actual video.cpp clone/publication adapter, followed by
    // the actual Vulkan converter. The wrapper owns neither actual device.
    return video::convert_with_encode_device_for_tests(std::make_unique<forwarding_graph_device_t>(*encoder.device, clone), frame);
  }
}

TEST(PipeWireProducerPoolTests, ProducerWithoutModifierNegotiatesUsableShmAndReusesPixels) {
  test_utils::pipewire_producer_graph_t graph {false, frame_width, frame_height};
  ASSERT_TRUE(graph.start()) << graph.snapshot();
  print_graph_snapshot("shm reuse: started", graph);
  EXPECT_FALSE(graph.capture->negotiated_dmabuf());
  auto image = std::make_shared<host_frame_t>(0);
  bool first_step = true;
  for (const std::uint8_t marker : {10, 30, 50, 70, 90, 110, 130, 150}) {
    if (first_step) print_graph_snapshot("shm reuse: before first step", graph);
    ASSERT_TRUE(graph.step(marker)) << "marker " << static_cast<int>(marker) << ' ' << graph.snapshot();
    ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame)
      << "marker " << static_cast<int>(marker) << ' ' << graph.snapshot();
    if (first_step) print_graph_snapshot("shm reuse: after first frame", graph);
    first_step = false;
    std::shared_ptr<platf::img_t> filled = image;
    ASSERT_TRUE(graph.capture->fill_frame(filled));
    EXPECT_TRUE(std::all_of(image->pixels.begin(), image->pixels.end(), [&](auto p) { return p == marker; }));
    EXPECT_EQ(image->frame_metadata.transport, platf::frame_transport_e::shm);
    EXPECT_EQ(image->frame_metadata.residency, platf::frame_residency_e::cpu);
    EXPECT_TRUE(pipewire_capture::frame_requires_cpu_copy(image->frame_metadata));
  }
  EXPECT_EQ(graph.writes.size(), 2);
  EXPECT_EQ(graph.submitted, 8);
  for (const auto &[buffer, times] : graph.writes) EXPECT_GE(times, 3);
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
  graph.close();
  EXPECT_EQ(graph.added, 2);
  EXPECT_EQ(graph.removed, 2);
}

TEST(PipeWireProducerPoolTests, HeldCaptureFramesExhaustTheRealPoolUntilOneReturns) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  graph_dmabuf_pool_t pool;
  auto graph = pool.graph();
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(graph.capture->negotiated_dmabuf());
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  ASSERT_TRUE(graph.step(0));
  auto first = publish_graph_frame(graph); ASSERT_TRUE(first);
  auto *first_descriptor = dynamic_cast<egl::img_descriptor_t *>(first.get()); ASSERT_NE(first_descriptor, nullptr);
  const auto first_key = first_descriptor->dmabuf_buffer_key;
  ASSERT_TRUE(graph.step(85));
  auto second = publish_graph_frame(graph); ASSERT_TRUE(second);
  ASSERT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 2);
  ASSERT_TRUE(graph.step(255));
  EXPECT_EQ(graph.submitted, 2) << "producer overwrote a published frame while its two-buffer pool was held";
  EXPECT_GE(graph.empty, 1);
  std::shared_ptr<platf::img_t> clone;
  ASSERT_EQ(convert_graph_frame(encoder, first, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {16,16,16,16}));
  clone.reset(); first.reset();
  ASSERT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 1);
  ASSERT_TRUE(graph.produce_after_return(255));
  auto replacement = publish_graph_frame(graph); ASSERT_TRUE(replacement);
  EXPECT_EQ(dynamic_cast<egl::img_descriptor_t *>(replacement.get())->dmabuf_buffer_key, first_key);
  ASSERT_EQ(convert_graph_frame(encoder, replacement, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {235,235,235,235}));
  clone.reset(); replacement.reset(); second.reset();
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
  graph.close();
  EXPECT_TRUE(pool.storage.empty());
  EXPECT_EQ(graph.removed, 2);
}

TEST(PipeWireProducerPoolTests, TimedOutGpuReadKeepsRealStorageUntilDeviceRetirement) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  graph_dmabuf_pool_t pool;
  auto graph = pool.graph(); ASSERT_TRUE(graph.start());
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install(); boundary.timeout = true;
  ASSERT_TRUE(graph.step(0));
  auto first = publish_graph_frame(graph); ASSERT_TRUE(first);
  const auto first_key = dynamic_cast<egl::img_descriptor_t *>(first.get())->dmabuf_buffer_key;
  boundary.lease = first;
  std::shared_ptr<platf::img_t> clone;
  EXPECT_EQ(convert_graph_frame(encoder, first, clone), platf::convert_capture_unreadable);
  first.reset(); clone.reset();
  ASSERT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 1);
  EXPECT_FALSE(boundary.lease.expired());
  ASSERT_TRUE(graph.step(85));
  auto second = publish_graph_frame(graph); ASSERT_TRUE(second);
  ASSERT_TRUE(graph.step(255));
  EXPECT_EQ(graph.submitted, 2);
  EXPECT_GE(graph.empty, 1);
  encoder.device.reset(); // real vkDeviceWaitIdle retires the submitted GPU read
  EXPECT_TRUE(boundary.idle_observed);
  EXPECT_TRUE(boundary.lease.expired());
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 1);
  ASSERT_TRUE(graph.produce_after_return(255));
  auto replacement = publish_graph_frame(graph); ASSERT_TRUE(replacement);
  EXPECT_EQ(dynamic_cast<egl::img_descriptor_t *>(replacement.get())->dmabuf_buffer_key, first_key);
  boundary.timeout = false;
  encoder_frame_t next;
  ASSERT_TRUE(next.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.lease = replacement;
  ASSERT_EQ(convert_graph_frame(next, replacement, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(next.frame, frame_width, frame_height, {235,235,235,235}));
  clone.reset(); replacement.reset(); second.reset(); next.device.reset();
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
  graph.close(); EXPECT_TRUE(pool.storage.empty());
}

TEST(PipeWireProducerPoolTests, RefusedLiveImportReleasesItsPoolAndNewShmCaptureWorks) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  {
    graph_dmabuf_pool_t pool;
    auto graph = pool.graph(); ASSERT_TRUE(graph.start());
    log_capture_t logs;
    capture_admission_boundary_t admission;
    encoder_frame_t encoder;
    ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
    admission.install(); admission.import_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    ASSERT_TRUE(graph.step(255));
    auto image = publish_graph_frame(graph); ASSERT_TRUE(image);
    std::shared_ptr<platf::img_t> clone;
    EXPECT_EQ(convert_graph_frame(encoder, image, clone), platf::convert_capture_unreadable);
    EXPECT_FALSE(logs.lines_with("Failed to import DMA-BUF").empty());
    EXPECT_EQ(admission.imports, 1);
    clone.reset(); image.reset();
    EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
    encoder.device.reset(); graph.close(); EXPECT_TRUE(pool.storage.empty());
  }
  // Explicit new compatible capture, not a claim of automatic mid-stream
  // fallback. Its real MemFd frame remains CPU-resident through RAM upload.
  test_utils::pipewire_producer_graph_t fallback {false, frame_width, frame_height};
  ASSERT_TRUE(fallback.start());
  ASSERT_FALSE(fallback.capture->negotiated_dmabuf());
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));
  auto image = std::make_shared<host_frame_t>(0);
  ASSERT_TRUE(fallback.step(255));
  ASSERT_EQ(fallback.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame);
  std::shared_ptr<platf::img_t> filled = image;
  ASSERT_TRUE(fallback.capture->fill_frame(filled));
  ASSERT_EQ(encoder.device->convert(*image), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {235,235,235,235}));
  EXPECT_EQ(image->frame_metadata.transport, platf::frame_transport_e::shm);
  EXPECT_EQ(image->frame_metadata.residency, platf::frame_residency_e::cpu);
}

TEST(PipeWireProducerPoolTests, NewGraphGenerationReadsNewPixelsWhileOldPublicationRemainsHeld) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  graph_dmabuf_pool_t old_pool, new_pool;
  auto old_graph = old_pool.graph(); ASSERT_TRUE(old_graph.start());
  auto new_graph = new_pool.graph(); ASSERT_TRUE(new_graph.start());
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  ASSERT_TRUE(old_graph.step(255));
  auto old_image = publish_graph_frame(old_graph); ASSERT_TRUE(old_image);
  std::shared_ptr<platf::img_t> clone;
  ASSERT_EQ(convert_graph_frame(encoder, old_image, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {235,235,235,235}));
  clone.reset();
  ASSERT_TRUE(new_graph.step(0));
  auto new_image = publish_graph_frame(new_graph); ASSERT_TRUE(new_image);
  auto *old_descriptor = dynamic_cast<egl::img_descriptor_t *>(old_image.get());
  auto *new_descriptor = dynamic_cast<egl::img_descriptor_t *>(new_image.get());
  ASSERT_NE(old_descriptor, nullptr); ASSERT_NE(new_descriptor, nullptr);
  EXPECT_EQ(old_descriptor->sequence, new_descriptor->sequence);
  EXPECT_NE(old_descriptor->dmabuf_buffer_key, new_descriptor->dmabuf_buffer_key);
  ASSERT_EQ(convert_graph_frame(encoder, new_image, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {16,16,16,16}));
  clone.reset(); old_image.reset();
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*old_graph.capture), 0);
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*new_graph.capture), 1);
  new_image.reset(); encoder.device.reset();
  old_graph.close(); new_graph.close();
  EXPECT_TRUE(old_pool.storage.empty()); EXPECT_TRUE(new_pool.storage.empty());
}

TEST(PipeWireProducerPoolTests, RealShmSizeChangeRequestsReinitAndNewCaptureReadsNewPixels) {
  test_utils::pipewire_producer_graph_t graph {false, frame_width, frame_height};
  ASSERT_TRUE(graph.start()) << graph.snapshot();
  print_graph_snapshot("shm size change: started", graph);
  auto image = std::make_shared<host_frame_t>(17);
  print_graph_snapshot("shm size change: before first step", graph);
  ASSERT_TRUE(graph.step(255)) << graph.snapshot();
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame) << graph.snapshot();
  print_graph_snapshot("shm size change: after first frame", graph);
  std::shared_ptr<platf::img_t> filled = image;
  ASSERT_TRUE(graph.capture->fill_frame(filled));
  ASSERT_TRUE(std::all_of(image->pixels.begin(), image->pixels.end(), [](auto p) { return p == 255; }));
  // Leave another old-size frame at the capture front when the real producer
  // updates EnumFormat; no capture callback is called directly by the test.
  ASSERT_TRUE(graph.step(85)) << graph.snapshot();
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame) << graph.snapshot();
  ASSERT_TRUE(graph.renegotiate_size(128, 72));
  ASSERT_TRUE(graph.wait([&] { return !graph.capture->running(); }));
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(2)), pipewire_capture::wait_result_e::reinit);
  EXPECT_EQ(graph.capture->frame_info().width, 128);
  EXPECT_EQ(graph.capture->frame_info().height, 72);
  EXPECT_FALSE(graph.capture->fill_frame(filled));
  EXPECT_TRUE(std::all_of(image->pixels.begin(), image->pixels.end(), [](auto p) { return p == 255; }));
  graph.close();
  EXPECT_EQ(graph.added, graph.removed);
  EXPECT_EQ(graph.destroyed, 1);

  test_utils::pipewire_producer_graph_t next {false, 128, 72};
  ASSERT_TRUE(next.start()) << next.snapshot();
  print_graph_snapshot("shm size change: next started", next);
  auto replacement = std::make_shared<host_frame_t>(255, 128, 72);
  print_graph_snapshot("shm size change: next before first step", next);
  ASSERT_TRUE(next.step(0)) << next.snapshot();
  ASSERT_EQ(next.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame) << next.snapshot();
  print_graph_snapshot("shm size change: next after first frame", next);
  filled = replacement;
  ASSERT_TRUE(next.capture->fill_frame(filled));
  EXPECT_TRUE(std::all_of(replacement->pixels.begin(), replacement->pixels.end(), [](auto p) { return p == 0; }));
  EXPECT_EQ(replacement->frame_metadata.transport, platf::frame_transport_e::shm);
  next.close();
  EXPECT_EQ(next.added, next.removed);
}

TEST(PipeWireProducerPoolTests, RealDmaBufSizeChangeRetainsOldPixelsUntilLateConsumerReturns) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  graph_dmabuf_pool_t pool;
  auto graph = pool.graph(); ASSERT_TRUE(graph.start());
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  ASSERT_TRUE(graph.step(255));
  auto image = publish_graph_frame(graph); ASSERT_TRUE(image);
  ASSERT_TRUE(graph.step(85)); // unconsumed front still owns the other buffer
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::frame);
  pool.width = 128; pool.height = 72;
  ASSERT_TRUE(graph.renegotiate_size(128, 72));
  ASSERT_TRUE(graph.wait([&] { return !graph.capture->running(); }));
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(2)), pipewire_capture::wait_result_e::reinit);
  EXPECT_EQ(graph.capture->frame_info().width, 128);
  EXPECT_EQ(graph.capture->frame_info().height, 72);
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 1);
  std::shared_ptr<platf::img_t> clone;
  // The old published descriptor and exported storage remain readable after
  // actual producer renegotiation/removal. No new-size frame is admitted by
  // this terminal capture generation.
  ASSERT_EQ(convert_graph_frame(encoder, image, clone), 0);
  EXPECT_TRUE(has_grey_quadrants(encoder.frame, frame_width, frame_height, {235,235,235,235}));
  EXPECT_FALSE(graph.capture->fill_frame(clone));
  clone.reset(); image.reset(); encoder.device.reset();
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
  graph.close();
  EXPECT_EQ(graph.added, graph.removed);
  EXPECT_TRUE(pool.storage.empty());
}

TEST(PipeWireProducerPoolTests, LiveGraphShutdownWaitsForTimedOutGpuConsumerRetirement) {
  if (!vk::validate()) GTEST_SKIP() << "No Vulkan Video encoder on this host";
  graph_dmabuf_pool_t pool;
  auto graph = pool.graph(); ASSERT_TRUE(graph.start());
  capture_completion_boundary_t boundary;
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));
  boundary.install(); boundary.timeout = true;
  ASSERT_TRUE(graph.step(255));
  auto image = publish_graph_frame(graph); ASSERT_TRUE(image);
  boundary.lease = image;
  std::shared_ptr<platf::img_t> clone;
  ASSERT_EQ(convert_graph_frame(encoder, image, clone), platf::convert_capture_unreadable);
  clone.reset(); image.reset();
  ASSERT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 1);
  std::atomic_bool stopped {false};
  std::thread closer([&] { graph.shutdown_capture(); stopped.store(true); });
  auto join = util::fail_guard([&] { encoder.device.reset(); closer.join(); });
  ASSERT_EQ(graph.capture->wait_for_frame(std::chrono::seconds(2)), pipewire_capture::wait_result_e::reinit);
  EXPECT_FALSE(stopped.load()) << "capture disconnected while the GPU still retained producer storage";
  ASSERT_FALSE(stopped.load());
  EXPECT_FALSE(boundary.lease.expired());
  ASSERT_TRUE(graph.wait([&] { return graph.removed == 0 && pool.storage.size() == 2; }));
  // A real device idle completes the injected wait failure. Shutdown then
  // drains through actual capture/core/producer callbacks, with no mock queue.
  encoder.device.reset();
  closer.join(); join.disable();
  EXPECT_TRUE(boundary.idle_observed);
  EXPECT_TRUE(boundary.lease.expired());
  EXPECT_TRUE(stopped.load());
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::leases(*graph.capture), 0);
  EXPECT_EQ(pipewire_capture::capture_graph_test_access::loop(*graph.capture), nullptr);
  EXPECT_EQ(graph.source, nullptr);
  EXPECT_EQ(graph.destroyed, 1);
  EXPECT_EQ(graph.added, graph.removed);
  EXPECT_TRUE(pool.storage.empty());
  graph.capture->shutdown(); // idempotent with aliases still alive
  graph.close();
}
#endif

#endif
