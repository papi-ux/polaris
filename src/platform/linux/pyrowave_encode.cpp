/**
 * @file src/platform/linux/pyrowave_encode.cpp
 * @brief PyroWave encode support.
 */
// PyroWave's header refuses to compile unless the Vulkan API is already declared, and says so with
// an #error.
//
// The system header, not the volk copy PyroWave builds against. volk declares every entry point as
// a function pointer variable, Polaris's own Vulkan encoder uses the ordinary prototypes, and with
// link time optimisation the compiler sees both and refuses: "function redeclared as variable".
// The types are the same either way; only the linkage of the entry points differs, and Polaris does
// not call any of them from here.
#include <vulkan/vulkan.h>

#include "pyrowave.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/pyrowave_encode.h"
#include "src/platform/linux/pyrowave_upload.h"
#include "src/platform/linux/pyrowave_vulkan.h"

using namespace std::literals;

namespace pyrowave_encode {

  namespace {

    /**
     * The device, made once and kept.
     *
     * Polaris creates the Vulkan device and lends it, rather than letting PyroWave create one. The
     * codec's own device works for as long as the only thing handed to it is planes in system
     * memory: it reports its instance, physical device and device and nothing else, with no queue and
     * no family index, so there is nothing to record an upload or a barrier against. Owning it is
     * what any path that keeps the picture on the GPU needs, and it costs nothing on the path that
     * does not.
     *
     * Never destroyed, deliberately. It outlives every session by design, a static destructor would
     * run it after the threads that might still be encoding, and the process is about to end anyway.
     */
    vk_device_t *shared_vulkan() {
      static std::once_flag once;
      static vk_device_t *owned = nullptr;
      std::call_once(once, [] {
        auto device = std::make_unique<vk_device_t>();
        if (device->create()) {
          owned = device.release();
        }
      });
      return owned;
    }

    pyrowave_device shared_device() {
      auto *owned = shared_vulkan();
      return owned ? owned->codec : nullptr;
    }

    /**
     * Whether this host will hand the codec pictures on the GPU at all.
     *
     * On unless something says otherwise, in the shape Polaris already uses for the portal's dmabuf
     * override. A driver that mishandles an upload should be recoverable without a rebuild, and
     * having both paths reachable at will is what lets a test put the same picture through each and
     * compare what comes out.
     */
    bool gpu_input_allowed() {
      const char *setting = std::getenv("POLARIS_PYROWAVE_GPU_INPUT");
      if (!setting || !*setting) {
        return true;
      }
      const std::string_view value {setting};
      if (value == "0" || value == "off" || value == "no" || value == "false") {
        BOOST_LOG(info) << "PyroWave: POLARIS_PYROWAVE_GPU_INPUT is off, so frames are converted "sv
                        << "on the CPU"sv;
        return false;
      }
      return true;
    }

    class pyrowave_session_t: public session_t {
    public:
      pyrowave_session_t(const vk_device_t &owner, pyrowave_encoder encoder, int width, int height,
                         chroma_e chroma, dynamic_range_e range):
          owner {&owner},
          gpu {gpu_input_allowed() ? gpu_e::unknown : gpu_e::no},
          encoder {encoder},
          width {width},
          height {height},
          chroma {chroma},
          shift {chroma_shift(chroma)},
          range {range} {}

      ~pyrowave_session_t() override {
        if (scaler) {
          sws_freeContext(scaler);
        }
        if (encoder) {
          pyrowave_encoder_destroy(encoder);
        }
      }

