/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

// local includes
#include "input.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "video_colorspace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;

namespace video {

  /* Encoding configuration requested by remote client */
  struct config_t {
    // DO NOT CHANGE ORDER OR ADD FIELDS IN THE MIDDLE!!!!!
    // ONLY APPEND NEW FIELD AFTERWARDS!!!!!!!!!
    // BIG F WORD to Sunshine!!!!!!!!!
    int width;  // Video width in pixels
    int height;  // Video height in pixels
    int framerate;  // Requested framerate, used in individual frame bitrate budget calculation
    int bitrate;  // Video bitrate in kilobits (1000 bits) for requested framerate
    int slicesPerFrame;  // Number of slices per frame
    int numRefFrames;  // Max number of reference frames

    /* Requested color range and SDR encoding colorspace, HDR encoding colorspace is always BT.2020+ST2084
       Color range (encoderCscMode & 0x1) : 0 - limited, 1 - full
       SDR encoding colorspace (encoderCscMode >> 1) : 0 - BT.601, 1 - BT.709, 2 - BT.2020 */
    int encoderCscMode;

    int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1

    /* Encoding color depth (bit depth): 0 - 8-bit, 1 - 10-bit
       HDR encoding activates when color depth is higher than 8-bit and the display which is being captured is operating in HDR mode */
    int dynamicRange;

    int chromaSamplingType;  // 0 - 4:2:0, 1 - 4:4:4

    int enableIntraRefresh;  // 0 - disabled, 1 - enabled

    int encodingFramerate; // Requested display framerate
    bool input_only;
  };

