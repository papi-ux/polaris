/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

// local includes
#include "capture_generation.h"
#include "encoder_probe_reuse.h"
#include "launch_failure.h"
#include "pyrowave_availability.h"
#include <functional>
#include "input.h"
#include "nvenc/nvenc_config.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "stream_packet_owner.h"
#include "video_colorspace.h"
#include "video_rate.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;
namespace config { struct video_t; }
namespace stream_stats { struct capture_source_t; }

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

    int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1, 3 - PyroWave

    /* Encoding color depth (bit depth): 0 - 8-bit, 1 - 10-bit
       HDR encoding activates when color depth is higher than 8-bit and the display which is being captured is operating in HDR mode */
    int dynamicRange;

    int chromaSamplingType;  // 0 - 4:2:0, 1 - 4:4:4

    int enableIntraRefresh;  // 0 - disabled, 1 - enabled

    int encodingFramerate; // Requested display framerate
    bool input_only;
    capture_generation::identity_t capture_generation;
    // Appended fields preserve positional initializers used by existing clients.
    AVRational stream_rate {0, 1};  // RTSP stream request, before integer budget rounding
    AVRational encode_rate {0, 1};  // Host limiter: launch rate when enabled, stream rate otherwise
    std::uint64_t session_generation = 0;  // Host-owned identity for source-frame diagnostics
    // What this session's capture request is measured against, taken as the session starts.
    capture_generation::request_context_t capture_request;

  };

  inline AVRational framerate_to_rational(const config_t &config) {
    return rate::valid(config.stream_rate) ? config.stream_rate : rate::fraction(config.framerate, 1);
  }

  // Apply ANNOUNCE's stream request independently of the admitted launch rate.
  // Keep the legacy integer bitrate budget and Warp fields alongside exact rates.
  inline bool configure_announced_rates(config_t &config, int max_fps, int refresh_x100,
                                       int launch_fps_millihertz, bool limit_framerate) {
    const auto stream_rate = rate::from_wire(max_fps, refresh_x100);
    if (!rate::valid(stream_rate) || launch_fps_millihertz <= 0) return false;
    config.stream_rate = stream_rate;
    config.encode_rate = limit_framerate ? rate::from_millihertz(launch_fps_millihertz) : stream_rate;
    config.encodingFramerate = limit_framerate ? launch_fps_millihertz :
      (max_fps > 1000 ? max_fps : max_fps * 1000);
    config.framerate = max_fps > 4000 ? static_cast<int>(std::round(static_cast<float>(max_fps) / 1000)) : max_fps;
    return true;
  }

  inline AVRational encoding_framerate_to_rational(const config_t &config) {
    if (rate::valid(config.encode_rate)) return config.encode_rate;
    return config.encodingFramerate > 0 ? rate::from_millihertz(config.encodingFramerate) : framerate_to_rational(config);
  }

  inline std::chrono::nanoseconds capture_frame_interval(const config_t &config) {
    return rate::interval(framerate_to_rational(config));
  }

  inline std::chrono::nanoseconds encoding_frame_interval(const config_t &config) {
    return rate::interval(encoding_framerate_to_rational(config));
  }

  platf::mem_type_e map_base_dev_type(AVHWDeviceType type);

  /**
   * @brief Why interactive capture setup could not be completed, when it could not.
   *
   * Two very different things stop a Mirror Desktop launch here, and only one of them is about the
   * screen sharing prompt. An explicit encoder choice is strict and never falls back, so a host
   * whose encoder failed its probe has none selected at all, and answering that with "approve the
   * prompt" sends someone to a dialog that was never shown.
   */
  enum class capture_preparation_e {
    ready,  ///< nothing to prepare, or the prompt was answered
    no_encoder,  ///< no encoder is selected, so there is nothing to prepare capture for
    not_prepared,  ///< the backend could not prepare capture; the prompt is the usual reason
  };

  // Complete interactive Mirror Desktop capture setup before RTSP admission.
  capture_preparation_e prepare_capture_for_launch(const config_t &config, std::shared_ptr<void> &preparation);
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

  /**
   * @brief The bitStreamFormat a client asks for to get the compute codec.
   *
   * Three, after H.264, HEVC and AV1. Defined here rather than in moonlight-common-c because
   * Polaris only ever writes these numbers: the client sends a string, Polaris parses it to an int,
   * and nothing in the streaming library needs to know the name of a codec it will never decode.
   */
  inline constexpr int VIDEO_FORMAT_PYROWAVE = 3;

  /**
   * @brief The bit that says this host can encode it, in ServerCodecModeSupport.
   *
   * Above every bit Sunshine's extensions already claim, so it cannot be mistaken for one. A
   * Moonlight client reads the mask, finds a bit it has no name for, and ignores it, which is the
   * whole of the compatibility story: it can never ask for a codec it does not know exists.
   */
  inline constexpr std::uint32_t SCM_PYROWAVE = 0x00800000;

  /**
   * @brief This host can carry PyroWave with a chroma sample per pixel rather than per four.
   *
   * Its own bit rather than something inferred from the first, because a client whose decoder is
   * built for 4:4:4 against a host that only sends 4:2:0 refuses every frame: the chroma travels in
   * each frame's sequence header and a decoder made for the other one will not take it. Better to
   * be told than to find out a frame at a time.
   */
  inline constexpr std::uint32_t SCM_PYROWAVE_444 = 0x01000000;

  /**
   * @brief This host can carry PyroWave as HDR10, full range BT.2020 with the PQ transfer function.
   *
   * Its own bit because the dynamic range is asked for at launch, over HTTP, before any of the RTSP
   * negotiation happens: by the time a client could read the SDP it has already committed. So this is
   * the only place a client can learn it before it has to decide.
   *
   * Narrower than SCM_PYROWAVE, and deliberately: HDR exists only on the path that hands the codec a
   * picture on the GPU, so a host that has the codec does not necessarily have this.
   */
  inline constexpr std::uint32_t SCM_PYROWAVE_HDR10 = 0x02000000;

  /**
   * @brief Why this host will not stream PyroWave to a client that asked for this, or nothing.
   *
   * Everything about a PyroWave request that can be judged without touching a display, in one place
   * and with no side effects, because the alternative is four conditions spread through an RTSP
   * handler that only a live client can reach. What cannot be judged here is left to the capture
   * path, which fails closed.
   *
   * @param config What the client asked for in its ANNOUNCE.
   * @param can_encode Whether this host has a device that can run the codec at all.
   * @param can_hdr Whether it can carry HDR10, which is the narrower question: that needs the path
   *   that hands the codec a picture on the GPU.
   * @return A sentence naming what is wrong, ready to log, or nothing when the request is servable.
   */
  std::optional<std::string> pyrowave_announce_refusal(const config_t &config, bool can_encode,
                                                       bool can_hdr);

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

  /**
   * @brief PyroWave's formats, which are almost none of them.
   *
   * The codec takes packed pixels and decides its own chroma at encoder creation, so there is no
   * eight bit versus ten bit choice to advertise. The device type is not a formality: it is what the
   * capture backends read to decide what to offer this session, and this codec owns a Vulkan device
   * that can import a dmabuf, which is a different answer from both system memory and from the
   * Vulkan device FFmpeg builds.
   */
  struct encoder_platform_formats_pyrowave: encoder_platform_formats_t {
    encoder_platform_formats_pyrowave() {
      encoder_platform_formats_t::dev_type = platf::mem_type_e::vulkan_pyrowave;
      encoder_platform_formats_t::pix_fmt_8bit = platf::pix_fmt_e::yuv420p;
      encoder_platform_formats_t::pix_fmt_10bit = platf::pix_fmt_e::yuv420p;
      // 4:4:4 is a create time choice inside the codec, not a pixel format Polaris hands it, and
      // this path only ever converts to 4:2:0 today. Naming the 4:2:0 format in all four slots
      // keeps the honest answer in one place rather than advertising a format nothing produces.
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = platf::pix_fmt_e::yuv420p;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = platf::pix_fmt_e::yuv420p;
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
        case VIDEO_FORMAT_PYROWAVE:
          // PyroWave, which has no profiles, so the encoder that carries it holds the same codec in
          // all three slots and any of them is the right answer. Only that encoder is ever asked:
          // ANNOUNCE refuses the format on a host that cannot run it, so this is a deliberate answer
          // rather than the guess the default arm makes. Without it the guess was reached, and every
          // session logged an unknown format and called itself H.264.
          return h264;
      }
    }

    uint32_t flags;
  };

  /**
   * @brief Maps the configured Vulkan rate-control mode to FFmpeg's option value.
   * @details Config value 0 means auto: FFmpeg's Vulkan auto sentinel is
   *          FF_VK_RC_MODE_AUTO (0xFFFFFFFF), which does not fit in an int
   *          option, so it is passed as the named "auto" constant instead of a
   *          raw zero that would select the driver's default rate control. Other
   *          values are VkVideoEncodeRateControlModeFlagBitsKHR and pass through
   *          unchanged.
   */
  std::string vulkan_rc_mode_option(int rc_mode);

  /**
   * @brief Clamp a configured Vulkan quality level to the driver-reported range.
   * @details Valid levels run 0..max_quality_levels-1. FFmpeg's own guard has an
   *          off-by-one that lets a level equal to the reported count through, so
   *          Polaris clamps on its side and logs when it does. A negative value is
   *          floored at 0 (level 0 always works). When no live probe has reported
   *          a count (-1), non-negative values pass through unchanged for FFmpeg to
   *          validate at session open.
   */
  int vulkan_quality_clamp(int configured, int max_quality_levels);

  /**
   * @brief Highest Vulkan quality level offered across the codecs an encoder can use.
   * @param levels maxQualityLevels per codec, indexed H.264, HEVC, AV1; -1 when unknown.
   * @param include_av1 Whether the encoder uses AV1. The Vulkan encoder keeps AV1
   *        fail-closed, so its AV1 count must not lower the level offered for H.264 and HEVC.
   * @return The smallest count-1 among the included codecs with a known count, or -1.
   */
  int vulkan_quality_max(const std::array<int, 3> &levels, bool include_av1);

  bool wait_for_capture_display_release(
    const std::shared_ptr<platf::display_t> &display,
    const std::function<bool()> &running,
    const std::function<void()> &drain_images
  );

  /**
   * @brief What convert() returns when nothing about this session will ever make the next frame work.
   *
   * Any non-zero answer from convert() fails that frame, and the capture thread treats failing a
   * frame as a reason to build the session again. That is right for a frame that arrived wrong and
   * wrong for a session that cannot read what capture produces: the new session is identical to the
   * old one, so it fails the same way, at whatever rate frames arrive. Measured at a thousand
   * sessions in two minutes, all of them logging the same sentence.
   *
   * A session that answers with this is saying the stream is over. The caller stops rather than
   * starting another, and the client is told: the stream ends with the frame conversion termination
   * code, which a Moonlight client shows as a fatal video encoding error and does not reconnect
   * into. A bare disconnect is what it reads as a dropped connection, and it reconnected into the
   * same refusal. That is the difference between an error someone can act on and a log nobody can
   * read.
   */
  constexpr int convert_session_is_over = -2;

  struct encode_session_t {
    enum class bitrate_update_e {
      rejected,
      applied,
      recreate_session
    };

    virtual ~encode_session_t() = default;

    // Base members are destroyed after derived codec and converter resources.
    std::shared_ptr<platf::display_t> capture_display_owner;

    /**
     * @return 0 when the frame was converted, convert_session_is_over when this session can never
     *         convert another, and any other non-zero value to fail this frame alone.
     */
    virtual int convert(frame_t &frame) = 0;

    virtual void request_idr_frame() = 0;

    virtual void request_normal_frame() = 0;

    virtual void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;

    /**
     * @brief Whether this encoder session can apply bitrate changes at runtime.
     */
    virtual bool supports_runtime_bitrate_update() const {
      return false;
    }

    /**
     * @brief Dynamically update the encoder bitrate at runtime.
     * @param new_bitrate_kbps New target bitrate in kilobits per second.
     * @return Whether the encoder rejected the request, applied it immediately,
     *         or requires an encoder-session recreation before confirmation.
     */
    virtual bitrate_update_e update_bitrate(int new_bitrate_kbps) {
      return bitrate_update_e::rejected;
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

#ifdef POLARIS_BUILD_VULKAN
  extern encoder_t vulkan;
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
      std::string old;
      std::string _new;

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      replace_t(std::string_view old, std::string_view _new):
          old {std::move(old)},
          _new {std::move(_new)} {
      }
    };

    // Packets can remain queued after their encoder session has been retired.
    std::shared_ptr<const std::vector<replace_t>> replacements;
    stream_packets::destination_t channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

    // T1 (encode-done): stamped by encode_avcodec()/encode_nvenc() right after
    // the real encode work finishes, before the packet is queued for send.
    // Travels with the packet to the send thread the same way frame_timestamp
    // (T0) already does, so the send thread can derive all three P0-3 stages
    // (T0->T1, T1->T2, T0->T2) from one thread without cross-thread
    // correlation.
    std::optional<std::chrono::steady_clock::time_point> encode_done_timestamp;
  };

  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    // Copy encoded bytes and side data before releasing any driver-owned
    // buffer/opaque reference. Call only from the encoder's owning thread.
    bool detach_encoder_buffer() {
      AVPacket *detached = av_packet_alloc();
      if (!detached) return false;
      if (av_new_packet(detached, av_packet->size) < 0 ||
          av_packet_copy_props(detached, av_packet) < 0) {
        av_packet_free(&detached);
        return false;
      }
      if (av_packet->size > 0) std::memcpy(detached->data, av_packet->data, av_packet->size);
      // Opaque references are never used by packet consumers and may own
      // hardware state. The remaining metadata is independently allocated.
      detached->opaque = nullptr;
      av_buffer_unref(&detached->opaque_ref);
      av_packet_free(&av_packet);
      av_packet = detached;
      return true;
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
  using packet_queue_t = safe::mail_raw_t::queue_t<packet_t>;

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

  /**
   * @brief Highest Vulkan quality level the probed driver exposes for every usable codec.
   * @details Returns maxQualityLevels-1, taken over the codecs the encoder uses whose count
   *          a live probe reported (-1 entries are skipped, and AV1 is skipped while the
   *          encoder keeps it off), or -1 when no encoder is selected, the active encoder is
   *          not Vulkan, or probing has not reported any count yet.
   */
  int advertised_vulkan_quality_max();

  void reset_encoder_probe_state();

  /**
   * @brief What refresh_advertised_codecs_for_auto_plan() found, and what it did about it.
   */
  enum class auto_plan_refresh_e {
    current,  ///< The advertised codecs came from a probe under the plan the host has now, or none did.
    reprobed,  ///< The plan had changed since that probe, and a probe under the new plan passed.
    deferred,  ///< The plan had changed, but a stream or an app is running, so nothing was probed.
    left_to_cage_probe,  ///< The plan moved to a private compositor route, which only the deferred cage
                         ///< probe can probe, so the encoder another plan probed was dropped for it.
    failed,  ///< The plan had changed and the probe under it failed. The encoder from before stays, and
             ///< this change is not probed again until a probe replaces that encoder or the plan returns.
  };

  /**
   * @brief Probe again when the host's Auto plan is no longer the plan its advertised codecs came from.
   *
   * serverinfo, the app lists and the launch profile resolver all advertise the codecs of the last
   * probe. On AMD, Auto tries Vulkan Video first on Gamescope Stream and VA-API on the routes around
   * it, and the two do not offer the same codecs: Vulkan Video there has no AV1 and no HDR. The route
   * can change with no launch to probe it. Steam Game Mode's hold swaps Gamescope Stream for Mirror
   * Desktop and hands it back, a client saves another host default mode, and a launch that switched
   * the mode for itself restores the default at teardown. A host left alone kept advertising the other
   * encoder's codecs, and the next launch probed the encoder its plan names and then refused what had
   * been advertised: an AV1 client at ANNOUNCE, an HDR launch with a 503.
   *
   * The plan is compared by its policy together with whether Vulkan Video offers no HDR under it
   * (encoder_selection_info_t::vulkan_withholds_hdr). Auto's policy names the route, the driver and
   * the codec settings that chose the encoder, and decides both. An explicit encoder keeps the policy
   * "explicit" on every route, so for encoder = vulkan only the flag shows a move into or out of
   * Gamescope Stream, and that host probes again as well. A private compositor route is never
   * probed here, because only the deferred cage probe can start labwc to probe it. An encoder another
   * plan probed is dropped instead, so serverinfo reads that probe's cache, and the probe runs when there is none: Steam
   * Game Mode's hold on a Private Stream host is refreshed onto VA-API, whose AV1 the next Private
   * Stream launch, on Vulkan Video, would refuse.
   *
   * @param stream_active Whether a stream or an app is running. The encoder a running stream uses
   *        stays, and a request made after it ends probes.
   */
  auto_plan_refresh_e refresh_advertised_codecs_for_auto_plan(bool stream_active);

  /**
   * @brief Retire a successful probe so the next launch cannot reuse it without probing.
   * @details Keeps the chosen encoder. /serverinfo advertises codecs from it until a launch
   *          probes again, and reset_encoder_probe_state() would advertise H.264 alone meanwhile.
   */
  void invalidate_encoder_probe_reuse();

  /**
   * @brief Encoder backends this binary can accept as an explicit selection.
   * @details "auto" is always first. The remaining names come from the
   *          platform/build encoder registry and still require a live probe
   *          when a stream launches.
   */
  std::vector<std::string> selectable_encoder_backends();

  /**
   * @brief Whether a normalized external backend id is selectable by this build.
   */
  bool encoder_backend_selectable(std::string_view backend);

  void capture(
    safe::mail_t mail,
    config_t config,
    stream_packets::destination_t channel_data
  );

  void capture(
    safe::mail_t mail,
    config_t config,
    stream_packets::destination_t channel_data,
    packet_queue_t packets
  );

  /**
   * @brief Whether the current display construction is part of encoder probing.
   * @details Portal capture uses this to avoid opening the picker during probes.
   */
  bool encoder_probe_active();

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
   * @brief Explain how the active encoder was selected.
   * @details This is deterministic launch-policy evidence for diagnostics and
   *          Doctor. Doctor may explain it but does not mutate the selection.
   */
  struct encoder_selection_info_t {
    std::string mode;
    std::string gpu_driver;
    std::string policy;
    std::string preferred_encoder;
    std::string fallback_encoder;
    std::string selected_encoder;
    std::string driver_version;
    std::string reason;
    bool exact_live_probe_required = false;
    bool fallback_used = false;
    /// Whether Vulkan Video, when it is the encoder, offers no HDR on this route: under Auto's
    /// policy (linux_encoder_auto_policy::vulkan_offers_no_hdr()), or for an explicit
    /// encoder = vulkan (linux_encoder_auto_policy::explicit_vulkan_offers_no_hdr()).
    bool vulkan_withholds_hdr = false;
  };

  encoder_selection_info_t active_encoder_selection_info();

  /**
   * @brief The sentence the host logs when a client asks for AV1 at ANNOUNCE and AV1 is off.
   * @param configured_av1_mode av1_mode as polaris.conf holds it.
   * @param selection The active encoder selection.
   * @details A client picks its codec from what the host offered before the launch, and a launch that
   *          switches into Gamescope Stream on AMD moves Auto to Vulkan Video, which carries no AV1.
   *          That stream is refused as it starts with no reason the client can show, so the host log
   *          says what took AV1 away, the setting or the encoder, and on that route how to keep it.
   */
  std::string av1_announce_refusal(int configured_av1_mode, const encoder_selection_info_t &selection);

  /**
   * @brief The extra sentence shown when a preferred NVENC encoder did not start.
   *
   * Points at the libav error that names the nvenc API version the linked FFmpeg
   * required, which is the one fact that separates "this driver is too old" from
   * every other reason an encoder can fail to open. Empty unless NVENC was asked
   * for, did not land, and a driver version is known.
   */
  std::string nvenc_fallback_detail(
    std::string_view preferred_encoder,
    std::string_view selected_encoder,
    std::string_view driver_version
  );

  /**
   * @brief Record a launch refusal for an encoder probe that just failed.
   * @param against_private_compositor True when the probe ran against Polaris' own compositor.
   * @details Names the capture-side cause when there is one (no capture backend, KMS without the
   *          capability), otherwise the encoder side with the NVENC driver detail when it applies.
   */
  void note_launch_refused_by_probe(bool against_private_compositor);

  /**
   * @brief Whether the selected encoder offers no HDR on this route.
   *
   * Vulkan Video on Gamescope Stream reads each frame through system memory as 8-bit BGRA, so the
   * probe clears its dynamic range, under Auto on AMD (linux_encoder_auto_policy::vulkan_offers_no_hdr())
   * and for an explicit encoder = vulkan unless HEVC Support asks for HDR
   * (linux_encoder_auto_policy::explicit_vulkan_offers_no_hdr()), and every HDR session it is asked
   * to build is refused.
   */
  bool active_encoder_withholds_hdr();

  /**
   * @brief Whether active_encoder_withholds_hdr() holds because polaris.conf sets encoder = vulkan,
   *        rather than because Auto chose Vulkan Video.
   */
  bool active_encoder_withholds_hdr_for_explicit_vulkan();

  /**
   * @brief Record why a launch that asks for HDR is refused by an encoder that passed its probe and
   *        offers no HDR: encoder_offers_no_hdr, naming the encoder, and on Gamescope Stream the
   *        settings that keep VA-API there.
   */
  void note_launch_refused_for_hdr(bool encoder_chosen_for_launch = false);

  /**
   * @brief Refuse a launch whose capture request cannot land on any capture source.
   * @param generation The capture generation the launch is about to install.
   * @return True when the launch was refused; the reason is recorded as capture_backend_unavailable.
   * @details Without this the launch succeeds, the video thread finds no backend, and the client
   *          sees the connection drop with a bare "-1" (#739).
   */
  bool refuse_launch_if_capture_unavailable(const capture_generation::identity_t &generation);

  /**
   * @brief What a generation's capture route hands PyroWave, judged without opening a display.
   * @details The backend dispatch would open for PyroWave's memory type, and for KMS the format of
   *          the framebuffer on the plane it would read, asked of the card. Unknown before the host
   *          has evaluated its capture sources, and on a build without PyroWave.
   */
  pyrowave_availability::route_e pyrowave_capture_route(const capture_generation::identity_t &generation);

  /**
   * @brief Why capabilities leaves PyroWave out of capture.codecs on this host, or nothing.
   * @details Judged for the route a launch that names no stream mode takes, and only when no mode a
   *          client can pick for one launch runs its own compositor; see
   *          pyrowave_availability::unavailable.
   */
  std::optional<pyrowave_availability::unavailable_t> pyrowave_unavailable();

  /**
   * @brief The refusal for a PyroWave stream on this generation's capture route, or nothing.
   * @details Records nothing. A launch hands the result to launch_failure::refuse; the RTSP
   *          handshake, which has no record to carry it to the client, logs it.
   */
  std::optional<launch_failure::record_t> pyrowave_capture_refusal(const capture_generation::identity_t &generation);

  /**
   * @brief pyrowave_capture_refusal for the generation the next stream will capture: the one the
   *        running launch installed, or the live configuration's when none has.
   */
  std::optional<launch_failure::record_t> pyrowave_session_capture_refusal();

  /**
   * @brief pyrowave_capture_refusal for a launch into the host's own stream mode, the one that
   *        names no mode.
   * @details Capabilities offers PyroWave while any mode a client can pick would stream it, so this is
   *          how the console says which launches are left out. Records nothing and opens no display.
   */
  std::optional<launch_failure::record_t> pyrowave_host_mode_refusal();

  /**
   * @brief Whether this host can carry PyroWave in HDR10, which needs the GPU input path.
   * @details False on a build without PyroWave, so callers need no build guard of their own.
   */
  bool pyrowave_hdr_available();

  /**
   * @brief Every encoder the probe can choose in this build, in its search order, and whether its
   *        table lets H.264, HEVC and AV1 carry 4:4:4.
   * @details A property of the build, not of this host's GPU: the probe still has to pass a codec in
   *          4:4:4 before a client is offered it. PyroWave is not among them, because the probe never
   *          chooses it; it carries 4:4:4 whenever the host can run it.
   */
  std::vector<std::pair<std::string_view, std::array<bool, 3>>> yuv444_encoders();

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
   * @brief Whether Linux Auto currently plans a GPU-native encoder route.
   * @details This remains useful before deferred private-compositor probing has
   *          selected an encoder, allowing the session manager to run the exact
   *          first-frame probe that makes the automatic choice safe.
   */
  bool automatic_encoder_prefers_gpu_native_capture();

  /**
   * @brief Whether a failed HEVC or AV1 10-bit probe speaks for the live capture path.
   * @details The portal does not connect to its PipeWire source while probing and
   *          encodes an NV12 dummy instead, so on a Gamescope Stream host that
   *          captures through the portal a failed 10-bit probe says nothing about
   *          the 10-bit DMA-BUF the live session negotiates. The capture setting is
   *          read the way dispatch reads it, so kwin counts as the portal it opens.
   */
  bool main10_probe_is_authoritative(std::string_view capture, std::string_view stream_mode);

  /**
   * @brief Validate that the active encoder can start the requested codec/runtime path right now.
   * @details This is intended for per-session checks after topology/runtime changes such as cage startup.
   * @return True when the active encoder can open and validate the requested codec configuration.
   */
  bool active_encoder_runtime_supports_config(const config_t &config);

  /**
   * @brief Validate that the active encoder can capture and convert at least one live GPU-native frame right now.
   * @details This is intended for runtime probes where a dummy encode session is not enough and we need
   *          to confirm that the current display path is actually delivering DMA-BUF resident frames
   *          that survive the active encoder conversion path.
   * @return True when a live frame arrives with DMA-BUF transport/GPU residency and converts successfully.
   */
  bool active_encoder_runtime_supports_live_gpu_capture(const config_t &config);

#ifdef POLARIS_TESTS
  void with_capture_preparation_for_tests(
    const std::function<capture_preparation_e(const config_t &, std::shared_ptr<void> &)> &prepare,
    const std::function<void()> &body
  );
  /** Own the supplied codec/converter through real frame submission and teardown. */
  std::vector<int> encode_and_destroy_avcodec_session_for_tests(
    avcodec_ctx_t context,
    std::unique_ptr<platf::avcodec_encode_device_t> device,
    std::size_t frame_count
  );

  /** One frame through the frame converter an avcodec session puts in front of its device. */
  int convert_with_encode_device_for_tests(std::unique_ptr<platf::avcodec_encode_device_t> device, frame_t &frame);

  /** What the encode loop raises on a session's mail to end a stream its encoder can never serve. */
  void end_stream_encoder_cannot_serve_for_tests(const safe::mail_t &mail);

  /**
   * The loop capture_async() runs for a session on the parallel encode path, which builds an encode
   * session on the display capture published and builds it again each time one returns, run over
   * @p display with the session's own mail. It returns when the loop does, which is when the loop
   * sees the stream's shutdown.
   */
  void encode_published_display_for_tests(const safe::mail_t &mail, config_t config,
                                          const std::shared_ptr<platf::display_t> &display,
                                          const stream_packets::destination_t &channel_data);

  int hevc_profile_for_input_for_tests(int bit_depth, int chroma_sampling_type);

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

  std::string parse_nvidia_driver_version_for_tests(std::string_view reported);

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

  /**
   * @brief Finalize a planned encoder selection against the encoder a probe chose, as the probe does.
   */
  void finalize_encoder_selection_info_for_tests(
    encoder_selection_info_t &info,
    std::string_view selected_encoder
  );

  int probe_encoders_with_hooks_for_tests(
    const probe_reuse::identity_t &identity,
    const std::function<bool(encoder_t &, bool)> &validate
  );

  /**
   * @brief Probe as probe_encoders_with_hooks_for_tests() does, with Auto planning for a GPU driven
   *        by gpu_driver instead of the selected render node's own driver.
   */
  int probe_encoders_with_hooks_for_tests(
    const probe_reuse::identity_t &identity,
    const std::function<bool(encoder_t &, bool)> &validate,
    std::string_view gpu_driver
  );

  /**
   * @brief The encoder selection Auto plans from the host's configuration now, for a GPU driven by
   *        gpu_driver instead of the selected render node's own driver.
   */
  encoder_selection_info_t planned_encoder_selection_info_for_tests(std::string_view gpu_driver);

  /**
   * @brief refresh_advertised_codecs_for_auto_plan() with the hooks
   *        probe_encoders_with_hooks_for_tests() takes, planning for gpu_driver. It never writes the
   *        encoder cache.
   */
  auto_plan_refresh_e refresh_advertised_codecs_for_auto_plan_with_hooks_for_tests(
    const probe_reuse::identity_t &identity,
    const std::function<bool(encoder_t &, bool)> &validate,
    std::string_view gpu_driver,
    bool stream_active
  );
  std::string encoder_probe_settings_for_tests(const config::video_t &settings);
  std::string current_encoder_topology_key_for_tests();

  std::chrono::milliseconds reset_display_retry_delay_for_tests(int attempt);

  bool capture_fallback_allowed_for_tests(std::string_view requested_display_name);

  bool display_switch_allowed_for_exact_capture_for_tests(std::string_view exact_display_name);

  bool capture_generations_match_for_tests(
    const capture_generation::identity_t &active,
    const capture_generation::identity_t &incoming
  );

  /**
   * @brief One attempt by a consuming session to publish what its display opened, as its encode
   *        loop makes it. published carries across attempts, as the loop keeps it.
   * @return Whether a write matched by the session's generation has landed.
   */
  bool publish_capture_backend_for_tests(const config_t &config, const platf::capture_route_t &route, bool &published);

  /// The same attempt with the frame record the loop keeps beside it, which a landing clears.
  bool publish_capture_backend_for_tests(
    const config_t &config,
    const platf::capture_route_t &route,
    bool &published,
    std::optional<stream_stats::capture_source_t> &reported_source
  );

  /// The same attempt as the parallel encode loop makes it, with the PyroWave route record it also
  /// keeps, which a landing clears too.
  bool publish_capture_backend_for_tests(
    const config_t &config,
    const platf::capture_route_t &route,
    bool &published,
    std::optional<stream_stats::capture_source_t> &reported_source,
    std::string &reported_pyrowave_route
  );

  /// What an encode loop records of a PyroWave session's route after a frame, with the record it keeps.
  void record_pyrowave_route_for_tests(const config_t &config, std::string_view route, std::string &reported);

  /**
   * Builds the encode session the host builds for a PyroWave stream, converts one frame from host
   * memory through it, and records its route the way the encode loop does, for config's generation.
   * @return What the loop kept, which is empty when nothing was written, or nullopt on a host with no
   *         Vulkan device PyroWave can use.
   */
  std::optional<std::string> pyrowave_route_of_a_host_frame_for_tests(const config_t &config);

  /// What an encode loop records for a frame its session accepted, with the record it keeps.
  void record_capture_source_for_tests(
    const config_t &config,
    const frame_t &frame,
    std::optional<stream_stats::capture_source_t> &reported_source
  );

  #ifdef __linux__
  /// What a session takes as it starts to measure its generation's capture request against.
  capture_generation::request_context_t capture_request_for_session_for_tests(const capture_generation::identity_t &generation);
  #endif

  std::optional<int> find_display_index_for_tests(
    const std::vector<std::string> &display_names,
    std::string_view requested_display_name
  );

  int refresh_display_selection_for_tests(std::vector<std::string> previous, int previous_index,
                                         std::string requested, const std::vector<std::string> &enumerated);

  std::optional<int> clamp_display_index_for_tests(int requested_index, std::size_t display_count);

  bool hdr_metadata_is_usable_for_tests(const SS_HDR_METADATA &metadata);

  int software_frame_input_linesize_for_tests(
    int row_pitch,
    int pixel_pitch,
    int image_width,
    int frame_width,
    int av_pixel_format
  );

  bool should_apply_nvenc_split_encode_mode_for_tests(
    std::string_view encoder_name,
    std::string_view codec_name
  );

  int nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode mode);

  std::optional<int> nvenc_split_encode_mode_option_value_for_tests(
    std::string_view encoder_name,
    std::string_view codec_name,
    nvenc::nvenc_split_encode_mode mode
  );

  enum class nvenc_split_encode_mode_decision_e {
    unsupported_encoder_or_codec,
    disabled,
    missing_ffmpeg_option,
    apply
  };

  struct nvenc_split_encode_mode_decision_t {
    nvenc_split_encode_mode_decision_e decision;
    std::optional<int> ffmpeg_value;
    bool should_warn;
  };

  nvenc_split_encode_mode_decision_t nvenc_split_encode_mode_decision_for_tests(
    std::string_view encoder_name,
    std::string_view codec_name,
    nvenc::nvenc_split_encode_mode mode,
    bool has_private_option
  );
#endif
}  // namespace video