      bool encode_packed(const uint8_t *bgra, int src_width, int src_height, int stride,
                       std::size_t max_bytes) override {
        // Before the arguments are even looked at, so that every way out of here leaves nothing to
        // read. A caller that missed the return value would otherwise send the previous picture
        // again under a new frame number, which the decoder accepts: a freeze rather than an error.
        frame.clear();
        if (!bgra || src_width <= 0 || src_height <= 0) {
          return false;
        }

        // The one thing that has to hold for the reads below to stay inside the buffer capture gave
        // us. Checked against the geometry rather than against metadata, because a backend that
        // fills in neither pixel_pitch nor a format still has to hand over rows this long.
        if (stride < src_width * 4) {
          BOOST_LOG(error) << "PyroWave: a "sv << src_width << " pixel row cannot fit in "sv
                           << stride << " bytes"sv;
          return false;
        }

        // The picture as it is, straight to the GPU, whatever shape it arrives in.
        if (gpu != gpu_e::no) {
          if (encode_on_gpu(bgra, src_width, src_height, stride, max_bytes)) {
            return true;
          }
          if (gpu == gpu_e::no) {
            // Except for HDR, which the CPU path cannot carry: the codec's system memory entry point
            // takes eight bit planes and nothing else, so falling back would mean encoding an SDR
            // picture and calling it HDR. A session that cannot use the GPU path is over.
            if (range == dynamic_range_e::hdr10) {
              BOOST_LOG(error) << "PyroWave: an HDR stream cannot fall back to the CPU converter, "sv
                               << "which is eight bit"sv;
              return false;
            }
            BOOST_LOG(info) << "PyroWave: falling back to converting frames on the CPU"sv;
          } else {
            // The path works and this frame did not. Saying so beats quietly producing a picture
            // through the other path and leaving nobody to notice the first one broke.
            return false;
          }
        }

        if (!prepare_scaler(src_width, src_height)) {
          return false;
        }

        // Into the middle of the destination, leaving the bars as prepare_scaler painted them. The
        // offsets are even so that a chroma sample lands on a chroma sample; an odd one shifts the
        // colour half a pixel off the luma it belongs to.
        const uint8_t *src[4] = {bgra, nullptr, nullptr, nullptr};
        const int src_stride[4] = {stride, 0, 0, 0};
        uint8_t *dst[4] = {
          planes[0].data() + static_cast<std::size_t>(offset_y) * width + offset_x,
          planes[1].data() + static_cast<std::size_t>(offset_y >> shift) * (width >> shift) + (offset_x >> shift),
          planes[2].data() + static_cast<std::size_t>(offset_y >> shift) * (width >> shift) + (offset_x >> shift),
          nullptr};
        const int dst_stride[4] = {width, width >> shift, width >> shift, 0};

        const auto conversion_started = std::chrono::steady_clock::now();
        if (sws_scale(scaler, src, src_stride, 0, src_height, dst, dst_stride) != fit_height) {
          BOOST_LOG(warning) << "PyroWave: colour conversion did not fill the frame"sv;
          return false;
        }
        const auto encode_started = std::chrono::steady_clock::now();

        const auto encoded = encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
        report_timing(conversion_started, encode_started);
        return encoded;
      }

