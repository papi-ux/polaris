/**
 * @file tests/unit/test_stream_recorder_cost_probe.cpp
 * @brief Report-only cost probe for the stream recorder (#357), in its own Linux executable.
 *
 * Six scenarios call the real recorder through its existing test seams with a synthetic,
 * deterministic train of encoded video frame payloads (counted before RTP fragmentation: the
 * production tap sees whole encoded frames). Each case writes one new JSON record with raw
 * nanosecond samples, fixture parameters and machine, build and source provenance.
 *
 * Only correctness invariants are asserted. Timings are reported, never gated: no threshold is
 * calibrated. Nothing here starts capture, an encoder, a stream, a session of any client, a live
 * keyframe request or anything on a user's host; keyframe events are fixture mail only, and every
 * file is written inside owned temporary directories or the explicitly configured output directory.
 *
 * Built only with -DPOLARIS_ENABLE_RECORDER_COST_PROBE=ON (Linux, BUILD_FULL_TESTS=ON) as the separate
 * executable test_polaris_recorder_cost.
 *
 * Inputs (environment):
 * - POLARIS_RECORDER_COST_OUTPUT_DIR (required): an existing absolute directory; each case creates
 *   recorder-cost-<case>.json there exclusively and fails if it already exists.
 * - POLARIS_RECORDER_COST_SOURCE_SHA (required): the 40 or 64 hex digit source commit.
 * - POLARIS_RECORDER_COST_SAMPLES (optional, 1..10000, default 64) and
 *   POLARIS_RECORDER_COST_WARMUP (optional, 0..1000, default 8); a malformed value fails.
 * - POLARIS_RECORDER_COST_RUNNER (optional): recorded verbatim, or null when unset.
 */

