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
   * @brief How much chroma a session carries.
   *
   * 4:2:0 halves it in both directions, which is what every hardware video codec does and what the
   * eye notices least. 4:4:4 keeps all of it, which costs roughly twice the bitrate for the same
   * quantiser and is the thing people actually ask this codec for: text, thin UI lines and
   * saturated edges stop smearing.
   */
  enum class chroma_e {
    yuv420,
    yuv444,
  };

  /**
   * @brief How far to shift a luma extent to get a chroma one. Zero for 4:4:4, one for 4:2:0.
   */
  inline int chroma_shift(chroma_e chroma) {
    return chroma == chroma_e::yuv420 ? 1 : 0;
  }

  /**
   * @brief What a client has to recognise before this host will stream the codec to it.
   *
   * Three things a decoder cannot work out for itself, in one string. The codec revision, because
   * its bitstream and its C API are both unstable before 1.0, so an encoder and a decoder built a
   * few commits apart can agree on every byte of the protocol and still produce noise. The
   * colourimetry, because the bitstream reserves fields for primaries, transfer function and range
   * and nothing upstream writes them yet, so they arrive as zero and mean nothing: these frames are
   * full range Rec. 709 SDR 4:2:0 and the only place that is written down is here. And a version,
   * for when one of those changes.
   *
   * It is deliberately brittle. Streaming a wrong guess produces a picture that is merely wrong,
   * washed out or off hue or noise, with nothing in any log to say why; refusing to stream produces
   * a sentence. While the codec is behind a build flag and both ends ship together that trade is
   * free, and it is the reason a Polaris and a Nova from different releases will not pair on this
   * codec. Before it could be anyone's default this has to become a negotiation of capabilities
   * rather than a single token, or upstream has to start writing the colourimetry it already has
   * room for.
   *
   * Moves with the third-party/pyrowave submodule pin. The two disagreeing is the failure this
   * exists to catch, so it cannot catch it for itself.
   */
  inline constexpr const char *profile_token = "pyrowave-186f0393-sdr420-v1";

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
     * @brief Encode one frame from packed BGRA in host memory, scaled to the session's size.
     *
     * What capture actually hands over, and rarely at the size the client asked for: a 7680x2160
     * monitor feeding a 1280x800 tablet is the ordinary case. The session's size is the stream's,
     * because it is the size the decoder at the other end was created with and the size written
     * into every frame's sequence header, and a decoder that reads a size it did not expect drops
     * the frame and says so about nothing else.
     *
     * So the source is scaled to fit inside it with its aspect ratio kept, centred, and the bars
     * left black. The conversion happens here on the CPU, which is the bring-up path: it works
     * against every capture backend without importing a buffer, and it is the wrong way to do it
     * once a dmabuf can reach the GPU directly.
     * @param bgra First byte of the top left pixel.
     * @param src_width Width of what capture handed over, not of the stream.
     * @param src_height Height of the same.
     * @param stride Bytes per row, which capture rarely makes equal to width times four.
     * @param max_bytes The most this frame may occupy.
     * @return false when the frame could not be converted or encoded.
     */
    virtual bool encode_bgra(const uint8_t *bgra, int src_width, int src_height, int stride,
                             std::size_t max_bytes) = 0;

    /**
     * @brief Encode the last converted picture again, as a new frame.
     *
     * For the frames a host repeats when capture has nothing new. Every frame this codec produces
     * carries a sequence number, and a decoder drops one it has already decoded, so the same
     * bitstream sent twice is a frame thrown away at the other end. Encoding again costs about a
     * millisecond and skips the colour conversion, which is the expensive half.
     *
     * @param max_bytes The most this frame may occupy.
     * @return false when there is no converted picture to encode, or the encode failed.
     */
    virtual bool encode_retained(std::size_t max_bytes) = 0;

    /**
     * @brief Whether frames reach the codec as a picture on the GPU rather than as planes.
     *
     * Decided by trying it, on the first frame a session is asked to encode, so it says nothing
     * before then. Worth asking afterwards: the two routes cost very different amounts of host CPU,
     * and which one a session took is the first thing to know when a host cannot keep up.
     */
    virtual bool uses_gpu_input() const = 0;

    /**
     * @brief The encoded frame, as one contiguous bitstream. Valid until the next encode.
     *
     * One blob rather than the packet list the codec will also hand out, because the bitstream
     * delimits itself: a sequence header, then coded blocks each carrying its own length, and the
     * decoder's push entry point walks them until the buffer runs out. Handing it the whole frame
     * at once is byte for byte the same work as handing it every packet in turn, and the bytes the
     * codec writes do not depend on where the packet boundaries were drawn, only the boundary
     * table does.
     *
     * So the split buys nothing here. It exists to let a lossy link drop one packet and still
     * decode the frame, and this transport does not offer that: it protects a whole frame with FEC
     * and reassembles it or loses it. Sending the packets separately would need each to be its own
     * frame on the wire, which every frame paced, counted and FEC protected stream around it
     * assumes means one picture. That is a different transport, not an addition to this one.
     *
     * Empty when the last encode failed.
     */
    virtual const std::vector<uint8_t> &bitstream() const = 0;
  };

  /**
   * @brief Make an encoder for a frame size, or nullptr when this machine cannot.
   * @param width Frame width; rounded down to even, because 4:2:0 has no half chroma sample.
   * @param height Frame height, likewise.
   */
  std::unique_ptr<session_t> make_session(int width, int height, chroma_e chroma);

}  // namespace pyrowave_encode