      /**
       * Make a converter for this source geometry, or keep the one already made.
       *
       * Kept between frames: a stream is thousands of identically shaped frames, and building the
       * scaler for each would dominate a codec that encodes in a tenth of a millisecond. Rebuilt
       * when the source changes size, which happens when the captured display does.
       */
      bool prepare_scaler(int src_width, int src_height) {
        if (scaler && src_width == source_width && src_height == source_height) {
          return true;
        }

        if (scaler) {
          sws_freeContext(scaler);
          scaler = nullptr;
        }

        // The same fit Polaris's software encode path uses, rounded to even here because this code
        // addresses the chroma planes itself rather than leaving the arithmetic to a pixel format
        // descriptor.
        const auto scale = std::min(static_cast<double>(width) / src_width,
                                    static_cast<double>(height) / src_height);
        fit_width = std::max(2, static_cast<int>(src_width * scale) & ~1);
        fit_height = std::max(2, static_cast<int>(src_height * scale) & ~1);
        offset_x = ((width - fit_width) / 2) & ~1;
        offset_y = ((height - fit_height) / 2) & ~1;

        // Built by hand rather than through sws_getContext, because that one initialises
        // immediately and there is no way to ask it for threads afterwards.
        //
        // Worth the extra lines: measured over three thousand frames at 1080p, this conversion cost
        // 3.9 ms a frame against 1.1 ms for the encode it feeds, so a full frame of BGRA turned into
        // planar YUV on one core was most of what this host spent. Polaris already runs its software
        // encode path's converter across min_threads and this had simply never been told to.
        //
        // Bilinear rather than nearest, because this scales a desktop down far more often than it
        // leaves it alone, and nearest turns small text into noise that a wavelet codec then spends
        // its whole bitrate on.
        scaler = sws_alloc_context();
        if (!scaler) {
          BOOST_LOG(error) << "PyroWave: could not allocate a colour converter"sv;
          return false;
        }

        const auto threads = std::max(1, config::video.min_threads);
        AVDictionary *options = nullptr;
        av_dict_set_int(&options, "srcw", src_width, 0);
        av_dict_set_int(&options, "srch", src_height, 0);
        av_dict_set_int(&options, "src_format", AV_PIX_FMT_BGRA, 0);
        av_dict_set_int(&options, "dstw", fit_width, 0);
        av_dict_set_int(&options, "dsth", fit_height, 0);
        av_dict_set_int(&options, "dst_format",
                        chroma == chroma_e::yuv420 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV444P, 0);
        av_dict_set_int(&options, "sws_flags", SWS_BILINEAR, 0);
        av_dict_set_int(&options, "threads", threads, 0);
        // The range as an option rather than only as a call, because sws_init_context builds its
        // conversion tables from these and would overwrite anything set beforehand. Both ends full,
        // since the source is full range RGB and the stream is full range YCbCr.
        av_dict_set_int(&options, "src_range", 1, 0);
        av_dict_set_int(&options, "dst_range", 1, 0);

        const auto applied = av_opt_set_dict(scaler, &options);
        av_dict_free(&options);
        if (applied < 0) {
          BOOST_LOG(error) << "PyroWave: this build's swscale will not take those options"sv;
          sws_freeContext(scaler);
          scaler = nullptr;
          return false;
        }

        if (sws_init_context(scaler, nullptr, nullptr) < 0) {
          BOOST_LOG(error) << "PyroWave: could not initialise a "sv << src_width << 'x' << src_height
                           << " to "sv << fit_width << 'x' << fit_height << " colour converter"sv;
          sws_freeContext(scaler);
          scaler = nullptr;
          return false;
        }

        // Said out loud, because the default is neither of the things anyone would assume. swscale
        // converts RGB to YUV as limited range BT.601 unless told otherwise, and a decoder reading
        // these frames as full range Rec. 709 shows raised blacks, flattened whites and the wrong
        // hue on anything saturated, while looking entirely plausible on a desktop. The bitstream
        // has fields for this and upstream writes none of them, so the only agreement available is
        // the one in profile_token, and this is the end of it that has to be true.
        //
        // After init, not before. Everything here is built through sws_alloc_context so that the
        // converter can be given threads, and sws_init_context computes its range tables from the
        // options it was handed: a call before it is one it overwrites. This was set before init and
        // silently did nothing, which is a mistake with no symptom on this side at all. What caught
        // it was decoding a frame in a test and looking at the colours.
        const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
        if (sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 1,
                                     0, 1 << 16, 1 << 16) < 0) {
          BOOST_LOG(error) << "PyroWave: this converter will not do full range Rec. 709"sv;
          sws_freeContext(scaler);
          scaler = nullptr;
          return false;
        }
        BOOST_LOG(info) << "PyroWave: converting on "sv << threads << " threads"sv;

        planes[0].assign(static_cast<std::size_t>(width) * height, 0);
        planes[1].assign(static_cast<std::size_t>(width >> shift) * (height >> shift), 128);
        planes[2].assign(static_cast<std::size_t>(width >> shift) * (height >> shift), 128);

        source_width = src_width;
        source_height = src_height;

        if (fit_width != width || fit_height != height) {
          BOOST_LOG(info) << "PyroWave: fitting "sv << src_width << 'x' << src_height << " into "sv
                          << width << 'x' << height << " as "sv << fit_width << 'x' << fit_height
                          << " at "sv << offset_x << ',' << offset_y;
        }
        return true;
      }

