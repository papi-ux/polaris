/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <boost/regex.hpp>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#ifdef POLARIS_BUILD_PIPEWIRE
  #include <filesystem>

  #include <pipewire/pipewire.h>
  #include <spa/param/audio/format-utils.h>
  #include <spa/param/audio/format.h>
  #include <spa/param/props.h>
  #include <spa/utils/result.h>

  #include <unistd.h>
  #include <sys/stat.h>
#endif

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"

namespace platf {
  using namespace std::literals;

  constexpr pa_channel_position_t position_mapping[] {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

#ifdef POLARIS_BUILD_PIPEWIRE
  /**
   * @brief Detect if PipeWire is running and ensure the runtime environment
   * is properly configured, especially for systemd services that may not
   * have access to the user's audio session.
   *
   * This function checks for the PipeWire socket at the standard location
   * and sets XDG_RUNTIME_DIR and PIPEWIRE_RUNTIME_DIR if they are not
   * already set. This is the most common fix for audio failures when
   * Polaris runs as a system service.
   *
   * @return true if PipeWire appears to be available
   */
  static bool ensure_pipewire_environment() {
    // Check if XDG_RUNTIME_DIR is already set
    const char *xdg_runtime = std::getenv("XDG_RUNTIME_DIR");

    if (xdg_runtime) {
      // Check if PipeWire socket exists in the current runtime dir
      std::string pw_socket = std::string(xdg_runtime) + "/pipewire-0";
      struct stat st;
      if (stat(pw_socket.c_str(), &st) == 0) {
        BOOST_LOG(info) << "PipeWire socket found at: "sv << pw_socket;
        return true;
      }
    }

    // If running as a system service, XDG_RUNTIME_DIR may not be set.
    // Try common runtime directories for active user sessions.
    // Scan /run/user/ for directories containing a pipewire-0 socket.
    std::string run_user = "/run/user";
    std::error_code ec;
    if (std::filesystem::exists(run_user, ec)) {
      for (const auto &entry : std::filesystem::directory_iterator(run_user, ec)) {
        if (!entry.is_directory()) continue;

        std::string pw_socket = entry.path().string() + "/pipewire-0";
        struct stat st;
        if (stat(pw_socket.c_str(), &st) == 0) {
          std::string runtime_dir = entry.path().string();
          BOOST_LOG(info) << "Found PipeWire socket at: "sv << pw_socket;

          if (!xdg_runtime) {
            BOOST_LOG(info) << "Setting XDG_RUNTIME_DIR to: "sv << runtime_dir;
            setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 0);
          }

          // Always set PIPEWIRE_RUNTIME_DIR to ensure pw_context_connect finds it
          setenv("PIPEWIRE_RUNTIME_DIR", runtime_dir.c_str(), 0);

          return true;
        }
      }
    }

    BOOST_LOG(debug) << "No PipeWire socket found"sv;
    return false;
  }

  /**
   * @brief PipeWire stream data used during capture callbacks.
   */
  struct pw_capture_data_t {
    std::mutex mutex;
    std::condition_variable cv;

    // Ring buffer for audio samples
    std::vector<float> buffer;
    std::size_t write_pos = 0;
    std::size_t read_pos = 0;
    std::size_t available = 0;
    std::size_t buffer_capacity = 0;

    bool error = false;
    bool active = false;
    bool format_negotiated = false;

    int channels = 0;
    std::uint32_t sample_rate = 0;

    // Debug counters (atomics so they can be read without the mutex)
    std::atomic<uint64_t> total_callbacks {0};
    std::atomic<uint64_t> total_samples_written {0};
  };

  /**
   * @brief PipeWire-based microphone capture using pw_stream.
   */
  struct pw_mic_attr_t: public mic_t {
    struct pw_thread_loop *loop = nullptr;
    struct pw_stream *stream = nullptr;
    pw_capture_data_t capture_data;

    // PipeWire event callbacks
    struct pw_stream_events stream_events;

