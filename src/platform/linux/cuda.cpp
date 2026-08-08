/**
 * @file src/platform/linux/cuda.cpp
 * @brief Definitions for CUDA encoding.
 */
// standard includes
#include <array>
#include <bitset>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <iomanip>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

// lib includes
#include <drm_fourcc.h>
#include <ffnvcodec/dynlink_loader.h>
#include <NvFBC.h>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
}

// local includes
#include "cuda.h"
#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/stream_stats.h"
#include "src/utility.h"
#include "src/video.h"
#include "wayland.h"

#define POLARIS_STRINGVIEW_HELPER(x) x##sv
#define POLARIS_STRINGVIEW(x) POLARIS_STRINGVIEW_HELPER(x)

#define CU_CHECK(x, y) \
  if (check((x), POLARIS_STRINGVIEW(y ": "))) \
  return -1

#define CU_CHECK_IGNORE(x, y) \
  check((x), POLARIS_STRINGVIEW(y ": "))

namespace fs = std::filesystem;

using namespace std::literals;

namespace cuda {
  constexpr auto cudaDevAttrMaxThreadsPerBlock = (CUdevice_attribute) 1;
  constexpr auto cudaDevAttrMaxThreadsPerMultiProcessor = (CUdevice_attribute) 39;
  constexpr unsigned cudaExternalMemoryDedicated = 1;

  void pass_error(const std::string_view &sv, const char *name, const char *description) {
    BOOST_LOG(error) << sv << name << ':' << description;
  }

  void cff(CudaFunctions *cf) {
    cuda_free_functions(&cf);
  }

  using cdf_t = util::safe_ptr<CudaFunctions, cff>;

  static cdf_t cdf;

  inline static int check(CUresult result, const std::string_view &sv) {
    if (result != CUDA_SUCCESS) {
      const char *name;
      const char *description;

      cdf->cuGetErrorName(result, &name);
      cdf->cuGetErrorString(result, &description);

      BOOST_LOG(error) << sv << name << ':' << description;
      return -1;
    }

    return 0;
  }

  void freeStream(CUstream stream) {
    CU_CHECK_IGNORE(cdf->cuStreamDestroy(stream), "Couldn't destroy cuda stream");
  }

  void unregisterResource(CUgraphicsResource resource) {
    CU_CHECK_IGNORE(cdf->cuGraphicsUnregisterResource(resource), "Couldn't unregister resource");
  }

  using registered_resource_t = util::safe_ptr<CUgraphicsResource_st, unregisterResource>;

  class img_t: public platf::img_t {
  public:
    tex_t tex;
  };

  int init() {
    auto status = cuda_load_functions(&cdf, nullptr);
    if (status) {
      BOOST_LOG(error) << "Couldn't load cuda: "sv << status;

      return -1;
    }

    CU_CHECK(cdf->cuInit(0), "Couldn't initialize cuda");

    return 0;
  }

  class cuda_t: public platf::avcodec_encode_device_t {
  public:
    int init(int in_width, int in_height) {
      if (!cdf) {
        BOOST_LOG(warning) << "cuda not initialized"sv;
        return -1;
      }

      data = (void *) 0x1;
      hardware_device_index = 0;

      width = in_width;
      height = in_height;

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      auto hwframe_ctx = (AVHWFramesContext *) hw_frames_ctx->data;
      if (hwframe_ctx->sw_format != AV_PIX_FMT_NV12) {
        BOOST_LOG(error) << "cuda::cuda_t doesn't support any format other than AV_PIX_FMT_NV12"sv;
        return -1;
      }

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for NVENC"sv;
          return -1;
        }
      }

      auto cuda_ctx = (AVCUDADeviceContext *) hwframe_ctx->device_ctx->hwctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      auto sws_opt = sws_t::make(width, height, frame->width, frame->height, width * 4);
      if (!sws_opt) {
        return -1;
      }

      sws = std::move(*sws_opt);

      linear_interpolation = width != frame->width || height != frame->height;

