/**
 * @file tests/unit/test_recorder_bitstream_negative.cpp
 * @brief Negative and later-key inputs built from real libx264/libx265 fixture packets (#369, under #329).
 *
 * Every case starts from the qualified 64x64 CPU encoder fixture (tests/unit/recorder_encoder_fixture_support.h,
 * unchanged) and requires its frozen baseline: 8 packets, key flags at 0 and 4. Then it builds a closed,
 * named input list from those real packets with deterministic recipes (recipe-v1 below), or uses them
 * unmodified, and:
 *
 * - CBS cases read every pushed packet, in push order, through ONE persistent coded-bitstream (CBS) context,
 *   call the product's cbs::validate_sps on the first packet and the product's cbs::strip_filler_data on a
 *   copy of each. Their returns, units and active-SPS state are RECORDED ONLY. The only assertions are
 *   completion and test-owned integrity: the caller's input bytes are unchanged, the zeroed FFmpeg padding
 *   after each input (which FFmpeg is permitted to read) is unchanged, a 64-byte canary after that padding is
 *   unchanged, and the stripped copy never grows. That detects stray writes into test-owned guard bytes
 *   only; it is not a bounds, read-safety or memory-safety result. The test binary does not instrument the
 *   pinned FFmpeg archive, and no instrumented-library gate has run.
 * - Every case pushes its list into the replay buffer of the selected session at a controlled arrival clock
 *   and saves. It asserts the existing admission counters, the window-limit cause where the case is about
 *   it, the save status and fields, and byte transparency: the published file equals the admitted packets
 *   concatenated in push order. The recorder does not parse or repair content.
 *
 * Every case records one closed property set as GTest XML properties and one identical console line
 * (POLARIS_RECORDER_NEGATIVE_JSON). It also writes raw evidence, by exclusive creation, under the
 * directory named by POLARIS_RECORDER_NEGATIVE_EVIDENCE_DIR: the real source packets, every pushed packet,
 * the admitted concatenation, the actual published file and a manifest, so a separate readback can rebuild
 * each recipe and compare the saved bytes with the raw admitted inputs. A missing or unusable directory, an
 * existing case directory, or any write error fails the case. The packet PTS stays the fixture's synthetic
 * frame counter, and the provenance counters are separate synthetic producer counters; neither is a product
 * clock or presentation time. Nothing here is a parser validity or refusal contract, decode, container or
 * playback. A missing encoder or a failed open or save is a failure, never a skip.
 */
#include "recorder_encoder_fixture_support.h"