      /**
       * Where this frame sits in the image the codec reads, which is how the bars get drawn.
       *
       * The codec's scaler fills its output with its input, so whatever it scales it also stretches
       * and there is no letterbox in it. Padding what it is given is the letterbox: an image with the
       * stream's shape, the picture centred in it and black around the edges, which the scaler then
       * maps whole to whole. Costs the GPU some sampling over the bars and costs the host nothing,
       * because the copy is still only the picture and the bars are cleared once.
       */
      placement_t placement_for(int src_width, int src_height) const {
        placement_t where = {};
        const auto source_is_wider =
          static_cast<long long>(src_width) * height > static_cast<long long>(src_height) * width;

        if (source_is_wider) {
          where.image_width = src_width;
          where.image_height = static_cast<int>(static_cast<long long>(src_width) * height / width);
          where.offset_y = ((where.image_height - src_height) / 2) & ~1;
        } else {
          where.image_height = src_height;
          where.image_width = static_cast<int>(static_cast<long long>(src_height) * width / height);
          where.offset_x = ((where.image_width - src_width) / 2) & ~1;
        }

        // Rounding down above can leave the image a pixel short of the picture plus its offset, and
        // an image the picture does not fit in is refused rather than silently cropped.
        where.image_width = std::max(where.image_width, src_width + where.offset_x);
        where.image_height = std::max(where.image_height, src_height + where.offset_y);
        return where;
      }

      /**
       * Put the frame on the GPU and encode it there.
       *
       * The colour conversion goes with it. What was a full frame of BGRA turned into planar YUV on
       * the CPU becomes part of the same compute pass that runs the wavelet transform, and what the
       * host still does per frame is one copy into memory the GPU can read. The conversion it
       * replaces is the codec's own, full range with BT.709 coefficients and centred chroma, which is
       * the same thing profile_token promises and the same thing swscale was told to produce.
       */
      /**
       * How capture's bytes are laid out, which is the one thing the dynamic range changes on the way
       * in.
       *
       * Ten bit capture arrives packed into the same four bytes a pixel, two bits unused and ten each
       * for blue, green and red from the top of the word down, which is what DRM calls XBGR2101010
       * and Vulkan calls A2B10G10R10. That is the format Polaris's portal capture offers first and
       * the one KWin picks. A compositor that insisted on the other order would hand over red and
       * blue swapped, and there is no way to tell from here, which is another reason this codec
       * refuses to stream to a client that has not agreed a profile token.
       */
      VkFormat source_format() const {
        return range == dynamic_range_e::hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                               : VK_FORMAT_B8G8R8A8_UNORM;
      }

      bool encode_blank(std::size_t max_bytes) override {
        frame.clear();
        if (!encoder || max_bytes == 0 || gpu == gpu_e::no) {
          // Nothing on the GPU to make black, so the planes stand in: they are already zero for luma
          // and mid grey for chroma, which is the same picture.
          if (planes[0].empty()) {
            prepare_scaler(width, height);
          }
          if (planes[0].empty()) {
            return false;
          }
          std::fill(planes[0].begin(), planes[0].end(), 0);
          std::fill(planes[1].begin(), planes[1].end(), 128);
          std::fill(planes[2].begin(), planes[2].end(), 128);
          return encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
        }

        if (!staging) {
          staging = upload_t::make(*owner);
          if (!staging) {
            gpu = gpu_e::no;
            return false;
          }
        }
        if (!staging->begin_blank(width, height, source_format())) {
          return false;
        }
        if (!encode_recorded(max_bytes)) {
          return false;
        }
        if (gpu == gpu_e::unknown) {
          gpu = gpu_e::yes;
        }
        return true;
      }

      bool encode_imported(const dmabuf_t &buffer, std::size_t max_bytes) override {
        frame.clear();
        if (!encoder || max_bytes == 0 || gpu == gpu_e::no) {
          BOOST_LOG(error) << "PyroWave: a frame arrived on the GPU and this session cannot take it"sv;
          return false;
        }
        if (!staging) {
          staging = upload_t::make(*owner);
          if (!staging) {
            BOOST_LOG(error) << "PyroWave: could not make the path a captured frame arrives through"sv;
            gpu = gpu_e::no;
            return false;
          }
        }

        const auto where = placement_for(buffer.width, buffer.height);
        const auto started = std::chrono::steady_clock::now();
        if (!staging->begin_imported(buffer, where, range == dynamic_range_e::hdr10)) {
          return false;
        }
        if (!encode_recorded(max_bytes)) {
          return false;
        }

        if (gpu == gpu_e::unknown) {
          gpu = gpu_e::yes;
          BOOST_LOG(info) << "PyroWave: encoding a "sv << buffer.width << 'x' << buffer.height
                          << " frame where capture left it, on "sv << owner->gpu_name
                          << (where.has_bars() ? ", letterboxed"sv : ""sv);
        }
        // No copy to time, so the whole frame is the encode.
        report_timing(started, started);
        return true;
      }

