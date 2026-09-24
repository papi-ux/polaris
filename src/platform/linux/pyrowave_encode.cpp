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
#include <cstring>
#include <mutex>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/pyrowave_encode.h"

using namespace std::literals;

namespace pyrowave_encode {

  namespace {

    /**
     * The device, made once and kept.
     *
     * PyroWave makes its own Vulkan device here rather than borrowing the one FFmpeg built for the
     * Vulkan encoder. That costs a second device on a host that runs both, and buys a bring-up path
     * with no interop to get wrong. Borrowing is what the zero copy work will need, and it can take
     * the handles this device already knows how to report.
     */
    pyrowave_device shared_device() {
      static std::once_flag once;
      static pyrowave_device device = nullptr;
      std::call_once(once, [] {
        const auto result = pyrowave_create_default_device(&device);
        if (result != PYROWAVE_SUCCESS) {
          BOOST_LOG(info) << "PyroWave: no usable Vulkan device on this host (result "sv
                          << static_cast<int>(result) << ')';
          device = nullptr;
        }
      });
      return device;
    }

    class pyrowave_session_t: public session_t {
    public:
      pyrowave_session_t(pyrowave_encoder encoder, int width, int height):
          encoder {encoder},
          width {width},
          height {height} {}

      ~pyrowave_session_t() override {
        if (scaler) {
          sws_freeContext(scaler);
        }
        if (encoder) {
          pyrowave_encoder_destroy(encoder);
        }
      }

      bool encode_bgra(const uint8_t *bgra, int src_width, int src_height, int stride,
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
          planes[1].data() + static_cast<std::size_t>(offset_y / 2) * (width / 2) + offset_x / 2,
          planes[2].data() + static_cast<std::size_t>(offset_y / 2) * (width / 2) + offset_x / 2,
          nullptr};
        const int dst_stride[4] = {width, width / 2, width / 2, 0};

        const auto conversion_started = std::chrono::steady_clock::now();
        if (sws_scale(scaler, src, src_stride, 0, src_height, dst, dst_stride) != fit_height) {
          BOOST_LOG(warning) << "PyroWave: colour conversion did not fill the frame"sv;
          return false;
        }
        const auto encode_started = std::chrono::steady_clock::now();

        const auto encoded = encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);

        // Where the host's time actually goes, once every few hundred frames.
        //
        // The conversion is a full frame of BGRA turned into planar YUV on one CPU core, and it sits
        // in front of a codec that does its own work on the GPU in a fraction of a millisecond. It
        // was always the bring-up shortcut rather than a design, and this says how much removing it
        // would be worth before anyone spends a week on importing a dmabuf.
        const auto finished = std::chrono::steady_clock::now();
        const auto to_ms = [](auto from, auto to) {
          return std::chrono::duration<double, std::milli>(to - from).count();
        };
        convert_ms_total += to_ms(conversion_started, encode_started);
        encode_ms_total += to_ms(encode_started, finished);
        // Once early, so a session says what it costs, then rarely, so a long one can show drift
        // without filling the log. Five seconds in and every five minutes after, at sixty frames a
        // second.
        ++timed_frames;
        if (timed_frames == 300 || timed_frames % 18000 == 0) {
          BOOST_LOG(info) << "PyroWave: over "sv << timed_frames << " frames, colour conversion "sv
                          << (convert_ms_total / timed_frames) << " ms and encode "sv
                          << (encode_ms_total / timed_frames) << " ms a frame"sv;
        }

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
        av_dict_set_int(&options, "dst_format", AV_PIX_FMT_YUV420P, 0);
        av_dict_set_int(&options, "sws_flags", SWS_BILINEAR, 0);
        av_dict_set_int(&options, "threads", threads, 0);

        const auto applied = av_opt_set_dict(scaler, &options);
        av_dict_free(&options);
        if (applied < 0) {
          BOOST_LOG(error) << "PyroWave: this build's swscale will not take those options"sv;
          sws_freeContext(scaler);
          scaler = nullptr;
          return false;
        }

        // Said out loud, because the default is neither of the things anyone would assume. swscale
        // converts RGB to YUV as limited range BT.601 unless told otherwise, and a decoder reading
        // these frames as Rec. 709 gets the hue wrong on anything saturated while looking entirely
        // plausible on a desktop. The bitstream has fields for this and upstream does not write
        // them, so the only agreement available is the one in profile_token, and this is the end of
        // it that has to be true.
        //
        // Set before init rather than after, which is what sws_setColorspaceDetails on an
        // initialised context would have been doing.
        const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
        if (sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 1,
                                     0, 1 << 16, 1 << 16) < 0) {
          BOOST_LOG(error) << "PyroWave: this converter will not do full range Rec. 709"sv;
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
        BOOST_LOG(info) << "PyroWave: converting on "sv << threads << " threads"sv;

        planes[0].assign(static_cast<std::size_t>(width) * height, 0);
        planes[1].assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);
        planes[2].assign(static_cast<std::size_t>(width / 2) * (height / 2), 128);

        source_width = src_width;
        source_height = src_height;

        if (fit_width != width || fit_height != height) {
          BOOST_LOG(info) << "PyroWave: fitting "sv << src_width << 'x' << src_height << " into "sv
                          << width << 'x' << height << " as "sv << fit_width << 'x' << fit_height
                          << " at "sv << offset_x << ',' << offset_y;
        }
        return true;
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
        buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        buffer.width = width;
        buffer.height = height;
        buffer.data[0] = const_cast<uint8_t *>(y);
        buffer.data[1] = const_cast<uint8_t *>(u);
        buffer.data[2] = const_cast<uint8_t *>(v);
        buffer.row_stride_in_bytes[0] = static_cast<std::size_t>(width);
        buffer.row_stride_in_bytes[1] = static_cast<std::size_t>(width / 2);
        buffer.row_stride_in_bytes[2] = static_cast<std::size_t>(width / 2);
        buffer.plane_size_in_bytes[0] = buffer.row_stride_in_bytes[0] * static_cast<std::size_t>(height);
        buffer.plane_size_in_bytes[1] = buffer.row_stride_in_bytes[1] * static_cast<std::size_t>(height / 2);
        buffer.plane_size_in_bytes[2] = buffer.row_stride_in_bytes[2] * static_cast<std::size_t>(height / 2);

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
        if (planes[0].empty() || planes[1].empty() || planes[2].empty()) {
          return false;
        }
        return encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
      }

      const std::vector<uint8_t> &bitstream() const override {
        return frame;
      }

    private:
      /// Above the 16380 bytes a single coded block can reach, so every packet fits inside it.
      static constexpr std::size_t reference_boundary = 16384;

      pyrowave_encoder encoder = nullptr;
      int width = 0;
      int height = 0;
      int source_width = 0;
      int source_height = 0;
      int fit_width = 0;
      int fit_height = 0;
      int offset_x = 0;
      int offset_y = 0;
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

  std::unique_ptr<session_t> make_session(int width, int height) {
    auto device = shared_device();
    if (!device) {
      return nullptr;
    }

    // 4:2:0 has no half chroma sample, and the library refuses an odd extent rather than rounding
    // one for us.
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
      return nullptr;
    }

    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = width;
    info.height = height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

    pyrowave_encoder encoder = nullptr;
    const auto result = pyrowave_encoder_create(&info, &encoder);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: could not make a "sv << width << 'x' << height
                         << " encoder (result "sv << static_cast<int>(result) << ')';
      return nullptr;
    }

    return std::make_unique<pyrowave_session_t>(encoder, width, height);
  }

}  // namespace pyrowave_encode