    static void stream_process_cb(void *userdata) {
      auto *self = static_cast<pw_mic_attr_t *>(userdata);
      struct pw_buffer *b = pw_stream_dequeue_buffer(self->stream);
      if (!b) {
        BOOST_LOG(warning) << "PipeWire process callback: no buffer available"sv;
        return;
      }

      struct spa_buffer *buf = b->buffer;

      if (buf->n_datas == 0 || buf->datas[0].data == nullptr) {
        BOOST_LOG(debug) << "PipeWire process callback: empty buffer (n_datas="sv
                         << buf->n_datas << ")"sv;
        pw_stream_queue_buffer(self->stream, b);
        return;
      }

      auto *src = static_cast<float *>(buf->datas[0].data);
      // Use chunk->size for actual data delivered; offset marks the start within the mapped region
      uint32_t offset = SPA_MIN(buf->datas[0].chunk->offset, buf->datas[0].maxsize);
      uint32_t n_bytes = SPA_MIN(buf->datas[0].chunk->size, buf->datas[0].maxsize - offset);
      uint32_t n_samples = n_bytes / sizeof(float);

      // Advance src past the offset
      src = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(src) + offset);

      self->capture_data.total_callbacks.fetch_add(1, std::memory_order_relaxed);

      if (n_samples > 0) {
        std::lock_guard<std::mutex> lock(self->capture_data.mutex);
        auto &cd = self->capture_data;

        for (uint32_t i = 0; i < n_samples; i++) {
          if (cd.available < cd.buffer_capacity) {
            cd.buffer[cd.write_pos] = src[i];
            cd.write_pos = (cd.write_pos + 1) % cd.buffer_capacity;
            cd.available++;
          }
          // If buffer is full, drop oldest samples
          else {
            cd.buffer[cd.write_pos] = src[i];
            cd.write_pos = (cd.write_pos + 1) % cd.buffer_capacity;
            cd.read_pos = (cd.read_pos + 1) % cd.buffer_capacity;
          }
        }

        cd.cv.notify_one();
        self->capture_data.total_samples_written.fetch_add(n_samples, std::memory_order_relaxed);
      }

      pw_stream_queue_buffer(self->stream, b);
    }

    static void stream_state_changed_cb(void *userdata, enum pw_stream_state old,
                                          enum pw_stream_state state, const char *errmsg) {
      auto *self = static_cast<pw_mic_attr_t *>(userdata);

      BOOST_LOG(info) << "PipeWire stream state: "sv
                      << pw_stream_state_as_string(old) << " -> "sv
                      << pw_stream_state_as_string(state);

      if (state == PW_STREAM_STATE_ERROR) {
        BOOST_LOG(error) << "PipeWire stream error: "sv << (errmsg ? errmsg : "unknown");
        std::lock_guard<std::mutex> lock(self->capture_data.mutex);
        self->capture_data.error = true;
        self->capture_data.cv.notify_one();
      }

      if (state == PW_STREAM_STATE_STREAMING) {
        BOOST_LOG(info) << "PipeWire stream is now actively streaming audio"sv;
        std::lock_guard<std::mutex> lock(self->capture_data.mutex);
        self->capture_data.active = true;
        self->capture_data.cv.notify_one();
      }

      if (state == PW_STREAM_STATE_PAUSED) {
        BOOST_LOG(debug) << "PipeWire stream paused (buffers negotiated)"sv;
      }
    }