      bool encode_on_gpu(const uint8_t *pixels, int src_width, int src_height, int stride,
                         std::size_t max_bytes) {
        if (!staging) {
          staging = upload_t::make(*owner);
          if (!staging) {
            gpu = gpu_e::no;
            return false;
          }
        }

        const auto where = placement_for(src_width, src_height);
        const auto copy_started = std::chrono::steady_clock::now();
        if (!staging->begin(pixels, src_width, src_height, stride, source_format(), where)) {
          // Nothing was recorded, so there is a working path left to take on the first frame and
          // nothing to unwind on a later one.
          if (gpu == gpu_e::unknown) {
            gpu = gpu_e::no;
          }
          return false;
        }
        const auto encode_started = std::chrono::steady_clock::now();

        if (!encode_recorded(max_bytes)) {
          if (gpu == gpu_e::unknown) {
            gpu = gpu_e::no;
          }
          return false;
        }

        if (gpu == gpu_e::unknown) {
          gpu = gpu_e::yes;
          BOOST_LOG(info) << "PyroWave: encoding straight from a "sv << src_width << 'x' << src_height
                          << " picture on "sv << owner->gpu_name;
          if (where.has_bars()) {
            BOOST_LOG(info) << "PyroWave: letterboxed into "sv << where.image_width << 'x'
                            << where.image_height << " at "sv << where.offset_x << ','
                            << where.offset_y;
          }
        }
        report_timing(copy_started, encode_started);
        return true;
      }

      /**
       * Add the encode to the command buffer the staging path left open, then run the lot.
       *
       * One submission and one fence for the copy, the colour conversion, the wavelet transform and
       * the entropy coding. That is what the codec asks for when the command buffer belongs to the
       * caller, and it is why nothing here waits twice.
       */
      bool encode_recorded(std::size_t max_bytes) {
        const bool hdr = range == dynamic_range_e::hdr10;

        pyrowave_scaled_encode_info info = {};
        info.view = staging->view();
        // The same space at both ends, which asks for the scale to happen in linear light and the
        // colour matrix in the space the samples arrived in, and skips a conversion between primaries
        // that nothing here needs. HDR10 brings BT.2020 primaries and the PQ transfer function with
        // it; SDR is sRGB, which for the matrix means Rec. 709.
        info.input_color_space = hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.output_color_space = info.input_color_space;
        // Sixteen bits an intermediate sample for HDR, because PQ spends most of its range on the
        // dark end and eight bits of it band visibly. Eight, dithered, for an eight bit stream.
        info.intermediate_plane_format = hdr ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        info.ycbcr_chroma_midpoint = 0.5f;

        budget = max_bytes;
        const pyrowave_rate_control rate_control = {max_bytes};

        // Held until the frame is gathered, because the command buffer below is device state rather
        // than session state and packetizing reads what its submission wrote. Not the queue lock:
        // that one is a leaf the codec takes for itself from inside Granite, and holding it across a
        // codec call is how two sessions deadlock. pyrowave_vulkan.h has the order.
        const std::lock_guard<std::mutex> lock {owner->command_buffer_lock};

        pyrowave_device_set_command_buffer(owner->codec, staging->command_buffer());
        const auto result = pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, nullptr, nullptr,
                                                                          &info, &rate_control);
        // Cleared whatever happened. Leaving a command buffer set on the device would have the next
        // call record into one that is closed, or freed.
        pyrowave_device_set_command_buffer(owner->codec, VK_NULL_HANDLE);

        if (result != PYROWAVE_SUCCESS) {
          BOOST_LOG(warning) << "PyroWave: encode on the GPU failed (result "sv
                             << static_cast<int>(result) << ')';
          staging->abandon();
          return false;
        }
        if (!staging->end()) {
          return false;
        }
        return collect_frame();
      }