#include <src/config.h>
#include <src/globals.h>
#include <src/stream_recorder.h>
#include <src/thread_safe.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using clock_type = std::chrono::steady_clock;
  using stream_recorder::continuous_state_e;

  constexpr int k_hevc = 1;
  constexpr auto k_bounded = 10s;
  constexpr std::size_t k_frame_interval_ns = 16'666'667;  // 60 frames per second
  constexpr std::size_t k_keyframe_period = 120;           // A keyframe every 2 s at 60 fps
  constexpr std::size_t k_keyframe_bytes = 256 * 1024;
  constexpr std::size_t k_dependent_bytes[] = {16 * 1024, 32 * 1024, 48 * 1024, 64 * 1024};

  // ---- Minimal JSON writer: the probe owns its output format and has no other dependency ----

  std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out += escaped;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out + "\"";
  }

  /// An ordered JSON object built from already-encoded values.
  class json_object_t {
  public:
    json_object_t &raw(std::string_view key, std::string value) {
      fields_.emplace_back(std::string {key}, std::move(value));
      return *this;
    }
    json_object_t &str(std::string_view key, std::string_view value) { return raw(key, json_string(value)); }
    json_object_t &num(std::string_view key, std::uint64_t value) { return raw(key, std::to_string(value)); }
    json_object_t &flag(std::string_view key, bool value) { return raw(key, value ? "true" : "false"); }
    json_object_t &null(std::string_view key) { return raw(key, "null"); }
    json_object_t &obj(std::string_view key, const json_object_t &value) { return raw(key, value.dump()); }

    std::string dump() const {
      std::string out = "{";
      for (std::size_t i = 0; i < fields_.size(); ++i) {
        out += (i ? "," : "") + json_string(fields_[i].first) + ":" + fields_[i].second;
      }
      return out + "}";
    }

  private:
    std::vector<std::pair<std::string, std::string>> fields_;
  };

  std::string json_numbers(const std::vector<std::uint64_t> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + std::to_string(values[i]);
    }
    return out + "]";
  }

  std::string json_array(const std::vector<std::string> &encoded) {
    std::string out = "[";
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      out += (i ? "," : "") + encoded[i];
    }
    return out + "]";
  }

  // ---- Parameters and provenance: explicit, validated, never silently defaulted ----

  struct parameter_t {
    std::uint64_t value = 0;
    std::string source;  ///< "default" or "environment"
  };

  /// A bounded decimal parameter; an unset one takes its documented default, a malformed one fails.
  std::optional<parameter_t> bounded_parameter(const char *name, std::uint64_t fallback, std::uint64_t low,
                                               std::uint64_t high, std::string &error) {
    const char *text = std::getenv(name);
    if (!text) {
      return parameter_t {fallback, "default"};
    }
    const std::string_view view {text};
    if (view.empty() || view.size() > 6 || !std::all_of(view.begin(), view.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      error = std::string {name} + " must be a decimal integer";
      return std::nullopt;
    }
    const auto value = std::stoull(std::string {view});
    if (value < low || value > high) {
      error = std::string {name} + " must be within " + std::to_string(low) + ".." + std::to_string(high);
      return std::nullopt;
    }
    return parameter_t {value, "environment"};
  }

  bool is_hex_sha(std::string_view text) {
    return (text.size() == 40 || text.size() == 64) &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  }

  std::string cpu_model() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
      if (line.rfind("model name", 0) == 0) {
        const auto colon = line.find(':');
        return colon == std::string::npos ? std::string {} : line.substr(line.find_first_not_of(" \t", colon + 1));
      }
    }
    return {};
  }

  long max_rss_kib() {
    rusage usage {};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
  }

  /// One synthetic encoded frame: deterministic size and bytes.
  struct frame_t {
    std::vector<std::uint8_t> bytes;
    bool keyframe = false;
  };

  frame_t synthetic_frame(std::size_t index) {
    frame_t frame;
    frame.keyframe = index % k_keyframe_period == 0;
    const auto size = frame.keyframe ? k_keyframe_bytes : k_dependent_bytes[index % std::size(k_dependent_bytes)];
    frame.bytes.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      frame.bytes[i] = static_cast<std::uint8_t>((index * 131 + i * 7) & 0xff);
    }
    return frame;
  }

  json_object_t train_description() {
    std::vector<std::uint64_t> dependent(std::begin(k_dependent_bytes), std::end(k_dependent_bytes));
    return json_object_t {}
      .str("unit", "encoded video frame payload bytes, before RTP fragmentation")
      .num("frameIntervalNs", k_frame_interval_ns)
      .str("arrivalClockNote", "reference artificial replay-arrival step; timings are unpaced wall-clock calls, not 60 fps playback")
      .str("patternNote", "reference train; fixed-payload keyframe subrecords override this pattern")
      .num("keyframePeriodFrames", k_keyframe_period)
      .num("keyframeBytes", k_keyframe_bytes)
      .raw("dependentBytesCycle", json_numbers(dependent))
      .str("codec", "hevc (synthetic bytes, never decoded)");
  }

  template<class F>
  std::uint64_t timed_ns(F &&work) {
    const auto start = clock_type::now();
    work();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count());
  }

  /// Bounded wait for a counter to reach a target (no sleeps: the writer signals each call).
  class counter_t {
  public:
    void add() {
      {
        std::lock_guard lock {mutex_};
        ++value_;
      }
      changed_.notify_all();
    }

    bool wait_for(std::uint64_t target) {
      std::unique_lock lock {mutex_};
      return changed_.wait_for(lock, k_bounded, [&] { return value_ >= target; });
    }

    std::uint64_t value() {
      std::lock_guard lock {mutex_};
      return value_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t value_ = 0;
  };

  /// Write permits for the throttled writer; release() lets every later write through.
  class permits_t {
  public:
    bool take() {
      std::unique_lock lock {mutex_};
      const bool ok = changed_.wait_for(lock, 3 * k_bounded, [&] { return released_ || granted_ > 0; });
      if (ok && !released_) {
        --granted_;
      }
      return ok;
    }

    void grant() {
      {
        std::lock_guard lock {mutex_};
        ++granted_;
      }
      changed_.notify_all();
    }

    void release() {
      {
        std::lock_guard lock {mutex_};
        released_ = true;
      }
      changed_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t granted_ = 0;
    bool released_ = false;
  };

  /**
   * Each case starts from the recorder's process start state, owns a private temporary directory
   * created exclusively, and restores config, seams, sessions and writers in TearDown on every path:
   * permits are released, fixture events stopped, helper threads joined, any file stopped and the
   * recorder reset before the members go.
   */
  class StreamRecorderCostProbe: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      case_name = info->name();
      for (int attempt = 0; attempt < 100 && owned.empty(); ++attempt) {
        const auto candidate = fs::temp_directory_path() / ("polaris-recorder-cost-" + case_name + "-" +
                                                           std::to_string(::getpid()) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
          owned = candidate;
        }
      }
      ASSERT_FALSE(owned.empty()) << "no exclusive temporary directory";
      config::recording.output_dir = (owned / "recordings").string();
      std::error_code made;
      ASSERT_TRUE(fs::create_directory(owned / "recordings", made)) << made.message();
      config::recording.replay_buffer_minutes = 1;
      publish(false, false);

      std::string error;
      const auto samples_param = bounded_parameter("POLARIS_RECORDER_COST_SAMPLES", 64, 1, 10000, error);
      ASSERT_TRUE(samples_param) << error;
      const auto warmup_param = bounded_parameter("POLARIS_RECORDER_COST_WARMUP", 8, 0, 1000, error);
      ASSERT_TRUE(warmup_param) << error;
      samples = samples_param->value;
      samples_from = samples_param->source;
      warmup = warmup_param->value;
      warmup_from = warmup_param->source;

      const char *sha = std::getenv("POLARIS_RECORDER_COST_SOURCE_SHA");
      ASSERT_TRUE(sha && is_hex_sha(sha)) << "POLARIS_RECORDER_COST_SOURCE_SHA must be the 40 or 64 hex digit source commit";
      source_sha = sha;
      const char *dir = std::getenv("POLARIS_RECORDER_COST_OUTPUT_DIR");
      ASSERT_TRUE(dir && fs::path {dir}.is_absolute()) << "POLARIS_RECORDER_COST_OUTPUT_DIR must be an absolute directory";
      std::error_code ec;
      ASSERT_TRUE(fs::is_directory(fs::path {dir}, ec)) << "POLARIS_RECORDER_COST_OUTPUT_DIR must exist";
      output = fs::path {dir} / ("recorder-cost-" + case_name + ".json");
      ASSERT_FALSE(fs::exists(fs::symlink_status(output, ec))) << "refusing to replace an existing output " << output;
      rss_before = max_rss_kib();
      ASSERT_GE(rss_before, 0) << "getrusage failed before measurement";
    }

    void TearDown() override {
      permits.release();
      for (auto &event : events) {
        event->stop();
      }
      threads.clear();
      if (stream_recorder::is_recording()) {
        stream_recorder::stop_recording();
      }
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_recording;
      stream_recorder::load_config();
      if (!owned.empty()) {
        std::error_code ec;
        fs::remove_all(owned, ec);
      }
    }

    static void publish(bool enabled, bool replay_buffer) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay_buffer;
      stream_recorder::load_config();
    }

    /// Record a deterministic correctness invariant; any false one fails the case.
    void invariant(std::string_view name, bool passed) {
      EXPECT_TRUE(passed) << name;
      invariants.push_back(json_object_t {}.str("name", name).flag("passed", passed).dump());
    }

    /// A subrecord with the warm-up it actually ran: the case's configured warm-up unless given.
    json_object_t subrecord(std::string_view name, std::string_view measures, const std::vector<std::uint64_t> &ns,
                            std::optional<std::uint64_t> warmup_run = std::nullopt) {
      return json_object_t {}
        .str("name", name)
        .str("measures", measures)
        .num("warmup", warmup_run.value_or(warmup))
        .num("samples", ns.size())
        .raw("ns", json_numbers(ns));
    }

    /// Write this case's record to its new, exclusively created file.
    void write_record(const std::vector<std::string> &subrecords, const json_object_t &outcomes) {
      utsname host {};
      ASSERT_EQ(::uname(&host), 0);
      const auto model = cpu_model();
      const char *runner = std::getenv("POLARIS_RECORDER_COST_RUNNER");
      json_object_t machine;
      machine.str("sysname", host.sysname).str("release", host.release).str("machine", host.machine)
        .num("hardwareConcurrency", std::thread::hardware_concurrency()).num("uid", ::getuid());
      if (model.empty()) {
        machine.null("cpuModel").str("cpuModelNote", "/proc/cpuinfo has no model name line");
      } else {
        machine.str("cpuModel", model);
      }
      json_object_t build;
      build.str("compiler", __VERSION__).num("cplusplus", __cplusplus)
#ifdef NDEBUG
        .flag("ndebug", true)
#else
        .flag("ndebug", false)
#endif
#ifdef __OPTIMIZE__
        .flag("optimized", true);
#else
        .flag("optimized", false);
#endif
      json_object_t record;
      const auto rss_after = max_rss_kib();
      ASSERT_GE(rss_after, rss_before) << "getrusage failed or process high-water memory regressed";
      record.num("schema", 1).str("probe", "polaris-recorder-cost").str("case", case_name)
        .str("sourceSha", source_sha)
        .str("sourceShaOrigin", "caller-reported via POLARIS_RECORDER_COST_SOURCE_SHA; not proof of the compiled source")
        .str("timing", "report-only; no thresholds are applied")
        .obj("parameters", json_object_t {}
                             .num("samples", samples).str("samplesSource", samples_from)
                             .num("warmup", warmup).str("warmupSource", warmup_from))
        .obj("train", train_description())
        .obj("machine", machine)
        .obj("build", build);
      if (runner) {
        record.str("runner", runner);
      } else {
        record.null("runner");
      }
      record.raw("subrecords", json_array(subrecords))
        .obj("outcomes", outcomes)
        .raw("invariants", json_array(invariants))
        .obj("memory", json_object_t {}
                         .str("note", "process high-water ru_maxrss in KiB, not current RSS and not recorder-only memory")
                         .raw("maxRssKiBBefore", std::to_string(rss_before))
                         .raw("maxRssKiBAfter", std::to_string(rss_after)));
      const auto text = record.dump() + "\n";
      const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
      ASSERT_GE(fd, 0) << "could not create " << output << " exclusively: " << std::strerror(errno);
      std::size_t written = 0;
      while (written < text.size()) {
        const auto n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        written += static_cast<std::size_t>(n);
      }
      const bool closed = ::close(fd) == 0;
      ASSERT_EQ(written, text.size()) << "short write to " << output;
      ASSERT_TRUE(closed) << "close failed for " << output;
    }

    static void push_at(std::uint64_t generation, const frame_t &frame, clock_type::time_point at) {
      stream_recorder::detail::push_packet_at(frame.bytes.data(), frame.bytes.size(), true, frame.keyframe, 0,
                                              generation, k_hevc, at);
    }

    static void push_now(std::uint64_t generation, const frame_t &frame) {
      stream_recorder::push_packet(frame.bytes.data(), frame.bytes.size(), true, frame.keyframe, 0, generation, k_hevc);
    }

    /// Push frames [first, first + count) of the one synthetic train at their own frame times
    /// (frame n arrives at base + n frame intervals), timing each push when asked.
    std::vector<std::uint64_t> fill_replay(std::uint64_t generation, std::size_t first, std::size_t count,
                                           clock_type::time_point base, bool timed) {
      std::vector<std::uint64_t> ns;
      for (std::size_t n = first; n < first + count; ++n) {
        const auto frame = synthetic_frame(n);
        const auto at = base + std::chrono::nanoseconds(n * k_frame_interval_ns);
        const auto elapsed = timed_ns([&] { push_at(generation, frame, at); });
        if (timed) {
          ns.push_back(elapsed);
        }
      }
      return ns;
    }

    config::recording_t saved_recording;
    fs::path owned;
    fs::path output;
    std::string case_name;
    std::string source_sha;
    std::uint64_t samples = 0;
    std::uint64_t warmup = 0;
    std::string samples_from;
    std::string warmup_from;
    long rss_before = -1;
    std::vector<std::string> invariants;
    permits_t permits;
    counter_t writes;
    std::vector<safe::mail_raw_t::event_t<bool>> events;
    std::vector<std::jthread> threads;  // Last: joined in TearDown before the state above goes.
  };
}  // namespace