    static void stream_param_changed_cb(void *userdata, uint32_t id, const struct spa_pod *param) {
      auto *self = static_cast<pw_mic_attr_t *>(userdata);

      if (param == nullptr || id != SPA_PARAM_Format) {
        return;
      }

      struct spa_audio_info_raw audio_fmt;
      if (spa_format_audio_raw_parse(param, &audio_fmt) < 0) {
        BOOST_LOG(warning) << "PipeWire: failed to parse negotiated audio format"sv;
        return;
      }

      BOOST_LOG(debug) << "PipeWire negotiated format: "sv
                       << audio_fmt.channels << " ch, "sv
                       << audio_fmt.rate << " Hz, format "sv << audio_fmt.format;

      if (audio_fmt.format != SPA_AUDIO_FORMAT_F32) {
        BOOST_LOG(warning) << "PipeWire: negotiated non-F32 format ("sv << audio_fmt.format
                           << "), expected F32 ("sv << SPA_AUDIO_FORMAT_F32 << ")"sv;
      }

      {
        std::lock_guard<std::mutex> lock(self->capture_data.mutex);
        self->capture_data.format_negotiated = true;
      }
    }

    capture_e sample(std::vector<float> &sample_buf) override {
      auto needed = sample_buf.size();

      std::unique_lock<std::mutex> lock(capture_data.mutex);

      // Wait until we have enough samples or an error occurs, with a timeout
      // to detect streams that never start delivering data.
      if (!capture_data.cv.wait_for(lock, std::chrono::seconds(5), [&]() {
        return capture_data.available >= needed || capture_data.error;
      })) {
        // Timeout: log diagnostic information
        BOOST_LOG(warning) << "PipeWire sample() timed out waiting for audio data. "sv
                           << "active="sv << capture_data.active
                           << " format_negotiated="sv << capture_data.format_negotiated
                           << " callbacks="sv << capture_data.total_callbacks.load(std::memory_order_relaxed)
                           << " samples_written="sv << capture_data.total_samples_written.load(std::memory_order_relaxed)
                           << " available="sv << capture_data.available
                           << " needed="sv << needed;
        return capture_e::error;
      }

      if (capture_data.error) {
        return capture_e::error;
      }

      // Copy samples from ring buffer
      for (std::size_t i = 0; i < needed; i++) {
        sample_buf[i] = capture_data.buffer[capture_data.read_pos];
        capture_data.read_pos = (capture_data.read_pos + 1) % capture_data.buffer_capacity;
      }
      capture_data.available -= needed;

      return capture_e::ok;
    }