      return 0;
    }

    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);

      auto tex = tex_t::make(height, width * 4);
      if (!tex) {
        return;
      }

      // The default green color is ugly.
      // Update the background color
      platf::img_t img;
      img.width = width;
      img.height = height;
      img.pixel_pitch = 4;
      img.row_pitch = img.width * img.pixel_pitch;

      std::vector<std::uint8_t> image_data;
      image_data.resize(img.row_pitch * img.height);

      img.data = image_data.data();

      if (sws.load_ram(img, tex->array)) {
        return;
      }

      sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex->texture.linear, stream.get(), {frame->width, frame->height, 0, 0});
    }

    cudaTextureObject_t tex_obj(const tex_t &tex) const {
      return linear_interpolation ? tex.texture.linear : tex.texture.point;
    }

    stream_t stream;
    frame_t hwframe;

    int width, height;

    // When height and width don't change, it's not necessary to use linear interpolation
    bool linear_interpolation;

    sws_t sws;
  };

  class cuda_ram_t: public cuda_t {
  public:
    int convert(platf::img_t &img) override {
      return sws.load_ram(img, tex.array) || sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(tex), stream.get());
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      if (cuda_t::set_frame(frame, hw_frames_ctx)) {
        return -1;
      }

      auto tex_opt = tex_t::make(height, width * 4);
      if (!tex_opt) {
        return -1;
      }

      tex = std::move(*tex_opt);

      return 0;
    }

    tex_t tex;
  };

  class cuda_vram_t: public cuda_t {
  public:
    int convert(platf::img_t &img) override {
      return sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(((img_t *) &img)->tex), stream.get());
    }
  };

  /**
   * @brief Opens the DRM device associated with the CUDA device index.
   * @param index CUDA device index to open.
   * @return File descriptor or -1 on failure.
   */
  file_t open_drm_fd_for_cuda_device(int index) {
    CUdevice device;
    CU_CHECK(cdf->cuDeviceGet(&device, index), "Couldn't get CUDA device");

    // There's no way to directly go from CUDA to a DRM device, so we'll
    // use sysfs to look up the DRM device name from the PCI ID.
    std::array<char, 13> pci_bus_id;
    CU_CHECK(cdf->cuDeviceGetPCIBusId(pci_bus_id.data(), pci_bus_id.size(), device), "Couldn't get CUDA device PCI bus ID");
    BOOST_LOG(debug) << "Found CUDA device with PCI bus ID: "sv << pci_bus_id.data();

    // Linux uses lowercase hexadecimal while CUDA uses uppercase
    std::transform(pci_bus_id.begin(), pci_bus_id.end(), pci_bus_id.begin(), [](char c) {
      return std::tolower(c);
    });

    // Look for the name of the primary node in sysfs
    try {
      char sysfs_path[PATH_MAX];
      std::snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s/drm", pci_bus_id.data());
      fs::path sysfs_dir {sysfs_path};
      for (auto &entry : fs::directory_iterator {sysfs_dir}) {
        auto file = entry.path().filename();
        auto filestring = file.generic_string();
        if (std::string_view {filestring}.substr(0, 4) != "card"sv) {
          continue;
        }

        BOOST_LOG(debug) << "Found DRM primary node: "sv << filestring;

        fs::path dri_path {"/dev/dri"sv};
        auto device_path = dri_path / file;
        return open(device_path.c_str(), O_RDWR);
      }
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(error) << "Failed to read sysfs: "sv << err.what();
    }

    BOOST_LOG(error) << "Unable to find DRM device with PCI bus ID: "sv << pci_bus_id.data();
    return -1;
  }

  std::optional<int> cuda_device_index_for_render_node(const fs::path &render_node) {
    if (render_node.parent_path() != "/dev/dri" ||
        !render_node.filename().string().starts_with("renderD")) {
      return std::nullopt;
    }

    std::error_code ec;
    const auto render_pci_device = fs::canonical(
      fs::path {"/sys/class/drm"} / render_node.filename() / "device", ec);
    if (ec) {
      BOOST_LOG(error) << "Unable to resolve DRM render node ["sv << render_node.string()
                       << "] to a PCI device: "sv << ec.message();
      return std::nullopt;
    }

    int device_count = 0;
    if (cdf->cuDeviceGetCount(&device_count) != CUDA_SUCCESS) {
      return std::nullopt;
    }
    for (int i = 0; i < device_count; ++i) {
      CUdevice device;
      if (cdf->cuDeviceGet(&device, i) != CUDA_SUCCESS) {
        continue;
      }

      std::array<char, 13> pci_bus_id {};
      if (cdf->cuDeviceGetPCIBusId(pci_bus_id.data(), pci_bus_id.size(), device) != CUDA_SUCCESS) {
        continue;
      }
      std::transform(pci_bus_id.begin(), pci_bus_id.end(), pci_bus_id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });

      ec.clear();
      const auto cuda_pci_device = fs::canonical(
        fs::path {"/sys/bus/pci/devices"} / pci_bus_id.data(), ec);
      if (!ec && cuda_pci_device == render_pci_device) {
        return i;
      }
    }

    return std::nullopt;
  }

  class gl_cuda_vram_t: public platf::avcodec_encode_device_t {
  public:
    struct rgb_cache_entry_t {
      std::uint64_t buffer_key {0};
      egl::rgb_t rgb;
    };

    /**
     * @brief Initialize the GL->CUDA encoding device.
     * @param in_width Width of captured frames.
     * @param in_height Height of captured frames.
     * @param offset_x Offset of content in captured frame.
     * @param offset_y Offset of content in captured frame.
     * @return 0 on success or -1 on failure.
     */
    int init(int in_width, int in_height, int offset_x, int offset_y) {
      // Select the CUDA device by exact render-node PCI identity when a DRM
      // adapter path is configured. Never silently fall back to CUDA device 0
      // for an explicit render node: that would violate same-GPU DMA-BUF use.
      int cuda_device_index = 0;
      if (!::config::video.adapter_name.empty()) {
        if (::config::video.adapter_name.starts_with("/dev/dri/renderD")) {
          const auto selected = cuda_device_index_for_render_node(::config::video.adapter_name);
          if (!selected) {
            BOOST_LOG(error) << "Configured render node ["sv << ::config::video.adapter_name
                             << "] does not map to a CUDA device"sv;
            return -1;
          }
          cuda_device_index = *selected;
          BOOST_LOG(info) << "Selected CUDA device "sv << cuda_device_index
                          << " from render node ["sv << ::config::video.adapter_name << ']';
        } else {
          bool found = false;
          int device_count = 0;
          cdf->cuDeviceGetCount(&device_count);
          for (int i = 0; i < device_count; i++) {
            CUdevice dev;
            if (cdf->cuDeviceGet(&dev, i) == CUDA_SUCCESS) {
              char name[256];
              if (cdf->cuDeviceGetName(name, sizeof(name), dev) == CUDA_SUCCESS &&
                  ::config::video.adapter_name == name) {
                cuda_device_index = i;
                found = true;
                BOOST_LOG(info) << "Selected CUDA device " << i << ": " << name;
                break;
              }
            }
          }
          if (!found) {
            BOOST_LOG(error) << "Configured CUDA adapter ["sv << ::config::video.adapter_name
                             << "] was not found"sv;
            return -1;
          }
        }
      }

      // Keep the historical non-null hardware marker and pass the selected
      // ordinal explicitly to FFmpeg CUDA context creation.
      data = (void *) 0x1;
      hardware_device_index = cuda_device_index;
      file = std::move(open_drm_fd_for_cuda_device(cuda_device_index));
      if (file.el < 0) {
        char string[1024];
        BOOST_LOG(error) << "Couldn't open DRM FD for CUDA device: "sv << strerror_r(errno, string, sizeof(string));
        return -1;
      }

      gbm.reset(gbm::create_device(file.el));
      if (!gbm) {
        BOOST_LOG(error) << "Couldn't create GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
        return -1;
      }

      display = egl::make_display(gbm.get());
      if (!display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      width = in_width;
      height = in_height;

      sequence = 0;

      this->offset_x = offset_x;
      this->offset_y = offset_y;

      return 0;
    }

    /**
     * @brief Initialize color conversion into target CUDA frame.
     * @param frame Destination CUDA frame to write into.
     * @param hw_frames_ctx_buf FFmpeg hardware frame context.
     * @return 0 on success or -1 on failure.
     */
    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for VAAPI"sv;
          return -1;
        }
      }

      auto hw_frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      sw_format = hw_frames_ctx->sw_format;

      auto nv12_opt = egl::create_target(frame->width, frame->height, sw_format);
      if (!nv12_opt) {
        return -1;
      }

      auto sws_opt = egl::sws_t::make(width, height, frame->width, frame->height, sw_format);
      if (!sws_opt) {
        return -1;
      }

      this->sws = std::move(*sws_opt);
      this->nv12 = std::move(*nv12_opt);

      auto cuda_ctx = (AVCUDADeviceContext *) hw_frames_ctx->device_ctx->hwctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      CU_CHECK(cdf->cuGraphicsGLRegisterImage(&y_res, nv12->tex[0], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "Couldn't register Y plane texture");
      CU_CHECK(cdf->cuGraphicsGLRegisterImage(&uv_res, nv12->tex[1], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "Couldn't register UV plane texture");

      return 0;
    }

    /**
     * @brief Convert the captured image into the target CUDA frame.
     * @param img Captured screen image.
     * @return 0 on success or -1 on failure.
     */
    int convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;
      egl::rgb_t *active_rgb = nullptr;

      if (descriptor.sequence == 0) {
        // For dummy images, use a blank RGB texture instead of importing a DMA-BUF
        blank_rgb = egl::create_blank(img);
        active_rgb = &blank_rgb;
        active_buffer_key = 0;
      } else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;

        if (descriptor.dmabuf_buffer_key != 0) {
          for (auto &entry : rgb_cache) {
            if (entry.buffer_key == descriptor.dmabuf_buffer_key && entry.rgb.el.xrgb8 != EGL_NO_IMAGE) {
              active_rgb = &entry.rgb;
              break;
            }
          }
        }

        if (!active_rgb) {
          auto rgb_opt = egl::import_source(display.get(), descriptor.sd);
          if (!rgb_opt) {
            return -1;
          }

          auto &entry = rgb_cache[rgb_cache_slot];
          entry.buffer_key = descriptor.dmabuf_buffer_key;
          entry.rgb = std::move(*rgb_opt);
          active_rgb = &entry.rgb;
          rgb_cache_slot = (rgb_cache_slot + 1) % rgb_cache.size();
        }

        active_buffer_key = descriptor.dmabuf_buffer_key;
        descriptor.reset();
      } else {
        if (active_buffer_key == 0) {
          active_rgb = &blank_rgb;
        } else {
          for (auto &entry : rgb_cache) {
            if (entry.buffer_key == active_buffer_key && entry.rgb.el.xrgb8 != EGL_NO_IMAGE) {
              active_rgb = &entry.rgb;
              break;
            }
          }
        }
      }

      auto active_texture = active_rgb ? (*active_rgb)->tex[0] : 0;
      if (!active_rgb || active_texture == 0) {
        BOOST_LOG(error)
          << "CUDA DMABUF converter has no active RGB surface: descriptor_sequence="sv << descriptor.sequence
          << " converter_sequence="sv << sequence
          << " descriptor_buffer_key="sv << descriptor.dmabuf_buffer_key
          << " active_buffer_key="sv << active_buffer_key
          << " active_texture="sv << active_texture;
        return -1;
      }

      // Perform the color conversion and scaling in GL
      sws.load_vram(descriptor, offset_x, offset_y, (*active_rgb)->tex[0]);
      sws.convert(nv12->buf);

      auto fmt_desc = av_pix_fmt_desc_get(sw_format);

      // Map the GL textures to read for CUDA
      CUgraphicsResource resources[2] = {y_res.get(), uv_res.get()};
      CU_CHECK(cdf->cuGraphicsMapResources(2, resources, stream.get()), "Couldn't map GL textures in CUDA");

      // Copy from the GL textures to the target CUDA frame
      for (int i = 0; i < 2; i++) {
        CUDA_MEMCPY2D cpy = {};
        cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        CU_CHECK(cdf->cuGraphicsSubResourceGetMappedArray(&cpy.srcArray, resources[i], 0, 0), "Couldn't get mapped plane array");

        cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.dstDevice = (CUdeviceptr) frame->data[i];
        cpy.dstPitch = frame->linesize[i];
        cpy.WidthInBytes = (frame->width * fmt_desc->comp[i].step) >> (i ? fmt_desc->log2_chroma_w : 0);
        cpy.Height = frame->height >> (i ? fmt_desc->log2_chroma_h : 0);

        CU_CHECK_IGNORE(cdf->cuMemcpy2DAsync(&cpy, stream.get()), "Couldn't copy texture to CUDA frame");
      }

      // Unmap the textures to allow modification from GL again
      CU_CHECK(cdf->cuGraphicsUnmapResources(2, resources, stream.get()), "Couldn't unmap GL textures from CUDA");
      return 0;
    }

    /**
     * @brief Configures shader parameters for the specified colorspace.
     */
    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);
    }

    file_t file;
    gbm::gbm_t gbm;
    egl::display_t display;
    egl::ctx_t ctx;

    // This must be destroyed before display_t
    stream_t stream;
    frame_t hwframe;

    egl::sws_t sws;
    egl::nv12_t nv12;
    AVPixelFormat sw_format;

    int width, height;

    std::uint64_t sequence;
    egl::rgb_t blank_rgb;
    std::array<rgb_cache_entry_t, 2> rgb_cache {};
    std::size_t rgb_cache_slot {0};
    std::uint64_t active_buffer_key {0};

    registered_resource_t y_res;
    registered_resource_t uv_res;

    int offset_x, offset_y;
  };

  class cuda_dmabuf_t: public cuda_t {
  public:
    struct source_import_t {
      std::uint64_t buffer_key {0};
      VkBuffer buffer {VK_NULL_HANDLE};
      VkDeviceMemory memory {VK_NULL_HANDLE};
      VkDeviceSize allocation_size {0};
      VkDeviceSize copy_offset {0};
      VkDeviceSize copy_size {0};
      std::uint32_t pitch {0};
    };

    ~cuda_dmabuf_t() override {
      if (!cu_ctx) {
        release_vulkan();
        return;
      }
      CUcontext popped = nullptr;
      if (cdf->cuCtxPushCurrent(cu_ctx) != CUDA_SUCCESS) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: couldn't activate CUDA context for ordered cleanup; retaining shared resources"sv;
        return;
      }
      release_cuda_destination();
      staging_tex.reset();
      sws.color_matrix.reset();
      stream.reset();
      release_vulkan();
      cdf->cuCtxPopCurrent(&popped);
    }

    int init(int in_width, int in_height) {
      if (!cdf) {
        return -1;
      }

      int cuda_device_index = 0;
      if (!::config::video.adapter_name.empty()) {
        if (::config::video.adapter_name.starts_with("/dev/dri/renderD")) {
          const auto selected = cuda_device_index_for_render_node(::config::video.adapter_name);
          if (!selected) {
            BOOST_LOG(error) << "Configured render node ["sv << ::config::video.adapter_name
                             << "] does not map to a CUDA device"sv;
            return -1;
          }
          cuda_device_index = *selected;
        } else {
          bool found = false;
          int device_count = 0;
          cdf->cuDeviceGetCount(&device_count);
          for (int i = 0; i < device_count; ++i) {
            CUdevice dev;
            char name[256];
            if (cdf->cuDeviceGet(&dev, i) == CUDA_SUCCESS &&
                cdf->cuDeviceGetName(name, sizeof(name), dev) == CUDA_SUCCESS &&
                ::config::video.adapter_name == name) {
              cuda_device_index = i;
              found = true;
              break;
            }
          }
          if (!found) {
            BOOST_LOG(error) << "Configured CUDA adapter ["sv << ::config::video.adapter_name
                             << "] was not found"sv;
            return -1;
          }
        }
      }

      data = (void *) 0x1;
      hardware_device_index = cuda_device_index;
      width = in_width;
      height = in_height;
      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) override {
      // Accept NV12 (SDR) and P010 (HDR 10-bit). Base cuda_t only allows NV12.
      // CUDA hw frames use frame->format=AV_PIX_FMT_CUDA; real layout is sw_format.
      this->hwframe.reset(frame);
      this->frame = frame;

      auto *hwframe_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      if (hwframe_ctx->sw_format != AV_PIX_FMT_NV12 && hwframe_ctx->sw_format != AV_PIX_FMT_P010) {
        BOOST_LOG(error) << "CUDA DMABUF: unsupported sw_format (need NV12 or P010), got "sv
                         << hwframe_ctx->sw_format;
        return -1;
      }
      encode_sw_format = hwframe_ctx->sw_format;

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, frame, 0)) {
          BOOST_LOG(error) << "CUDA DMABUF: Couldn't get hwframe for NVENC"sv;
          return -1;
        }
      }

      auto *device_ctx = (AVCUDADeviceContext *) hwframe_ctx->device_ctx->hwctx;
      cu_ctx = device_ctx->cuda_ctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }
      device_ctx->stream = stream.get();

      auto sws_opt = sws_t::make(width, height, frame->width, frame->height, width * 4);
      if (!sws_opt) {
        return -1;
      }
      sws = std::move(*sws_opt);
      linear_interpolation = width != frame->width || height != frame->height;

      staging_tex = tex_t::make(height, width * 4);
      if (!staging_tex) {
        return -1;
      }

      CUcontext popped = nullptr;
      if (!cu_ctx || cdf->cuCtxPushCurrent(cu_ctx) != CUDA_SUCCESS) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: selected CUDA context unavailable"sv;
        return -1;
      }
      if (!init_vulkan()) {
        activate_fallback("Vulkan initialization or capability validation failed"sv);
      }
      cdf->cuCtxPopCurrent(&popped);
      return 0;
    }

    int convert(platf::img_t &img) override {
      CUcontext popped = nullptr;
      if (!cu_ctx || cdf->cuCtxPushCurrent(cu_ctx) != CUDA_SUCCESS) {
        return -1;
      }
      const int result = convert_inner(static_cast<egl::img_descriptor_t &>(img));
      cdf->cuCtxPopCurrent(&popped);
      return result;
    }

  private:
    struct layout_t {
      VkDeviceSize required_size;
      VkDeviceSize offset;
      VkDeviceSize copy_size;
      std::uint32_t pitch;
    };

    bool vk_call(VkResult result, std::string_view action) {
      if (result == VK_SUCCESS) {
        return true;
      }
      BOOST_LOG(error) << "CUDA DMABUF Vulkan: "sv << action << " failed VkResult="sv << result;
      return false;
    }

    void activate_fallback(std::string_view reason) {
      if (!force_mmap) {
        release_cuda_destination();
        release_vulkan();
      }
      force_mmap = true;
      active_frame = false;
      if (!fallback_logged) {
        BOOST_LOG(error)
          << "CUDA DMABUF: Vulkan bridge failed — STICKY FALLBACK convert_path=mmap_cuda reason="sv
          << reason << ". DmaBuf capture remains active, but encode conversion crosses CPU memory; not gpu_native."sv;
        fallback_logged = true;
      }
      stream_stats::update_encode_path_metadata(
        "mmap_cuda",
        platf::frame_residency_e::cpu,
        platf::frame_format_e::bgra8
      );
    }

    std::optional<layout_t> layout(const egl::surface_descriptor_t &sd) const {
      // 8-bit BGRx/BGRA and 10-bit XBGR2101010 are all single-plane 32bpp LINEAR.
      if (sd.fds[0] < 0 || sd.fds[1] >= 0 || sd.fds[2] >= 0 || sd.fds[3] >= 0 ||
          (sd.fourcc != DRM_FORMAT_XRGB8888 && sd.fourcc != DRM_FORMAT_ARGB8888 &&
           sd.fourcc != DRM_FORMAT_XBGR2101010) ||
          sd.width <= 0 || sd.height <= 0 || sd.width != width || sd.height != height ||
          sd.modifier != DRM_FORMAT_MOD_LINEAR || (sd.offsets[0] & 3) || (sd.pitches[0] & 3)) {
        return std::nullopt;
      }
      const auto min_pitch = static_cast<VkDeviceSize>(sd.width) * 4;
      const auto pitch = static_cast<VkDeviceSize>(sd.pitches[0]);
      const auto frame_height = static_cast<VkDeviceSize>(sd.height);
      if (pitch < min_pitch || pitch > std::numeric_limits<VkDeviceSize>::max() / frame_height) {
        return std::nullopt;
      }
      const auto copy_size = pitch * frame_height;
      const auto offset = static_cast<VkDeviceSize>(sd.offsets[0]);
      if (offset > std::numeric_limits<VkDeviceSize>::max() - copy_size) {
        return std::nullopt;
      }
      return layout_t {offset + copy_size, offset, copy_size, sd.pitches[0]};
    }

    std::optional<VkDeviceSize> dmabuf_allocation_size(int fd, VkDeviceSize required_size, std::string_view action) const {
      errno = 0;
      const auto end = lseek(fd, 0, SEEK_END);
      if (end < 0) {
        BOOST_LOG(error) << "CUDA DMABUF: "sv << action << " couldn't query allocation size with lseek: fd="sv
                         << fd << " error="sv << strerror(errno);
        return std::nullopt;
      }
      const auto allocation_size = static_cast<VkDeviceSize>(end);
      if (allocation_size < required_size) {
        BOOST_LOG(error) << "CUDA DMABUF: "sv << action << " allocation too small: allocation_size="sv
                         << allocation_size << " required_offset_pitch_height="sv << required_size;
        return std::nullopt;
      }
      return allocation_size;
    }

    int convert_inner(egl::img_descriptor_t &descriptor) {
      if (descriptor.sequence == 0) {
        return convert_blank();
      }
      if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;
        int result = force_mmap ? convert_mmap(descriptor) : vulkan_and_convert(descriptor);
        if (result < 0 && !force_mmap) {
          activate_fallback("frame import/copy failed"sv);
          result = convert_mmap(descriptor);
        }
        descriptor.reset();
        active_frame = result == 0;
        return result;
      }
      if (!active_frame) {
        return convert_blank();
      }
      const auto texture = force_mmap ? tex_obj(*staging_tex) : destination_texture;
      const bool src_xb30 = source_fourcc == DRM_FORMAT_XBGR2101010;
      // SPA BGRx → DRM XRGB8888 LE is B,G,R,X (classic bgra_to_rgb).
      // SPA RGBx → DRM XBGR8888 LE is R,G,B,X (no swizzle). Never treat XRGB as RGB8 —
      // that R/B-swaps every client (iPhone looked "fixed", Bedroom/Mac/TV broken).
      const bool src_rgb8 = !src_xb30 &&
        (source_fourcc == DRM_FORMAT_XBGR8888 || source_fourcc == DRM_FORMAT_ABGR8888);
      return sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], texture, stream.get(),
        encode_sw_format == AV_PIX_FMT_P010, src_xb30, src_rgb8);
    }

    int convert_blank() {
      if (!staging_tex || check(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream before blank upload: "sv)) {
        return -1;
      }
      platf::img_t blank;
      blank.width = width;
      blank.height = height;
      blank.pixel_pitch = 4;
      blank.row_pitch = width * 4;
      std::vector<std::uint8_t> zeros(static_cast<std::size_t>(blank.row_pitch) * blank.height, 0);
      blank.data = zeros.data();
      return sws.load_ram(blank, staging_tex->array) ||
        sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(*staging_tex), stream.get(),
          encode_sw_format == AV_PIX_FMT_P010, false);
    }

    int convert_mmap(egl::img_descriptor_t &descriptor) {
      const auto frame_layout = layout(descriptor.sd);
      if (!frame_layout || !staging_tex) {
        BOOST_LOG(error) << "CUDA DMABUF: mmap fallback rejected non-LINEAR one-plane packed-4 descriptor"sv;
        return -1;
      }
      const auto allocation_size = dmabuf_allocation_size(
        descriptor.sd.fds[0], frame_layout->required_size, "mmap fallback"sv);
      if (!allocation_size || *allocation_size > std::numeric_limits<std::size_t>::max()) {
        if (allocation_size) {
          BOOST_LOG(error) << "CUDA DMABUF: mmap allocation exceeds addressable size: allocation_size="sv << *allocation_size;
        }
        return -1;
      }
      stream_stats::update_encode_path_metadata(
        "mmap_cuda",
        platf::frame_residency_e::cpu,
        platf::frame_format_e::bgra8
      );
      if (check(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream before mmap upload: "sv)) {
        return -1;
      }
      void *map = mmap(nullptr, static_cast<std::size_t>(*allocation_size), PROT_READ, MAP_SHARED, descriptor.sd.fds[0], 0);
      if (map == MAP_FAILED) {
        BOOST_LOG(error) << "CUDA DMABUF: mmap failed: allocation_size="sv << *allocation_size
                         << " error="sv << strerror(errno);
        return -1;
      }
      platf::img_t host;
      host.width = width;
      host.height = height;
      host.pixel_pitch = 4;
      host.row_pitch = static_cast<std::int32_t>(frame_layout->pitch);
      host.data = static_cast<std::uint8_t *>(map) + frame_layout->offset;
      const int load_result = sws.load_ram(host, staging_tex->array);
      munmap(map, static_cast<std::size_t>(*allocation_size));
      if (load_result) {
        return -1;
      }
      // mmap staging is uchar4; XB30 bytes will be wrong here — only SDR/fallback path.
      // Match vulkan_and_convert: XBGR/ABGR = RGB bytes; XRGB/ARGB = BGR bytes.
      const bool src_rgb8 = descriptor.sd.fourcc == DRM_FORMAT_XBGR8888 ||
        descriptor.sd.fourcc == DRM_FORMAT_ABGR8888;
      return sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(*staging_tex), stream.get(),
        encode_sw_format == AV_PIX_FMT_P010, false, src_rgb8);
    }

    bool has_device_extension(const std::vector<VkExtensionProperties> &extensions, const char *name) const {
      return std::any_of(extensions.begin(), extensions.end(), [&](const auto &extension) {
        return std::strcmp(extension.extensionName, name) == 0;
      });
    }

    bool external_buffer_supported(VkExternalMemoryHandleTypeFlagBits handle_type, VkBufferUsageFlags usage, VkExternalMemoryFeatureFlags feature) {
      VkPhysicalDeviceExternalBufferInfo info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = 0,
        .usage = usage,
        .handleType = handle_type,
      };
      VkExternalBufferProperties properties {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      vkGetPhysicalDeviceExternalBufferProperties(physical_device, &info, &properties);
      const auto &external = properties.externalMemoryProperties;
      return (external.externalMemoryFeatures & feature) == feature &&
             (external.compatibleHandleTypes & handle_type) != 0;
    }

    bool init_vulkan() {
      if (!cdf->cuImportExternalMemory || !cdf->cuExternalMemoryGetMappedBuffer ||
          !cdf->cuDestroyExternalMemory || (!cdf->cuDeviceGetUuid_v2 && !cdf->cuDeviceGetUuid)) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: required CUDA external-memory or UUID symbols unavailable"sv;
        return false;
      }

      CUdevice cuda_device;
      CUuuid cuda_uuid {};
      if (cdf->cuCtxGetDevice(&cuda_device) != CUDA_SUCCESS ||
          ((cdf->cuDeviceGetUuid_v2 && cdf->cuDeviceGetUuid_v2(&cuda_uuid, cuda_device) != CUDA_SUCCESS) ||
           (!cdf->cuDeviceGetUuid_v2 && cdf->cuDeviceGetUuid(&cuda_uuid, cuda_device) != CUDA_SUCCESS))) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: couldn't query selected CUDA context UUID"sv;
        return false;
      }

      auto enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
      std::uint32_t api_version = VK_API_VERSION_1_0;
      if (!enumerate_instance_version || enumerate_instance_version(&api_version) != VK_SUCCESS || api_version < VK_API_VERSION_1_1) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: Vulkan 1.1 loader required"sv;
        return false;
      }
      VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "polaris-cuda-dmabuf",
        .apiVersion = VK_API_VERSION_1_1,
      };
      VkInstanceCreateInfo instance_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
      };
      if (!vk_call(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance"sv)) {
        return false;
      }

      std::uint32_t physical_count = 0;
      if (!vk_call(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr), "vkEnumeratePhysicalDevices(count)"sv) || !physical_count) {
        return false;
      }
      std::vector<VkPhysicalDevice> physical_devices(physical_count);
      if (!vk_call(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()), "vkEnumeratePhysicalDevices"sv)) {
        return false;
      }
      for (const auto candidate : physical_devices) {
        VkPhysicalDeviceIDProperties id {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        if (std::memcmp(id.deviceUUID, cuda_uuid.bytes, VK_UUID_SIZE) != 0) {
          continue;
        }
        if (properties.properties.apiVersion < VK_API_VERSION_1_1) {
          BOOST_LOG(error) << "CUDA DMABUF Vulkan: CUDA-matched physical device supports only Vulkan "sv
                           << VK_API_VERSION_MAJOR(properties.properties.apiVersion) << '.'
                           << VK_API_VERSION_MINOR(properties.properties.apiVersion) << "; Vulkan 1.1 required"sv;
          return false;
        }
        physical_device = candidate;
        break;
      }
      if (!physical_device) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: no Vulkan physical device matches selected CUDA UUID"sv;
        return false;
      }

      std::uint32_t extension_count = 0;
      if (!vk_call(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr), "vkEnumerateDeviceExtensionProperties(count)"sv)) {
        return false;
      }
      std::vector<VkExtensionProperties> extensions(extension_count);
      if (!vk_call(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, extensions.data()), "vkEnumerateDeviceExtensionProperties"sv)) {
        return false;
      }
      constexpr std::array required_extensions {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      };
      for (const auto *extension : required_extensions) {
        if (!has_device_extension(extensions, extension)) {
          BOOST_LOG(error) << "CUDA DMABUF Vulkan: missing device extension "sv << extension;
          return false;
        }
      }

      if (!external_buffer_supported(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
          !external_buffer_supported(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: required external-buffer import/export capability unavailable"sv;
        return false;
      }

      std::uint32_t queue_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queue_properties(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queue_properties.data());
      for (std::uint32_t i = 0; i < queue_count; ++i) {
        if (queue_properties[i].queueCount && (queue_properties[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
          queue_family = i;
          break;
        }
      }
      if (queue_family == VK_QUEUE_FAMILY_IGNORED) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: no transfer-capable queue family"sv;
        return false;
      }

      float priority = 1.0f;
      VkDeviceQueueCreateInfo queue_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
      };
      VkDeviceCreateInfo device_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = static_cast<std::uint32_t>(required_extensions.size()),
        .ppEnabledExtensionNames = required_extensions.data(),
      };
      if (!vk_call(vkCreateDevice(physical_device, &device_info, nullptr, &device), "vkCreateDevice"sv)) {
        return false;
      }
      vkGetDeviceQueue(device, queue_family, 0, &queue);
      get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
      get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
      if (!get_memory_fd || !get_memory_fd_properties) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: external-memory-fd entry points unavailable"sv;
        return false;
      }

      vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
      VkCommandPoolCreateInfo pool_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family,
      };
      if (!vk_call(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool), "vkCreateCommandPool"sv)) {
        return false;
      }
      VkCommandBufferAllocateInfo command_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
      };
      if (!vk_call(vkAllocateCommandBuffers(device, &command_info, &command_buffer), "vkAllocateCommandBuffers"sv)) {
        return false;
      }
      VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      return vk_call(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence"sv);
    }

    std::optional<std::uint32_t> memory_type(std::uint32_t bits, VkMemoryPropertyFlags preferred = 0) const {
      for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & preferred) == preferred) {
          return i;
        }
      }
      if (preferred) {
        return memory_type(bits);
      }
      return std::nullopt;
    }

    bool ensure_destination(const layout_t &frame_layout) {
      if (destination_buffer) {
        return destination_copy_size == frame_layout.copy_size && destination_pitch == frame_layout.pitch;
      }
      VkExternalMemoryBufferCreateInfo external_info {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      };
      VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_info,
        .size = frame_layout.copy_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      if (!vk_call(vkCreateBuffer(device, &buffer_info, nullptr, &destination_buffer), "vkCreateBuffer(destination)"sv)) {
        return false;
      }
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, destination_buffer, &requirements);
      const auto type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!type) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: no destination memory type"sv;
        return false;
      }
      VkExportMemoryAllocateInfo export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      };
      VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &export_info,
        .buffer = destination_buffer,
      };
      VkMemoryAllocateInfo allocation_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = *type,
      };
      if (!vk_call(vkAllocateMemory(device, &allocation_info, nullptr, &destination_memory), "vkAllocateMemory(destination)"sv) ||
          !vk_call(vkBindBufferMemory(device, destination_buffer, destination_memory, 0), "vkBindBufferMemory(destination)"sv)) {
        return false;
      }
      destination_allocation_size = requirements.size;
      destination_copy_size = frame_layout.copy_size;
      destination_pitch = frame_layout.pitch;

      VkMemoryGetFdInfoKHR fd_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = destination_memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      };
      int export_fd = -1;
      if (!vk_call(get_memory_fd(device, &fd_info, &export_fd), "vkGetMemoryFdKHR"sv)) {
        return false;
      }
      CUDA_EXTERNAL_MEMORY_HANDLE_DESC cuda_handle {};
      cuda_handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
      cuda_handle.handle.fd = export_fd;
      cuda_handle.size = destination_allocation_size;
      cuda_handle.flags = cudaExternalMemoryDedicated;
      CUresult cuda_result = cdf->cuImportExternalMemory(&destination_external_memory, &cuda_handle);
      if (cuda_result == CUDA_SUCCESS) {
        export_fd = -1;
      }
      if (export_fd >= 0) {
        close(export_fd);
      }
      if (check(cuda_result, "Couldn't import Vulkan destination into CUDA: "sv)) {
        return false;
      }
      CUDA_EXTERNAL_MEMORY_BUFFER_DESC map_info {};
      map_info.size = destination_allocation_size;
      if (check(cdf->cuExternalMemoryGetMappedBuffer(&destination_device_ptr, destination_external_memory, &map_info),
            "Couldn't map Vulkan destination in CUDA: "sv)) {
        return false;
      }
      auto texture = make_pitch2d_texture(
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(destination_device_ptr)),
        width,
        height,
        destination_pitch,
        linear_interpolation,
        source_fourcc == DRM_FORMAT_XBGR2101010
      );
      if (!texture) {
        return false;
      }
      destination_texture = *texture;
      return true;
    }

    void release_source(source_import_t &source) {
      if (source.buffer) {
        vkDestroyBuffer(device, source.buffer, nullptr);
      }
      if (source.memory) {
        vkFreeMemory(device, source.memory, nullptr);
      }
      source = {};
    }

    source_import_t *source_for(egl::img_descriptor_t &descriptor, const layout_t &frame_layout) {
      const auto allocation_size = dmabuf_allocation_size(
        descriptor.sd.fds[0], frame_layout.required_size, "Vulkan source import"sv);
      if (!allocation_size) {
        return nullptr;
      }
      for (auto &source : sources) {
        if (source.buffer && source.buffer_key == descriptor.dmabuf_buffer_key) {
          if (source.allocation_size == *allocation_size && source.copy_offset == frame_layout.offset &&
              source.copy_size == frame_layout.copy_size && source.pitch == frame_layout.pitch) {
            return &source;
          }
          release_source(source);
          break;
        }
      }

      auto &source = sources[source_slot];
      source_slot = (source_slot + 1) % sources.size();
      release_source(source);
      VkExternalMemoryBufferCreateInfo external_info {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_info,
        .size = *allocation_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      if (!vk_call(vkCreateBuffer(device, &buffer_info, nullptr, &source.buffer), "vkCreateBuffer(source)"sv)) {
        return nullptr;
      }
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, source.buffer, &requirements);
      if (*allocation_size < requirements.size) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: source allocation doesn't cover Vulkan memory requirement: allocation_size="sv
                         << *allocation_size << " layout_required="sv << frame_layout.required_size
                         << " vk_memory_requirement="sv << requirements.size;
        release_source(source);
        return nullptr;
      }
      int import_fd = fcntl(descriptor.sd.fds[0], F_DUPFD_CLOEXEC, 0);
      if (import_fd < 0) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: DMA-BUF fd duplication failed: allocation_size="sv
                         << *allocation_size << " error="sv << strerror(errno);
        release_source(source);
        return nullptr;
      }
      VkMemoryFdPropertiesKHR fd_properties {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      if (!vk_call(get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, import_fd, &fd_properties),
            "vkGetMemoryFdPropertiesKHR"sv)) {
        close(import_fd);
        release_source(source);
        return nullptr;
      }
      const auto type = memory_type(requirements.memoryTypeBits & fd_properties.memoryTypeBits);
      if (!type) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: imported DMA-BUF has no compatible memory type: allocation_size="sv
                         << *allocation_size << " vk_memory_requirement="sv << requirements.size;
        close(import_fd);
        release_source(source);
        return nullptr;
      }
      VkImportMemoryFdInfoKHR import_info {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
      };
      VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .buffer = source.buffer,
      };
      VkMemoryAllocateInfo allocation_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = *allocation_size,
        .memoryTypeIndex = *type,
      };
      const VkResult allocation_result = vkAllocateMemory(device, &allocation_info, nullptr, &source.memory);
      if (allocation_result == VK_SUCCESS) {
        import_fd = -1;
      }
      if (import_fd >= 0) {
        close(import_fd);
      }
      if (!vk_call(allocation_result, "vkAllocateMemory(source import)"sv)) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: source import allocation failed: allocation_size="sv
                         << *allocation_size << " vk_memory_requirement="sv << requirements.size;
        release_source(source);
        return nullptr;
      }
      if (!vk_call(vkBindBufferMemory(device, source.buffer, source.memory, 0), "vkBindBufferMemory(source)"sv)) {
        BOOST_LOG(error) << "CUDA DMABUF Vulkan: source bind failed: allocation_size="sv << *allocation_size
                         << " vk_memory_requirement="sv << requirements.size;
        release_source(source);
        return nullptr;
      }
      source.buffer_key = descriptor.dmabuf_buffer_key;
      source.allocation_size = *allocation_size;
      source.copy_offset = frame_layout.offset;
      source.copy_size = frame_layout.copy_size;
      source.pitch = frame_layout.pitch;
      return &source;
    }

    int vulkan_and_convert(egl::img_descriptor_t &descriptor) {
      const auto frame_layout = layout(descriptor.sd);
      if (!frame_layout) {
        return -1;
      }
      // Recreate destination texture when packed format changes (8-bit vs 10-bit).
      if (source_fourcc && source_fourcc != descriptor.sd.fourcc) {
        release_cuda_destination();
      }
      source_fourcc = descriptor.sd.fourcc;
      if (!device || !ensure_destination(*frame_layout)) {
        return -1;
      }
      auto *source = source_for(descriptor, *frame_layout);
      if (!source || check(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream before Vulkan overwrite: "sv)) {
        return -1;
      }
      if (!vk_call(vkResetFences(device, 1, &fence), "vkResetFences"sv) ||
          !vk_call(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer"sv)) {
        return -1;
      }
      VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      if (!vk_call(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer"sv)) {
        return -1;
      }
      VkBufferCopy copy {
        .srcOffset = source->copy_offset,
        .dstOffset = 0,
        .size = source->copy_size,
      };
      vkCmdCopyBuffer(command_buffer, source->buffer, destination_buffer, 1, &copy);
      VkBufferMemoryBarrier barrier {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = destination_buffer,
        .offset = 0,
        .size = destination_copy_size,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
      if (!vk_call(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer"sv)) {
        return -1;
      }
      VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
      };
      if (!vk_call(vkQueueSubmit(queue, 1, &submit_info, fence), "vkQueueSubmit"sv) ||
          !vk_call(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"sv)) {
        return -1;
      }
      // frame->format is AV_PIX_FMT_CUDA for NVENC; use stored sw_format for layout.
      const bool dst_p010 = encode_sw_format == AV_PIX_FMT_P010;
      const bool src_xb30 = descriptor.sd.fourcc == DRM_FORMAT_XBGR2101010;
      // XRGB/AR24 (SPA BGRx): B,G,R,X → bgra_to_rgb. XBGR/AB24 (SPA RGBx): R,G,B,X.
      const bool src_rgb8 = !src_xb30 &&
        (descriptor.sd.fourcc == DRM_FORMAT_XBGR8888 || descriptor.sd.fourcc == DRM_FORMAT_ABGR8888);
      const int result = sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1],
        destination_texture, stream.get(), dst_p010, src_xb30, src_rgb8);
      if (result) {
        return result;
      }
      // Ensure fill+convert kernels finish before NVENC samples the CUDA frame.
      if (check(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream after convert: "sv)) {
        return -1;
      }
      const auto fmt = dst_p010 ? platf::frame_format_e::p010 : platf::frame_format_e::bgra8;
      stream_stats::update_encode_path_metadata(
        "vulkan_cuda",
        platf::frame_residency_e::gpu,
        fmt
      );
      if (!vulkan_logged) {
        BOOST_LOG(info) << "CUDA DMABUF: convert_path=vulkan_cuda "sv << width << 'x' << height
                        << " pitch="sv << destination_pitch
                        << " fourcc=0x"sv << std::hex << descriptor.sd.fourcc << std::dec
                        << " src_xb30="sv << src_xb30 << " src_rgb8="sv << src_rgb8
                        << " dst_p010="sv << dst_p010
                        << " source imports cached; destination mapping persistent"sv;
        vulkan_logged = true;
      }
      return 0;
    }

    void release_cuda_destination() {
      if (stream) {
        CU_CHECK_IGNORE(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream during Vulkan bridge cleanup");
      }
      if (destination_texture) {
        destroy_texture(destination_texture);
        destination_texture = 0;
      }
      if (destination_device_ptr) {
        CU_CHECK_IGNORE(cdf->cuMemFree(destination_device_ptr), "Couldn't free CUDA external-memory mapping");
        destination_device_ptr = 0;
      }
      if (destination_external_memory) {
        CU_CHECK_IGNORE(cdf->cuDestroyExternalMemory(destination_external_memory), "Couldn't destroy CUDA external memory");
        destination_external_memory = nullptr;
      }
    }

    void release_vulkan() {
      if (device) {
        vkDeviceWaitIdle(device);
        for (auto &source : sources) {
          release_source(source);
        }
        if (destination_buffer) {
          vkDestroyBuffer(device, destination_buffer, nullptr);
        }
        if (destination_memory) {
          vkFreeMemory(device, destination_memory, nullptr);
        }
        if (fence) {
          vkDestroyFence(device, fence, nullptr);
        }
        if (command_pool) {
          vkDestroyCommandPool(device, command_pool, nullptr);
        }
        vkDestroyDevice(device, nullptr);
      }
      if (instance) {
        vkDestroyInstance(instance, nullptr);
      }
      sources = {};
      source_slot = 0;
      instance = VK_NULL_HANDLE;
      physical_device = VK_NULL_HANDLE;
      memory_properties = {};
      device = VK_NULL_HANDLE;
      queue_family = VK_QUEUE_FAMILY_IGNORED;
      queue = VK_NULL_HANDLE;
      command_pool = VK_NULL_HANDLE;
      command_buffer = VK_NULL_HANDLE;
      fence = VK_NULL_HANDLE;
      get_memory_fd = nullptr;
      get_memory_fd_properties = nullptr;
      destination_buffer = VK_NULL_HANDLE;
      destination_memory = VK_NULL_HANDLE;
      destination_allocation_size = 0;
      destination_copy_size = 0;
      destination_pitch = 0;
    }

    std::array<source_import_t, 4> sources {};
    std::size_t source_slot {0};
    std::uint64_t sequence {0};
    CUcontext cu_ctx {nullptr};
    bool force_mmap {false};
    bool fallback_logged {false};
    bool vulkan_logged {false};
    bool active_frame {false};
    std::optional<tex_t> staging_tex;

    VkInstance instance {VK_NULL_HANDLE};
    VkPhysicalDevice physical_device {VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties {};
    VkDevice device {VK_NULL_HANDLE};
    std::uint32_t queue_family {VK_QUEUE_FAMILY_IGNORED};
    VkQueue queue {VK_NULL_HANDLE};
    VkCommandPool command_pool {VK_NULL_HANDLE};
    VkCommandBuffer command_buffer {VK_NULL_HANDLE};
    VkFence fence {VK_NULL_HANDLE};
    PFN_vkGetMemoryFdKHR get_memory_fd {nullptr};
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties {nullptr};

    VkBuffer destination_buffer {VK_NULL_HANDLE};
    VkDeviceMemory destination_memory {VK_NULL_HANDLE};
    VkDeviceSize destination_allocation_size {0};
    VkDeviceSize destination_copy_size {0};
    std::uint32_t destination_pitch {0};
    CUexternalMemory destination_external_memory {nullptr};
    CUdeviceptr destination_device_ptr {0};
    cudaTextureObject_t destination_texture {0};
    std::uint32_t source_fourcc {0};
    AVPixelFormat encode_sw_format {AV_PIX_FMT_NONE};
  };

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram) {
    if (init()) {
      return nullptr;
    }

    std::unique_ptr<cuda_t> cuda;

    if (vram) {
      cuda = std::make_unique<cuda_vram_t>();
    } else {
      cuda = std::make_unique<cuda_ram_t>();
    }

    if (cuda->init(width, height)) {
      return nullptr;
    }

    return cuda;
  }

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param width Width of captured frames.
   * @param height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y) {
    if (init()) {
      return nullptr;
    }

    auto cuda = std::make_unique<gl_cuda_vram_t>();

    if (cuda->init(width, height, offset_x, offset_y)) {
      return nullptr;
    }

    return cuda;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_dmabuf_encode_device(int width, int height) {
    if (init()) {
      return nullptr;
    }

    auto device = std::make_unique<cuda_dmabuf_t>();
    if (device->init(width, height)) {
      return nullptr;
    }
    // Prefer true 10-bit GPU frames when capture is xBGR_210LE + HDR client.
    // Patch 04 still forces NV12 for non-HDR streams (even if client asks 10-bit SDR).
    // convert() writes P010 when frame is AV_PIX_FMT_P010*, else NV12 from BGRx.
    device->prefer_8bit_encode = false;
    return device;
  }

  namespace nvfbc {
    static PNVFBCCREATEINSTANCE createInstance {};
    static NVFBC_API_FUNCTION_LIST func {NVFBC_VERSION};

    static constexpr inline NVFBC_BOOL nv_bool(bool b) {
      return b ? NVFBC_TRUE : NVFBC_FALSE;
    }

    static void *handle {nullptr};

    int init() {
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libnvidia-fbc.so.1", "libnvidia-fbc.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &createInstance, "NvFBCCreateInstance"},
      };

      if (dyn::load(handle, funcs)) {
        dlclose(handle);
        handle = nullptr;

        return -1;
      }

      auto status = cuda::nvfbc::createInstance(&cuda::nvfbc::func);
      if (status) {
        BOOST_LOG(error) << "Unable to create NvFBC instance"sv;

        dlclose(handle);
        handle = nullptr;
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    class ctx_t {
    public:
      ctx_t(NVFBC_SESSION_HANDLE handle) {
        NVFBC_BIND_CONTEXT_PARAMS params {NVFBC_BIND_CONTEXT_PARAMS_VER};

        if (func.nvFBCBindContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't bind NvFBC context to current thread: " << func.nvFBCGetLastErrorStr(handle);
        }

        this->handle = handle;
      }

      ~ctx_t() {
        NVFBC_RELEASE_CONTEXT_PARAMS params {NVFBC_RELEASE_CONTEXT_PARAMS_VER};
        if (func.nvFBCReleaseContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't release NvFBC context from current thread: " << func.nvFBCGetLastErrorStr(handle);
        }
      }

      NVFBC_SESSION_HANDLE handle;
    };

    class handle_t {
      enum flag_e {
        SESSION_HANDLE,
        SESSION_CAPTURE,
        MAX_FLAGS,
      };

    public:
      handle_t() = default;

      handle_t(handle_t &&other):
          handle_flags {other.handle_flags},
          handle {other.handle} {
        other.handle_flags.reset();
      }

      handle_t &operator=(handle_t &&other) {
        std::swap(handle_flags, other.handle_flags);
        std::swap(handle, other.handle);

        return *this;
      }

      static std::optional<handle_t> make() {
        NVFBC_CREATE_HANDLE_PARAMS params {NVFBC_CREATE_HANDLE_PARAMS_VER};

        // Set privateData to allow NvFBC on consumer NVIDIA GPUs.
        // Based on https://github.com/keylase/nvidia-patch/blob/3193b4b1cea91527bf09ea9b8db5aade6a3f3c0a/win/nvfbcwrp/nvfbcwrp_main.cpp#L23-L25 .
        const unsigned int MAGIC_PRIVATE_DATA[4] = {0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA};
        params.privateData = MAGIC_PRIVATE_DATA;
        params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);

        handle_t handle;
        auto status = func.nvFBCCreateHandle(&handle.handle, &params);
        if (status) {
          auto *last_error = handle.handle ? func.nvFBCGetLastErrorStr(handle.handle) : nullptr;
          auto error_text = (last_error && *last_error) ? std::string_view {last_error} : "no NvFBC error string available"sv;
          BOOST_LOG(error) << "Failed to create NvFBC session handle: status="sv << static_cast<int>(status) << " error="sv << error_text;

          return std::nullopt;
        }

        handle.handle_flags[SESSION_HANDLE] = true;

        return handle;
      }

      const char *last_error() {
        return func.nvFBCGetLastErrorStr(handle);
      }

      std::optional<NVFBC_GET_STATUS_PARAMS> status() {
        NVFBC_GET_STATUS_PARAMS params {NVFBC_GET_STATUS_PARAMS_VER};

        auto status = func.nvFBCGetStatus(handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to get NvFBC status: "sv << last_error();

          return std::nullopt;
        }

        return params;
      }

      int capture(NVFBC_CREATE_CAPTURE_SESSION_PARAMS &capture_params) {
        if (func.nvFBCCreateCaptureSession(handle, &capture_params)) {
          BOOST_LOG(error) << "Failed to start capture session: "sv << last_error();
          return -1;
        }

        handle_flags[SESSION_CAPTURE] = true;

        NVFBC_TOCUDA_SETUP_PARAMS setup_params {
          NVFBC_TOCUDA_SETUP_PARAMS_VER,
          NVFBC_BUFFER_FORMAT_BGRA,
        };

        if (func.nvFBCToCudaSetUp(handle, &setup_params)) {
          BOOST_LOG(error) << "Failed to setup cuda interop with nvFBC: "sv << last_error();
          return -1;
        }
        return 0;
      }

      int stop() {
        if (!handle_flags[SESSION_CAPTURE]) {
          return 0;
        }

        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params {NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER};

        if (func.nvFBCDestroyCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy capture session: "sv << last_error();

          return -1;
        }

        handle_flags[SESSION_CAPTURE] = false;

        return 0;
      }

      int reset() {
        if (!handle_flags[SESSION_HANDLE]) {
          return 0;
        }

        stop();

        NVFBC_DESTROY_HANDLE_PARAMS params {NVFBC_DESTROY_HANDLE_PARAMS_VER};

        ctx_t ctx {handle};
        if (func.nvFBCDestroyHandle(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy session handle: "sv << func.nvFBCGetLastErrorStr(handle);
        }

        handle_flags[SESSION_HANDLE] = false;

        return 0;
      }

      ~handle_t() {
        reset();
      }

      std::bitset<MAX_FLAGS> handle_flags;

      NVFBC_SESSION_HANDLE handle;
    };

    class display_t: public platf::display_t {
    public:
      int init(const std::string_view &display_name, const ::video::config_t &config) {
        auto handle = handle_t::make();
        if (!handle) {
          return -1;
        }

        ctx_t ctx {handle->handle};

        auto status_params = handle->status();
        if (!status_params) {
          return -1;
        }

        int streamedMonitor = -1;
        if (!display_name.empty()) {
          if (status_params->bXRandRAvailable) {
            auto monitor_nr = util::from_view(display_name);

            if (monitor_nr < 0 || monitor_nr >= status_params->dwOutputNum) {
              BOOST_LOG(warning) << "Can't stream monitor ["sv << monitor_nr << "], it needs to be between [0] and ["sv << status_params->dwOutputNum - 1 << "], defaulting to virtual desktop"sv;
            } else {
              streamedMonitor = monitor_nr;
            }
          } else {
            BOOST_LOG(warning) << "XrandR not available, streaming entire virtual desktop"sv;
          }
        }

        delay = std::chrono::nanoseconds {1s} / config.framerate;

        capture_params = NVFBC_CREATE_CAPTURE_SESSION_PARAMS {NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER};

        capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        capture_params.bDisableAutoModesetRecovery = nv_bool(true);

        capture_params.dwSamplingRateMs = 1000 /* ms */ / config.framerate;

        if (streamedMonitor != -1) {
          auto &output = status_params->outputs[streamedMonitor];

          width = output.trackedBox.w;
          height = output.trackedBox.h;
          offset_x = output.trackedBox.x;
          offset_y = output.trackedBox.y;

          capture_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
          capture_params.dwOutputId = output.dwId;
        } else {
          capture_params.eTrackingType = NVFBC_TRACKING_SCREEN;

          width = status_params->screenSize.w;
          height = status_params->screenSize.h;
        }

        env_width = status_params->screenSize.w;
        env_height = status_params->screenSize.h;

        this->handle = std::move(*handle);
        return 0;
      }

      platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        {
          // We must create at least one texture on this thread before calling NvFBCToCudaSetUp()
          // Otherwise it fails with "Unable to register an OpenGL buffer to a CUDA resource (result: 201)" message
          std::shared_ptr<platf::img_t> img_dummy;
          pull_free_image_cb(img_dummy);
        }

        // Force display_t::capture to initialize handle_t::capture
        cursor_visible = !*cursor;

        ctx_t ctx {handle.handle};
        auto fg = util::fail_guard([&]() {
          handle.reset();
        });

        sleep_overshoot_logger.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
            sleep_overshoot_logger.first_point(next_frame);
            sleep_overshoot_logger.second_point_now_and_log();
          }

          next_frame += delay;
          if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 150ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }

        return platf::capture_e::ok;
      }

      // Reinitialize the capture session.
      platf::capture_e reinit(bool cursor) {
        if (handle.stop()) {
          return platf::capture_e::error;
        }

        cursor_visible = cursor;
        if (cursor) {
          capture_params.bPushModel = nv_bool(false);
          capture_params.bWithCursor = nv_bool(true);
          capture_params.bAllowDirectCapture = nv_bool(false);
        } else {
          capture_params.bPushModel = nv_bool(true);
          capture_params.bWithCursor = nv_bool(false);
          capture_params.bAllowDirectCapture = nv_bool(true);
        }

        if (handle.capture(capture_params)) {
          return platf::capture_e::error;
        }

        // If trying to capture directly, test if it actually does.
        if (capture_params.bAllowDirectCapture) {
          CUdeviceptr device_ptr;
          NVFBC_FRAME_GRAB_INFO info;

          NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
            NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
            NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
            &device_ptr,
            &info,
            0,
          };

          // Direct Capture may fail the first few times, even if it's possible
          for (int x = 0; x < 3; ++x) {
            if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
              if (status == NVFBC_ERR_MUST_RECREATE) {
                return platf::capture_e::reinit;
              }

              BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();

              return platf::capture_e::error;
            }

            if (info.bDirectCapture) {
              break;
            }

            BOOST_LOG(debug) << "Direct capture failed attempt ["sv << x << ']';
          }

          if (!info.bDirectCapture) {
            BOOST_LOG(debug) << "Direct capture failed, trying the extra copy method"sv;
            // Direct capture failed
            capture_params.bPushModel = nv_bool(false);
            capture_params.bWithCursor = nv_bool(false);
            capture_params.bAllowDirectCapture = nv_bool(false);

            if (handle.stop() || handle.capture(capture_params)) {
              return platf::capture_e::error;
            }
          }
        }

        return platf::capture_e::ok;
      }

      platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        if (cursor != cursor_visible) {
          auto status = reinit(cursor);
          if (status != platf::capture_e::ok) {
            return status;
          }
        }

        CUdeviceptr device_ptr;
        NVFBC_FRAME_GRAB_INFO info;

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
          NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
          NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
          &device_ptr,
          &info,
          (std::uint32_t) timeout.count(),
        };

        if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
          if (status == NVFBC_ERR_MUST_RECREATE) {
            return platf::capture_e::reinit;
          }

          BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();
          return platf::capture_e::error;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (img_t *) img_out.get();

        if (img->tex.copy((std::uint8_t *) device_ptr, img->height, img->row_pitch)) {
          return platf::capture_e::error;
        }

        // P0-3 T0: this is where the captured frame genuinely becomes
        // available. Matches the same stamping point used by every other
        // Linux capture backend (wlgrab, kmsgrab, x11grab, cage_screencopy) -
        // this one was the only display_t implementer missing it.
        img_out->frame_timestamp = std::chrono::steady_clock::now();

        return platf::capture_e::ok;
      }

      std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
        return ::cuda::make_avcodec_encode_device(width, height, true);
      }

      std::shared_ptr<platf::img_t> alloc_img() override {
        auto img = std::make_shared<cuda::img_t>();

        img->data = nullptr;
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->width * img->pixel_pitch;

        auto tex_opt = tex_t::make(height, width * img->pixel_pitch);
        if (!tex_opt) {
          return nullptr;
        }

        img->tex = std::move(*tex_opt);

        return img;
      };

      int dummy_img(platf::img_t *) override {
        return 0;
      }

      std::chrono::nanoseconds delay;

      bool cursor_visible;
      handle_t handle;

      NVFBC_CREATE_CAPTURE_SESSION_PARAMS capture_params;
    };
  }  // namespace nvfbc
}  // namespace cuda