// 1. The production broadcast tap's disabled check, and a direct disabled push: separate costs.
TEST_F(StreamRecorderCostProbe, DisabledTap) {
  ASSERT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::disabled);
  // The exact condition src/stream.cpp evaluates per encoded frame before ever calling push_packet.
  std::vector<std::uint64_t> tap_ns;
  std::uint64_t tap_true = 0;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    bool feed = false;
    const auto elapsed = timed_ns([&] {
      feed = stream_recorder::is_recording() || stream_recorder::current_mode() == stream_recorder::mode_t::replay_buffer;
    });
    tap_true += feed ? 1 : 0;
    if (i >= warmup) {
      tap_ns.push_back(elapsed);
    }
  }
  // A direct call to push_packet while disabled: the fast path, which the real tap never reaches.
  const auto frame = synthetic_frame(1);
  std::vector<std::uint64_t> push_ns;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    const auto elapsed = timed_ns([&] { push_now(7001, frame); });
    if (i >= warmup) {
      push_ns.push_back(elapsed);
    }
  }
  const auto replay = stream_recorder::replay_stats();
  const auto status = stream_recorder::recorder_status();
  invariant("tap condition is false while disabled", tap_true == 0);
  invariant("disabled push buffers nothing", replay.packets == 0 && replay.bytes == 0);
  invariant("disabled push counts no refusal", replay.refused_unselected == 0 && replay.refused_invalid == 0 &&
                                                  replay.refused_awaiting_keyframe == 0);
  invariant("disabled push opens no file", !status.recording && status.continuous.packets_admitted == 0);
  write_record({subrecord("production_tap_reads", "is_recording() || current_mode() == replay_buffer, as src/stream.cpp evaluates it; push_packet is not called", tap_ns).dump(),
                subrecord("direct_disabled_push", "push_packet() while disabled (fast path); the production tap never makes this call", push_ns)
                  .num("payloadBytes", frame.bytes.size()).dump()},
               json_object_t {}.num("tapTrue", tap_true).num("bufferedPackets", replay.packets));
}

