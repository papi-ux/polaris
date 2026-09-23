/**
 * @file src/platform/linux/pyrowave_encode.h
 * @brief PyroWave, an intra-only wavelet codec that runs as plain Vulkan compute.
 *
 * Everything that needs a Vulkan header lives behind this one, so the rest of Polaris can use the
 * codec without pulling the Vulkan API into every translation unit that includes it.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pyrowave_encode {

  /**
   * @brief The PyroWave version this binary is linked against, as MAJOR.MINOR.PATCH.
   *
   * Read from the library rather than from the build system. The two can disagree, and a codec
   * whose own ABI is unstable before 1.0 is exactly the one to ask rather than assume.
   */
  std::string api_version();

  /**
   * @brief Whether this machine can run the codec at all.
   *
   * Answers by asking for a device, because that is the only honest answer: the encoder needs
   * subgroup size control, shaderInt16 and 8 bit storage, and a driver either provides them or it
   * does not. Cheap to call once, so call it once and remember.
   */
  bool available();

  /**
   * @brief One encoder, sized at construction, producing one frame at a time.
   *
   * Intra-only, so there is no reference chain to hold and nothing to invalidate: every frame
   * stands alone and the caller may stop asking at any point without consequence.
   */
  class session_t {
  public:
    virtual ~session_t() = default;

    /**
     * @brief Encode one frame from planar YUV in host memory.
     *
     * The bring-up path. It copies, so it is not what a stream should use once capture hands over
     * a dmabuf, but it is the one path that works the same on every backend and it is how the
     * codec proves itself before anything else is wired up.
     * @param y Luma plane, width*height bytes, no padding.
     * @param u Chroma plane, quarter size.
     * @param v Chroma plane, quarter size.
     * @param max_bytes The most this frame may occupy. The encoder targets it exactly.
     * @return false when the encode failed; the packet list is then empty.
     */
    virtual bool encode(const uint8_t *y, const uint8_t *u, const uint8_t *v, std::size_t max_bytes) = 0;

    /**
     * @brief Encode one frame from packed BGRA in host memory.
     *
     * What capture actually hands over. The conversion to planar YUV happens here, on the CPU,
     * which is the bring-up path: it works against every capture backend without importing a
     * buffer, and it is the wrong way to do it once a dmabuf can reach the GPU directly.
     * @param bgra First byte of the top left pixel.
     * @param stride Bytes per row, which capture rarely makes equal to width times four.
     * @param max_bytes The most this frame may occupy.
     * @return false when the frame could not be converted or encoded.
     */
    virtual bool encode_bgra(const uint8_t *bgra, int stride, std::size_t max_bytes) = 0;

    /**
     * @brief The encoded frame, split at a boundary the network can carry.
     *
     * Every packet is independent: PyroWave codes 64x64 blocks of coefficients in isolation, so a
     * frame that loses one still decodes. Valid until the next encode.
     * @param packet_boundary The largest packet the caller will send.
     */
    virtual std::vector<std::vector<uint8_t>> packets(std::size_t packet_boundary) = 0;
  };

  /**
   * @brief Make an encoder for a frame size, or nullptr when this machine cannot.
   * @param width Frame width; rounded down to even, because 4:2:0 has no half chroma sample.
   * @param height Frame height, likewise.
   */
  std::unique_ptr<session_t> make_session(int width, int height);

}  // namespace pyrowave_encode
