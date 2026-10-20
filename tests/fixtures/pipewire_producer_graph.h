/**
 * A real, private PipeWire graph with a two-buffer producer. No dequeue,
 * process, publication or enqueue callback is mocked. This is graph acceptance,
 * not compositor/portal or physical-stream acceptance.
 */
#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
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
    /** The capture stream state exactly as the real on_state_changed recorded it. */
    static pw_stream_state stream_state(capture_t &capture) {
      std::lock_guard lock(capture.frame_mtx_);
      return capture.stream_state_;
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
    // Both link hooks are added together under the owned loop lock and leave
    // together (remove_link_listeners) before any proxy, core or callback
    // state they could reach is destroyed. link_state and link_infos are
    // written only by the owned loop thread, from the real link info events,
    // and read under the owned loop lock like every other counter here.
    spa_hook link_listener {};
    spa_hook link_proxy_listener {};
    bool link_listeners_added = false;
    pw_link_state link_state = PW_LINK_STATE_INIT;
    std::size_t link_infos = 0;
    // Milliseconds after link creation at which start() first observed the
    // previous readiness (negotiated, driving, two buffers) and the full one
    // (plus link ACTIVE and capture STREAMING); -1 while unobserved. Plain
    // numbers written inside the one wait() predicate, under the loop lock.
    std::int64_t old_ready_ms = -1, ready_ms = -1;
    allocation_t allocate;
    write_t write;
    remove_t remove;
    std::map<pw_buffer *, std::size_t> writes;
    std::size_t completed = 0, submitted = 0, empty = 0, added = 0, removed = 0, formats = 0, destroyed = 0;
    std::uint8_t marker = 0;
    bool failed = false;
    const bool dmabuf;
    int width, height;

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

    /**
     * A bounded, locked reading for diagnostics: the owned loop lock first,
     * then the capture's own state under its frame mutex, the order the real
     * callbacks and the wait() predicates already use. Never call it from a
     * wait() predicate, which holds the loop lock. Safe before prepare(), after
     * a failed start() and after close(), when there is no loop to lock. A null
     * source or link with a live loop is a partial lifecycle and is reported as
     * such. It is called only by the thread that owns this graph, the same one
     * that calls close() and shutdown_capture(), so it never races shutdown.
     */
    std::string snapshot() {
      auto *loop = access::loop(*capture);
      if (loop) pw_thread_loop_lock(loop);
      auto unlock = util::fail_guard([&] { if (loop) pw_thread_loop_unlock(loop); });
      const auto capture_state = access::stream_state(*capture);
      std::size_t write_total = 0;
      for (const auto &[buffer, times] : writes) write_total += times;
      std::ostringstream out;
      out << "loop=" << (loop != nullptr) << " link=" << (link != nullptr) << " source=" << (source != nullptr)
          << " link_listeners=" << link_listeners_added << " link_infos=" << link_infos
          << " link_state=" << static_cast<int>(link_state) << '/' << pw_link_state_as_string(link_state)
          << " stream_state=" << static_cast<int>(capture_state) << '/' << pw_stream_state_as_string(capture_state)
          << " driving=" << (source && pw_stream_is_driving(source))
          << " negotiated=" << capture->negotiated() << " running=" << capture->running()
          << " completed=" << completed << " submitted=" << submitted << " empty=" << empty
          << " added=" << added << " removed=" << removed
          << " writes=" << writes.size() << '/' << write_total
          << " formats=" << formats << " destroyed=" << destroyed << " failed=" << failed
          << " old_ready_ms=" << old_ready_ms << " ready_ms=" << ready_ms;
      return out.str();
    }

    const spa_pod *build_format(spa_pod_builder &builder, int w, int h) const {
      // PipeWire 1.0.5's raw-format helper passes max_framerate by value to
      // a vararg that expects a pointer. Typed builders preserve the same
      // variable-rate offer on those headers without invoking that helper.
      spa_pod_frame frame {};
      const spa_rectangle size {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
      const spa_fraction variable_rate {0, 1}, maximum_rate {60, 1};
      spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
      spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video);
      spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
      spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
      spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
      spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRx);
      spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
      spa_pod_builder_rectangle(&builder, size.width, size.height);
      spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
      spa_pod_builder_fraction(&builder, variable_rate.num, variable_rate.denom);
      if (dmabuf) {
        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(&builder, DRM_FORMAT_MOD_LINEAR);
      }
      spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_maxFramerate, 0);
      spa_pod_builder_fraction(&builder, maximum_rate.num, maximum_rate.denom);
      return static_cast<const spa_pod *>(spa_pod_builder_pop(&builder, &frame));
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
        .destroy = destroy,
        .param_changed = param_changed,
        .add_buffer = add_buffer,
        .remove_buffer = remove_buffer,
        .process = process,
        .trigger_done = trigger_done,
      };
      pw_stream_add_listener(source, &listener, &events, this);
      alignas(8) std::array<std::uint8_t, 512> bytes {};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
      const spa_pod *format = build_format(builder, width, height);
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
      if (link) add_link_listeners();
      pw_thread_loop_unlock(access::loop(*capture));
      // Ready means the graph the capture will actually read from: the real
      // link reports ACTIVE and the capture stream reports STREAMING, within
      // the same two-second budget as the negotiated/driving/two-buffer
      // conditions. The predicate also keeps, as plain milliseconds, when each
      // stage was first seen; it formats and prints nothing and takes no lock
      // beyond the one wait() already holds and the capture's own frame mutex.
      const auto created = std::chrono::steady_clock::now();
      const auto elapsed = [&] {
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - created).count());
      };
      return link && wait([&] {
        const bool old_ready = capture->negotiated() && pw_stream_is_driving(source) && added == 2;
        if (old_ready && old_ready_ms < 0) old_ready_ms = elapsed();
        const bool ready = old_ready && link_state == PW_LINK_STATE_ACTIVE && access::stream_state(*capture) == PW_STREAM_STATE_STREAMING;
        if (ready && ready_ms < 0) ready_ms = elapsed();
        return ready;
      }) && !failed;
    }

    /** Caller holds the owned loop lock and link is a live proxy. */
    void add_link_listeners() {
      static const pw_proxy_events proxy_events {
        .version = PW_VERSION_PROXY_EVENTS,
        .destroy = link_destroy,
      };
      static const pw_link_events link_events {
        .version = PW_VERSION_LINK_EVENTS,
        .info = link_info,
      };
      // The proxy hook learns of any destruction (ours, the owned core's at
      // shutdown, or a test's); the object hook receives the real link info
      // events, which is what pw_link_add_listener does for a client proxy on
      // every PipeWire this builds against. Both are attached before the
      // first server reply can arrive.
      pw_proxy_add_listener(link, &link_proxy_listener, &proxy_events, this);
      pw_proxy_add_object_listener(link, &link_listener, &link_events, this);
      link_listeners_added = true;
    }

    /** Caller holds the owned loop lock or the loop is stopped. Idempotent. */
    void remove_link_listeners() {
      if (!link_listeners_added) return;
      spa_hook_remove(&link_listener);
      spa_hook_remove(&link_proxy_listener);
      link_listeners_added = false;
    }

    bool step(std::uint8_t next_marker) {
      auto *loop = access::loop(*capture);
      pw_thread_loop_lock(loop);
      marker = next_marker;
      const auto previous = completed;
      // Older non-RT streams schedule their producer callback on the main
      // loop before triggering the data loop. Trigger from that main loop so
      // the callback fills its buffer before the graph can consume it. The
      // blocking invoke releases and reacquires the thread-loop lock itself.
      const auto result = pw_loop_invoke(pw_thread_loop_get_loop(loop),
        [](spa_loop *, bool, std::uint32_t, const void *, std::size_t, void *opaque) {
          return pw_stream_trigger_process(static_cast<pipewire_producer_graph_t *>(opaque)->source);
        }, 0, nullptr, 0, true, this);
      pw_thread_loop_unlock(loop);
      // Never destroy a stream while do_trigger_driver still owns its pointer.
      return result >= 0 && wait([&] { return completed > previous; }) && !failed;
    }

    bool produce_after_return(std::uint8_t next_marker) {
      const auto previous = submitted;
      // The returned consumer buffer crosses the graph's IO handoff before
      // it becomes dequeuable at the producer. Drive at most two completed
      // iterations; never turn a failing assertion into an unbounded retry.
      for (int handoff = 0; handoff < 2; ++handoff) {
        if (!step(next_marker)) return false;
        if (submitted == previous + 1) return true;
        if (submitted != previous) return false;
      }
      return false;
    }

    bool renegotiate_size(int w, int h) {
      alignas(8) std::array<std::uint8_t, 512> bytes {};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
      const spa_pod *format = build_format(builder, w, h);
      pw_thread_loop_lock(access::loop(*capture));
      const int result = pw_stream_update_params(source, &format, 1);
      pw_thread_loop_unlock(access::loop(*capture));
      return result >= 0;
    }

    void shutdown_capture() {
      // Exercise production shutdown with this live graph still present. The
      // owned core destroys its producer and link; the stream destroy event
      // clears our pointer before the callback state can be destroyed. Our
      // link hooks leave first, under the loop lock, so that teardown never
      // reaches fixture memory through the link proxy.
      if (access::loop(*capture)) {
        pw_thread_loop_lock(access::loop(*capture));
        remove_link_listeners();
        pw_thread_loop_unlock(access::loop(*capture));
      }
      capture->shutdown();
      link = nullptr;
    }

    static void param_changed(void *opaque, std::uint32_t id, const spa_pod *param) {
      if (id != SPA_PARAM_Format || !param) return;
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      spa_video_info_raw info {};
      if (spa_format_video_raw_parse(param, &info) < 0) { g.failed = true; return; }
      g.width = static_cast<int>(info.size.width);
      g.height = static_cast<int>(info.size.height);
      ++g.formats;
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
    static void destroy(void *opaque) {
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      g.source = nullptr;
      ++g.destroyed;
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
    static void link_info(void *opaque, const pw_link_info *info) {
      if (!info) return;
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      ++g.link_infos;
      g.link_state = info->state;
    }
    static void link_destroy(void *opaque) {
      // Whoever destroys the proxy (close, the owned core at shutdown, a test)
      // does so under the loop lock; both hooks leave before the proxy is freed
      // and nothing can reach this fixture through the link afterwards.
      auto &g = *static_cast<pipewire_producer_graph_t *>(opaque);
      g.remove_link_listeners();
      g.link = nullptr;
    }

    void close() {
      if (!access::loop(*capture)) return;
      capture->stop();
      // Also safe after a failed trigger: stop the owned loop before destroying
      // callback state. No user daemon or default core participates.
      pw_thread_loop_stop(access::loop(*capture));
      pw_thread_loop_lock(access::loop(*capture));
      // Same order as before; only the link hooks leave first, ahead of the
      // proxy they are attached to. The stream hook stays for its destroy event.
      remove_link_listeners();
      if (link) { pw_proxy_destroy(link); link = nullptr; }
      if (source) { pw_stream_destroy(source); source = nullptr; }
      pw_thread_loop_unlock(access::loop(*capture));
      capture->shutdown();
    }
    ~pipewire_producer_graph_t() { close(); }
  };
}