#include <src/cbs.h>
#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/cbs_h264.h>
#include <libavcodec/cbs_h265.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
}

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using recorder_fixture::properties_t;

  constexpr auto k_marker = "POLARIS_RECORDER_NEGATIVE_JSON";
  constexpr auto k_evidence_env = "POLARIS_RECORDER_NEGATIVE_EVIDENCE_DIR";
  constexpr auto k_recipe_version = "recipe-v1";
  constexpr std::uint64_t k_session = 369;   ///< The selected session
  constexpr std::uint64_t k_foreign = 370;   ///< Never registered
  constexpr std::uint64_t k_restarted = 371; ///< The session started after k_session ends (F3)
  constexpr std::uint64_t k_overlap = 370;   ///< A second REGISTERED session in the overlap cases (F5)
  constexpr auto k_stamp = "20261010_170945";
  constexpr std::int64_t k_producer_counter_base = 36900;  ///< Synthetic producer counters, not the PTS
  constexpr auto k_arrival_step = 16ms;  ///< Controlled arrival clock, not a presentation time
  constexpr std::size_t k_canary_bytes = 64;
  constexpr std::uint32_t k_garbage_seed = 369;
  constexpr std::size_t k_invalid_size = 10;  ///< The size passed with a null payload (N5)

  struct codec_case_t {
    const recorder_fixture::encoder_spec_t *spec;
    AVCodecID codec_id;
    int video_format;  ///< The recorder's codec number: 0=H.264, 1=HEVC
    const char *label;
    const char *tag;   ///< Evidence and source prefix
  };

  const codec_case_t k_h264 {&recorder_fixture::k_libx264, AV_CODEC_ID_H264, 0, "H.264", "h264"};
  const codec_case_t k_hevc {&recorder_fixture::k_libx265, AV_CODEC_ID_HEVC, 1, "HEVC", "hevc"};

  // The product's own decompose policy (src/cbs.cpp), as in the #365 positive test.
  const CodedBitstreamUnitType k_h264_decompose[] = {
    H264_NAL_SLICE,
    H264_NAL_IDR_SLICE,
    H264_NAL_SPS,
    H264_NAL_PPS,
  };
  const CodedBitstreamUnitType k_hevc_decompose[] = {
    HEVC_NAL_TRAIL_N,
    HEVC_NAL_TRAIL_R,
    HEVC_NAL_TSA_N,
    HEVC_NAL_TSA_R,
    HEVC_NAL_STSA_N,
    HEVC_NAL_STSA_R,
    HEVC_NAL_RADL_N,
    HEVC_NAL_RADL_R,
    HEVC_NAL_RASL_N,
    HEVC_NAL_RASL_R,
    HEVC_NAL_BLA_W_LP,
    HEVC_NAL_BLA_W_RADL,
    HEVC_NAL_BLA_N_LP,
    HEVC_NAL_IDR_W_RADL,
    HEVC_NAL_IDR_N_LP,
    HEVC_NAL_CRA_NUT,
    HEVC_NAL_VPS,
    HEVC_NAL_SPS,
    HEVC_NAL_PPS,
  };

  // ---------------------------------------------------------------------------------------------
  // recipe-v1: deterministic byte edits of real packets. Start codes are found by a byte scan here,
  // never by CBS, so each recipe can be rebuilt from the raw source packet alone.
  // ---------------------------------------------------------------------------------------------

  /// One Annex B unit: [begin, end) includes its start code prefix; `header` is its first header byte.
  struct unit_span_t {
    std::size_t begin = 0;
    std::size_t header = 0;
    std::size_t end = 0;
  };

  /// Every 00 00 01 start code in order; a single preceding zero byte makes it a 4-byte prefix. Bytes
  /// before the first start code are not a unit. Trailing zero bytes stay with the earlier unit.
  std::vector<unit_span_t> annex_b_units(const std::vector<std::uint8_t> &b) {
    std::vector<unit_span_t> units;
    for (std::size_t p = 0; p + 2 < b.size(); ++p) {
      if (b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1) {
        unit_span_t u;
        u.begin = (p > 0 && b[p - 1] == 0) ? p - 1 : p;
        u.header = p + 3;
        units.push_back(u);
        p += 2;
      }
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
      units[i].end = i + 1 < units.size() ? units[i + 1].begin : b.size();
    }
    return units;
  }

  /// The NAL unit type at `header`, or -1 when there is no header byte.
  int nal_type(const std::vector<std::uint8_t> &b, std::size_t header, AVCodecID codec_id) {
    if (header >= b.size()) {
      return -1;
    }
    return codec_id == AV_CODEC_ID_H264 ? (b[header] & 0x1f) : ((b[header] >> 1) & 0x3f);
  }

  bool is_parameter_set(int type, AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_H264 ? (type == 7 || type == 8) : (type == 32 || type == 33 || type == 34);
  }

  int sps_type(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_H264 ? 7 : 33;
  }

  struct recipe_t {
    std::string text;  ///< The exact recipe, recorded and written to the manifest
    std::vector<std::uint8_t> bytes;
    std::string error;  ///< A recipe precondition that did not hold; the case fails
  };

  recipe_t trunc_half(const std::vector<std::uint8_t> &b) {
    recipe_t r;
    r.text = std::string {k_recipe_version} + " trunc-half: keep bytes [0, floor(size/2))";
    r.bytes.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(b.size() / 2));
    if (r.bytes.empty()) {
      r.error = "trunc-half produced no bytes";
    }
    return r;
  }

  recipe_t trunc_in_sps(const std::vector<std::uint8_t> &b, AVCodecID codec_id) {
    recipe_t r;
    r.text = std::string {k_recipe_version} +
             " trunc-in-sps: keep bytes [0, first-SPS-unit header offset + 4); SPS type " + std::to_string(sps_type(codec_id)) +
             " found by an Annex B 00 00 01 scan";
    for (const auto &u : annex_b_units(b)) {
      if (nal_type(b, u.header, codec_id) == sps_type(codec_id)) {
        const std::size_t cut = u.header + 4;
        if (cut >= u.end) {
          r.error = "the SPS unit is too short for trunc-in-sps";
          return r;
        }
        r.bytes.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
        return r;
      }
    }
    r.error = "no SPS unit found by the start-code scan";
    return r;
  }

  recipe_t strip_start_codes(const std::vector<std::uint8_t> &b) {
    recipe_t r;
    r.text = std::string {k_recipe_version} + " strip-startcodes: remove every unit's 00 00 01 or 00 00 00 01 prefix";
    std::size_t from = 0;
    for (const auto &u : annex_b_units(b)) {
      r.bytes.insert(r.bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(u.begin));
      from = u.header;
    }
    r.bytes.insert(r.bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(from), b.end());
    if (r.bytes.empty() || r.bytes.size() == b.size()) {
      r.error = "strip-startcodes removed nothing or everything";
    }
    return r;
  }

  recipe_t drop_parameter_sets(const std::vector<std::uint8_t> &b, AVCodecID codec_id) {
    recipe_t r;
    r.text = std::string {k_recipe_version} + " drop-param-sets: remove whole units (prefix included) of types " +
             (codec_id == AV_CODEC_ID_H264 ? "7,8" : "32,33,34") + " found by an Annex B 00 00 01 scan";
    const auto units = annex_b_units(b);
    if (units.empty()) {
      r.error = "no units found by the start-code scan";
      return r;
    }
    r.bytes.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(units.front().begin));
    std::size_t dropped = 0;
    for (const auto &u : units) {
      if (is_parameter_set(nal_type(b, u.header, codec_id), codec_id)) {
        ++dropped;
        continue;
      }
      r.bytes.insert(r.bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(u.begin), b.begin() + static_cast<std::ptrdiff_t>(u.end));
    }
    if (dropped == 0) {
      r.error = "no parameter-set unit to drop";
    } else if (r.bytes.empty()) {
      r.error = "drop-param-sets left no bytes";
    }
    return r;
  }

  recipe_t garbage(std::size_t size, std::uint32_t seed) {
    recipe_t r;
    r.text = std::string {k_recipe_version} + " garbage: " + std::to_string(size) + " bytes, x = x*1103515245+12345 (mod 2^32) from seed " +
             std::to_string(seed) + ", byte = (x >> 16) & 0xff after each step";
    std::uint32_t x = seed;
    r.bytes.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      x = x * 1103515245u + 12345u;
      r.bytes.push_back(static_cast<std::uint8_t>((x >> 16) & 0xff));
    }
    if (r.bytes.empty()) {
      r.error = "garbage size is zero";
    }
    return r;
  }

  // ---------------------------------------------------------------------------------------------
  // The closed input list of a case.
  // ---------------------------------------------------------------------------------------------

  enum class session_event_e {
    none,
    end_selected,      ///< session_ended(k_session) before this push
    start_restarted,   ///< session_started(k_restarted, true, format) before this push
    admit_save,        ///< admit a held save (F4) before this push
    start_overlap,     ///< session_started(k_overlap, true, format) before this push (F5)
    end_overlap,       ///< session_ended(k_overlap) before this push (F5)
  };

  struct push_t {
    std::string provenance;  ///< "real", "constructed-from-real" or "synthetic"
    std::string source;      ///< e.g. "h264:0", or "none"
    std::string recipe;      ///< Empty for an unmodified packet
    std::vector<std::uint8_t> bytes;
    bool null_payload = false;  ///< Push a null pointer with k_invalid_size (N5)
    bool key = false;
    int video_format = 0;
    std::uint64_t generation = k_session;
    std::string expected;  ///< "admitted", "refused-invalid" or "refused-unselected"
    bool in_save = false;  ///< Expected in the published file, in push order
    bool cbs = false;      ///< Read by the CBS observations
    session_event_e before = session_event_e::none;
    std::string snapshot_before;  ///< Take a named replay snapshot after the event, before this push
    std::string snapshot_after;   ///< Take a named replay snapshot right after this push
  };

  /// One intermediate replay snapshot a case expects. Every field is asserted.
  struct snapshot_expect_t {
    std::string label;
    std::size_t registered_sessions = 0;
    std::uint64_t selected_generation = 0;
    std::size_t packets = 0;
    std::uint64_t refused_unselected = 0;
    std::uint64_t refused_awaiting_keyframe = 0;
    std::uint64_t discontinuities = 0;
    bool awaiting_keyframe = false;
    bool keyframe_start_run = false;
    stream_recorder::window_limit_e last_window_limit = stream_recorder::window_limit_e::none;
  };

  struct encoded_t {
    const codec_case_t *codec = nullptr;
    recorder_fixture::run_t run;
    std::string result = "not run";
    std::string baseline = "not evaluated";
    bool ok = false;
  };

  encoded_t encode(const codec_case_t &c) {
    encoded_t e;
    e.codec = &c;
    e.result = recorder_fixture::encode_run(*c.spec, e.run);
    if (e.result.empty()) {
      e.baseline = recorder_fixture::validate_baseline(e.run);
      e.ok = e.baseline.empty();
    }
    return e;
  }

  push_t real_push(const encoded_t &e, std::size_t i, bool in_save, bool cbs) {
    const auto &p = e.run.packets[i];
    push_t push;
    push.provenance = "real";
    push.source = std::string {e.codec->tag} + ":" + std::to_string(i);
    push.bytes = p.bytes;
    push.key = (p.flags & AV_PKT_FLAG_KEY) != 0;
    push.video_format = e.codec->video_format;
    push.expected = "admitted";
    push.in_save = in_save;
    push.cbs = cbs;
    return push;
  }

  push_t constructed_push(const encoded_t &e, std::size_t i, recipe_t recipe, bool cbs) {
    push_t push = real_push(e, i, true, cbs);
    push.provenance = recipe.text.find("garbage") != std::string::npos ? "synthetic" : "constructed-from-real";
    if (push.provenance == "synthetic") {
      push.source = "none (length of " + push.source + ")";
    }
    push.recipe = std::move(recipe.text);
    push.bytes = std::move(recipe.bytes);
    return push;
  }

  /// What one case expects of the replay buffer and the save. Every field is asserted.
  struct expect_t {
    std::size_t live_packets = 0;
    std::uint64_t refused_unselected = 0;
    std::uint64_t refused_invalid = 0;
    std::uint64_t refused_oversized = 0;
    std::uint64_t refused_awaiting_keyframe = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t selected_generation = k_session;
    int run_video_format = 0;
    bool assert_limit = false;  ///< Only where the case is about the cause
    stream_recorder::window_limit_e last_window_limit = stream_recorder::window_limit_e::none;
    std::size_t save_packets = 0;
    std::size_t save_keyframes = 0;
    int save_video_format = 0;
    std::uint64_t save_generation = k_session;
  };

  struct case_t {
    std::string name;   ///< The GTest case name, also the evidence directory name
    std::string ids;    ///< Design case IDs (REVIEWED-DESIGN-REVISION2.md)
    std::string summary;
    int session_format = 0;  ///< The format passed to session_started(k_session, ...)
    std::vector<const encoded_t *> runs;
    std::vector<push_t> pushes;
    std::string recipe_error;  ///< A recipe precondition failure
    expect_t expect;
    std::vector<snapshot_expect_t> snapshots;  ///< Intermediate snapshots, in the order they are taken
  };

  const char *limit_name(stream_recorder::window_limit_e l) {
    using L = stream_recorder::window_limit_e;
    switch (l) {
      case L::none:
        return "none";
      case L::duration:
        return "duration";
      case L::payload_cap:
        return "payload_cap";
      case L::entry_cap:
        return "entry_cap";
      case L::configuration:
        return "configuration";
      case L::selection:
        return "selection";
      case L::codec:
        return "codec";
      case L::discontinuity:
        return "discontinuity";
    }
    return "unknown";
  }

  const char *status_name(stream_recorder::detail::save_status_e s) {
    using S = stream_recorder::detail::save_status_e;
    switch (s) {
      case S::saved:
        return "saved";
      case S::busy:
        return "busy";
      case S::disabled:
        return "disabled";
      case S::empty:
        return "empty";
      case S::storage_error:
        return "storage_error";
    }
    return "unknown";
  }

  std::string sha(const std::vector<std::uint8_t> &b) {
    return recorder_fixture::sha256_hex(b.data(), b.size());
  }

  // ---------------------------------------------------------------------------------------------
  // CBS observations with test-owned guard bytes.
  // ---------------------------------------------------------------------------------------------

  std::uint8_t canary_byte(std::size_t i) {
    return static_cast<std::uint8_t>(0xa5 ^ ((i * 29) & 0xff));
  }

  /// Input bytes, then AV_INPUT_BUFFER_PADDING_SIZE zero bytes (FFmpeg may read them), then a canary.
  struct guarded_t {
    std::vector<std::uint8_t> storage;
    std::size_t size = 0;

    explicit guarded_t(const std::vector<std::uint8_t> &bytes):
        storage(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE + k_canary_bytes, 0),
        size(bytes.size()) {
      std::copy(bytes.begin(), bytes.end(), storage.begin());
      for (std::size_t i = 0; i < k_canary_bytes; ++i) {
        storage[size + AV_INPUT_BUFFER_PADDING_SIZE + i] = canary_byte(i);
      }
    }

    bool original_intact(const std::vector<std::uint8_t> &bytes) const {
      return bytes.size() == size && std::equal(bytes.begin(), bytes.end(), storage.begin());
    }

    bool padding_intact() const {
      for (std::size_t i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; ++i) {
        if (storage[size + i] != 0) {
          return false;
        }
      }
      return true;
    }

    bool canary_intact() const {
      for (std::size_t i = 0; i < k_canary_bytes; ++i) {
        if (storage[size + AV_INPUT_BUFFER_PADDING_SIZE + i] != canary_byte(i)) {
          return false;
        }
      }
      return true;
    }
  };

  struct packet_deleter {
    void operator()(AVPacket *p) const {
      av_packet_free(&p);
    }
  };

  void no_free(void *, std::uint8_t *) {
  }

  /// A packet over the guarded storage (no copy): a read-only buffer reference covering input and
  /// padding only. The storage must outlive every reference, including a CBS fragment's.
  std::unique_ptr<AVPacket, packet_deleter> packet_over(guarded_t &g, bool key) {
    std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};
    if (!packet) {
      return {};
    }
    packet->buf = av_buffer_create(g.storage.data(), g.size + AV_INPUT_BUFFER_PADDING_SIZE, no_free, nullptr, AV_BUFFER_FLAG_READONLY);
    if (!packet->buf) {
      return {};
    }
    packet->data = g.storage.data();
    packet->size = static_cast<int>(g.size);
    packet->flags = key ? AV_PKT_FLAG_KEY : 0;
    return packet;
  }

  struct cbs_context_deleter {
    void operator()(CodedBitstreamContext *c) const {
      ff_cbs_close(&c);
    }
  };

  struct fragment_t {
    CodedBitstreamFragment frag {};

    ~fragment_t() {
      ff_cbs_fragment_free(&frag);
    }
  };

  bool reader_active_sps(const CodedBitstreamContext *ctx, AVCodecID codec_id) {
    if (codec_id == AV_CODEC_ID_H264) {
      return static_cast<const CodedBitstreamH264Context *>(ctx->priv_data)->active_sps != nullptr;
    }
    return static_cast<const CodedBitstreamH265Context *>(ctx->priv_data)->active_sps != nullptr;
  }

  struct cbs_read_t {
    std::size_t push_index = 0;
    std::string read_return = "not reached";
    int unit_count = 0;
    std::string unit_types;
    std::string unit_bytes;
    bool active_sps = false;
    bool original_intact = false;
    bool padding_intact = false;
    bool canary_intact = false;
    std::size_t stripped_size = 0;
    std::string stripped_sha256;
    bool strip_canary_intact = false;
  };

  struct cbs_observed_t {
    std::string init_return = "not reached";
    std::vector<cbs_read_t> reads;
    std::string validate_sps_first = "not reached";
    bool validate_original_intact = false;
    bool validate_padding_intact = false;
    bool validate_canary_intact = false;
    bool final_integrity = false;  ///< Every guarded input re-checked after the last read
    std::string error;             ///< Allocation or setup failure (not a read result)
  };

  /// Read every CBS push in order through one persistent context. Asserts nothing.
  cbs_observed_t observe_cbs(const codec_case_t &c, const std::vector<push_t> &pushes) {
    cbs_observed_t observed;
    std::vector<std::size_t> indexes;
    std::vector<guarded_t> guarded;
    for (std::size_t i = 0; i < pushes.size(); ++i) {
      if (pushes[i].cbs) {
        indexes.push_back(i);
        guarded.emplace_back(pushes[i].bytes);
      }
    }
    if (guarded.empty()) {
      return observed;
    }

    // The product helper's own fresh reader, on the first CBS input only.
    {
      const auto packet = packet_over(guarded.front(), pushes[indexes.front()].key);
      if (!packet) {
        observed.error = "packet allocation failed for validate_sps";
        return observed;
      }
      observed.validate_sps_first = cbs::validate_sps(packet.get(), c.codec_id) ? "1" : "0";
    }
    observed.validate_original_intact = guarded.front().original_intact(pushes[indexes.front()].bytes);
    observed.validate_padding_intact = guarded.front().padding_intact();
    observed.validate_canary_intact = guarded.front().canary_intact();

    CodedBitstreamContext *raw = nullptr;
    const int init = ff_cbs_init(&raw, c.codec_id, nullptr);
    observed.init_return = std::to_string(init);
    std::unique_ptr<CodedBitstreamContext, cbs_context_deleter> ctx {raw};
    if (init < 0 || !ctx) {
      observed.error = "ff_cbs_init returned " + std::to_string(init);
      return observed;
    }
    if (c.codec_id == AV_CODEC_ID_H264) {
      ctx->decompose_unit_types = k_h264_decompose;
      ctx->nb_decompose_unit_types = static_cast<int>(std::size(k_h264_decompose));
    } else {
      ctx->decompose_unit_types = k_hevc_decompose;
      ctx->nb_decompose_unit_types = static_cast<int>(std::size(k_hevc_decompose));
    }

    {
      fragment_t fragment;
      for (std::size_t n = 0; n < guarded.size(); ++n) {
        const auto &push = pushes[indexes[n]];
        cbs_read_t read;
        read.push_index = indexes[n];

        // The product's in-place filler strip, on a separate copy with its own trailing canary.
        std::vector<std::uint8_t> copy(push.bytes.size() + k_canary_bytes);
        std::copy(push.bytes.begin(), push.bytes.end(), copy.begin());
        for (std::size_t i = 0; i < k_canary_bytes; ++i) {
          copy[push.bytes.size() + i] = canary_byte(i);
        }
        read.stripped_size = cbs::strip_filler_data(copy.data(), push.bytes.size(), c.codec_id);
        read.stripped_sha256 = read.stripped_size <= push.bytes.size() ? recorder_fixture::sha256_hex(copy.data(), read.stripped_size) : "";
        read.strip_canary_intact = true;
        for (std::size_t i = 0; i < k_canary_bytes; ++i) {
          read.strip_canary_intact = read.strip_canary_intact && copy[push.bytes.size() + i] == canary_byte(i);
        }

        const auto packet = packet_over(guarded[n], push.key);
        if (!packet) {
          observed.reads.push_back(read);
          observed.error = "packet allocation failed at push " + std::to_string(indexes[n]);
          return observed;
        }
        ff_cbs_fragment_reset(&fragment.frag);
        const int ret = ff_cbs_read_packet(ctx.get(), &fragment.frag, packet.get());
        read.read_return = std::to_string(ret);
        read.unit_count = fragment.frag.nb_units;
        for (int u = 0; u < fragment.frag.nb_units; ++u) {
          read.unit_types += (u ? "," : "") + std::to_string(fragment.frag.units[u].type);
          read.unit_bytes += (u ? "," : "") + std::to_string(fragment.frag.units[u].data_size);
        }
        // The persistent context stays initialized whatever this read returned, so its active-SPS
        // state is observed after every call, independently of the return (recorded, never asserted).
        read.active_sps = reader_active_sps(ctx.get(), c.codec_id);
        read.original_intact = guarded[n].original_intact(push.bytes);
        read.padding_intact = guarded[n].padding_intact();
        read.canary_intact = guarded[n].canary_intact();
        observed.reads.push_back(read);
      }
      // The fragment (and any buffer reference it holds) is released here, before the storage.
    }
    ctx.reset();
    observed.final_integrity = true;
    for (std::size_t n = 0; n < guarded.size(); ++n) {
      observed.final_integrity = observed.final_integrity && guarded[n].original_intact(pushes[indexes[n]].bytes) &&
                                 guarded[n].padding_intact() && guarded[n].canary_intact();
    }
    return observed;
  }

  // ---------------------------------------------------------------------------------------------
  // Raw evidence: exclusive creation only.
  // ---------------------------------------------------------------------------------------------

  /// Write `bytes` to a new file; fails if it exists or on any short write or close error.
  std::string write_exclusive(const fs::path &path, const std::uint8_t *data, std::size_t size) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
      return "cannot create " + path.string() + ": " + std::strerror(errno);
    }
    std::size_t done = 0;
    while (done < size) {
      const auto n = ::write(fd, data + done, size - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        const int error = errno;
        ::close(fd);
        return "short write to " + path.string() + ": " + std::strerror(error);
      }
      done += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
      return "close failed for " + path.string() + ": " + std::strerror(errno);
    }
    return {};
  }

  std::string write_exclusive(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    return write_exclusive(path, bytes.data(), bytes.size());
  }

  std::string write_exclusive(const fs::path &path, const std::string &text) {
    return write_exclusive(path, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }

  std::string two_digits(std::size_t i) {
    return (i < 10 ? "0" : "") + std::to_string(i);
  }

  // ---------------------------------------------------------------------------------------------
  // The fixture: one replay configuration in a directory this fixture created exclusively.
  // ---------------------------------------------------------------------------------------------

  class RecorderBitstreamNegativeTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_config = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-bitstream-negative-"} + info->name() + "-" +
                                       std::to_string(::getpid()) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      output_dir = owned_parent / "recordings";
      config::recording.output_dir = output_dir.string();
      config::recording.replay_buffer_minutes = 1;
      config::recording.enabled = true;
      config::recording.replay_buffer = true;
      stream_recorder::load_config();
      stream_recorder::detail::save_hooks_t hooks;
      hooks.name_stamp = [] {
        return std::string {k_stamp};
      };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    void TearDown() override {
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_config;
      stream_recorder::load_config();
      if (!owned_parent.empty()) {
        std::error_code ec;
        fs::remove_all(owned_parent, ec);
      }
    }

    void run_case(const case_t &k) {
      // Every encoder run must hold its frozen baseline before anything else is attempted.
      bool encoded = !k.runs.empty();
      for (const auto *e : k.runs) {
        encoded = encoded && e->ok;
      }
      const bool ready = encoded && k.recipe_error.empty();

      // CBS observations (CBS cases only), on the codec of the first CBS push.
      cbs_observed_t cbs;
      const codec_case_t *cbs_codec = nullptr;
      for (const auto &p : k.pushes) {
        if (p.cbs) {
          cbs_codec = p.video_format == 0 ? &k_h264 : &k_hevc;
          break;
        }
      }
      if (ready && cbs_codec) {
        cbs = observe_cbs(*cbs_codec, k.pushes);
      }

      // Replay pushes, session events and the save.
      std::vector<std::uint8_t> expected;
      stream_recorder::replay_stats_t stats;
      stream_recorder::detail::save_result_t result;
      stream_recorder::detail::save_result_t held_admission;
      bool used_held_save = false;
      std::vector<std::uint8_t> file;
      std::vector<std::pair<std::string, stream_recorder::replay_stats_t>> snapshots;
      std::string push_result = "not run";
      const steady::time_point read_at = t0 + 1s + k_arrival_step * static_cast<int>(k.pushes.size());
      if (ready) {
        stream_recorder::session_started(k_session, true, k.session_format);
        stream_recorder::detail::admitted_save_t held;
        for (std::size_t i = 0; i < k.pushes.size(); ++i) {
          const auto &p = k.pushes[i];
          const auto at = t0 + k_arrival_step * static_cast<int>(i);
          if (p.before == session_event_e::end_selected) {
            stream_recorder::session_ended(k_session);
          } else if (p.before == session_event_e::start_restarted) {
            stream_recorder::session_started(k_restarted, true, p.video_format);
          } else if (p.before == session_event_e::admit_save) {
            auto admission = stream_recorder::detail::admit_replay_save(at);
            held_admission = admission.result;
            held = std::move(admission.save);
            used_held_save = true;
          } else if (p.before == session_event_e::start_overlap) {
            stream_recorder::session_started(k_overlap, true, p.video_format);
          } else if (p.before == session_event_e::end_overlap) {
            stream_recorder::session_ended(k_overlap);
          }
          if (!p.snapshot_before.empty()) {
            snapshots.emplace_back(p.snapshot_before, stream_recorder::detail::replay_stats_at(at));
          }
          if (p.null_payload) {
            stream_recorder::detail::push_packet_at(nullptr, k_invalid_size, true, p.key, 0, p.generation, p.video_format, at);
          } else if (p.bytes.empty()) {
            static const std::uint8_t one = 0;
            stream_recorder::detail::push_packet_at(&one, 0, true, p.key, 0, p.generation, p.video_format, at);
          } else {
            stream_recorder::packet_provenance_t provenance;
            provenance.frame_index = k_producer_counter_base + static_cast<std::int64_t>(i);
            stream_recorder::detail::push_video_at(p.bytes.data(), p.bytes.size(), p.key, p.generation, p.video_format,
                                                   provenance, at);
          }
          if (!p.snapshot_after.empty()) {
            snapshots.emplace_back(p.snapshot_after, stream_recorder::detail::replay_stats_at(at));
          }
          if (p.in_save) {
            expected.insert(expected.end(), p.bytes.begin(), p.bytes.end());
          }
        }
        push_result = "ok";
        stats = stream_recorder::detail::replay_stats_at(read_at);
        if (used_held_save) {
          result = stream_recorder::detail::complete_replay_save(std::move(held));
        } else {
          result = stream_recorder::detail::save_replay_result_at(read_at);
        }
        if (result.status == stream_recorder::detail::save_status_e::saved) {
          std::ifstream in(result.path, std::ios::binary);
          file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
      }
      const bool saved = result.status == stream_recorder::detail::save_status_e::saved;
      const std::string file_equal = !saved ? "not saved" : (file == expected ? "identical" : "differs");
      const bool in_owned_dir = saved && fs::path(result.path).parent_path() == output_dir;

      // Raw evidence, written before any assertion so a failing case still leaves what it observed.
      const std::string evidence = ready ? write_evidence(k, expected, file, saved) : "not written: the case was not ready";

      properties_t properties;
      const auto add = [&properties](std::string key, std::string value) {
        properties.emplace_back(std::move(key), std::move(value));
      };
      add("scope", "negative and later-key inputs built from real fixture packets; replay admission, counters, save status and byte "
                   "transparency asserted; CBS results recorded only; no parser validity, refusal, decode, playback or memory-safety claim");
      add("case", k.name);
      add("caseIds", k.ids);
      add("summary", k.summary);
      add("processUid", std::to_string(::getuid()));
      add("recipeVersion", k_recipe_version);
      add("ffmpegVersionInfo", av_version_info());
      add("libavcodecVersion", std::to_string(avcodec_version()));
      add("libavcodecIdent", LIBAVCODEC_IDENT);
#if defined(__VERSION__)
      add("compilerVersion", __VERSION__);
#else
      add("compilerVersion", "unavailable");
#endif
      add("memorySafetyNote", "guard-byte checks detect stray writes into test-owned padding and canaries only; FFmpeg padding is "
                              "permitted read access; no instrumented-library bounds or memory-safety gate has run");
      add("ptsNote", "packet PTS is the fixture's synthetic frame counter; provenance frame_index values are synthetic producer counters " +
                       std::to_string(k_producer_counter_base) + "+push index; neither is a product clock or presentation time");
      add("arrivalClock", "controlled steady clock: t0 + push index * 16 ms; not a presentation time");
      add("sessionGeneration", std::to_string(k_session));
      add("sessionStartFormat", std::to_string(k.session_format));
      add("recipeError", k.recipe_error);
      for (std::size_t r = 0; r < k.runs.size(); ++r) {
        const auto &e = *k.runs[r];
        const std::string pre = std::string {"run"} + std::to_string(r);
        add(pre + "Codec", e.codec->label);
        add(pre + "Encoder", e.codec->spec->name);
        add(pre + "EncoderParams", e.codec->spec->params_value);
        add(pre + "InputSize", std::to_string(recorder_fixture::k_width) + "x" + std::to_string(recorder_fixture::k_height) + " yuv420p");
        add(pre + "EncodeResult", e.result.empty() ? "ok" : e.result);
        add(pre + "BaselineResult", e.baseline.empty() ? "ok" : e.baseline);
        add(pre + "PacketCount", std::to_string(e.run.packets.size()));
        for (std::size_t i = 0; i < e.run.packets.size(); ++i) {
          const auto &p = e.run.packets[i];
          const std::string pk = pre + "Packet" + std::to_string(i);
          add(pk + "Size", std::to_string(p.size));
          add(pk + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
          add(pk + "SyntheticPts", std::to_string(p.pts));
          add(pk + "Sha256", p.sha256);
        }
      }
      add("pushCount", std::to_string(k.pushes.size()));
      for (std::size_t i = 0; i < k.pushes.size(); ++i) {
        const auto &p = k.pushes[i];
        const std::string pk = "push" + std::to_string(i);
        add(pk + "Provenance", p.provenance);
        add(pk + "Source", p.source);
        add(pk + "Recipe", p.recipe);
        add(pk + "BeforeEvent", p.before == session_event_e::end_selected ? "session_ended(selected)" :
                                p.before == session_event_e::start_restarted ? "session_started(" + std::to_string(k_restarted) + ")" :
                                p.before == session_event_e::admit_save ? "admit_replay_save" :
                                p.before == session_event_e::start_overlap ? "session_started(" + std::to_string(k_overlap) + ", eligible)" :
                                p.before == session_event_e::end_overlap ? "session_ended(" + std::to_string(k_overlap) + ")" : "none");
        add(pk + "SnapshotBefore", p.snapshot_before);
        add(pk + "SnapshotAfter", p.snapshot_after);
        add(pk + "Payload", p.null_payload ? "null pointer, size " + std::to_string(k_invalid_size) : "bytes");
        add(pk + "Size", std::to_string(p.bytes.size()));
        add(pk + "Sha256", p.null_payload ? "" : sha(p.bytes));
        add(pk + "Key", p.key ? "1" : "0");
        add(pk + "VideoFormat", std::to_string(p.video_format));
        add(pk + "Generation", std::to_string(p.generation));
        add(pk + "Expected", p.expected);
        add(pk + "InSave", p.in_save ? "1" : "0");
      }
      const bool cbs_case = cbs_codec != nullptr;
      add("cbsCase", cbs_case ? "1" : "0");
      if (cbs_case) {
        add("cbsCodec", cbs_codec->label);
        add("cbsContext", "one persistent context for every CBS push, in push order; the fragment is reset between packets");
        add("cbsDecompose", "parameter sets and slice headers, as src/cbs.cpp");
        add("cbsSetupError", cbs.error);
        add("cbsInitReturn", cbs.init_return);
        add("productValidateSpsActiveVuiPresentFirstInput", cbs.validate_sps_first);
        add("validateSpsOriginalIntact", cbs.validate_original_intact ? "1" : "0");
        add("validateSpsPaddingIntact", cbs.validate_padding_intact ? "1" : "0");
        add("validateSpsCanaryIntact", cbs.validate_canary_intact ? "1" : "0");
        add("cbsReadCount", std::to_string(cbs.reads.size()));
        for (const auto &r : cbs.reads) {
          const std::string pk = "cbsPush" + std::to_string(r.push_index);
          add(pk + "ReadReturn", r.read_return);
          add(pk + "UnitCount", std::to_string(r.unit_count));
          add(pk + "UnitTypes", r.unit_types);
          add(pk + "UnitBytes", r.unit_bytes);
          add(pk + "ActiveSpsAfterRead", r.active_sps ? "1" : "0");
          add(pk + "OriginalIntact", r.original_intact ? "1" : "0");
          add(pk + "PaddingIntact", r.padding_intact ? "1" : "0");
          add(pk + "CanaryIntact", r.canary_intact ? "1" : "0");
          add(pk + "StrippedSize", std::to_string(r.stripped_size));
          add(pk + "StrippedSha256", r.stripped_sha256);
          add(pk + "StripCanaryIntact", r.strip_canary_intact ? "1" : "0");
        }
        add("cbsFinalIntegrity", cbs.final_integrity ? "1" : "0");
      }
      add("pushResult", push_result);
      add("liveReplayPackets", std::to_string(stats.packets));
      add("selectedGeneration", std::to_string(stats.selected_generation));
      add("runVideoFormat", std::to_string(stats.run_video_format));
      add("refusedUnselected", std::to_string(stats.refused_unselected));
      add("refusedInvalid", std::to_string(stats.refused_invalid));
      add("refusedOversized", std::to_string(stats.refused_oversized));
      add("refusedAwaitingKeyframe", std::to_string(stats.refused_awaiting_keyframe));
      add("discontinuities", std::to_string(stats.discontinuities));
      add("lastWindowLimit", limit_name(stats.last_window_limit));
      add("snapshotCount", std::to_string(snapshots.size()));
      for (std::size_t s = 0; s < snapshots.size(); ++s) {
        const auto &[label, v] = snapshots[s];
        const std::string pk = "snapshot" + std::to_string(s);
        add(pk + "Label", label);
        add(pk + "RegisteredSessions", std::to_string(v.registered_sessions));
        add(pk + "SelectedGeneration", std::to_string(v.selected_generation));
        add(pk + "Packets", std::to_string(v.packets));
        add(pk + "RefusedUnselected", std::to_string(v.refused_unselected));
        add(pk + "RefusedAwaitingKeyframe", std::to_string(v.refused_awaiting_keyframe));
        add(pk + "Discontinuities", std::to_string(v.discontinuities));
        add(pk + "AwaitingKeyframe", v.awaiting_keyframe ? "1" : "0");
        add(pk + "KeyframeStartRun", v.keyframe_start_run ? "1" : "0");
        add(pk + "RunVideoFormat", std::to_string(v.run_video_format));
        add(pk + "LastWindowLimit", limit_name(v.last_window_limit));
      }
      add("heldSave", used_held_save ? "1" : "0");
      add("heldAdmissionAdmitted", used_held_save ? (held_admission.admitted ? "1" : "0") : "");
      add("heldAdmissionPackets", used_held_save ? std::to_string(held_admission.packets) : "");
      add("heldAdmissionVideoFormat", used_held_save ? std::to_string(held_admission.video_format) : "");
      add("saveStatus", ready ? status_name(result.status) : "not run");
      add("saveError", result.error);
      add("saveInOwnedDirectory", in_owned_dir ? "1" : "0");
      add("savePackets", std::to_string(result.packets));
      add("saveBytes", std::to_string(result.bytes));
      add("saveKeyframes", std::to_string(result.keyframes));
      add("saveVideoFormat", std::to_string(result.video_format));
      add("saveGeneration", std::to_string(result.generation));
      add("expectedBytes", std::to_string(expected.size()));
      add("expectedSha256", ready ? sha(expected) : "");
      add("fileBytes", std::to_string(file.size()));
      add("fileSha256", saved ? sha(file) : "");
      add("fileEqualsAdmittedPushes", file_equal);
      add("evidenceDirectory", k.name);
      add("evidenceResult", evidence.empty() ? "ok" : evidence);
      recorder_fixture::record_properties(k_marker, properties);

      for (const auto *e : k.runs) {
        ASSERT_EQ(e->result, "") << e->codec->label << " encoder run";
        ASSERT_EQ(e->baseline, "") << e->codec->label << " frozen encoder baseline";
      }
      ASSERT_EQ(k.recipe_error, "") << "recipe precondition";
      EXPECT_EQ(evidence, "") << "raw evidence";

      if (cbs_case) {
        // Completion and test-owned integrity only. Read returns and units are observations.
        EXPECT_EQ(cbs.error, "") << "CBS setup";
        EXPECT_TRUE(cbs.validate_original_intact && cbs.validate_padding_intact && cbs.validate_canary_intact)
          << "validate_sps changed test-owned input, padding or canary bytes";
        std::size_t cbs_pushes = 0;
        for (const auto &p : k.pushes) {
          cbs_pushes += p.cbs ? 1 : 0;
        }
        EXPECT_EQ(cbs.reads.size(), cbs_pushes) << "every CBS input must be read";
        for (const auto &r : cbs.reads) {
          EXPECT_TRUE(r.original_intact && r.padding_intact && r.canary_intact)
            << "ff_cbs_read_packet changed test-owned input, padding or canary bytes at push " << r.push_index;
          EXPECT_TRUE(r.strip_canary_intact) << "strip_filler_data wrote past its copy at push " << r.push_index;
          EXPECT_LE(r.stripped_size, k.pushes[r.push_index].bytes.size()) << "strip_filler_data grew push " << r.push_index;
        }
        EXPECT_TRUE(cbs.final_integrity) << "a guarded input changed after the last read";
      }

      const auto &x = k.expect;
      EXPECT_EQ(stats.packets, x.live_packets) << "live replay packets";
      EXPECT_EQ(stats.selected_generation, x.selected_generation);
      EXPECT_EQ(stats.run_video_format, x.run_video_format);
      EXPECT_EQ(stats.refused_unselected, x.refused_unselected);
      EXPECT_EQ(stats.refused_invalid, x.refused_invalid);
      EXPECT_EQ(stats.refused_oversized, x.refused_oversized);
      EXPECT_EQ(stats.refused_awaiting_keyframe, x.refused_awaiting_keyframe);
      EXPECT_EQ(stats.discontinuities, x.discontinuities);
      if (x.assert_limit) {
        EXPECT_STREQ(limit_name(stats.last_window_limit), limit_name(x.last_window_limit));
      }
      ASSERT_EQ(snapshots.size(), k.snapshots.size()) << "every expected intermediate snapshot must be taken";
      for (std::size_t s = 0; s < snapshots.size(); ++s) {
        const auto &[label, v] = snapshots[s];
        const auto &w = k.snapshots[s];
        EXPECT_EQ(label, w.label);
        EXPECT_EQ(v.registered_sessions, w.registered_sessions) << label;
        EXPECT_EQ(v.selected_generation, w.selected_generation) << label;
        EXPECT_EQ(v.packets, w.packets) << label;
        EXPECT_EQ(v.refused_unselected, w.refused_unselected) << label;
        EXPECT_EQ(v.refused_awaiting_keyframe, w.refused_awaiting_keyframe) << label;
        EXPECT_EQ(v.discontinuities, w.discontinuities) << label;
        EXPECT_EQ(v.awaiting_keyframe, w.awaiting_keyframe) << label;
        EXPECT_EQ(v.keyframe_start_run, w.keyframe_start_run) << label;
        EXPECT_STREQ(limit_name(v.last_window_limit), limit_name(w.last_window_limit)) << label;
      }
      if (used_held_save) {
        EXPECT_TRUE(held_admission.admitted) << "the held save must be admitted";
        EXPECT_EQ(held_admission.packets, x.save_packets);
        EXPECT_EQ(held_admission.video_format, x.save_video_format);
      }
      ASSERT_TRUE(saved) << status_name(result.status) << " " << result.error;
      EXPECT_TRUE(in_owned_dir) << result.path;
      EXPECT_EQ(result.packets, x.save_packets);
      EXPECT_EQ(result.keyframes, x.save_keyframes);
      EXPECT_EQ(result.video_format, x.save_video_format);
      EXPECT_EQ(result.generation, x.save_generation);
      EXPECT_EQ(result.bytes, expected.size());
      EXPECT_EQ(file.size(), expected.size());
      EXPECT_EQ(file_equal, "identical") << "the saved file must be the admitted pushes, in push order, byte for byte";
    }

    /// Per-case raw evidence; an empty string when every file was created and written.
    std::string write_evidence(const case_t &k, const std::vector<std::uint8_t> &expected,
                               const std::vector<std::uint8_t> &file, bool saved) const {
      const char *root = std::getenv(k_evidence_env);
      if (!root || !*root) {
        return std::string {k_evidence_env} + " is not set";
      }
      const fs::path base {root};
      std::error_code ec;
      if (!base.is_absolute() || !fs::is_directory(base, ec)) {
        return std::string {k_evidence_env} + " is not an existing absolute directory: " + base.string();
      }
      const fs::path dir = base / k.name;
      if (!fs::create_directory(dir, ec)) {
        return "the case directory already exists or cannot be created: " + dir.string();
      }

      std::string manifest = "{\"marker\":" + recorder_fixture::json_string(k_marker) + ",\"case\":" + recorder_fixture::json_string(k.name) +
                             ",\"caseIds\":" + recorder_fixture::json_string(k.ids) +
                             ",\"recipeVersion\":" + recorder_fixture::json_string(k_recipe_version) + ",\"sources\":[";
      bool first = true;
      for (const auto *e : k.runs) {
        for (std::size_t i = 0; i < e->run.packets.size(); ++i) {
          const auto &p = e->run.packets[i];
          const std::string name = std::string {"source-"} + e->codec->tag + "-" + two_digits(i) + ".bin";
          if (auto error = write_exclusive(dir / name, p.bytes); !error.empty()) {
            return error;
          }
          manifest += std::string {first ? "" : ","} + "{\"file\":" + recorder_fixture::json_string(name) +
                      ",\"source\":" + recorder_fixture::json_string(std::string {e->codec->tag} + ":" + std::to_string(i)) +
                      ",\"key\":" + ((p.flags & AV_PKT_FLAG_KEY) ? "true" : "false") + ",\"size\":" + std::to_string(p.bytes.size()) +
                      ",\"sha256\":" + recorder_fixture::json_string(p.sha256) + "}";
          first = false;
        }
      }
      manifest += "],\"pushes\":[";
      for (std::size_t i = 0; i < k.pushes.size(); ++i) {
        const auto &p = k.pushes[i];
        std::string name;
        if (!p.null_payload) {
          name = "push-" + two_digits(i) + ".bin";
          if (auto error = write_exclusive(dir / name, p.bytes); !error.empty()) {
            return error;
          }
        }
        manifest += std::string {i ? "," : ""} + "{\"index\":" + std::to_string(i) +
                    ",\"file\":" + (name.empty() ? std::string {"null"} : recorder_fixture::json_string(name)) +
                    ",\"provenance\":" + recorder_fixture::json_string(p.provenance) + ",\"source\":" + recorder_fixture::json_string(p.source) +
                    ",\"recipe\":" + recorder_fixture::json_string(p.recipe) +
                    ",\"nullPayload\":" + (p.null_payload ? "true" : "false") + ",\"size\":" + std::to_string(p.bytes.size()) +
                    ",\"sha256\":" + (p.null_payload ? std::string {"null"} : recorder_fixture::json_string(sha(p.bytes))) +
                    ",\"key\":" + (p.key ? "true" : "false") + ",\"videoFormat\":" + std::to_string(p.video_format) +
                    ",\"generation\":" + std::to_string(p.generation) + ",\"expected\":" + recorder_fixture::json_string(p.expected) +
                    ",\"inSave\":" + (p.in_save ? "true" : "false") + ",\"cbs\":" + (p.cbs ? "true" : "false") + "}";
      }
      if (auto error = write_exclusive(dir / "admitted-concatenation.bin", expected); !error.empty()) {
        return error;
      }
      if (saved) {
        if (auto error = write_exclusive(dir / "saved.bin", file); !error.empty()) {
          return error;
        }
      }
      manifest += "],\"admittedConcatenation\":{\"file\":\"admitted-concatenation.bin\",\"size\":" + std::to_string(expected.size()) +
                  ",\"sha256\":" + recorder_fixture::json_string(sha(expected)) + "},\"saved\":" +
                  (saved ? "{\"file\":\"saved.bin\",\"size\":" + std::to_string(file.size()) + ",\"sha256\":" +
                             recorder_fixture::json_string(sha(file)) + "}" :
                           std::string {"null"}) +
                  "}\n";
      return write_exclusive(dir / "manifest.json", manifest);
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved_config;
    fs::path owned_parent;
    fs::path output_dir;
  };

  // ---------------------------------------------------------------------------------------------
  // The closed case inventory. Each builder returns one case from real runs encoded in the test body.
  // ---------------------------------------------------------------------------------------------

  /// Real 0..7 with push 0 replaced by `recipe` (N1, N3, N4) or push `index` replaced (L1, L3).
  case_t replace_one(const encoded_t &e, std::string name, std::string ids, std::string summary, std::size_t index,
                     recipe_t recipe) {
    case_t k;
    k.name = std::move(name);
    k.ids = std::move(ids);
    k.summary = std::move(summary);
    k.session_format = e.codec->video_format;
    k.runs = {&e};
    if (!e.ok) {
      return k;
    }
    k.recipe_error = recipe.error;
    for (std::size_t i = 0; i < e.run.packets.size(); ++i) {
      k.pushes.push_back(i == index ? constructed_push(e, i, recipe, true) : real_push(e, i, true, true));
    }
    k.expect.live_packets = e.run.packets.size();
    k.expect.run_video_format = e.codec->video_format;
    k.expect.save_packets = e.run.packets.size();
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = e.codec->video_format;
    return k;
  }

  /// N5: real 0..3, one invalid owner frame (null payload or zero size), then real 4..7.
  case_t invalid_owner_frame(const encoded_t &e, std::string name, bool null_payload) {
    case_t k;
    k.name = std::move(name);
    k.ids = "N5";
    k.summary = null_payload ? "a null-payload owner frame between real packets breaks the run until the next real key"
                             : "a zero-size owner frame between real packets breaks the run until the next real key";
    k.session_format = e.codec->video_format;
    k.runs = {&e};
    if (!e.ok) {
      return k;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      k.pushes.push_back(real_push(e, i, false, false));
    }
    push_t bad;
    bad.provenance = "synthetic";
    bad.source = "none";
    bad.recipe = null_payload ? "null payload pointer with size 10" : "zero-size payload";
    bad.null_payload = null_payload;
    bad.key = false;
    bad.video_format = e.codec->video_format;
    bad.expected = "refused-invalid";
    k.pushes.push_back(bad);
    for (std::size_t i = 4; i < e.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(e, i, true, false));
    }
    k.expect.live_packets = 4;
    k.expect.refused_invalid = 1;
    k.expect.discontinuities = 1;
    k.expect.run_video_format = e.codec->video_format;
    k.expect.assert_limit = true;
    k.expect.last_window_limit = stream_recorder::window_limit_e::discontinuity;
    k.expect.save_packets = 4;
    k.expect.save_keyframes = 1;
    k.expect.save_video_format = e.codec->video_format;
    return k;
  }

  /// L4: real 1..7 only, with packet 1 flagged key: the flag, not the content, starts the run.
  case_t flag_lie(const encoded_t &e, std::string name) {
    case_t k;
    k.name = std::move(name);
    k.ids = "L4";
    k.summary = "a real dependent packet flagged key starts the run; the recorder trusts the flag and saves verbatim";
    k.session_format = e.codec->video_format;
    k.runs = {&e};
    if (!e.ok) {
      return k;
    }
    for (std::size_t i = 1; i < e.run.packets.size(); ++i) {
      auto p = real_push(e, i, true, false);
      if (i == 1) {
        p.provenance = "constructed-from-real";
        p.recipe = std::string {k_recipe_version} + " flag-lie: unmodified bytes of packet 1 pushed with is_keyframe=true";
        p.key = true;
      }
      k.pushes.push_back(p);
    }
    k.expect.live_packets = e.run.packets.size() - 1;
    k.expect.run_video_format = e.codec->video_format;
    k.expect.save_packets = e.run.packets.size() - 1;
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = e.codec->video_format;
    return k;
  }

  /// F2: the selected session's real 0..7, each followed by the same packet from a never-registered generation.
  case_t foreign_generation(const encoded_t &e, std::string name) {
    case_t k;
    k.name = std::move(name);
    k.ids = "F2";
    k.summary = "a never-registered generation's real packets interleave the selected run and are refused as unselected";
    k.session_format = e.codec->video_format;
    k.runs = {&e};
    if (!e.ok) {
      return k;
    }
    for (std::size_t i = 0; i < e.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(e, i, true, false));
      auto foreign = real_push(e, i, false, false);
      foreign.generation = k_foreign;
      foreign.expected = "refused-unselected";
      k.pushes.push_back(foreign);
    }
    k.expect.live_packets = e.run.packets.size();
    k.expect.refused_unselected = e.run.packets.size();
    k.expect.run_video_format = e.codec->video_format;
    k.expect.save_packets = e.run.packets.size();
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = e.codec->video_format;
    return k;
  }

  /**
   * F5 (root R4): a second REGISTERED eligible session overlaps the selected one. Source facts
   * (src/stream_recorder.cpp at M): session_started() of a second session reselects to none and
   * clears the run with cause `selection` (reselect_replay_session); packets of an unselected
   * generation are refused as unselected before anything else (append_replay_packet);
   * session_ended() clears the run with cause `selection` and reselects the sole remaining eligible
   * session; an empty run refuses a dependent frame as awaiting a keyframe. None of these counts a
   * discontinuity. Sequence: real 0-3 (selected), register k_overlap, first-owner real key 4 and
   * second-owner real key 0 (both refused), end k_overlap, first-owner real dependent 3 (refused,
   * awaiting a key), then first-owner real 4-7 from the fresh key 4: only those four are saved.
   */
  case_t registered_overlap(const encoded_t &e, std::string name) {
    case_t k;
    k.name = std::move(name);
    k.ids = "F5";
    k.summary = "a second registered session suspends replay (both owners refused); after it ends, a dependent frame is refused "
                "and the first session recovers only from a fresh real key";
    k.session_format = e.codec->video_format;
    k.runs = {&e};
    if (!e.ok) {
      return k;
    }
    const int fmt = e.codec->video_format;
    for (std::size_t i = 0; i < 4; ++i) {
      k.pushes.push_back(real_push(e, i, false, false));
    }
    auto first_key = real_push(e, 4, false, false);
    first_key.before = session_event_e::start_overlap;
    first_key.snapshot_before = "overlapStarted";
    first_key.expected = "refused-unselected";
    k.pushes.push_back(first_key);
    auto second_key = real_push(e, 0, false, false);
    second_key.generation = k_overlap;
    second_key.expected = "refused-unselected";
    second_key.snapshot_after = "overlapRefusedBothOwners";
    k.pushes.push_back(second_key);
    auto dependent = real_push(e, 3, false, false);
    dependent.before = session_event_e::end_overlap;
    dependent.snapshot_before = "overlapEnded";
    dependent.expected = "refused-awaiting-keyframe";
    dependent.snapshot_after = "dependentRefused";
    k.pushes.push_back(dependent);
    for (std::size_t i = 4; i < e.run.packets.size(); ++i) {
      auto p = real_push(e, i, true, false);
      if (i == 4) {
        p.snapshot_after = "freshKeyRecovered";
      }
      k.pushes.push_back(p);
    }
    using L = stream_recorder::window_limit_e;
    k.snapshots = {
      {"overlapStarted", 2, 0, 0, 0, 0, 0, false, false, L::selection},
      {"overlapRefusedBothOwners", 2, 0, 0, 2, 0, 0, false, false, L::selection},
      {"overlapEnded", 1, k_session, 0, 2, 0, 0, true, false, L::selection},
      {"dependentRefused", 1, k_session, 0, 2, 1, 0, true, false, L::selection},
      {"freshKeyRecovered", 1, k_session, 1, 2, 1, 0, false, true, L::selection},
    };
    k.expect.live_packets = 4;
    k.expect.refused_unselected = 2;
    k.expect.refused_awaiting_keyframe = 1;
    k.expect.discontinuities = 0;
    k.expect.run_video_format = fmt;
    k.expect.assert_limit = true;
    k.expect.last_window_limit = L::selection;
    k.expect.save_packets = 4;
    k.expect.save_keyframes = 1;
    k.expect.save_video_format = fmt;
    return k;
  }

  /// F1: real H.264 0..7, then real HEVC 0..7, one session: the codec change keeps only HEVC.
  case_t codec_change(const encoded_t &h264, const encoded_t &hevc) {
    case_t k;
    k.name = "F1H264ThenHevcInOneSessionSavesOnlyHevc";
    k.ids = "F1";
    k.summary = "a codec change inside one session clears the H.264 run; the save holds only the HEVC run";
    k.session_format = 0;
    k.runs = {&h264, &hevc};
    if (!h264.ok || !hevc.ok) {
      return k;
    }
    for (std::size_t i = 0; i < h264.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(h264, i, false, false));
    }
    for (std::size_t i = 0; i < hevc.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(hevc, i, true, false));
    }
    k.expect.live_packets = hevc.run.packets.size();
    k.expect.discontinuities = 1;
    k.expect.run_video_format = 1;
    k.expect.assert_limit = true;
    k.expect.last_window_limit = stream_recorder::window_limit_e::codec;
    k.expect.save_packets = hevc.run.packets.size();
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = 1;
    return k;
  }

  /// F3: real H.264 0..7 by the selected session, it ends, its packet 0 is refused, then a new session sends 0..7.
  case_t session_restart(const encoded_t &h264) {
    case_t k;
    k.name = "F3SessionRestartSavesOnlyTheNewGeneration";
    k.ids = "F3";
    k.summary = "after the selected session ends, its packets are refused and a newly started session's run is the save";
    k.session_format = 0;
    k.runs = {&h264};
    if (!h264.ok) {
      return k;
    }
    for (std::size_t i = 0; i < h264.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(h264, i, false, false));
    }
    auto late = real_push(h264, 0, false, false);
    late.before = session_event_e::end_selected;
    late.expected = "refused-unselected";
    k.pushes.push_back(late);
    for (std::size_t i = 0; i < h264.run.packets.size(); ++i) {
      auto p = real_push(h264, i, true, false);
      p.generation = k_restarted;
      if (i == 0) {
        p.before = session_event_e::start_restarted;
      }
      k.pushes.push_back(p);
    }
    k.expect.live_packets = h264.run.packets.size();
    k.expect.refused_unselected = 1;
    k.expect.selected_generation = k_restarted;
    k.expect.run_video_format = 0;
    k.expect.assert_limit = true;
    k.expect.last_window_limit = stream_recorder::window_limit_e::selection;
    k.expect.save_packets = h264.run.packets.size();
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = 0;
    k.expect.save_generation = k_restarted;
    return k;
  }

  /// F4: real H.264 0..7, a held save admitted, then real HEVC 0..7 changes the live codec before publication.
  case_t held_save_across_codec_change(const encoded_t &h264, const encoded_t &hevc) {
    case_t k;
    k.name = "F4AdmittedSaveKeepsItsCodecAcrossALaterCodecChange";
    k.ids = "F4";
    k.summary = "a save admitted before a codec change publishes only its captured H.264 run (existing admit/complete seam)";
    k.session_format = 0;
    k.runs = {&h264, &hevc};
    if (!h264.ok || !hevc.ok) {
      return k;
    }
    for (std::size_t i = 0; i < h264.run.packets.size(); ++i) {
      k.pushes.push_back(real_push(h264, i, true, false));
    }
    for (std::size_t i = 0; i < hevc.run.packets.size(); ++i) {
      auto p = real_push(hevc, i, false, false);
      if (i == 0) {
        p.before = session_event_e::admit_save;
      }
      k.pushes.push_back(p);
    }
    k.expect.live_packets = hevc.run.packets.size();
    k.expect.discontinuities = 1;
    k.expect.run_video_format = 1;
    k.expect.assert_limit = true;
    k.expect.last_window_limit = stream_recorder::window_limit_e::codec;
    k.expect.save_packets = h264.run.packets.size();
    k.expect.save_keyframes = 2;
    k.expect.save_video_format = 0;
    return k;
  }
}  // namespace

