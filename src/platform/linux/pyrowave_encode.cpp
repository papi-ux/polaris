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
#include <algorithm>
#include <limits>

// local includes
#include "src/logging.h"
#include "src/platform/linux/pyrowave_encode.h"

using namespace std::literals;

namespace pyrowave_encode {

  std::optional<std::size_t> frame_budget(int bitrate_kbps, int fps_num, int fps_den) {
    if (bitrate_kbps <= 0 || fps_num <= 0 || fps_den <= 0) return std::nullopt;
    const auto bits_per_second = std::uint64_t(bitrate_kbps) * 1000;
    if (std::uint64_t(fps_den) > std::numeric_limits<std::uint64_t>::max() / bits_per_second) return std::nullopt;
    const auto bytes = bits_per_second * fps_den / (8 * std::uint64_t(fps_num));
    // Four 10-bit shard-count fields, with at least 992 payload bytes after
    // encryption. Reject unsupported requests instead of acknowledging a cap.
    if (bytes < 1024 || bytes > 3 * 1024 * 1024) return std::nullopt;
    return static_cast<std::size_t>(bytes);
  }

  namespace {

    // Devices are owned by sessions and released after their encoders.
    struct device_t {
      pyrowave_device value = nullptr;
      device_t() { pyrowave_create_default_device(&value); }
      ~device_t() { if (value) pyrowave_device_destroy(value); }
    };

    class pyrowave_session_t: public session_t {
    public:
      pyrowave_session_t(std::unique_ptr<device_t> device, pyrowave_encoder encoder, int width, int height, int source_width, int source_height):
          device {std::move(device)},
          encoder {encoder},
          width {width},
          height {height}, source_width {source_width}, source_height {source_height} {}

      ~pyrowave_session_t() override {
        if (scaler) {
          sws_freeContext(scaler);
        }
        if (encoder) {
          pyrowave_encoder_destroy(encoder);
        }
      }

