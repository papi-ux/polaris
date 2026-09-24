/**
 * @file src/platform/linux/pyrowave_encode.h
 * @brief PyroWave, an intra-only wavelet codec that runs as plain Vulkan compute.
 *
 * Everything that needs a Vulkan header lives behind this one, so the rest of Polaris can use the
 * codec without pulling the Vulkan API into every translation unit that includes it.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <vector>
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
   * @brief What the samples in a stream mean.
   *
   * Not a quality setting. It decides how many bits a captured pixel arrives in, which transfer
   * function those bits carry, and which primaries the colour matrix is built from, and the two ends
   * have to agree on all three or the picture is merely wrong rather than broken.
   */
  enum class dynamic_range_e {
    sdr,  ///< Full range Rec. 709, eight bits a sample, which is what clients ask for today.
    hdr10,  ///< Full range BT.2020 with the PQ transfer function, ten bits a sample in.
  };

  /**
   * @brief How far to shift a luma extent to get a chroma one. Zero for 4:4:4, one for 4:2:0.
   */
  inline int chroma_shift(chroma_e chroma) {
    return chroma == chroma_e::yuv420 ? 1 : 0;
  }

  /**
   * @brief A captured frame that already lives on the GPU, described the way DRM describes it.
   *
   * The fields a dmabuf always carries: which descriptors hold the memory, what the pixels are, how
   * they are laid out and where each plane starts. Plain integers on purpose, so that handing one of
   * these over needs no Vulkan header and no capture backend header either.
   *
   * The descriptors stay the caller's. Importing duplicates the one it uses.
   */
  struct dmabuf_t {
    int fds[4] = {-1, -1, -1, -1};
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t pitches[4] = {};
    std::uint32_t offsets[4] = {};
    int width = 0;
    int height = 0;

    /**
     * @brief Which of capture's buffers this frame came out of, or zero when nobody said.
     *
     * Capture cycles a small pool and hands the same buffers back in turn, and it stamps each one
     * with a number of its own that is never reused. That is what lets this host describe a buffer to
     * Vulkan once instead of once a frame, which is a sixth of the frame time at 1080p.
     *
     * A number rather than a descriptor on purpose. A descriptor can be closed and the same integer
     * handed out again for something else, which would make a cache read the wrong picture; these
     * come from a counter that only goes up.
     */
    std::uint64_t buffer_key = 0;
  };

  /**
   * @brief Whether this host can take a captured frame as a dmabuf rather than as a copy.
   *
   * A property of the GPU and its driver, not of any one frame: the device has to have the external
   * memory extensions, and a host without them copies instead. Asked before capture is told what to
   * offer, because offering a dmabuf to a host that cannot import one is a stream with no picture.
   */
  bool dmabuf_import_available();

  /**
   * @brief Which layouts capture may hand this host a frame in, for one DRM format.
   *
   * Empty when this host cannot import that format at all, which is also the answer that keeps
   * capture on the copying path. See the note on importable_modifiers for why this is asked rather
   * than assumed.
   */
  std::vector<std::uint64_t> importable_dmabuf_modifiers(std::uint32_t fourcc);

  /**
   * @brief Whether this codec can read pixels in a DRM format at all.
   *
   * A property of the format alone, asked without a device and without a frame. Separate from
   * whether a particular buffer can be imported, which also depends on the driver and the layout:
   * this one answers whether there is any point trying, and a no does not change for as long as a
   * capture is producing that format.
   *
   * KDE composites HDR into ABGR16161616F, sixteen bits of float a channel, which is the answer this
   * is usually asked about.
   */
  bool can_read_dmabuf_format(std::uint32_t fourcc);

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
   * @brief The same agreement for an HDR10 stream, which is a different one about the same bytes.
   *
   * Its own token rather than a flag beside the other one, for the reason the other one exists: the
   * bitstream says nothing about primaries or transfer function, so a client that decodes PQ BT.2020
   * as if it were Rec. 709 gets a washed out, wrongly hued picture and no error. Nothing advertises
   * this yet. The encoder can produce it and no client can read it, so the negotiation is the next
   * piece rather than a missing one.
   */
  inline constexpr const char *hdr_profile_token = "pyrowave-186f0393-hdr2020pq420-v1";

  /// The token a stream of this kind has to agree on.
  inline constexpr const char *profile_token_for(dynamic_range_e range) {
    return range == dynamic_range_e::hdr10 ? hdr_profile_token : profile_token;
  }

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
   * @brief Whether this host can carry HDR10, which is a narrower question than whether it can encode.
   *
   * HDR exists only on the path that hands the codec a picture on the GPU, because its system memory
   * entry point is eight bit. So a host with the codec but without that path can stream SDR and must
   * not offer HDR, and this is the difference. Asked before a client is told what is available, and
   * again before one is believed.
   */
  bool hdr_available();

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
     * @brief Encode one frame of packed pixels in host memory, scaled to the session's size.
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
     * @param pixels First byte of the top left pixel. Eight bit BGRA for an SDR session and
     *   ten bit packed xBGR for an HDR one, which are the two things Polaris's capture hands over,
     *   both at four bytes a pixel.
     * @param src_width Width of what capture handed over, not of the stream.
     * @param src_height Height of the same.
     * @param stride Bytes per row, which capture rarely makes equal to width times four.
     * @param max_bytes The most this frame may occupy.
     * @return false when the frame could not be converted or encoded.
     */
    virtual bool encode_packed(const uint8_t *pixels, int src_width, int src_height, int stride,
                               std::size_t max_bytes) = 0;

    /**
     * @brief Encode a captured frame that is already on the GPU, copying nothing across the bus.
     *
     * What encode_packed does without the copy that is most of its cost. The frame is described,
     * taken off the queue family that filled it, and read where it lies; a stream that needs bars
     * costs one copy between two images on the GPU, which is a tenth of the price of one across it.
     *
     * @param buffer What capture handed over.
     * @param max_bytes The most this frame may occupy.
     * @return false when this frame could not be imported or encoded. One frame, not the session:
     *   the caller may hand over the next one.
     */
    virtual bool encode_imported(const dmabuf_t &buffer, std::size_t max_bytes) = 0;

    /**
     * @brief Encode a black frame, for when there is no picture to send yet.
     *
     * Polaris primes an encoder by converting a dummy image before the first real frame arrives, and
     * on the path where frames live on the GPU that image carries no pixels for this to read. The
     * picture it stands for is made rather than read.
     *
     * @param max_bytes The most this frame may occupy.
     * @return false when the frame could not be encoded.
     */
    virtual bool encode_blank(std::size_t max_bytes) = 0;

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
     * @brief How many of capture's buffers this session has had to describe to Vulkan.
     *
     * Zero until a frame arrives as a dmabuf, then one per buffer in capture's pool: describing one
     * costs about 0.2 ms of a 1080p frame and 0.9 ms of a 7680x2160 one, so it is done once per buffer
     * and kept rather than once per frame. A number that climbs with the frame count means nothing is
     * being recognised, and every frame is paying that again.
     */
    virtual unsigned buffers_described() const = 0;

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
  std::unique_ptr<session_t> make_session(int width, int height, chroma_e chroma,
                                         dynamic_range_e range = dynamic_range_e::sdr);

}  // namespace pyrowave_encode
