/**
 * A real, private PipeWire graph with a two-buffer producer. No dequeue,
 * process, publication or enqueue callback is mocked. This is graph acceptance,
 * not compositor/portal or physical-stream acceptance.
 */
#pragma once

#include <chrono>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "src/utility.h"
#include <pipewire/impl.h>
#include <spa/param/video/format-utils.h>
#include "src/platform/linux/pipewire_capture.h"

namespace pipewire_capture {
  struct capture_graph_test_access {
    static bool prepare(capture_t &capture) {
      static std::once_flag initialized;
      std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
      capture.loop_ = pw_thread_loop_new("polaris-private-producer-pool", nullptr);
      if (!capture.loop_) return false;
      capture.context_ = pw_context_new(pw_thread_loop_get_loop(capture.loop_),
        pw_properties_new("config.name", POLARIS_SOURCE_DIR "/tests/fixtures/pipewire-pool.conf", nullptr), 0);
      if (!capture.context_) return false;
      capture.core_ = pw_context_connect_self(capture.context_, nullptr, 0);
      return capture.core_ && pw_thread_loop_start(capture.loop_) >= 0;
    }

    static bool connect(capture_t &capture, std::uint32_t node_id) {
      capture.options_.node_id = node_id;
      if (!capture.connect_stream(true)) return false;
      std::lock_guard lock(capture.frame_mtx_);
      capture.running_ = true;
      capture.terminal_result_ = wait_result_e::timeout;
      return !capture.queue_buffer_for_test_;
    }
    static pw_thread_loop *loop(capture_t &capture) { return capture.loop_; }
    static pw_core *core(capture_t &capture) { return capture.core_; }
    static std::uint32_t node(capture_t &capture) { return pw_stream_get_node_id(capture.stream_); }
    static std::size_t leases(capture_t &capture) {
      std::lock_guard lock(capture.frame_mtx_);
      return capture.active_dmabuf_leases_;
    }
  };
}

namespace test_utils {
  struct pipewire_producer_graph_t {
    using allocation_t = std::function<bool(pw_buffer *)>;
    using write_t = std::function<bool(pw_buffer *, std::uint8_t)>;
    using remove_t = std::function<void(pw_buffer *)>;
    using access = pipewire_capture::capture_graph_test_access;
    std::shared_ptr<pipewire_capture::capture_t> capture;
    pw_stream *source = nullptr;
    pw_proxy *link = nullptr;
    spa_hook listener {};
    allocation_t allocate;
    write_t write;
    remove_t remove;
    std::map<pw_buffer *, std::size_t> writes;
    std::size_t completed = 0, submitted = 0, empty = 0, added = 0, removed = 0;
    std::uint8_t marker = 0;
    bool failed = false;
    const bool dmabuf;
    const int width, height;

    pipewire_producer_graph_t(bool gpu, int w, int h, allocation_t allocation = {}, write_t writer = {}, remove_t remover = {}):
        allocate(std::move(allocation)), write(std::move(writer)), remove(std::move(remover)), dmabuf(gpu), width(w), height(h) {
      pipewire_capture::capture_options_t options;
      options.requested_width = w;
      options.requested_height = h;
      options.requested_rate = {60, 1};
      // The SHM test deliberately offers a compatible DMA-BUF pod to a source
      // without a modifier: real negotiation must still choose CPU/MemFd.
      options.may_use_dmabuf = true;
      options.dmabuf_formats = {{SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR}};
      options.capture_render_node = "/dev/dri/renderD128";
      capture = std::make_shared<pipewire_capture::capture_t>(options);
    }

    template<class Predicate> bool wait(Predicate predicate) {
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      do {
        pw_thread_loop_lock(access::loop(*capture));
        const bool done = predicate();
        pw_thread_loop_unlock(access::loop(*capture));
        if (done) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < end);
      return false;
    }