// 2. Replay buffer admission: one owned payload copy per frame under the recorder lock.
TEST_F(StreamRecorderCostProbe, ReplayAdmission) {
  publish(true, true);
  stream_recorder::session_started(7002, true, k_hevc);
  const auto base = clock_type::now();
  // Warm-up frames 0..warmup-1, then the timed frames continue the same train from frame `warmup`.
  fill_replay(7002, 0, warmup, base, false);
  const auto ns = fill_replay(7002, warmup, samples, base, true);
  const auto replay = stream_recorder::detail::replay_stats_at(base + std::chrono::nanoseconds((warmup + samples) * k_frame_interval_ns));
  invariant("every frame is held or trimmed exactly once", replay.packets + replay.trimmed == warmup + samples);
  invariant("held bytes stay within the cap", replay.bytes <= replay.max_bytes && replay.packets <= replay.max_packets);
  invariant("nothing refused", replay.refused_invalid == 0 && replay.refused_oversized == 0 &&
                                 replay.refused_unselected == 0 && replay.refused_awaiting_keyframe == 0);
  invariant("the run is the selected session's", replay.selected_generation == 7002 && replay.run_video_format == k_hevc);
  write_record({subrecord("replay_push", "push_packet_at() in replay mode for the owner session", ns).dump()},
               json_object_t {}
                 .str("payloadAccounting", "owned payload copies counted by the recorder; not an allocator measurement")
                 .num("heldPackets", replay.packets).num("heldBytes", replay.bytes)
                 .num("uniquePayloadBytes", replay.unique_payload_bytes).num("trimmed", replay.trimmed)
                 .num("maxBytes", replay.max_bytes).num("maxPackets", replay.max_packets)
                 .num("discontinuities", replay.discontinuities));
}