      bool encode_bgra(const uint8_t *bgra, int stride, std::size_t max_bytes) override {
        encoded_budget = 0;
        if (!bgra || stride < source_width * 4) {
          return false;
        }

        // Kept between frames: a stream is thousands of identically shaped frames, and building
        // the scaler for each one would dominate a codec that encodes in a tenth of a millisecond.
        if (!scaler) {
          const auto scale = std::min(double(width) / source_width, double(height) / source_height);
          scaled_width = std::max(2, int(source_width * scale) & ~1);
          scaled_height = std::max(2, int(source_height * scale) & ~1);
          offset_x = ((width - scaled_width) / 2) & ~1;
          offset_y = ((height - scaled_height) / 2) & ~1;
          scaler = sws_getContext(source_width, source_height, AV_PIX_FMT_BGRA,
                                  scaled_width, scaled_height, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
          if (!scaler) {
            BOOST_LOG(error) << "PyroWave: could not make a "sv << width << 'x' << height
                             << " colour converter"sv;
            return false;
          }
          const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
          if (sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
            sws_freeContext(scaler);
            scaler = nullptr;
            return false;
          }
          planes[0].resize(static_cast<std::size_t>(width) * height, 0);
          planes[1].resize(static_cast<std::size_t>(width / 2) * (height / 2), 128);
          planes[2].resize(static_cast<std::size_t>(width / 2) * (height / 2), 128);
        }

        const uint8_t *src[4] = {bgra, nullptr, nullptr, nullptr};
        const int src_stride[4] = {stride, 0, 0, 0};
        uint8_t *dst[4] = {planes[0].data() + offset_y * width + offset_x,
          planes[1].data() + (offset_y / 2) * (width / 2) + offset_x / 2,
          planes[2].data() + (offset_y / 2) * (width / 2) + offset_x / 2, nullptr};
        const int dst_stride[4] = {width, width / 2, width / 2, 0};

        if (sws_scale(scaler, src, src_stride, 0, source_height, dst, dst_stride) != scaled_height) {
          BOOST_LOG(warning) << "PyroWave: colour conversion did not fill the frame"sv;
          return false;
        }

        return encode(planes[0].data(), planes[1].data(), planes[2].data(), max_bytes);
      }

      bool encode(const uint8_t *y, const uint8_t *u, const uint8_t *v, std::size_t max_bytes) override {
        encoded_budget = 0;
        if (!encoder || !y || !u || !v || max_bytes < 1024 || max_bytes > max_frame_bytes / 2) {
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
        encoded_budget = max_bytes;
        return true;
      }

      std::vector<std::vector<uint8_t>> packets(std::size_t packet_boundary) override {
        std::vector<std::vector<uint8_t>> out;
        if (!encoder || !encoded_budget || packet_boundary < 8 || packet_boundary > 64 * 1024) {
          return out;
        }

        std::size_t count = 0;
        if (pyrowave_encoder_compute_num_packets(encoder, packet_boundary, &count) != PYROWAVE_SUCCESS || count == 0 || count > max_frame_bytes / packet_boundary) {
          return out;
        }

        std::vector<pyrowave_packet> descriptors(count);
        // A coefficient block can exceed a small split target. Reserve the
        // encoded frame budget as well as the packet-count estimate.
        std::vector<uint8_t> bitstream(std::max(count * packet_boundary, encoded_budget + 16));
        std::size_t written = 0;
        if (pyrowave_encoder_packetize(encoder, descriptors.data(), packet_boundary, &written,
                                       bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS || written > count) {
          return out;
        }

        out.reserve(written);
        for (std::size_t i = 0; i < written; ++i) {
          const auto &descriptor = descriptors[i];
          if (descriptor.offset > bitstream.size() || descriptor.size > bitstream.size() - descriptor.offset ||
              descriptor.size > std::max<std::size_t>(packet_boundary, 16380) || descriptor.size < 8 || descriptor.size % 4) {
            // A packet that runs past the buffer means the library and this caller disagree about
            // the bitstream layout, which is not something to paper over one packet at a time.
            BOOST_LOG(error) << "PyroWave: packet "sv << i << " runs past the bitstream"sv;
            return {};
          }
          out.emplace_back(bitstream.begin() + static_cast<std::ptrdiff_t>(descriptor.offset),
                           bitstream.begin() + static_cast<std::ptrdiff_t>(descriptor.offset + descriptor.size));
        }
        return out;
      }

    private:
      // Each session owns its queue and allocator; concurrent streams must not
      // mutate a single upstream device's frame/command-buffer state.
      std::unique_ptr<device_t> device;
      pyrowave_encoder encoder = nullptr;
      int width = 0;
      int height = 0;
      std::size_t encoded_budget = 0;
      int source_width = 0, source_height = 0;
      int scaled_width = 0, scaled_height = 0, offset_x = 0, offset_y = 0;
      SwsContext *scaler = nullptr;
      std::array<std::vector<uint8_t>, 3> planes;
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
    static const bool supported = [] { return device_t{}.value != nullptr; }();
    return supported;
  }

  std::vector<uint8_t> pack_frame(const std::vector<std::vector<uint8_t>>& packets, int width, int height) {
    if (width < 16 || height < 16 || width > 4096 || height > 4096 || width % 2 || height % 2 ||
        packets.empty() || packets.size() > max_frame_bytes / 12) return {};
    std::size_t total = 0;
    for (const auto& packet : packets) {
      if (packet.size() < 8 || packet.size() > packet_bytes || packet.size() % 4 ||
          packet.size() > max_frame_bytes - total) return {};
      total += packet.size();
    }
    const auto& first = packets.front();
    const auto header = std::uint32_t(first[0]) | (std::uint32_t(first[1]) << 8) |
                        (std::uint32_t(first[2]) << 16) | (std::uint32_t(first[3]) << 24);
    if (!(header & 0x80000000u) || (header & 0x3fff) + 1 != unsigned(width) ||
        ((header >> 14) & 0x3fff) + 1 != unsigned(height)) return {};
    std::vector<uint8_t> out;
    out.reserve(total);
    for (const auto& packet : packets) out.insert(out.end(), packet.begin(), packet.end());
    return out;
  }

  std::unique_ptr<session_t> make_session(int width, int height, int source_width, int source_height) {
    // 4:2:0 has no half chroma sample, and the library refuses an odd extent rather than rounding
    // one for us.
    if (!source_width) source_width = width;
    if (!source_height) source_height = height;
    if (width < 16 || height < 16 || width > 4096 || height > 4096 || width % 2 || height % 2 ||
        source_width <= 0 || source_height <= 0 || source_width > 16384 || source_height > 16384) {
      return nullptr;
    }

    auto device = std::make_unique<device_t>();
    if (!device->value) return nullptr;

    pyrowave_encoder_create_info info = {};
    info.device = device->value;
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

    return std::make_unique<pyrowave_session_t>(std::move(device), encoder, width, height, source_width, source_height);
  }

}  // namespace pyrowave_encode