    bool start() {
      if (!access::prepare(*capture)) return false;
      pw_thread_loop_lock(access::loop(*capture));
      auto unlock = util::fail_guard([&] { pw_thread_loop_unlock(access::loop(*capture)); });
      source = pw_stream_new(access::core(*capture), "polaris-owned-two-buffer-producer",
        pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_DRIVER, "true", nullptr));
      if (!source) return false;
      static const pw_stream_events events {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = param_changed,
        .add_buffer = add_buffer,
        .remove_buffer = remove_buffer,
        .process = process,
        .trigger_done = trigger_done,
      };
      pw_stream_add_listener(source, &listener, &events, this);
      alignas(8) std::array<std::uint8_t, 512> bytes {};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
      spa_video_info_raw info {};
      info.format = SPA_VIDEO_FORMAT_BGRx;
      info.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
      info.framerate = {0, 1};
      info.max_framerate = {60, 1};
      if (dmabuf) { info.flags = SPA_VIDEO_FLAG_MODIFIER; info.modifier = DRM_FORMAT_MOD_LINEAR; }
      const spa_pod *format = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
      const auto flags = PW_STREAM_FLAG_DRIVER | (dmabuf ? PW_STREAM_FLAG_ALLOC_BUFFERS : PW_STREAM_FLAG_MAP_BUFFERS);
      if (pw_stream_connect(source, PW_DIRECTION_OUTPUT, PW_ID_ANY, static_cast<pw_stream_flags>(flags), &format, 1) < 0) return false;
      unlock.disable();
      pw_thread_loop_unlock(access::loop(*capture));
      if (!wait([&] { return pw_stream_get_node_id(source) != SPA_ID_INVALID; })) return false;
      pw_thread_loop_lock(access::loop(*capture));
      const bool connected = access::connect(*capture, pw_stream_get_node_id(source));
      pw_thread_loop_unlock(access::loop(*capture));
      if (!connected || !wait([&] { return access::node(*capture) != SPA_ID_INVALID; })) return false;
      pw_thread_loop_lock(access::loop(*capture));
      auto *props = pw_properties_new(nullptr, nullptr);
      pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", pw_stream_get_node_id(source));
      pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", access::node(*capture));
      link = static_cast<pw_proxy *>(pw_core_create_object(access::core(*capture), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
      pw_properties_free(props);
      pw_thread_loop_unlock(access::loop(*capture));
      return link && wait([&] { return capture->negotiated() && pw_stream_is_driving(source) && added == 2; }) && !failed;
    }

    bool step(std::uint8_t next_marker) {
      pw_thread_loop_lock(access::loop(*capture));
      marker = next_marker;
      const auto previous = completed;
      const auto result = pw_stream_trigger_process(source);
      pw_thread_loop_unlock(access::loop(*capture));
      // Never destroy a stream while do_trigger_driver still owns its pointer.
      return result >= 0 && wait([&] { return completed > previous; }) && !failed;
    }

    static void param_changed(void *opaque, std::uint32_t id, const spa_pod *param) {
      if (id != SPA_PARAM_Format || !param) return;
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      alignas(8) std::array<std::uint8_t, 512> bytes {};
      spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
      const spa_pod *buffers = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(g.width * g.height * 4),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(g.width * 4),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << (g.dmabuf ? SPA_DATA_DmaBuf : SPA_DATA_MemFd))));
      g.failed |= pw_stream_update_params(g.source, &buffers, 1) < 0;
    }
    static void add_buffer(void *opaque, pw_buffer *buffer) {
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      ++g.added;
      if (g.dmabuf) g.failed |= !g.allocate || !g.allocate(buffer);
    }
    static void remove_buffer(void *opaque, pw_buffer *buffer) {
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      ++g.removed;
      if (g.remove) g.remove(buffer);
    }
    static void process(void *opaque) {
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      auto *buffer = pw_stream_dequeue_buffer(g.source);
      if (!buffer) { ++g.empty; return; }
      auto &data = buffer->buffer->datas[0];
      if (g.dmabuf) g.failed |= !g.write || !g.write(buffer, g.marker);
      else if (data.data) std::memset(data.data, g.marker, g.width * g.height * 4);
      else g.failed = true;
      data.chunk->offset = 0;
      data.chunk->size = g.width * g.height * 4;
      data.chunk->stride = g.width * 4;
      ++g.writes[buffer];
      ++g.submitted;
      g.failed |= pw_stream_queue_buffer(g.source, buffer) < 0;
    }
    static void trigger_done(void *opaque) { ++static_cast<pipewire_producer_graph_t *>(opaque)->completed; }

    void close() {
      if (!access::loop(*capture)) return;
      capture->stop();
      // Also safe after a failed trigger: stop the owned loop before destroying
      // callback state. No user daemon or default core participates.
      pw_thread_loop_stop(access::loop(*capture));
      pw_thread_loop_lock(access::loop(*capture));
      if (link) { pw_proxy_destroy(link); link = nullptr; }
      if (source) { pw_stream_destroy(source); source = nullptr; }
      pw_thread_loop_unlock(access::loop(*capture));
      capture->shutdown();
    }
    ~pipewire_producer_graph_t() { close(); }
  };
}