      /**
       * Where the host's time actually goes, once every few hundred frames.
       *
       * Kept in the same two numbers whichever path produced the frame, because comparing them is
       * the point: the first is what the host spends getting the picture into a form the codec can
       * read, and the second is the codec.
       */
      void report_timing(std::chrono::steady_clock::time_point convert_started,
                         std::chrono::steady_clock::time_point encode_started) {
        const auto finished = std::chrono::steady_clock::now();
        const auto to_ms = [](auto from, auto to) {
          return std::chrono::duration<double, std::milli>(to - from).count();
        };
        convert_ms_total += to_ms(convert_started, encode_started);
        encode_ms_total += to_ms(encode_started, finished);

        // Once early, so a session says what it costs, then rarely, so a long one can show drift
        // without filling the log. Five seconds in and every five minutes after, at sixty frames a
        // second.
        ++timed_frames;
        if (timed_frames == 300 || timed_frames % 18000 == 0) {
          BOOST_LOG(info) << "PyroWave: over "sv << timed_frames << " frames, "sv
                          << (gpu == gpu_e::yes ? "the copy to the GPU "sv : "colour conversion "sv)
                          << (convert_ms_total / timed_frames) << " ms and encode "sv
                          << (encode_ms_total / timed_frames) << " ms a frame"sv;
        }
      }

      bool encode(const uint8_t *y, const uint8_t *u, const uint8_t *v, std::size_t max_bytes) override {
        // Before the arguments are even looked at, so that every way out of here leaves nothing to
        // read. A caller that missed the return value would otherwise send the previous picture
        // again under a new frame number, which the decoder accepts: a freeze rather than an error.
        frame.clear();
        if (!encoder || !y || !u || !v || max_bytes == 0) {
          return false;
        }

        pyrowave_cpu_buffer buffer = {};
        buffer.format = chroma == chroma_e::yuv420 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P
                                                   : PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
        buffer.width = width;
        buffer.height = height;
        buffer.data[0] = const_cast<uint8_t *>(y);
        buffer.data[1] = const_cast<uint8_t *>(u);
        buffer.data[2] = const_cast<uint8_t *>(v);
        buffer.row_stride_in_bytes[0] = static_cast<std::size_t>(width);
        buffer.row_stride_in_bytes[1] = static_cast<std::size_t>(width >> shift);
        buffer.row_stride_in_bytes[2] = static_cast<std::size_t>(width >> shift);
        buffer.plane_size_in_bytes[0] = buffer.row_stride_in_bytes[0] * static_cast<std::size_t>(height);
        buffer.plane_size_in_bytes[1] = buffer.row_stride_in_bytes[1] * static_cast<std::size_t>(height >> shift);
        buffer.plane_size_in_bytes[2] = buffer.row_stride_in_bytes[2] * static_cast<std::size_t>(height >> shift);

        budget = max_bytes;
        const pyrowave_rate_control rate_control = {max_bytes};
        const auto result = pyrowave_encoder_encode_cpu_synchronous(encoder, &buffer, &rate_control);
        if (result != PYROWAVE_SUCCESS) {
          BOOST_LOG(warning) << "PyroWave: encode failed (result "sv << static_cast<int>(result) << ')';
          return false;
        }
        return collect_frame();
      }

      /**
       * Gather the encoded frame into one buffer, or leave it empty and say why.
       *
       * The boundary handed to the codec is a reference, not a promise to the network: it decides
       * where the codec draws its packet table, and the bytes it writes are the same either way.
       * It is chosen above the largest a single block can be, because the split test runs before a
       * block is added rather than after, so a block that does not fit in an empty packet still
       * goes in and overflows it. One block is at most 4095 words, since that is the width of the
       * payload length field in its header, so a boundary above 16380 bytes cannot overflow and
       * count times boundary is a real bound on the output rather than a hopeful one. That matters:
       * the codec only checks the buffer it was given with an assert, so a build with asserts off
       * would write past a buffer that was merely probably big enough.
       */
      bool collect_frame() {
        frame.clear();

        std::size_t count = 0;
        if (pyrowave_encoder_compute_num_packets(encoder, reference_boundary, &count) != PYROWAVE_SUCCESS || count == 0) {
          BOOST_LOG(error) << "PyroWave: the encoded frame reports no packets"sv;
          return false;
        }

        std::vector<pyrowave_packet> descriptors(count);
        frame.resize(count * reference_boundary);
        std::size_t written = 0;
        if (pyrowave_encoder_packetize(encoder, descriptors.data(), reference_boundary, &written,
                                       frame.data(), frame.size()) != PYROWAVE_SUCCESS || written == 0) {
          BOOST_LOG(error) << "PyroWave: the encoded frame could not be packetized"sv;
          frame.clear();
          return false;
        }

        // Checked rather than trusted. Reading the frame as one blob is only correct while the
        // packets tile the buffer from the start with no gaps, which is how the codec writes them
        // today; if that ever changes, a silently reordered or gapped bitstream is a corrupt
        // picture with no error anywhere, so find out here instead.
        std::size_t expected_offset = 0;
        for (std::size_t i = 0; i < written; ++i) {
          if (descriptors[i].offset != expected_offset) {
            BOOST_LOG(error) << "PyroWave: packet "sv << i << " starts at "sv << descriptors[i].offset
                             << " instead of "sv << expected_offset
                             << "; the bitstream is no longer one contiguous run"sv;
            frame.clear();
            return false;
          }
          expected_offset += descriptors[i].size;
        }

        if (expected_offset > frame.size()) {
          BOOST_LOG(error) << "PyroWave: the frame runs "sv << expected_offset << " bytes past its "sv
                           << frame.size() << " byte buffer"sv;
          frame.clear();
          return false;
        }

        frame.resize(expected_offset);
        validate_frame();
        return true;
      }