    ~pw_mic_attr_t() override {
      if (loop) {
        pw_thread_loop_stop(loop);
      }
      if (stream) {
        pw_stream_destroy(stream);
        stream = nullptr;
      }
      if (loop) {
        pw_thread_loop_destroy(loop);
        loop = nullptr;
      }

      BOOST_LOG(debug) << "PipeWire capture destroyed: total_callbacks="sv
                       << capture_data.total_callbacks.load()
                       << " total_samples="sv
                       << capture_data.total_samples_written.load();
    }
  };

  /**
   * @brief Create a PipeWire-based microphone capture stream.
   * @return A mic_t pointer on success, nullptr on failure.
   */
  static std::unique_ptr<mic_t> pw_microphone(const std::uint8_t *mapping, int channels,
                                               std::uint32_t sample_rate, std::uint32_t frame_size,
                                               const std::string &source_name) {
    auto mic = std::make_unique<pw_mic_attr_t>();

    // Initialize ring buffer: hold at least 4 frames worth of data
    mic->capture_data.channels = channels;
    mic->capture_data.sample_rate = sample_rate;
    mic->capture_data.buffer_capacity = frame_size * channels * 4;
    mic->capture_data.buffer.resize(mic->capture_data.buffer_capacity, 0.0f);

    BOOST_LOG(info) << "PipeWire: creating capture stream for source: "sv << source_name
                    << " (channels="sv << channels
                    << " rate="sv << sample_rate
                    << " frame_size="sv << frame_size
                    << " ring_buffer="sv << mic->capture_data.buffer_capacity << " samples)"sv;

    // Create PipeWire thread loop
    mic->loop = pw_thread_loop_new("polaris-audio", nullptr);
    if (!mic->loop) {
      BOOST_LOG(error) << "Failed to create PipeWire thread loop"sv;
      return nullptr;
    }

    // Build stream properties
    auto *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Game",
      PW_KEY_APP_NAME, "polaris",
      PW_KEY_NODE_NAME, "polaris-capture",
      PW_KEY_STREAM_CAPTURE_SINK, "true",
      nullptr
    );

    if (!source_name.empty()) {
      // The source_name comes from PulseAudio's monitor_source_name, e.g.
      // "sink-sunshine-stereo.monitor". With PW_KEY_STREAM_CAPTURE_SINK = "true",
      // PipeWire expects the target to be the SINK node name, not the monitor.
      // Strip the ".monitor" suffix to get the sink name.
      std::string target = source_name;
      const std::string monitor_suffix = ".monitor";
      if (target.size() > monitor_suffix.size() &&
          target.compare(target.size() - monitor_suffix.size(), monitor_suffix.size(), monitor_suffix) == 0) {
        target.erase(target.size() - monitor_suffix.size());
        BOOST_LOG(info) << "PipeWire: stripped .monitor suffix, targeting sink node: "sv << target;
      }
      else {
        BOOST_LOG(info) << "PipeWire: targeting node: "sv << target;
      }
      pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
    }

    // Set up stream events — include param_changed for format negotiation logging
    spa_zero(mic->stream_events);
    mic->stream_events.version = PW_VERSION_STREAM_EVENTS;
    mic->stream_events.process = pw_mic_attr_t::stream_process_cb;
    mic->stream_events.state_changed = pw_mic_attr_t::stream_state_changed_cb;
    mic->stream_events.param_changed = pw_mic_attr_t::stream_param_changed_cb;

    // Create the stream on the thread loop's pw_loop
    mic->stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(mic->loop),
      "polaris-capture",
      props,
      &mic->stream_events,
      mic.get()
    );

    if (!mic->stream) {
      BOOST_LOG(error) << "Failed to create PipeWire stream"sv;
      return nullptr;
    }

    // Build audio format parameters
    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    // Build channel positions for SPA
    struct spa_audio_info_raw audio_info = {};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.rate = sample_rate;
    audio_info.channels = channels;

    // Map channel positions
    static const enum spa_audio_channel spa_channel_map[] = {
      SPA_AUDIO_CHANNEL_FL,   // FRONT_LEFT
      SPA_AUDIO_CHANNEL_FR,   // FRONT_RIGHT
      SPA_AUDIO_CHANNEL_FC,   // FRONT_CENTER
      SPA_AUDIO_CHANNEL_LFE,  // LOW_FREQUENCY
      SPA_AUDIO_CHANNEL_RL,   // BACK_LEFT / REAR_LEFT
      SPA_AUDIO_CHANNEL_RR,   // BACK_RIGHT / REAR_RIGHT
      SPA_AUDIO_CHANNEL_SL,   // SIDE_LEFT
      SPA_AUDIO_CHANNEL_SR,   // SIDE_RIGHT
    };

    for (int i = 0; i < channels; i++) {
      audio_info.position[i] = spa_channel_map[mapping[i]];
    }

    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    // Start the thread loop BEFORE connecting, so the loop is running
    // to process the connection handshake and state transitions.
    if (pw_thread_loop_start(mic->loop) < 0) {
      BOOST_LOG(error) << "Failed to start PipeWire thread loop"sv;
      return nullptr;
    }

    // Lock the thread loop while we connect the stream.
    // PipeWire requires the loop lock to be held when calling API functions
    // on objects associated with a running thread loop.
    pw_thread_loop_lock(mic->loop);

    // Connect the stream as a capture (recording) stream.
    // Do NOT use PW_STREAM_FLAG_RT_PROCESS: without it, the process callback
    // runs on the thread loop's main thread (with the loop lock held), which
    // is safe for our std::mutex-based ring buffer. RT_PROCESS would call the
    // callback from the realtime data thread, causing priority inversion issues.
    auto res = pw_stream_connect(
      mic->stream,
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS
      ),
      params,
      1
    );

    pw_thread_loop_unlock(mic->loop);

    if (res < 0) {
      BOOST_LOG(error) << "Failed to connect PipeWire stream: "sv << spa_strerror(res);
      return nullptr;
    }

    // Wait briefly for the stream to reach STREAMING state.
    // This catches configuration errors early instead of blocking in sample().
    {
      std::unique_lock<std::mutex> lock(mic->capture_data.mutex);
      bool ready = mic->capture_data.cv.wait_for(lock, std::chrono::seconds(3), [&]() {
        return mic->capture_data.active || mic->capture_data.error;
      });

      if (mic->capture_data.error) {
        BOOST_LOG(error) << "PipeWire stream entered error state during setup"sv;
        return nullptr;
      }

      if (!ready) {
        BOOST_LOG(warning) << "PipeWire stream did not reach STREAMING state within 3 seconds "sv
                           << "(format_negotiated="sv << mic->capture_data.format_negotiated << ")"sv;
        // Don't fail here — some setups take longer. sample() has its own timeout.
      }
    }

    BOOST_LOG(info) << "PipeWire audio capture initialized: "sv
                    << channels << " channels, "sv << sample_rate << " Hz"sv
                    << " (active="sv << mic->capture_data.active << ")"sv;

    return mic;
  }

  static bool pipewire_initialized = false;

  /**
   * @brief Initialize PipeWire library (once).
   */
  static void init_pipewire() {
    if (!pipewire_initialized) {
      pw_init(nullptr, nullptr);
      pipewire_initialized = true;
    }
  }