// N1 (CBS) and N2 (replay of the same input) are one case: a truncated real key, then real 1..7.
TEST_F(RecorderBitstreamNegativeTests, H264N1TruncatedHalfKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264N1TruncatedHalfKeyIsReadAndSavedVerbatim", "N1,N2", "key packet 0 cut in half",
                       0, e.ok ? trunc_half(e.run.packets[0].bytes) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN1TruncatedHalfKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcN1TruncatedHalfKeyIsReadAndSavedVerbatim", "N1,N2", "key packet 0 cut in half",
                       0, e.ok ? trunc_half(e.run.packets[0].bytes) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264N1TruncatedInsideSpsIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264N1TruncatedInsideSpsIsReadAndSavedVerbatim", "N1,N2", "key packet 0 cut inside its SPS",
                       0, e.ok ? trunc_in_sps(e.run.packets[0].bytes, AV_CODEC_ID_H264) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN1TruncatedInsideSpsIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcN1TruncatedInsideSpsIsReadAndSavedVerbatim", "N1,N2", "key packet 0 cut inside its SPS",
                       0, e.ok ? trunc_in_sps(e.run.packets[0].bytes, AV_CODEC_ID_HEVC) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264N3StartCodesStrippedKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264N3StartCodesStrippedKeyIsReadAndSavedVerbatim", "N3", "key packet 0 without Annex B start codes",
                       0, e.ok ? strip_start_codes(e.run.packets[0].bytes) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN3StartCodesStrippedKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcN3StartCodesStrippedKeyIsReadAndSavedVerbatim", "N3", "key packet 0 without Annex B start codes",
                       0, e.ok ? strip_start_codes(e.run.packets[0].bytes) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264N4GarbageKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264N4GarbageKeyIsReadAndSavedVerbatim", "N4", "seeded garbage of packet 0's length pushed as the key",
                       0, e.ok ? garbage(e.run.packets[0].bytes.size(), k_garbage_seed) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN4GarbageKeyIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcN4GarbageKeyIsReadAndSavedVerbatim", "N4", "seeded garbage of packet 0's length pushed as the key",
                       0, e.ok ? garbage(e.run.packets[0].bytes.size(), k_garbage_seed) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264N5NullPayloadOwnerFrameBreaksTheRun) {
  const auto e = encode(k_h264);
  run_case(invalid_owner_frame(e, "H264N5NullPayloadOwnerFrameBreaksTheRun", true));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN5NullPayloadOwnerFrameBreaksTheRun) {
  const auto e = encode(k_hevc);
  run_case(invalid_owner_frame(e, "HevcN5NullPayloadOwnerFrameBreaksTheRun", true));
}