// 3. Continuous admission: storage-free writer, native writes to an owned file, and a throttled writer.
TEST_F(StreamRecorderCostProbe, ContinuousAdmission) {
  std::vector<std::string> subrecords;
  json_object_t outcomes;
  auto run = [&](const std::string &name, std::function<long long(int, const std::uint8_t *, std::size_t)> write) {
    stream_recorder::detail::continuous_hooks_t hooks;
    hooks.name_stamp = [name] { return std::string {"cost-"} + name; };
    hooks.write = std::move(write);
    stream_recorder::detail::set_continuous_hooks_for_tests(hooks);
    stream_recorder::session_started(7003, true, k_hevc);
    stream_recorder::start_recording();
    ASSERT_TRUE(stream_recorder::is_recording());
    std::vector<std::uint64_t> ns;
    for (std::uint64_t i = 0; i < warmup + samples; ++i) {
      const auto frame = synthetic_frame(i);
      const auto elapsed = timed_ns([&] { push_now(7003, frame); });
      // Synchronized: the writer finishes this frame before the next is offered, so no drop can
      // come from the producer outrunning storage.
      ASSERT_TRUE(stream_recorder::detail::wait_continuous_drained_for_tests(k_bounded));
      if (i >= warmup) {
        ns.push_back(elapsed);
      }
    }
    const auto s = stream_recorder::recorder_status().continuous;
    invariant(name + ": every frame admitted and written", s.packets_admitted == warmup + samples &&
                                                             s.packets_written == s.packets_admitted &&
                                                             s.dropped_backpressure == 0 && s.queued_packets == 0);
    subrecords.push_back(subrecord(name, "push_packet() admission into the continuous queue; writer drained between frames", ns)
                           .num("bytesWritten", s.bytes_written).dump());
    stream_recorder::stop_recording();
    stream_recorder::detail::reset_replay_for_tests();
  };
  run("storage_free_writer", [](int, const std::uint8_t *, std::size_t size) { return static_cast<long long>(size); });
  if (HasFatalFailure()) return;
  run("native_tmp_writer", [](int fd, const std::uint8_t *data, std::size_t size) {
    return stream_recorder::detail::native_write(fd, data, size);
  });
  if (HasFatalFailure()) return;

  // Throttled: a small queue, and the writer may complete one write call per granted permit; the
  // producer grants one permit after every `ratio` frames and waits until it was used.
  constexpr std::uint64_t ratio = 4;
  constexpr std::size_t limit_bytes = 512 * 1024;
  constexpr std::size_t limit_slots = 8;
  stream_recorder::detail::set_continuous_limits_for_tests(limit_bytes, limit_slots);
  stream_recorder::detail::continuous_hooks_t hooks;
  hooks.name_stamp = [] { return std::string {"cost-throttled"}; };
  hooks.write = [this](int, const std::uint8_t *, std::size_t size) -> long long {
    if (!permits.take()) {
      errno = ETIMEDOUT;
      return -1;
    }
    writes.add();
    return static_cast<long long>(size);
  };
  stream_recorder::detail::set_continuous_hooks_for_tests(hooks);
  stream_recorder::session_started(7004, true, k_hevc);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  std::vector<std::uint64_t> ns;
  std::uint64_t granted = 0;
  std::size_t peak_queued_packets = 0;
  std::size_t peak_queued_bytes = 0;
  const auto total = warmup + samples;
  for (std::uint64_t i = 0; i < total; ++i) {
    const auto frame = synthetic_frame(i);
    const auto elapsed = timed_ns([&] { push_now(7004, frame); });
    if (i >= warmup) {
      ns.push_back(elapsed);
    }
    const auto s = stream_recorder::recorder_status().continuous;
    peak_queued_packets = std::max(peak_queued_packets, s.queued_packets);
    peak_queued_bytes = std::max(peak_queued_bytes, s.queued_bytes);
    if ((i + 1) % ratio == 0 && s.queued_packets > 0) {
      permits.grant();
      ++granted;
      ASSERT_TRUE(writes.wait_for(granted)) << "the throttled writer did not use its permit";
    }
  }
  permits.release();
  ASSERT_TRUE(stream_recorder::detail::wait_continuous_drained_for_tests(k_bounded));
  const auto s = stream_recorder::recorder_status().continuous;
  invariant("throttled: every offered frame is admitted, dropped or refused once",
            s.packets_admitted + s.dropped_backpressure + s.refused_awaiting_keyframe == total);
  invariant("throttled: admitted packets are all written after release",
            s.packets_written + s.discarded_packets == s.packets_admitted && s.queued_packets == 0);
  invariant("throttled: the queue never exceeded its capacity",
            peak_queued_packets <= limit_slots && peak_queued_bytes <= limit_bytes);
  subrecords.push_back(subrecord("throttled_writer", "push_packet() admission while the writer completes one write per permit", ns)
                         .num("permitEveryFrames", ratio).num("permitsGranted", granted)
                         .num("queueLimitBytes", limit_bytes).num("queueLimitSlots", limit_slots).dump());
  outcomes.obj("throttled", json_object_t {}
                              .num("offered", total).num("admitted", s.packets_admitted).num("written", s.packets_written)
                              .num("droppedBackpressure", s.dropped_backpressure)
                              .num("refusedAwaitingKeyframe", s.refused_awaiting_keyframe)
                              .num("discontinuities", s.discontinuities)
                              .num("peakQueuedPackets", peak_queued_packets).num("peakQueuedBytes", peak_queued_bytes)
                              .num("writeCalls", writes.value()));
  write_record(subrecords, outcomes);
}