namespace platf {
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize nvfbc display with the given hw device type"sv;
      return nullptr;
    }

    auto display = std::make_shared<cuda::nvfbc::display_t>();

    if (display->init(display_name, config)) {
      return nullptr;
    }

    return display;
  }

  std::vector<std::string> nvfbc_display_names() {
    if (cuda::init() || cuda::nvfbc::init()) {
      return {};
    }

    std::vector<std::string> display_names;

    auto handle = cuda::nvfbc::handle_t::make();
    if (!handle) {
      return {};
    }

    auto status_params = handle->status();
    if (!status_params) {
      return {};
    }

    if (!status_params->bIsCapturePossible) {
      BOOST_LOG(error) << "NVidia driver doesn't support NvFBC screencasting"sv;
    }

    BOOST_LOG(info) << "Found ["sv << status_params->dwOutputNum << "] outputs"sv;
    BOOST_LOG(info) << "Virtual Desktop: "sv << status_params->screenSize.w << 'x' << status_params->screenSize.h;
    BOOST_LOG(info) << "XrandR: "sv << (status_params->bXRandRAvailable ? "available"sv : "unavailable"sv);

    for (auto x = 0; x < status_params->dwOutputNum; ++x) {
      auto &output = status_params->outputs[x];
      BOOST_LOG(info) << "-- Output --"sv;
      BOOST_LOG(debug) << "  ID: "sv << output.dwId;
      BOOST_LOG(debug) << "  Name: "sv << output.name;
      BOOST_LOG(info) << "  Resolution: "sv << output.trackedBox.w << 'x' << output.trackedBox.h;
      BOOST_LOG(info) << "  Offset: "sv << output.trackedBox.x << 'x' << output.trackedBox.y;
      display_names.emplace_back(std::to_string(x));
    }

    return display_names;
  }
}  // namespace platf