  platf::mem_type_e map_base_dev_type(AVHWDeviceType type);
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt);

  void free_ctx(AVCodecContext *ctx);
  void free_frame(AVFrame *frame);
  void free_buffer(AVBufferRef *ref);

  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_ctx>;
  using avcodec_frame_t = util::safe_ptr<AVFrame, free_frame>;
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, free_buffer>;
  using sws_t = util::safe_ptr<SwsContext, sws_freeContext>;
  using img_event_t = std::shared_ptr<safe::event_t<std::shared_ptr<platf::img_t>>>;

  struct conversion_request_t {
    platf::frame_format_e target_format {platf::frame_format_e::unknown};
    platf::frame_residency_e target_residency {platf::frame_residency_e::unknown};
    platf::mem_type_e target_device {platf::mem_type_e::unknown};
  };

  struct native_handle_t {
    platf::mem_type_e device_type {platf::mem_type_e::unknown};
    void *resource {nullptr};
    int fd {-1};
  };

  struct frame_t {
    std::shared_ptr<platf::img_t> image;
    std::uint8_t *cpu_data {nullptr};
    int width {0};
    int height {0};
    int pixel_pitch {0};
    int row_pitch {0};
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    native_handle_t native {};
    platf::frame_metadata_t source_metadata {};
    platf::frame_metadata_t metadata {};

    frame_t() = default;

    explicit frame_t(std::shared_ptr<platf::img_t> image):
        image {std::move(image)} {
      if (this->image) {
        cpu_data = this->image->data;
        width = this->image->width;
        height = this->image->height;
        pixel_pitch = this->image->pixel_pitch;
        row_pitch = this->image->row_pitch;
        timestamp = this->image->frame_timestamp;
        native.device_type = this->image->native_device_type;
        native.resource = this->image->native_resource;
        native.fd = this->image->native_fd;
        source_metadata = this->image->frame_metadata;
        metadata = source_metadata;
      }
    }

    bool valid() const {
      return width > 0 && height > 0 && (has_compat_image() || has_cpu_data() || has_native_handle());
    }

    bool has_compat_image() const {
      return static_cast<bool>(image);
    }

    bool has_cpu_data() const {
      return cpu_data != nullptr;
    }

    bool has_native_handle() const {
      return native.resource != nullptr || native.fd >= 0;
    }

    platf::img_t *compat_img() const {
      return image.get();
    }

    platf::frame_transport_e transport() const {
      return metadata.transport;
    }

    platf::frame_residency_e residency() const {
      return metadata.residency;
    }

    platf::frame_format_e format() const {
      return metadata.format;
    }

    bool gpu_resident() const {
      return metadata.residency == platf::frame_residency_e::gpu;
    }

    void apply_conversion_result(const conversion_request_t &request) {
      metadata.transport = platf::frame_transport_e::internal;
      metadata.residency = request.target_residency;
      metadata.format = request.target_format;
      native.device_type = request.target_device;
    }
  };

  class frame_converter_t {
  public:
    virtual ~frame_converter_t() = default;

    virtual std::string_view name() const = 0;
    virtual bool supports(const frame_t &src, const conversion_request_t &request) const = 0;
    virtual int convert(frame_t &frame, const conversion_request_t &request) = 0;
  };

  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;
    platf::pix_fmt_e pix_fmt_8bit, pix_fmt_10bit;
    platf::pix_fmt_e pix_fmt_yuv444_8bit, pix_fmt_yuv444_10bit;
  };

  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_8bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function
    ):
        avcodec_base_dev_type {avcodec_base_dev_type},
        avcodec_derived_dev_type {avcodec_derived_dev_type},
        avcodec_dev_pix_fmt {avcodec_dev_pix_fmt},
        avcodec_pix_fmt_8bit {avcodec_pix_fmt_8bit},
        avcodec_pix_fmt_10bit {avcodec_pix_fmt_10bit},
        avcodec_pix_fmt_yuv444_8bit {avcodec_pix_fmt_yuv444_8bit},
        avcodec_pix_fmt_yuv444_10bit {avcodec_pix_fmt_yuv444_10bit},
        init_avcodec_hardware_input_buffer {init_avcodec_hardware_input_buffer_function} {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
      pix_fmt_yuv444_8bit = map_pix_fmt(avcodec_pix_fmt_yuv444_8bit);
      pix_fmt_yuv444_10bit = map_pix_fmt(avcodec_pix_fmt_yuv444_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type, avcodec_derived_dev_type;
    AVPixelFormat avcodec_dev_pix_fmt;
    AVPixelFormat avcodec_pix_fmt_8bit, avcodec_pix_fmt_10bit;
    AVPixelFormat avcodec_pix_fmt_yuv444_8bit, avcodec_pix_fmt_yuv444_10bit;

    init_buffer_function_t init_avcodec_hardware_input_buffer;
  };

  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  struct encoder_t {
    std::string_view name;

    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      DYNAMIC_RANGE,  ///< HDR support.
      YUV444,  ///< YUV 4:4:4 support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    static std::string_view from_flag(flag_e flag) {
#define _CONVERT(x) \
  case flag_e::x: \
    return std::string_view(#x)
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(YUV444);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#undef _CONVERT

      return {"unknown"};
    }

    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      option_t(const option_t &) = default;

      std::string name;
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *, std::function<const std::string(const config_t &)>> value;

      option_t(std::string &&name, decltype(value) &&value):
          name {std::move(name)},
          value {std::move(value)} {
      }
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;

    struct codec_t {
      std::vector<option_t> common_options;
      std::vector<option_t> sdr_options;
      std::vector<option_t> hdr_options;
      std::vector<option_t> sdr444_options;
      std::vector<option_t> hdr444_options;
      std::vector<option_t> fallback_options;

      std::string name;
      std::bitset<MAX_FLAGS> capabilities;

      bool operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      std::bitset<MAX_FLAGS>::reference operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    } av1, hevc, h264;

    const codec_t &codec_from_config(const config_t &config) const {
      switch (config.videoFormat) {
        default:
          BOOST_LOG(error) << "Unknown video format " << config.videoFormat << ", falling back to H.264";
          // fallthrough
        case 0:
          return h264;
        case 1:
          return hevc;
        case 2:
          return av1;
      }
    }

    uint32_t flags;
  };

  struct encode_session_t {
    virtual ~encode_session_t() = default;

    virtual int convert(frame_t &frame) = 0;

    virtual void request_idr_frame() = 0;

    virtual void request_normal_frame() = 0;

    virtual void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;

    /**
     * @brief Dynamically update the encoder bitrate at runtime.
     * @param new_bitrate_kbps New target bitrate in kilobits per second.
     * @return `true` if the encoder supports runtime bitrate update and it succeeded.
     */
    virtual bool update_bitrate(int new_bitrate_kbps) {
      return false;  // Not supported by default
    }
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
#endif

#ifdef __linux__
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    virtual bool is_idr() = 0;

    virtual int64_t frame_index() = 0;

    virtual uint8_t *data() = 0;

    virtual size_t data_size() = 0;

    struct replace_t {
      std::string_view old;
      std::string_view _new;

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      replace_t(std::string_view old, std::string_view _new) noexcept:
          old {std::move(old)},
          _new {std::move(_new)} {
      }
    };

    std::vector<replace_t> *replacements = nullptr;
    void *channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
  };

  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    bool is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    int64_t frame_index() override {
      return av_packet->pts;
    }

    uint8_t *data() override {
      return av_packet->data;
    }

    size_t data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;
  };

  struct packet_raw_generic: packet_raw_t {
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data {std::move(frame_data)},
        index {frame_index},
        idr {idr} {
    }

    bool is_idr() override {
      return idr;
    }

    int64_t frame_index() override {
      return index;
    }

    uint8_t *data() override {
      return frame_data.data();
    }

    size_t data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;
    int64_t index;
    bool idr;
  };

  using packet_t = std::unique_ptr<packet_raw_t>;

  struct hdr_info_raw_t {
    explicit hdr_info_raw_t(bool enabled):
        enabled {enabled},
        metadata {} {};
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled {enabled},
        metadata {metadata} {};

    bool enabled;
    SS_HDR_METADATA metadata;
  };

  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  struct codec_capability_state_t {
    int hevc_mode = 0;
    int av1_mode = 0;
    std::array<bool, 3> yuv444_for_codec = {false, false, false};
  };

  extern int active_hevc_mode;
  extern int active_av1_mode;
  extern bool last_encoder_probe_supported_ref_frames_invalidation;
  extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1

  codec_capability_state_t advertised_codec_capability_state();
  bool advertised_codec_capability_state_ready();
  void reset_encoder_probe_state();

  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  );

  bool validate_encoder(encoder_t &encoder, bool expect_failure);

  /**
   * @brief Check if we can allow probing for the encoders.
   * @return True if there should be no issues with the probing, false if we should prevent it.
   */
  bool allow_encoder_probing();

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @param strict_configured_encoder If true and an encoder is explicitly configured, fail instead of falling back.
   * @param save_successful_cache If false, do not persist the selected encoder to the probe cache.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   */
  int probe_encoders(bool strict_configured_encoder = false, bool save_successful_cache = true);

  /**
   * @brief Get the name of the currently selected encoder.
   * @return Encoder name such as "nvenc", or an empty string if none is selected.
   */
  std::string active_encoder_name();

  /**
   * @brief Get the memory type used by the currently selected encoder.
   * @return The encoder device memory type, or unknown if no encoder is selected.
   */
  platf::mem_type_e active_encoder_mem_type();

  /**
   * @brief Return whether the active encoder benefits from a GPU-native capture path.
   * @return True for GPU-backed encoder paths such as CUDA and VAAPI.
   */
  bool active_encoder_requires_gpu_native_capture();

  /**
   * @brief Validate that the active encoder can start the requested codec/runtime path right now.
   * @details This is intended for per-session checks after topology/runtime changes such as cage startup.
   * @return True when the active encoder can open and validate the requested codec configuration.
   */
  bool active_encoder_runtime_supports_config(const config_t &config);

  /**
   * @brief Validate that the active encoder can capture at least one live GPU-native frame right now.
   * @details This is intended for runtime probes where a dummy encode session is not enough and we need
   *          to confirm that the current display path is actually delivering DMA-BUF resident frames.
   * @return True when a live frame arrives with DMA-BUF transport and GPU residency.
   */
  bool active_encoder_runtime_supports_live_gpu_capture(const config_t &config);

