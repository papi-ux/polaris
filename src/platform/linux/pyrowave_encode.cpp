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
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// standard includes
#include <array>
#include <mutex>

// local includes
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

      bool encode_bgra(const uint8_t *bgra, int stride, std::size_t max_bytes) override {
        // Before the arguments are even looked at, so that every way out of here leaves nothing to
        // read. A caller that missed the return value would otherwise send the previous picture
        // again under a new frame number, which the decoder accepts: a freeze rather than an error.
        frame.clear();
        if (!bgra || stride <= 0) {
          return false;
        }

        // Kept between frames: a stream is thousands of identically shaped frames, and building
        // the scaler for each one would dominate a codec that encodes in a tenth of a millisecond.
        if (!scaler) {
          scaler = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                  width, height, AV_PIX_FMT_YUV420P,
                                  SWS_POINT, nullptr, nullptr, nullptr);
          if (!scaler) {
            BOOST_LOG(error) << "PyroWave: could not make a "sv << width << 'x' << height
                             << " colour converter"sv;
            return false;
          }

          // Said out loud, because the default is neither of the things anyone would assume. swscale
          // converts RGB to YUV as limited range BT.601 unless told otherwise, and a decoder reading
          // these frames as Rec. 709 gets the hue wrong on anything saturated while looking entirely
          // plausible on a desktop. The bitstream has fields for this and upstream does not write
          // them, so the only agreement available is the one in profile_token, and this is the end of
          // it that has to be true.
          const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
          if (sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 1,
                                       0, 1 << 16, 1 << 16) < 0) {
            BOOST_LOG(error) << "PyroWave: this converter will not do full range Rec. 709"sv;
            sws_freeContext(scaler);
            scaler = nullptr;
            return false;
          }
          planes[0].resize(static_cast<std::size_t>(width) * height);
          planes[1].resize(static_cast<std::size_t>(width / 2) * (height / 2));
          planes[2].resize(static_cast<std::size_t>(width / 2) * (height / 2));
        }

        const uint8_t *src[4] = {bgra, nullptr, nullptr, nullptr};
        const int src_stride[4] = {stride, 0, 0, 0};
        uint8_t *dst[4] = {planes[0].data(), planes[1].data(), planes[2].data(), nullptr};
        const int dst_stride[4] = {width, width / 2, width / 2, 0};

        if (sws_scale(scaler, src, src_stride, 0, height, dst, dst_stride) != height) {
          BOOST_LOG(warning) << "PyroWave: colour conversion did not fill the frame"sv;
          return false;
        }

        return encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
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
        return true;
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
      SwsContext *scaler = nullptr;
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