      /**
       * Count the blocks in the frame and compare with what its own header claims.
       *
       * A frame arriving at the client with fewer blocks than its sequence header promises is
       * undecodable, and that is reproducibly what happens to frames sitting at the rate control
       * ceiling. This says which end is responsible: if the count is already short here, the
       * encoder produced an inconsistent frame and nothing in transit is to blame.
       *
       * Walks it exactly as the decoder's push entry point does, because the point is to see what
       * it will see. Silent when the frame is whole, which is almost always.
       */
      void validate_frame() {
        if (frame.size() < 8) {
          return;
        }

        uint32_t second = 0;
        std::memcpy(&second, frame.data() + 4, sizeof(second));
        const uint32_t claimed = second & 0xffffff;

        std::size_t offset = 8;  // past the sequence header
        uint32_t counted = 0;
        bool ran_off_the_end = false;

        while (offset + 8 <= frame.size()) {
          uint16_t descriptor = 0;
          std::memcpy(&descriptor, frame.data() + offset + 2, sizeof(descriptor));
          const bool extended = (descriptor >> 15) & 0x1;
          if (extended) {
            offset += 8;
            continue;
          }

          const std::size_t block_bytes = static_cast<std::size_t>(descriptor & 0x0fff) * 4;
          if (block_bytes == 0 || offset + block_bytes > frame.size()) {
            ran_off_the_end = true;
            break;
          }
          counted++;
          offset += block_bytes;
        }

        const bool whole = !ran_off_the_end && counted == claimed && offset == frame.size();
        if (whole) {
          ++whole_frames;
          // The frames the client cannot decode are the ones at the ceiling, so say what leaves here
          // for exactly those. If this size and the size the client reports are the same, the frame
          // survived the wire intact and the fault is further in; if they differ, something between
          // trims it.
          if (budget > 0 && frame.size() * 100 >= budget * 99) {
            ++ceiling_frames;
            if (ceiling_frames <= 4 || ceiling_frames % 100 == 0) {
              BOOST_LOG(info) << "PyroWave: ceiling frame "sv << ceiling_frames << " leaves at "sv
                              << frame.size() << " bytes of a "sv << budget << " budget, "sv
                              << claimed << " blocks, whole"sv;
            }
          }
          return;
        }

        ++short_frames;
        if (short_frames <= 3 || short_frames % 25 == 0) {
          BOOST_LOG(warning) << "PyroWave: frame "sv << short_frames << " of "sv
                             << (short_frames + whole_frames) << " is short before it leaves: "sv
                             << counted << " blocks of "sv << claimed << " claimed, "sv
                             << offset << " bytes walked of "sv << frame.size()
                             << (ran_off_the_end ? ", and a block ran past the end"sv : ""sv);
        }
      }