// 4. Lock contention: one producer pushing replay frames while readers take status snapshots.
TEST_F(StreamRecorderCostProbe, LockContention) {
  publish(true, true);
  stream_recorder::session_started(7005, true, k_hevc);
  constexpr int readers = 2;
  const auto total = warmup + samples;
  std::vector<std::uint64_t> producer_ns;
  producer_ns.reserve(samples);
  std::vector<std::vector<std::uint64_t>> reader_ns(readers);
  for (auto &ns : reader_ns) {
    ns.reserve(samples);  // No allocation inside the reader threads
  }
  std::atomic<std::uint64_t> try_lock_failures {0};
  std::latch start {1};  // Opened once by this thread; readers wait on it
  const auto base = clock_type::now();
  // Declared after everything the readers use, so it is destroyed (joining them) first. The opener
  // is declared after it, so it opens the latch before any join, even when a launch throws.
  std::vector<std::jthread> reader_threads;
  struct opener_t {
    std::latch &latch;
    bool opened = false;
    void open() {
      if (!opened) {
        opened = true;
        latch.count_down();
      }
    }
    ~opener_t() { open(); }
  } opener {start};
  reader_threads.reserve(readers);
  for (int r = 0; r < readers; ++r) {
    reader_threads.emplace_back([&, r] {
      start.wait();
      for (std::uint64_t i = 0; i < total; ++i) {
        const auto elapsed = timed_ns([] { (void) stream_recorder::recorder_status(); });
        if (!stream_recorder::detail::recorder_mutex_try_lock_for_tests()) {
          ++try_lock_failures;
        }
        if (i >= warmup) {
          reader_ns[r].push_back(elapsed);
        }
      }
    });
  }
  opener.open();
  for (std::uint64_t i = 0; i < total; ++i) {
    const auto frame = synthetic_frame(i);
    const auto at = base + std::chrono::nanoseconds(i * k_frame_interval_ns);
    const auto elapsed = timed_ns([&] { push_at(7005, frame, at); });
    if (i >= warmup) {
      producer_ns.push_back(elapsed);
    }
  }
  reader_threads.clear();  // Joins the readers: every reader finished its fixed number of calls.
  const auto replay = stream_recorder::detail::replay_stats_at(base + std::chrono::nanoseconds(total * k_frame_interval_ns));
  invariant("every producer frame is held or trimmed exactly once", replay.packets + replay.trimmed == total);
  invariant("nothing refused under contention", replay.refused_invalid == 0 && replay.refused_unselected == 0);
  std::vector<std::string> subrecords {subrecord("producer_push", "push_packet_at() while readers poll recorder_status()", producer_ns).dump()};
  for (int r = 0; r < readers; ++r) {
    subrecords.push_back(subrecord("reader_" + std::to_string(r), "recorder_status() while the producer pushes", reader_ns[r]).dump());
  }
  write_record(subrecords, json_object_t {}
                             .num("readers", readers).num("tryLockFailures", try_lock_failures.load())
                             .str("overlapNote", "readers start when the producer opens the latch; overlap is not guaranteed for every call"));
}