TEST_F(RecorderBitstreamNegativeTests, H264N5ZeroSizeOwnerFrameBreaksTheRun) {
  const auto e = encode(k_h264);
  run_case(invalid_owner_frame(e, "H264N5ZeroSizeOwnerFrameBreaksTheRun", false));
}

TEST_F(RecorderBitstreamNegativeTests, HevcN5ZeroSizeOwnerFrameBreaksTheRun) {
  const auto e = encode(k_hevc);
  run_case(invalid_owner_frame(e, "HevcN5ZeroSizeOwnerFrameBreaksTheRun", false));
}

TEST_F(RecorderBitstreamNegativeTests, H264L1LaterKeyWithoutParameterSetsIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264L1LaterKeyWithoutParameterSetsIsReadAndSavedVerbatim", "L1", "later key packet 4 without its parameter sets",
                       4, e.ok ? drop_parameter_sets(e.run.packets[4].bytes, AV_CODEC_ID_H264) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcL1LaterKeyWithoutParameterSetsIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcL1LaterKeyWithoutParameterSetsIsReadAndSavedVerbatim", "L1", "later key packet 4 without its parameter sets",
                       4, e.ok ? drop_parameter_sets(e.run.packets[4].bytes, AV_CODEC_ID_HEVC) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264L3FirstKeyWithoutParameterSetsIsReadAndSavedVerbatim) {
  const auto e = encode(k_h264);
  run_case(replace_one(e, "H264L3FirstKeyWithoutParameterSetsIsReadAndSavedVerbatim", "L3", "first key packet 0 without its parameter sets",
                       0, e.ok ? drop_parameter_sets(e.run.packets[0].bytes, AV_CODEC_ID_H264) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, HevcL3FirstKeyWithoutParameterSetsIsReadAndSavedVerbatim) {
  const auto e = encode(k_hevc);
  run_case(replace_one(e, "HevcL3FirstKeyWithoutParameterSetsIsReadAndSavedVerbatim", "L3", "first key packet 0 without its parameter sets",
                       0, e.ok ? drop_parameter_sets(e.run.packets[0].bytes, AV_CODEC_ID_HEVC) : recipe_t {}));
}

TEST_F(RecorderBitstreamNegativeTests, H264L4DependentFrameFlaggedKeyStartsTheRun) {
  const auto e = encode(k_h264);
  run_case(flag_lie(e, "H264L4DependentFrameFlaggedKeyStartsTheRun"));
}

TEST_F(RecorderBitstreamNegativeTests, HevcL4DependentFrameFlaggedKeyStartsTheRun) {
  const auto e = encode(k_hevc);
  run_case(flag_lie(e, "HevcL4DependentFrameFlaggedKeyStartsTheRun"));
}

TEST_F(RecorderBitstreamNegativeTests, H264F2ForeignGenerationIsRefusedAroundTheSelectedRun) {
  const auto e = encode(k_h264);
  run_case(foreign_generation(e, "H264F2ForeignGenerationIsRefusedAroundTheSelectedRun"));
}

TEST_F(RecorderBitstreamNegativeTests, HevcF2ForeignGenerationIsRefusedAroundTheSelectedRun) {
  const auto e = encode(k_hevc);
  run_case(foreign_generation(e, "HevcF2ForeignGenerationIsRefusedAroundTheSelectedRun"));
}

TEST_F(RecorderBitstreamNegativeTests, H264F5RegisteredOverlapRefusesBothOwnersThenRecoversAtAFreshKey) {
  const auto e = encode(k_h264);
  run_case(registered_overlap(e, "H264F5RegisteredOverlapRefusesBothOwnersThenRecoversAtAFreshKey"));
}

TEST_F(RecorderBitstreamNegativeTests, HevcF5RegisteredOverlapRefusesBothOwnersThenRecoversAtAFreshKey) {
  const auto e = encode(k_hevc);
  run_case(registered_overlap(e, "HevcF5RegisteredOverlapRefusesBothOwnersThenRecoversAtAFreshKey"));
}

TEST_F(RecorderBitstreamNegativeTests, F1H264ThenHevcInOneSessionSavesOnlyHevc) {
  const auto h264 = encode(k_h264);
  const auto hevc = encode(k_hevc);
  run_case(codec_change(h264, hevc));
}

TEST_F(RecorderBitstreamNegativeTests, F3SessionRestartSavesOnlyTheNewGeneration) {
  const auto h264 = encode(k_h264);
  run_case(session_restart(h264));
}

TEST_F(RecorderBitstreamNegativeTests, F4AdmittedSaveKeepsItsCodecAcrossALaterCodecChange) {
  const auto h264 = encode(k_h264);
  const auto hevc = encode(k_hevc);
  run_case(held_save_across_codec_change(h264, hevc));
}