      bool encode_retained(std::size_t max_bytes) override {
        frame.clear();

        // On the GPU the picture is already where the codec reads from and already in the layout it
        // reads in, so a repeat records the encode and nothing else. There is no falling back to the
        // planes here: on this path they were never filled, and encoding them would send a black
        // frame rather than the picture again.
        if (gpu == gpu_e::yes) {
          if (!staging || !staging->has_picture() || !staging->begin_retained()) {
            return false;
          }
          const auto started = std::chrono::steady_clock::now();
          if (!encode_recorded(max_bytes)) {
            return false;
          }
          report_timing(started, started);
          return true;
        }

        if (planes[0].empty() || planes[1].empty() || planes[2].empty()) {
          return false;
        }
        return encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
      }

      bool uses_gpu_input() const override {
        return gpu == gpu_e::yes;
      }

      const std::vector<uint8_t> &bitstream() const override {
        return frame;
      }

    private:
      /// Above the 16380 bytes a single coded block can reach, so every packet fits inside it.
      static constexpr std::size_t reference_boundary = 16384;

      /// Which path this session settled on, decided by trying rather than by guessing.
      enum class gpu_e {
        unknown,
        yes,
        no,
      };

      const vk_device_t *owner = nullptr;
      std::unique_ptr<upload_t> staging;
      gpu_e gpu = gpu_e::no;
      pyrowave_encoder encoder = nullptr;
      int width = 0;
      int height = 0;
      int source_width = 0;
      int source_height = 0;
      int fit_width = 0;
      int fit_height = 0;
      int offset_x = 0;
      int offset_y = 0;
      chroma_e chroma = chroma_e::yuv420;
      int shift = 1;
      dynamic_range_e range = dynamic_range_e::sdr;
      SwsContext *scaler = nullptr;
      double convert_ms_total = 0.0;
      double encode_ms_total = 0.0;
      uint64_t timed_frames = 0;
      uint64_t whole_frames = 0;
      uint64_t short_frames = 0;
      uint64_t ceiling_frames = 0;
      std::size_t budget = 0;
      std::array<std::vector<uint8_t>, 3> planes;
      std::vector<uint8_t> frame;
    };

  }  // namespace

  std::string api_version() {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
  }

  bool available() {
    return shared_device() != nullptr;
  }

  bool hdr_available() {
    return available() && gpu_input_allowed();
  }

  std::vector<std::uint64_t> importable_dmabuf_modifiers(std::uint32_t fourcc) {
    if (!gpu_input_allowed()) {
      return {};
    }
    auto *owner = shared_vulkan();
    if (!owner) {
      return {};
    }
    return importable_modifiers(*owner, fourcc);
  }

  bool dmabuf_import_available() {
    if (!gpu_input_allowed()) {
      return false;
    }
    auto *owner = shared_vulkan();
    return owner != nullptr && owner->can_import_dmabuf;
  }

  std::unique_ptr<session_t> make_session(int width, int height, chroma_e chroma,
                                         dynamic_range_e range) {
    auto *owner = shared_vulkan();
    if (!owner) {
      return nullptr;
    }
    auto device = owner->codec;

    // Refused here rather than at the first frame. HDR exists only on the path that hands the codec a
    // picture on the GPU, so a host that has been told not to use that path cannot serve HDR at all,
    // and finding that out before a client is promised a stream is the whole point.
    if (range == dynamic_range_e::hdr10 && !gpu_input_allowed()) {
      BOOST_LOG(error) << "PyroWave: HDR needs the GPU input path, which is switched off"sv;
      return nullptr;
    }

    // Only 4:2:0 has half a chroma sample to lose, and the library refuses an odd extent rather
    // than rounding one for us. 4:4:4 has a chroma sample per pixel and does not care.
    if (chroma == chroma_e::yuv420) {
      width &= ~1;
      height &= ~1;
    }
    if (width <= 0 || height <= 0) {
      return nullptr;
    }

    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = width;
    info.height = height;
    info.chroma = chroma == chroma_e::yuv420 ? PYROWAVE_CHROMA_SUBSAMPLING_420
                                             : PYROWAVE_CHROMA_SUBSAMPLING_444;

    pyrowave_encoder encoder = nullptr;
    const auto result = pyrowave_encoder_create(&info, &encoder);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a "sv << width << 'x' << height
                         << " encoder (result "sv << static_cast<int>(result) << ')';
      return nullptr;
    }

    return std::make_unique<pyrowave_session_t>(*owner, encoder, width, height, chroma, range);
  }

}  // namespace pyrowave_encode