// 5. Keyframe reservation: unwired production shape, a fixture channel, and fixture dispatch.
TEST_F(StreamRecorderCostProbe, KeyframeReservation) {
  std::vector<std::string> subrecords;
  json_object_t outcomes;
  auto storage_free = [] {
    stream_recorder::detail::continuous_hooks_t hooks;
    hooks.name_stamp = [] { return std::string {"cost-keyframe"}; };
    hooks.write = [](int, const std::uint8_t *, std::size_t size) { return static_cast<long long>(size); };
    stream_recorder::detail::set_continuous_hooks_for_tests(hooks);
  };
  const auto keyframe = synthetic_frame(0);
  const auto dependent = synthetic_frame(1);

  // Every timed push below is the production push_packet(); the storage-free writer is drained,
  // untimed, before the next frame. This excludes drain waits and backpressure; the woken
  // writer can still contend for its mutex during the timed push.
  auto push_drained = [](std::uint64_t generation, const frame_t &frame) {
    const auto elapsed = timed_ns([&] { push_now(generation, frame); });
    EXPECT_TRUE(stream_recorder::detail::wait_continuous_drained_for_tests(k_bounded));
    return elapsed;
  };
  constexpr std::string_view drained = "; production push_packet() timed, storage-free writer drained untimed before the next frame; woken writer contention remains possible";

  // (a) No channel: production registers none, so keyframe_reserve returns after its early checks.
  storage_free();
  stream_recorder::session_started(7006, true, k_hevc);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  push_drained(7006, keyframe);
  std::vector<std::uint64_t> unwired_ns;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    const auto elapsed = push_drained(7006, dependent);
    if (i >= warmup) {
      unwired_ns.push_back(elapsed);
    }
  }
  auto stats = stream_recorder::detail::keyframe_stats_for_tests();
  invariant("unwired: no reservation", stats.reservations == 0 && stats.attempted == 0);
  const auto unwired_status = stream_recorder::recorder_status().continuous;
  invariant("unwired: every frame admitted, none dropped",
            unwired_status.dropped_backpressure == 0 && unwired_status.packets_admitted == 1 + warmup + samples &&
              unwired_status.packets_written == unwired_status.packets_admitted);
  subrecords.push_back(subrecord("unwired_owner_push", std::string {"owner frames in the writing state with no channel (production shape: continuous admission takes writer.lock; keyframe_reserve returns before its additional writer.lock)"} + std::string {drained}, unwired_ns)
                        .num("payloadBytes", dependent.bytes.size()).str("arrivalClock", "actual steady clock, unpaced").dump());
  stream_recorder::stop_recording();
  stream_recorder::detail::reset_replay_for_tests();

  // (b) Fixture channel, writing state: keyframe_reserve takes one additional writer lock.
  storage_free();
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto event = mail->event<bool>(::mail::idr);
  events.push_back(event);
  auto channel = std::make_shared<stream_recorder::keyframe_channel_t>(event);
  stream_recorder::session_started(7007, true, k_hevc, channel);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  const bool start_request = event->try_pop();  // A file bound at start asks once
  push_drained(7007, keyframe);
  std::vector<std::uint64_t> wired_ns;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    const auto elapsed = push_drained(7007, dependent);
    if (i >= warmup) {
      wired_ns.push_back(elapsed);
    }
  }
  stats = stream_recorder::detail::keyframe_stats_for_tests();
  const auto before_dispatch = stats.attempted;
  invariant("fixture channel: writing frames reserve nothing", stats.reservations == before_dispatch && !event->peek());
  const auto wired_status = stream_recorder::recorder_status().continuous;
  invariant("fixture channel: every frame admitted, none dropped",
            wired_status.dropped_backpressure == 0 && wired_status.packets_admitted == 1 + warmup + samples &&
              wired_status.packets_written == wired_status.packets_admitted);
  subrecords.push_back(subrecord("fixture_channel_owner_push", std::string {"owner frames in the writing state with a fixture channel (one additional keyframe_reserve writer.lock per frame beyond continuous admission)"} + std::string {drained}, wired_ns)
                        .num("payloadBytes", dependent.bytes.size()).str("arrivalClock", "actual steady clock, unpaced").dump());

  // (c) Fixture dispatch: each recovery keyframe is admitted and drained untimed, then one
  // invalid-packet discontinuity is timed: its reservation and the fixture event raise.
  std::vector<std::uint64_t> dispatch_ns;
  std::uint64_t latched = 0;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    push_drained(7007, keyframe);  // Recovery ends: the next discontinuity may ask again at once
    const auto elapsed = timed_ns([] {
      stream_recorder::push_packet(nullptr, 0, true, false, 0, 7007, k_hevc);
    });
    latched += event->try_pop() ? 1 : 0;
    if (i >= warmup) {
      dispatch_ns.push_back(elapsed);
    }
  }
  stats = stream_recorder::detail::keyframe_stats_for_tests();
  const auto dispatch_invalid = stream_recorder::recorder_status().continuous.refused_invalid;
  invariant("fixture dispatch: one attempted raise per invalid packet", stats.attempted - before_dispatch == warmup + samples &&
                                                                dispatch_invalid == warmup + samples &&
                                                                latched == warmup + samples);
  invariant("fixture dispatch: nothing expired, stopped or fenced", stats.expired == 0 && stats.stopped == 0 && stats.fenced == 0);
  subrecords.push_back(subrecord("fixture_dispatch", "an invalid owner packet through production push_packet(), modelling a discontinuity rather than observed network loss: reservation plus raise of the fixture mail event (no encoder exists); each recovery keyframe admitted and drained untimed first", dispatch_ns)
                        .num("payloadBytes", 0).num("recoveryKeyframeBytes", keyframe.bytes.size()).str("arrivalClock", "actual steady clock, unpaced").dump());
  outcomes.flag("startRequestLatched", start_request).num("reservations", stats.reservations)
    .num("attempted", stats.attempted).num("fixtureEventsLatched", latched).num("refusedInvalid", dispatch_invalid)
    .str("note", "fixture mail events only; no live keyframe request reaches an encoder or host");
  write_record(subrecords, outcomes);
}

