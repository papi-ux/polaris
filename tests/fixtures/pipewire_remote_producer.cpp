/**
 * Standalone Video/Source producer for the cross-process producer-binding test.
 *
 * Usage: pipewire_remote_producer <client config> <remote name>
 * The parent passes PIPEWIRE_RUNTIME_DIR in an explicit environment, so this process can only reach
 * the parent's private socket. It never reads XDG_RUNTIME_DIR, so it cannot reach a user daemon.
 *
 * Protocol on stdout: one line "ready <node id>\n" once its node exists. It exits when stdin closes
 * (the parent's handle), on SIGTERM, or after a hard 30 s ceiling, so it can never outlive the test.
 */
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>

namespace {
  constexpr std::uint32_t width = 64;
  constexpr std::uint32_t height = 64;

  struct producer_t {
    pw_main_loop *loop = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener {};
    bool reported = false;
    bool failed = false;
  };

  void report_node(producer_t &p) {
    if (p.reported || !p.stream) return;
    const auto node = pw_stream_get_node_id(p.stream);
    if (node == SPA_ID_INVALID) return;
    p.reported = true;
    std::printf("ready %u\n", node);
    std::fflush(stdout);
  }

  void on_state_changed(void *data, pw_stream_state, pw_stream_state state, const char *) {
    auto &p = *static_cast<producer_t *>(data);
    if (state == PW_STREAM_STATE_ERROR) {
      p.failed = true;
      pw_main_loop_quit(p.loop);
      return;
    }
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) report_node(p);
  }

  void on_param_changed(void *data, std::uint32_t id, const spa_pod *param) {
    auto &p = *static_cast<producer_t *>(data);
    if (id != SPA_PARAM_Format || !param) return;
    alignas(8) std::array<std::uint8_t, 512> bytes {};
    spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const auto *buffers = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<int>(width * height * 4)),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int>(width * 4)),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
    p.failed |= pw_stream_update_params(p.stream, &buffers, 1) < 0;
  }

  void on_process(void *data) {
    auto &p = *static_cast<producer_t *>(data);
    auto *buffer = pw_stream_dequeue_buffer(p.stream);
    if (!buffer) return;
    auto &d = buffer->buffer->datas[0];
    if (d.data) std::memset(d.data, 0x40, width * height * 4);
    d.chunk->offset = 0;
    d.chunk->size = width * height * 4;
    d.chunk->stride = width * 4;
    pw_stream_queue_buffer(p.stream, buffer);
  }

  const spa_pod *build_format(spa_pod_builder &builder) {
    // Typed builders: PipeWire 1.0.5's raw-format helper mishandles max_framerate.
    spa_pod_frame frame {};
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
    spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video);
    spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
    spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRx);
    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
    spa_pod_builder_rectangle(&builder, width, height);
    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
    spa_pod_builder_fraction(&builder, 0, 1);
    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_maxFramerate, 0);
    spa_pod_builder_fraction(&builder, 60, 1);
    return static_cast<const spa_pod *>(spa_pod_builder_pop(&builder, &frame));
  }

  void on_stdin(void *data, int fd, std::uint32_t mask) {
    auto &p = *static_cast<producer_t *>(data);
    std::array<char, 64> sink {};
    const auto got = (mask & SPA_IO_IN) ? read(fd, sink.data(), sink.size()) : 0;
    if (got <= 0 && !(got < 0 && errno == EINTR)) pw_main_loop_quit(p.loop);
  }

  void on_ceiling(void *data, std::uint64_t) {
    auto &p = *static_cast<producer_t *>(data);
    p.failed = true;
    pw_main_loop_quit(p.loop);
  }

  void on_signal(void *data, int) {
    pw_main_loop_quit(static_cast<producer_t *>(data)->loop);
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <client config> <remote name>\n", argv[0]);
    return 2;
  }
  pw_init(&argc, &argv);
  producer_t p;
  int status = 1;
  p.loop = pw_main_loop_new(nullptr);
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  if (p.loop) {
    auto *loop = pw_main_loop_get_loop(p.loop);
    pw_loop_add_io(loop, STDIN_FILENO, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false, on_stdin, &p);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &p);
    if (auto *ceiling = pw_loop_add_timer(loop, on_ceiling, &p)) {
      timespec value {30, 0};
      pw_loop_update_timer(loop, ceiling, &value, nullptr, false);
    }
    context = pw_context_new(loop, pw_properties_new(PW_KEY_CONFIG_NAME, argv[1], nullptr), 0);
  }
  if (context) {
    core = pw_context_connect(context, pw_properties_new(PW_KEY_REMOTE_NAME, argv[2], nullptr), 0);
  }
  if (core) {
    p.stream = pw_stream_new(core, "polaris-remote-producer",
      pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_DRIVER, "true", nullptr));
  }
  if (p.stream) {
    static const pw_stream_events events = [] {
      pw_stream_events e {};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = on_state_changed;
      e.param_changed = on_param_changed;
      e.process = on_process;
      return e;
    }();
    pw_stream_add_listener(p.stream, &p.listener, &events, &p);
    alignas(8) std::array<std::uint8_t, 512> bytes {};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const spa_pod *format = build_format(builder);
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(p.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, &format, 1) >= 0) {
      pw_main_loop_run(p.loop);
      status = p.failed ? 1 : 0;
    }
  }
  if (p.stream) pw_stream_destroy(p.stream);
  if (core) pw_core_disconnect(core);
  if (context) pw_context_destroy(context);
  if (p.loop) pw_main_loop_destroy(p.loop);
  pw_deinit();
  return status;
}