#ifdef POLARIS_TESTS
  struct encoder_probe_cache_snapshot_t {
    std::string encoder_name;
    codec_capability_state_t capability_state {};
    bool has_capability_data = false;
  };

  bool write_driver_version_cache_for_tests(
    const std::filesystem::path &cache_path,
    const std::filesystem::path &binary_path,
    std::string_view binary_mtime,
    std::string_view driver_version
  );

  std::string read_driver_version_cache_for_tests(
    const std::filesystem::path &cache_path,
    const std::filesystem::path &binary_path,
    std::string_view binary_mtime
  );

  bool write_encoder_probe_cache_for_tests(
    const std::filesystem::path &cache_path,
    std::string_view driver_version,
    std::string_view topology,
    std::string_view encoder_name,
    const codec_capability_state_t &capability_state
  );

  encoder_probe_cache_snapshot_t read_encoder_probe_cache_for_tests(
    const std::filesystem::path &cache_path,
    std::string_view current_driver,
    std::string_view current_topology
  );

  std::string current_encoder_topology_key_for_tests();

  std::chrono::milliseconds reset_display_retry_delay_for_tests(int attempt);

  int software_frame_input_linesize_for_tests(
    int row_pitch,
    int pixel_pitch,
    int image_width,
    int frame_width,
    int av_pixel_format
  );
#endif
}  // namespace video