// 6. Replay save: snapshot admission under the lock, and completion into an owned directory.
TEST_F(StreamRecorderCostProbe, ReplaySaveSnapshot) {
  publish(true, true);
  stream_recorder::session_started(7008, true, k_hevc);
  const auto base = clock_type::now();
  const auto frames = warmup + samples;
  fill_replay(7008, 0, frames, base, false);
  const auto now = base + std::chrono::nanoseconds(frames * k_frame_interval_ns);
  const auto buffered = stream_recorder::detail::replay_stats_at(now);
  ASSERT_GT(buffered.packets, 0u);

  std::vector<std::uint64_t> admit_ns;
  std::uint64_t admitted = 0;
  for (std::uint64_t i = 0; i < warmup + samples; ++i) {
    stream_recorder::detail::save_admission_t admission;
    const auto elapsed = timed_ns([&] { admission = stream_recorder::detail::admit_replay_save(now); });
    admitted += admission.result.admitted ? 1 : 0;
    if (i >= warmup) {
      admit_ns.push_back(elapsed);
    }
  }  // Each admission is dropped unsaved: it releases its snapshot and reservation.

  // Completion writes real files, so it runs a fixed, recorded number of times.
  const std::uint64_t completions = std::min<std::uint64_t>(samples, 4);
  auto stamp = std::make_shared<std::atomic<std::uint64_t>>(0);
  stream_recorder::detail::save_hooks_t hooks;
  hooks.name_stamp = [stamp] { return "cost-save-" + std::to_string((*stamp)++); };
  stream_recorder::detail::set_save_hooks_for_tests(hooks);
  std::vector<std::uint64_t> complete_ns;
  std::uint64_t saved_bytes = 0;
  bool files_match = true;
  for (std::uint64_t i = 0; i < completions; ++i) {
    auto admission = stream_recorder::detail::admit_replay_save(now);
    ASSERT_TRUE(admission.result.admitted);
    stream_recorder::detail::save_result_t result;
    complete_ns.push_back(timed_ns([&] { result = stream_recorder::detail::complete_replay_save(std::move(admission.save)); }));
    std::error_code ec;
    files_match = files_match && result.status == stream_recorder::detail::save_status_e::saved &&
                  fs::file_size(result.path, ec) == result.bytes && !ec;
    saved_bytes = result.bytes;
  }
  const auto after = stream_recorder::detail::replay_stats_at(now);
  invariant("every snapshot admission captured the run", admitted == warmup + samples);
  invariant("every completion published exactly its captured bytes", files_match);
  invariant("dropped admissions and completions are all accounted",
            after.saves_completed == completions && after.saves_failed == warmup + samples && !after.save_in_progress);
  write_record({subrecord("admit_snapshot", "admit_replay_save(): snapshot of the buffered run under the recorder lock, then dropped", admit_ns).dump(),
                subrecord("complete_to_owned_dir", "complete_replay_save(): write, sync and publish into the fixture's temporary directory", complete_ns, 0)
                  .num("completions", completions).str("completionsNote", "min(samples, 4): real files are written").dump()},
               json_object_t {}
                 .num("bufferedPackets", buffered.packets).num("bufferedBytes", buffered.bytes)
                 .num("savedBytesPerFile", saved_bytes).num("savesCompleted", after.saves_completed)
                 .num("savesFailed", after.saves_failed));
}