#endif  // POLARIS_BUILD_PIPEWIRE

  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_simple, pa_simple_free> mic;

    capture_e sample(std::vector<float> &sample_buf) override {
      auto sample_size = sample_buf.size();

      auto buf = sample_buf.data();
      int status;
      if (pa_simple_read(mic.get(), buf, sample_size * sizeof(float), &status)) {
        BOOST_LOG(error) << "pa_simple_read() failed: "sv << pa_strerror(status);

        return capture_e::error;
      }

      return capture_e::ok;
    }
  };

  std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    auto mic = std::make_unique<mic_attr_t>();

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
    pa_channel_map pa_map;

    pa_map.channels = channels;
    std::for_each_n(pa_map.map, pa_map.channels, [mapping](auto &channel) mutable {
      channel = position_mapping[*mapping++];
    });

    pa_buffer_attr pa_attr = {
      .maxlength = uint32_t(-1),
      .tlength = uint32_t(-1),
      .prebuf = uint32_t(-1),
      .minreq = uint32_t(-1),
      .fragsize = uint32_t(frame_size * channels * sizeof(float))
    };

    int status;

    mic->mic.reset(
      pa_simple_new(nullptr, "sunshine", pa_stream_direction_t::PA_STREAM_RECORD, source_name.c_str(), "sunshine-record", &ss, &pa_map, &pa_attr, &status)
    );

    if (!mic->mic) {
      auto err_str = pa_strerror(status);
      BOOST_LOG(error) << "pa_simple_new() failed: "sv << err_str;
      return nullptr;
    }

    return mic;
  }

  namespace pa {
    template<bool B, class T>
    struct add_const_helper;

    template<class T>
    struct add_const_helper<true, T> {
      using type = const std::remove_pointer_t<T> *;
    };

    template<class T>
    struct add_const_helper<false, T> {
      using type = const T *;
    };

    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    using ctx_t = util::safe_ptr<pa_context, pa_context_unref>;
    using loop_t = util::safe_ptr<pa_mainloop, pa_mainloop_free>;
    using op_t = util::safe_ptr<pa_operation, pa_operation_unref>;
    using string_t = util::safe_ptr<char, pa_free<char>>;

    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }

      f(ctx, i, eol);
    }

    void cb_i(ctx_t::pointer ctx, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    void ctx_state_cb(ctx_t::pointer ctx, void *userdata) {
      auto &f = *(std::function<void(ctx_t::pointer)> *) userdata;

      f(ctx);
    }

    void success_cb(ctx_t::pointer ctx, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    bool process_env_has_session_audio_sink(pid_t pid, const std::string &sink) {
      if (pid <= 1 || sink.empty()) {
        return false;
      }

      std::ifstream env_file("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      if (!env_file) {
        return false;
      }

      std::string env {
        std::istreambuf_iterator<char> {env_file},
        std::istreambuf_iterator<char> {}
      };
      const std::string audio_sink_marker = "POLARIS_SESSION_AUDIO_SINK=" + sink;
      const std::string pulse_sink_marker = "PULSE_SINK=" + sink;

      std::size_t pos = 0;
      while (pos < env.size()) {
        const auto end = env.find('\0', pos);
        const auto len = end == std::string::npos ? std::string::npos : end - pos;
        const auto entry = std::string_view {env.data() + pos, len == std::string::npos ? env.size() - pos : len};
        if (entry == std::string_view {audio_sink_marker} || entry == std::string_view {pulse_sink_marker}) {
          return true;
        }
        if (end == std::string::npos) {
          break;
        }
        pos = end + 1;
      }

      return false;
    }

    class server_t: public audio_control_t {
      enum ctx_event_e : int {
        ready,
        terminated,
        failed
      };

    public:
      loop_t loop;
      ctx_t ctx;
      std::string requested_sink;

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;
        std::uint32_t surround51 = PA_INVALID_INDEX;
        std::uint32_t surround71 = PA_INVALID_INDEX;
      } index;

      std::unique_ptr<safe::event_t<ctx_event_e>> events;
      std::unique_ptr<std::function<void(ctx_t::pointer)>> events_cb;

      std::thread worker;

#ifdef POLARIS_BUILD_PIPEWIRE
      bool use_pipewire = false;
#endif

      int init() {
        events = std::make_unique<safe::event_t<ctx_event_e>>();

#ifdef POLARIS_BUILD_PIPEWIRE
        // Attempt to detect and configure PipeWire environment before
        // connecting to PulseAudio. This fixes the most common failure
        // mode where a systemd service cannot find the user's audio session.
        if (ensure_pipewire_environment()) {
          BOOST_LOG(info) << "PipeWire detected, will prefer native PipeWire for audio capture"sv;
          init_pipewire();
          use_pipewire = true;
        }
#endif

        loop.reset(pa_mainloop_new());
        ctx.reset(pa_context_new(pa_mainloop_get_api(loop.get()), "sunshine"));

        events_cb = std::make_unique<std::function<void(ctx_t::pointer)>>([this](ctx_t::pointer ctx) {
          switch (pa_context_get_state(ctx)) {
            case PA_CONTEXT_READY:
              events->raise(ready);
              break;
            case PA_CONTEXT_TERMINATED:
              BOOST_LOG(debug) << "Pulseadio context terminated"sv;
              events->raise(terminated);
              break;
            case PA_CONTEXT_FAILED:
              BOOST_LOG(debug) << "Pulseadio context failed"sv;
              events->raise(failed);
              break;
            case PA_CONTEXT_CONNECTING:
              BOOST_LOG(debug) << "Connecting to pulseaudio"sv;
            case PA_CONTEXT_UNCONNECTED:
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_SETTING_NAME:
              break;
          }
        });

        pa_context_set_state_callback(ctx.get(), ctx_state_cb, events_cb.get());

        auto status = pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr);
        if (status) {
          BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(status);
          return -1;
        }

        worker = std::thread {
          [](loop_t::pointer loop) {
            int retval;
            auto status = pa_mainloop_run(loop, &retval);

            if (status < 0) {
              BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
              return;
            }
          },
          loop.get()
        };

        auto event = events->pop();
        if (event == failed) {
          return -1;
        }

        return 0;
      }

      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_load_module(
            ctx.get(),
            "module-null-sink",
            to_string(name, channel_mapping, channels).c_str(),
            cb_i,
            alarm.get()
          ),
        };

        alarm->wait();
        return *alarm->status();
      }

      int unload_null(std::uint32_t i) {
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_unload_module(ctx.get(), i, success_cb, alarm.get())
        };

        alarm->wait();

        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        return 0;
      }

      std::optional<sink_t> sink_info() override {
        constexpr auto stereo = "sink-sunshine-stereo";
        constexpr auto surround51 = "sink-sunshine-surround51";
        constexpr auto surround71 = "sink-sunshine-surround71";

        auto alarm = safe::make_alarm<int>();

        sink_t sink;

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(ctx));

              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          // Ensure Sunshine won't create a sink that already exists.
          if (!std::strcmp(sink_info->name, stereo)) {
            index.stereo = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround51)) {
            index.surround51 = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround71)) {
            index.surround71 = sink_info->owner_module;

            ++nullcount;
          }
        };

        op_t op {pa_context_get_sink_info_list(ctx.get(), cb<pa_sink_info *>, &f)};

        if (!op) {
          BOOST_LOG(error) << "Couldn't create card info operation: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return std::nullopt;
        }

        alarm->wait();

        if (*alarm->status()) {
          return std::nullopt;
        }

        auto sink_name = get_default_sink_name();
        sink.host = sink_name;

        if (index.stereo == PA_INVALID_INDEX) {
          index.stereo = load_null(stereo, speaker::map_stereo, sizeof(speaker::map_stereo));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51, sizeof(speaker::map_surround51));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71, sizeof(speaker::map_surround71));
          if (index.surround71 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-71: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (sink_name.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          sink.null = std::make_optional(sink_t::null_t {stereo, surround51, surround71});
        }

        if (!sink.host.empty()) {
          const auto current_default = get_default_sink_name();
          const bool virtual_sink_promoted =
            current_default == stereo ||
            current_default == surround51 ||
            current_default == surround71;

          if (virtual_sink_promoted && current_default != sink.host) {
            BOOST_LOG(info) << "Linux audio isolation: restoring original default sink ["sv
                            << sink.host << "] after virtual sink creation promoted ["sv
                            << current_default << ']';
            set_sink(sink.host);
          }
        }

        return std::make_optional(std::move(sink));
      }

      std::string get_default_sink_name() {
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(ctx));
            alarm->ring(-1);
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        alarm->wait();
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          monitor_name = sink_info->monitor_source_name;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        alarm->wait();
        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      std::optional<std::uint32_t> sink_index_by_name(const std::string &sink_name) {
        std::optional<std::uint32_t> sink_index;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return sink_index;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          sink_index = sink_info->index;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        if (!sink_op) {
          BOOST_LOG(error) << "Couldn't create sink lookup operation for ["sv << sink_name
                           << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return std::nullopt;
        }

        alarm->wait();
        if (*alarm->status()) {
          return std::nullopt;
        }

        return sink_index;
      }

      void route_process_audio_to_sink(const std::string &sink) override {
        auto target_sink = sink_index_by_name(sink);
        if (!target_sink) {
          return;
        }

        struct sink_input_route_t {
          std::uint32_t index;
          std::uint32_t current_sink;
          pid_t pid;
          std::string app_name;
        };

        std::vector<sink_input_route_t> routes;
        auto alarm = safe::make_alarm<int>();

        cb_t<pa_sink_input_info *> input_f = [&](ctx_t::pointer ctx, const pa_sink_input_info *input_info, int eol) {
          if (!input_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink input info: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          if (input_info->sink == *target_sink) {
            return;
          }

          const char *pid_text = pa_proplist_gets(input_info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
          if (!pid_text || !*pid_text) {
            return;
          }

          char *end = nullptr;
          const auto pid_long = std::strtol(pid_text, &end, 10);
          if (end == pid_text || pid_long <= 1 || pid_long > std::numeric_limits<pid_t>::max()) {
            return;
          }

          const auto pid = static_cast<pid_t>(pid_long);
          if (!process_env_has_session_audio_sink(pid, sink)) {
            return;
          }

          const char *app_name = pa_proplist_gets(input_info->proplist, PA_PROP_APPLICATION_NAME);
          routes.push_back(sink_input_route_t {
            input_info->index,
            input_info->sink,
            pid,
            app_name ? app_name : "unknown"
          });
        };

        op_t list_op {pa_context_get_sink_input_info_list(ctx.get(), cb<pa_sink_input_info *>, &input_f)};
        if (!list_op) {
          BOOST_LOG(error) << "Couldn't create sink input list operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return;
        }

        alarm->wait();
        if (*alarm->status()) {
          return;
        }

        for (const auto &route : routes) {
          BOOST_LOG(info) << "Linux audio isolation: moving session audio stream ["sv
                          << route.app_name << "] pid="sv << route.pid
                          << " sink_input="sv << route.index
                          << " from sink #"sv << route.current_sink
                          << " to ["sv << sink << ']';

          auto move_alarm = safe::make_alarm<int>();
          op_t move_op {
            pa_context_move_sink_input_by_name(
              ctx.get(),
              route.index,
              sink.c_str(),
              success_cb,
              move_alarm.get()
            )
          };

          if (!move_op) {
            BOOST_LOG(warning) << "Linux audio isolation: failed to create move operation for sink_input="sv
                               << route.index << ": "sv << pa_strerror(pa_context_errno(ctx.get()));
            continue;
          }

          move_alarm->wait();
          if (*move_alarm->status()) {
            BOOST_LOG(warning) << "Linux audio isolation: failed to move session audio stream ["sv
                               << route.app_name << "] pid="sv << route.pid
                               << " to ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          }
        }
      }

      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, const std::string &selected_sink) override {
        // Sink choice priority:
        // 1. Selected session sink
        // 2. Config sink
        // 3. Last sink swapped to (Usually virtual in this case)
        // 4. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = selected_sink;
        if (sink_name.empty()) {
          sink_name = config::audio.sink;
        }
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        auto monitor_name = get_monitor_name(sink_name);

#ifdef POLARIS_BUILD_PIPEWIRE
        // If PipeWire is available, try native capture first
        if (use_pipewire) {
          BOOST_LOG(info) << "Attempting PipeWire audio capture for source: "sv << monitor_name;
          auto pw_mic = pw_microphone(mapping, channels, sample_rate, frame_size, monitor_name);
          if (pw_mic) {
            BOOST_LOG(info) << "Using PipeWire native audio capture"sv;
            return pw_mic;
          }
          BOOST_LOG(warning) << "PipeWire capture failed, falling back to PulseAudio"sv;
        }
#endif

        return ::platf::microphone(mapping, channels, sample_rate, frame_size, monitor_name);
      }

      bool is_sink_available(const std::string &sink) override {
        BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
        return true;
      }

      int set_sink(const std::string &sink) override {
        auto alarm = safe::make_alarm<int>();

        BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
        op_t op {
          pa_context_set_default_sink(
            ctx.get(),
            sink.c_str(),
            success_cb,
            alarm.get()
          ),
        };

        if (!op) {
          BOOST_LOG(error) << "Couldn't create set default-sink operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        alarm->wait();
        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return -1;
        }

        requested_sink = sink;

        return 0;
      }

      ~server_t() override {
        unload_null(index.stereo);
        unload_null(index.surround51);
        unload_null(index.surround71);

        if (worker.joinable()) {
          pa_context_disconnect(ctx.get());

          KITTY_WHILE_LOOP(auto event = events->pop(), event != terminated && event != failed, {
            event = events->pop();
          })

          pa_mainloop_quit(loop.get(), 0);
          worker.join();
        }
      }
    };
  }  // namespace pa

  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
