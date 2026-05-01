/**
 * @file src/platform/linux/portal_grab.cpp
 * @brief XDG Desktop Portal ScreenCast capture backend.
 *
 * Captures a window (or monitor) via the xdg-desktop-portal ScreenCast D-Bus
 * API, receiving frames through PipeWire. This is the primary capture backend
 * for the Polaris cage-as-window architecture.
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/mman.h>

#include <gio/gio.h>
#include <wayland-client.h>
#include <wlr-screencopy-unstable-v1.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/debug/types.h>
#include <spa/param/video/format.h>
#include <spa/utils/result.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

#include "src/platform/linux/cage_display_router.h"

#ifdef POLARIS_BUILD_CUDA
  #include "src/platform/linux/cuda.h"
#endif
#ifdef POLARIS_BUILD_VAAPI
  #include "src/platform/linux/vaapi.h"
#endif

using namespace std::literals;

namespace portal {

  // -----------------------------------------------------------------------
  // Restore token persistence
  // -----------------------------------------------------------------------

  static std::string token_path() {
    std::string base = config::sunshine.config_file.empty()
      ? std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config/sunshine"
      : config::sunshine.config_file.substr(0, config::sunshine.config_file.rfind('/'));
    return base + "/portal_restore_token.txt";
  }

  static std::string load_restore_token() {
    std::ifstream f(token_path());
    if (!f.good()) return "";
    std::string token;
    std::getline(f, token);
    return token;
  }

  static void save_restore_token(const std::string &token) {
    if (token.empty()) return;
    std::ofstream f(token_path());
    f << token;
    BOOST_LOG(info) << "portal: Saved restore token for future sessions"sv;
  }

  // -----------------------------------------------------------------------
  // D-Bus introspection
  // -----------------------------------------------------------------------

  bool
  is_portal_available() {
    GError *gerr = nullptr;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &gerr);
    if (!connection) {
      if (gerr) {
        BOOST_LOG(debug) << "Portal: could not connect to session bus: "sv << gerr->message;
        g_error_free(gerr);
      }
      return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
      connection,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.DBus.Introspectable",
      "Introspect",
      nullptr,
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      3000,
      nullptr,
      &gerr);

    if (!result) {
      if (gerr) {
        BOOST_LOG(debug) << "Portal: could not introspect portal desktop: "sv << gerr->message;
        g_error_free(gerr);
      }
      g_object_unref(connection);
      return false;
    }

    const gchar *xml = nullptr;
    g_variant_get(result, "(&s)", &xml);

    bool available = false;
    if (xml) {
      std::string introspection(xml);
      available = introspection.find("org.freedesktop.portal.ScreenCast") != std::string::npos;
    }

    g_variant_unref(result);
    g_object_unref(connection);
    return available;
  }

  // -----------------------------------------------------------------------
  // Portal D-Bus session
  // -----------------------------------------------------------------------

  struct portal_session_t {
    GDBusConnection *conn = nullptr;
    std::string session_handle;
    uint32_t pw_node_id = 0;
    std::string restore_token;
    bool ready = false;
    bool failed = false;

    ~portal_session_t() {
      if (conn) {
        if (!session_handle.empty()) {
          g_dbus_connection_call_sync(conn,
            "org.freedesktop.portal.Desktop",
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
        }
        g_object_unref(conn);
      }
    }
  };

  // Helper: call a portal method synchronously via D-Bus
  static GVariant *portal_call_sync(GDBusConnection *conn, const char *method,
    GVariant *params, int timeout_ms = 5000) {
    GError *gerr = nullptr;
    auto *result = g_dbus_connection_call_sync(conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast",
      method, params, nullptr,
      G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &gerr);
    if (!result && gerr) {
      BOOST_LOG(warning) << "portal: D-Bus call "sv << method << " failed: "sv << gerr->message;
      g_error_free(gerr);
    }
    return result;
  }

  // Helper: wait for a Response signal on a request path.
  // Uses a dedicated GMainLoop thread to ensure D-Bus signals are delivered.
  static GVariant *wait_for_response(GDBusConnection *conn, const std::string &request_path,
    int timeout_ms = 30000) {

    struct cb_data_t {
      GVariant *result = nullptr;
      std::atomic<bool> done{false};
      uint32_t response = 99;
      GMainLoop *loop = nullptr;
    } cb_data;

    // Create a dedicated main context and loop for this wait
    auto *ctx = g_main_context_new();
    cb_data.loop = g_main_loop_new(ctx, FALSE);

    // Push this context as the thread default so GDBus dispatches signals here
    g_main_context_push_thread_default(ctx);

    auto sub_id = g_dbus_connection_signal_subscribe(conn,
      "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request",
      "Response",
      request_path.c_str(),
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
      [](GDBusConnection *, const gchar *, const gchar *, const gchar *,
         const gchar *, GVariant *parameters, gpointer userdata) {
        auto *data = static_cast<cb_data_t *>(userdata);
        uint32_t resp = 0;
        GVariant *results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &resp, &results);
        data->response = resp;
        data->result = results;  // caller must unref
        data->done = true;
        if (data->loop) g_main_loop_quit(data->loop);
      },
      &cb_data, nullptr);

    // Run the main loop with a timeout
    auto timeout_source = g_timeout_source_new(timeout_ms);
    g_source_set_callback(timeout_source, [](gpointer userdata) -> gboolean {
      auto *data = static_cast<cb_data_t *>(userdata);
      if (!data->done) {
        if (data->loop) g_main_loop_quit(data->loop);
      }
      return G_SOURCE_REMOVE;
    }, &cb_data, nullptr);
    g_source_attach(timeout_source, ctx);
    g_source_unref(timeout_source);

    g_main_loop_run(cb_data.loop);

    g_dbus_connection_signal_unsubscribe(conn, sub_id);
    g_main_context_pop_thread_default(ctx);
    g_main_loop_unref(cb_data.loop);
    g_main_context_unref(ctx);

    if (!cb_data.done) {
      BOOST_LOG(warning) << "portal: Timeout waiting for response on "sv << request_path;
      return nullptr;
    }
    if (cb_data.response != 0) {
      BOOST_LOG(warning) << "portal: Response code "sv << cb_data.response
                         << " on "sv << request_path;
      if (cb_data.result) g_variant_unref(cb_data.result);
      return nullptr;
    }
    return cb_data.result;
  }

  // Build the request object path from the connection's unique name and a token
  static std::string make_request_path(GDBusConnection *conn, const std::string &token) {
    const char *name = g_dbus_connection_get_unique_name(conn);
    if (!name) return "";
    std::string sender(name);
    // ":1.234" → "1_234" (remove leading ':', replace '.' with '_')
    if (!sender.empty() && sender[0] == ':') {
      sender = sender.substr(1);
    }
    for (auto &c : sender) {
      if (c == '.') c = '_';
    }
    return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  }

  static std::unique_ptr<portal_session_t> create_portal_session(uint32_t capture_type = 2) {
    auto session = std::make_unique<portal_session_t>();

    GError *gerr = nullptr;
    session->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &gerr);
    if (!session->conn) {
      BOOST_LOG(warning) << "portal: Cannot connect to session bus"sv;
      if (gerr) g_error_free(gerr);
      session->failed = true;
      return session;
    }

    // Step 1: CreateSession
    BOOST_LOG(info) << "portal: Creating ScreenCast session..."sv;
    {
      std::string req_path = make_request_path(session->conn, "polaris_req1");

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token",
        g_variant_new_string("polaris_capture"));
      g_variant_builder_add(&builder, "{sv}", "handle_token",
        g_variant_new_string("polaris_req1"));

      auto *result = portal_call_sync(session->conn, "CreateSession",
        g_variant_new("(a{sv})", &builder));
      if (result) g_variant_unref(result);

      auto *resp = wait_for_response(session->conn, req_path, 10000);
      if (!resp) {
        session->failed = true;
        return session;
      }

      GVariant *handle_v = g_variant_lookup_value(resp, "session_handle", G_VARIANT_TYPE_STRING);
      if (handle_v) {
        session->session_handle = g_variant_get_string(handle_v, nullptr);
        g_variant_unref(handle_v);
        BOOST_LOG(info) << "portal: Session created"sv;
      }
      g_variant_unref(resp);
    }

    if (session->session_handle.empty()) {
      session->failed = true;
      return session;
    }

    // Step 2: SelectSources
    BOOST_LOG(info) << "portal: Selecting sources (type="sv << capture_type << ")..."sv;
    {
      std::string req_path = make_request_path(session->conn, "polaris_req2");

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "types",
        g_variant_new_uint32(capture_type));
      g_variant_builder_add(&builder, "{sv}", "cursor_mode",
        g_variant_new_uint32(2));
      g_variant_builder_add(&builder, "{sv}", "persist_mode",
        g_variant_new_uint32(2));
      g_variant_builder_add(&builder, "{sv}", "handle_token",
        g_variant_new_string("polaris_req2"));

      auto saved_token = load_restore_token();
      if (!saved_token.empty()) {
        BOOST_LOG(info) << "portal: Using saved restore token (auto-select)"sv;
        g_variant_builder_add(&builder, "{sv}", "restore_token",
          g_variant_new_string(saved_token.c_str()));
      }

      auto *result = portal_call_sync(session->conn, "SelectSources",
        g_variant_new("(oa{sv})", session->session_handle.c_str(), &builder));
      if (result) g_variant_unref(result);

      auto *resp = wait_for_response(session->conn, req_path, 10000);
      if (!resp) {
        session->failed = true;
        return session;
      }
      g_variant_unref(resp);
    }

    // Step 3: Start (may show window picker — long timeout)
    BOOST_LOG(info) << "portal: Starting capture (window picker may appear)..."sv;
    {
      std::string req_path = make_request_path(session->conn, "polaris_req3");

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token",
        g_variant_new_string("polaris_req3"));

      auto *result = portal_call_sync(session->conn, "Start",
        g_variant_new("(osa{sv})", session->session_handle.c_str(), "", &builder),
        120000);
      if (result) g_variant_unref(result);

      auto *resp = wait_for_response(session->conn, req_path, 120000);
      if (!resp) {
        session->failed = true;
        return session;
      }

      // Parse streams
      GVariant *streams_v = g_variant_lookup_value(resp, "streams", nullptr);
      if (streams_v) {
        GVariantIter iter;
        g_variant_iter_init(&iter, streams_v);
        GVariant *stream_entry = nullptr;
        while ((stream_entry = g_variant_iter_next_value(&iter)) != nullptr) {
          uint32_t node_id = 0;
          GVariant *props = nullptr;
          g_variant_get(stream_entry, "(u@a{sv})", &node_id, &props);
          session->pw_node_id = node_id;
          BOOST_LOG(info) << "portal: PipeWire node ID: "sv << node_id;
          if (props) g_variant_unref(props);
          g_variant_unref(stream_entry);
          break;
        }
        g_variant_unref(streams_v);
      }

      // Check for restore_token
      GVariant *token_v = g_variant_lookup_value(resp, "restore_token", G_VARIANT_TYPE_STRING);
      if (token_v) {
        session->restore_token = g_variant_get_string(token_v, nullptr);
        g_variant_unref(token_v);
        if (!session->restore_token.empty()) {
          save_restore_token(session->restore_token);
        }
      }

      g_variant_unref(resp);

      if (session->pw_node_id > 0) {
        session->ready = true;
      }
    }

    return session;
  }

  // -----------------------------------------------------------------------
  // PipeWire frame capture
  // -----------------------------------------------------------------------

  struct pw_capture_t {
    struct pw_thread_loop *pw_loop = nullptr;
    struct pw_stream *pw_stream_handle = nullptr;

    int frame_width = 0;
    int frame_height = 0;
    uint32_t frame_stride = 0;

    std::mutex frame_mtx;
    std::vector<uint8_t> frame_data;
    bool frame_available = false;
    std::condition_variable frame_cv;

    std::atomic<bool> running{false};
    std::atomic<bool> negotiated{false};

    ~pw_capture_t() {
      if (pw_loop) pw_thread_loop_stop(pw_loop);
      if (pw_stream_handle) pw_stream_destroy(pw_stream_handle);
      if (pw_loop) pw_thread_loop_destroy(pw_loop);
    }
  };

  static void on_process(void *userdata) {
    auto *cap = static_cast<pw_capture_t *>(userdata);
    struct pw_buffer *b = pw_stream_dequeue_buffer(cap->pw_stream_handle);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf->datas[0].data || buf->datas[0].chunk->size == 0) {
      pw_stream_queue_buffer(cap->pw_stream_handle, b);
      return;
    }

    uint32_t size = buf->datas[0].chunk->size;
    {
      std::lock_guard lk(cap->frame_mtx);
      cap->frame_data.resize(size);
      std::memcpy(cap->frame_data.data(), buf->datas[0].data, size);
      cap->frame_available = true;
      cap->frame_cv.notify_one();
    }

    pw_stream_queue_buffer(cap->pw_stream_handle, b);
  }

  static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    auto *cap = static_cast<pw_capture_t *>(userdata);
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return;
    if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw) return;

    struct spa_video_info_raw raw_info;
    spa_format_video_raw_parse(param, &raw_info);

    cap->frame_width = raw_info.size.width;
    cap->frame_height = raw_info.size.height;
    cap->frame_stride = cap->frame_width * 4;
    cap->negotiated = true;

    BOOST_LOG(info) << "portal: PipeWire format negotiated: "sv
                    << cap->frame_width << "x"sv << cap->frame_height;

    // Set buffer parameters
    uint8_t params_buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *buf_param = (const struct spa_pod *) spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr));

    pw_stream_update_params(cap->pw_stream_handle, &buf_param, 1);
  }

  static void on_state_changed(void *userdata, enum pw_stream_state old,
    enum pw_stream_state state, const char *errmsg) {
    BOOST_LOG(info) << "portal: PipeWire state: "sv
                    << pw_stream_state_as_string(old) << " -> "sv
                    << pw_stream_state_as_string(state);
    if (state == PW_STREAM_STATE_ERROR && errmsg) {
      BOOST_LOG(warning) << "portal: PipeWire error: "sv << errmsg;
    }
  }

  static const struct pw_stream_events pw_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
  };

  static std::unique_ptr<pw_capture_t> start_pw_capture(uint32_t node_id, int req_w, int req_h) {
    auto cap = std::make_unique<pw_capture_t>();

    pw_init(nullptr, nullptr);

    cap->pw_loop = pw_thread_loop_new("polaris-portal-capture", nullptr);
    if (!cap->pw_loop) {
      BOOST_LOG(warning) << "portal: Failed to create PipeWire loop"sv;
      return nullptr;
    }

    auto *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Screen",
      nullptr);

    cap->pw_stream_handle = pw_stream_new_simple(
      pw_thread_loop_get_loop(cap->pw_loop),
      "polaris-portal-capture",
      props,
      &pw_events,
      cap.get());

    if (!cap->pw_stream_handle) {
      BOOST_LOG(warning) << "portal: Failed to create PipeWire stream"sv;
      return nullptr;
    }

    // Accept any video format — let PipeWire/portal decide
    uint8_t params_buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    auto *fmt_param = (const struct spa_pod *) spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRA,
        SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_RGBA));

    if (pw_stream_connect(cap->pw_stream_handle,
          PW_DIRECTION_INPUT,
          node_id,
          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
          &fmt_param, 1) < 0) {
      BOOST_LOG(warning) << "portal: Failed to connect PipeWire stream to node "sv << node_id;
      return nullptr;
    }

    if (pw_thread_loop_start(cap->pw_loop) < 0) {
      BOOST_LOG(warning) << "portal: Failed to start PipeWire loop"sv;
      return nullptr;
    }

    cap->running = true;
    BOOST_LOG(info) << "portal: PipeWire capture started on node "sv << node_id;
    return cap;
  }

  // -----------------------------------------------------------------------
  // Direct wlr-screencopy capture from cage (no portal, no picker)
  // Self-contained Wayland client that connects to cage's socket.
  // -----------------------------------------------------------------------

  struct screencopy_state_t {
    // Wayland globals
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    wl_output *output = nullptr;
    zwlr_screencopy_manager_v1 *screencopy_mgr = nullptr;

    // SHM buffer
    int shm_fd = -1;
    void *shm_data = nullptr;
    size_t shm_size = 0;
    wl_shm_pool *shm_pool = nullptr;
    wl_buffer *shm_buffer = nullptr;

    // Frame info from buffer event
    uint32_t fmt = 0, width = 0, height = 0, stride = 0;
    bool buffer_info_received = false;
    bool frame_ready = false;
    bool frame_failed = false;

    void cleanup_buffer() {
      if (shm_buffer) { wl_buffer_destroy(shm_buffer); shm_buffer = nullptr; }
      if (shm_pool) { wl_shm_pool_destroy(shm_pool); shm_pool = nullptr; }
      if (shm_data) { munmap(shm_data, shm_size); shm_data = nullptr; }
      if (shm_fd >= 0) { ::close(shm_fd); shm_fd = -1; }
      shm_size = 0;
    }

    ~screencopy_state_t() {
      cleanup_buffer();
      if (screencopy_mgr) zwlr_screencopy_manager_v1_destroy(screencopy_mgr);
      if (shm) wl_shm_destroy(shm);
      if (output) wl_output_destroy(output);
      if (registry) wl_registry_destroy(registry);
      if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
  };

  // Registry listener
  static void sc_registry_global(void *data, wl_registry *reg, uint32_t id,
    const char *iface, uint32_t version) {
    auto *st = static_cast<screencopy_state_t *>(data);
    if (!std::strcmp(iface, wl_shm_interface.name)) {
      st->shm = static_cast<wl_shm *>(wl_registry_bind(reg, id, &wl_shm_interface, 1));
    } else if (!std::strcmp(iface, wl_output_interface.name) && !st->output) {
      st->output = static_cast<wl_output *>(wl_registry_bind(reg, id, &wl_output_interface, 1));
    } else if (!std::strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
      st->screencopy_mgr = static_cast<zwlr_screencopy_manager_v1 *>(
        wl_registry_bind(reg, id, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
    }
  }
  static void sc_registry_global_remove(void *, wl_registry *, uint32_t) {}
  static const wl_registry_listener sc_registry_listener = {
    sc_registry_global, sc_registry_global_remove
  };

  // Frame listener callbacks
  static void sc_frame_buffer(void *data, zwlr_screencopy_frame_v1 *,
    uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->fmt = format; st->width = w; st->height = h; st->stride = stride;
    st->buffer_info_received = true;
    BOOST_LOG(info) << "screencopy: frame format=" << format
                    << " (" << (format == 0 ? "ARGB8888" : format == 1 ? "XRGB8888" : "other") << ")"
                    << " " << w << "x" << h << " stride=" << stride;
  }
  static void sc_frame_flags(void *, zwlr_screencopy_frame_v1 *, uint32_t) {}
  static void sc_frame_ready(void *data, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->frame_ready = true;
  }
  static void sc_frame_failed(void *data, zwlr_screencopy_frame_v1 *) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->frame_failed = true;
  }
  static void sc_frame_damage(void *, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t, uint32_t) {}
  static void sc_frame_linux_dmabuf(void *, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t) {}
  static void sc_frame_buffer_done(void *data, zwlr_screencopy_frame_v1 *frame) {
    auto *st = static_cast<screencopy_state_t *>(data);
    if (!st->buffer_info_received || !st->shm) {
      st->frame_failed = true;
      return;
    }

    // Create SHM buffer in the compositor's native format
    st->cleanup_buffer();
    st->shm_size = st->stride * st->height;
    st->shm_fd = memfd_create("polaris-sc", MFD_CLOEXEC);
    if (st->shm_fd < 0 || ftruncate(st->shm_fd, st->shm_size) < 0) {
      st->frame_failed = true;
      return;
    }
    st->shm_data = mmap(nullptr, st->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, st->shm_fd, 0);
    if (st->shm_data == MAP_FAILED) { st->shm_data = nullptr; st->frame_failed = true; return; }

    st->shm_pool = wl_shm_create_pool(st->shm, st->shm_fd, st->shm_size);
    st->shm_buffer = wl_shm_pool_create_buffer(st->shm_pool, 0,
      st->width, st->height, st->stride, st->fmt);

    // Request copy
    zwlr_screencopy_frame_v1_copy(frame, st->shm_buffer);
  }

  static const zwlr_screencopy_frame_v1_listener sc_frame_listener = {
    .buffer = sc_frame_buffer,
    .flags = sc_frame_flags,
    .ready = sc_frame_ready,
    .failed = sc_frame_failed,
    .damage = sc_frame_damage,
    .linux_dmabuf = sc_frame_linux_dmabuf,
    .buffer_done = sc_frame_buffer_done,
  };

  /**
   * @brief Direct screencopy capture loop from cage.
   * Connects to cage's Wayland socket, captures frames via wlr-screencopy SHM,
   * and feeds them to the encoder. No portal, no picker, no D-Bus.
   */
  // Stop flag for screencopy — set when session ends
  static std::atomic<bool> g_screencopy_stop{false};

  static platf::capture_e cage_screencopy_capture(
    const std::string &cage_socket,
    int req_width, int req_height,
    const platf::display_t::push_captured_image_cb_t &push_cb,
    const platf::display_t::pull_free_image_cb_t &pull_cb,
    bool *cursor,
    int &out_width, int &out_height) {

    g_screencopy_stop = false;

    screencopy_state_t st;
    st.display = wl_display_connect(cage_socket.c_str());
    if (!st.display) {
      BOOST_LOG(warning) << "screencopy: Cannot connect to "sv << cage_socket;
      return platf::capture_e::reinit;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &sc_registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.screencopy_mgr || !st.output || !st.shm) {
      BOOST_LOG(warning) << "screencopy: Missing required protocols from cage"sv;
      return platf::capture_e::reinit;
    }

    BOOST_LOG(info) << "screencopy: Connected to cage on "sv << cage_socket;

    // Shared frame buffer between screencopy thread and capture() caller
    std::mutex frame_mtx;
    std::condition_variable frame_cv;
    std::vector<uint8_t> frame_buf;
    bool has_frame = false;
    std::atomic<bool> sc_running{true};
    std::atomic<bool> sc_error{false};

    // Run screencopy in a background thread so capture() can be interrupted
    auto sc_thread = std::thread([&]() {
      while (sc_running && !g_screencopy_stop) {
        st.buffer_info_received = false;
        st.frame_ready = false;
        st.frame_failed = false;

        // Check connection health before requesting a frame
        if (wl_display_get_error(st.display) != 0) {
          BOOST_LOG(warning) << "screencopy: Wayland connection error, stopping"sv;
          sc_error = true;
          return;
        }

        auto *frame = zwlr_screencopy_manager_v1_capture_output(
          st.screencopy_mgr, *cursor ? 1 : 0, st.output);
        zwlr_screencopy_frame_v1_add_listener(frame, &sc_frame_listener, &st);

        while (!st.frame_ready && !st.frame_failed && sc_running && !g_screencopy_stop) {
          // Flush before dispatch to detect broken connections
          if (wl_display_flush(st.display) < 0 && errno != EAGAIN) {
            BOOST_LOG(warning) << "screencopy: Wayland flush failed (compositor died?), stopping"sv;
            sc_error = true;
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
          }
          if (wl_display_dispatch(st.display) < 0) {
            BOOST_LOG(warning) << "screencopy: Wayland dispatch failed, stopping"sv;
            sc_error = true;
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
          }
        }
        zwlr_screencopy_frame_v1_destroy(frame);

        if (!sc_running || g_screencopy_stop || st.frame_failed) {
          if (st.frame_failed) sc_error = true;
          return;
        }

        // Copy frame to shared buffer
        if (st.shm_data && st.shm_size > 0) {
          std::lock_guard lk(frame_mtx);
          frame_buf.resize(st.shm_size);
          std::memcpy(frame_buf.data(), st.shm_data, st.shm_size);
          has_frame = true;
          frame_cv.notify_one();
        }
      }
    });

    // Consume frames from the screencopy thread
    while (!g_screencopy_stop) {
      std::unique_lock lk(frame_mtx);
      if (!has_frame) {
        frame_cv.wait_for(lk, 100ms, [&] { return has_frame || g_screencopy_stop || sc_error.load(); });
      }

      if (g_screencopy_stop || sc_error) {
        lk.unlock();
        break;
      }

      if (!has_frame) {
        lk.unlock();
        // Timeout — check if encoder wants us to stop
        std::shared_ptr<platf::img_t> timeout_img;
        if (!push_cb(std::move(timeout_img), false)) {
          break;
        }
        continue;
      }

      // Update dimensions
      if (st.width > 0 && st.height > 0) {
        out_width = st.width;
        out_height = st.height;
      }

      std::shared_ptr<platf::img_t> img_out;
      if (!pull_cb(img_out)) {
        lk.unlock();
        break;
      }

      int copy_h = std::min((int) st.height, img_out->height);
      int copy_w = std::min((int) st.width, img_out->width);

      bool is_3bpp = (st.stride == st.width * 3);

      // Debug: check if frame data has actual content (one-time log)
      static bool logged_frame_content = false;
      if (!logged_frame_content && st.shm_size > 0) {
        size_t nonzero = 0;
        for (size_t i = 0; i < std::min((size_t)st.shm_size, (size_t)1000); ++i) {
          if (((uint8_t*)frame_buf.data())[i] != 0) nonzero++;
        }
        BOOST_LOG(info) << "screencopy: Frame content check — "
                        << nonzero << "/1000 non-zero bytes, "
                        << "3bpp=" << is_3bpp
                        << ", shm_size=" << st.shm_size
                        << ", stride=" << st.stride
                        << ", " << st.width << "x" << st.height;
        logged_frame_content = true;
      }

      if (is_3bpp) {
        // BGR888 (3 bytes/pixel) → BGRA (4 bytes/pixel) conversion
        // wlroots BGR888 = memory order [B, G, R] per pixel
        // CUDA kernel reads texture as BGRA = memory order [B, G, R, A]
        // But the actual byte order from headless may be [R, G, B] — swap R↔B
        for (int y = 0; y < copy_h; ++y) {
          const uint8_t *src = frame_buf.data() + y * st.stride;
          uint8_t *dst = img_out->data + y * img_out->row_pitch;
          for (int x = 0; x < copy_w; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 2];  // B ← src R (swap)
            dst[x * 4 + 1] = src[x * 3 + 1];  // G
            dst[x * 4 + 2] = src[x * 3 + 0];  // R ← src B (swap)
            dst[x * 4 + 3] = 255;              // A (opaque)
          }
        }
      } else {
        // 4bpp format (XRGB8888/ARGB8888) — direct copy
        int copy_bytes = std::min((int) st.stride, img_out->row_pitch);
        for (int y = 0; y < copy_h; ++y) {
          std::memcpy(img_out->data + y * img_out->row_pitch,
            frame_buf.data() + y * st.stride, copy_bytes);
        }
      }
      img_out->frame_timestamp = std::chrono::steady_clock::now();
      has_frame = false;
      lk.unlock();

      if (!push_cb(std::move(img_out), true)) {
        break;
      }
    }

    // Signal thread to stop
    sc_running = false;
    g_screencopy_stop = true;

    // Flush pending events so the thread sees the stop flag on next iteration
    if (st.display) {
      wl_display_flush(st.display);
    }

    // Wait for the capture thread to exit (with timeout to avoid deadlock)
    if (sc_thread.joinable()) {
      auto future = std::async(std::launch::async, [&] { sc_thread.join(); });
      if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        BOOST_LOG(warning) << "screencopy: Capture thread didn't stop in 2s, detaching"sv;
        sc_thread.detach();
      }
    }

    BOOST_LOG(info) << "screencopy: Capture stopped"sv;
    return sc_error ? platf::capture_e::reinit : platf::capture_e::ok;
  }

  // -----------------------------------------------------------------------
  // Global portal session — persists across stream reconnections.
  // Created once (with window picker), reused for all subsequent streams.
  // -----------------------------------------------------------------------

  static std::unique_ptr<portal_session_t> g_portal;
  static std::unique_ptr<pw_capture_t> g_capture;
  static uint32_t g_node_id = 0;

  static bool ensure_global_session() {
    // Create the portal D-Bus session once (shows picker on first use)
    if (!g_portal || !g_portal->ready || g_portal->pw_node_id == 0) {
      g_portal.reset();
      g_portal = create_portal_session(2);
      if (!g_portal || g_portal->failed || !g_portal->ready || g_portal->pw_node_id == 0) {
        BOOST_LOG(warning) << "portal: Failed to create global session"sv;
        g_portal.reset();
        return false;
      }
      g_node_id = g_portal->pw_node_id;
      BOOST_LOG(info) << "portal: Global session ready, node "sv << g_node_id;
    }
    return true;
  }

  static pw_capture_t *ensure_global_capture(int width, int height) {
    // If capture is running and healthy, reuse it
    if (g_capture && g_capture->running) {
      return g_capture.get();
    }

    // PipeWire stream died or never created — need a fresh portal session
    // because the old PipeWire node is no longer valid after disconnect
    g_capture.reset();
    g_portal.reset();
    g_node_id = 0;

    if (!ensure_global_session()) {
      return nullptr;
    }

    g_capture = start_pw_capture(g_node_id, width, height);
    if (!g_capture) {
      BOOST_LOG(warning) << "portal: Failed to start PipeWire capture"sv;
      return nullptr;
    }

    // Wait for format negotiation
    for (int i = 0; i < 100 && !g_capture->negotiated; ++i) {
      std::this_thread::sleep_for(100ms);
    }

    return g_capture.get();
  }

  // -----------------------------------------------------------------------
  // Display backend
  // -----------------------------------------------------------------------

  class portal_display_t: public platf::display_t {
  public:
    ~portal_display_t() override {
      g_screencopy_stop = true;
    }

    int cfg_width = 0;
    int cfg_height = 0;
    platf::mem_type_e mem_type = platf::mem_type_e::system;

    bool probe_only = false;  // true during encoder probe (skip portal session)

    int
    init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      cfg_width = config.width;
      cfg_height = config.height;
      mem_type = hwdevice_type;

      if (!probe_only) {
        BOOST_LOG(info) << "portal: Initializing capture "sv
                        << cfg_width << "x"sv << cfg_height;

        // If cage/labwc compositor is configured (windowed or headless), skip portal entirely.
        // Direct wlr-screencopy will be used in capture() instead.
        // Check config, not runtime state — cage may not be running yet at init time.
        bool cage_configured = config::video.linux_display.use_cage_compositor;
        if (!cage_configured) {
          if (!ensure_global_session()) {
            return -1;
          }

          auto *cap = ensure_global_capture(cfg_width, cfg_height);
          if (!cap) {
            return -1;
          }

          if (cap->negotiated && cap->frame_width > 0 && cap->frame_height > 0) {
            cfg_width = cap->frame_width;
            cfg_height = cap->frame_height;
          }
        } else {
          BOOST_LOG(info) << "portal: Cage/labwc active — skipping portal, will use direct screencopy"sv;
        }
      } else {
        BOOST_LOG(info) << "portal: Probe mode — using dummy "sv
                        << cfg_width << "x"sv << cfg_height;
      }

      this->env_width = cfg_width;
      this->env_height = cfg_height;
      this->width = cfg_width;
      this->height = cfg_height;

      BOOST_LOG(info) << "portal: Capture ready — "sv << cfg_width << "x"sv << cfg_height
                      << " env="sv << this->env_width << "x"sv << this->env_height;
      return 0;
    }

    platf::capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb,
      const pull_free_image_cb_t &pull_free_image_cb,
      bool *cursor) override {

      BOOST_LOG(info) << "portal: capture() called"sv;

      // If cage/labwc is configured, use direct wlr-screencopy (no portal, no picker)
#ifdef __linux__
      if (config::video.linux_display.use_cage_compositor) {
        // Wait for labwc to be running (it may still be starting up)
        for (int wait = 0; wait < 50 && !cage_display_router::is_running(); ++wait) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (cage_display_router::is_running()) {
          auto cage_socket = cage_display_router::get_wayland_socket();
          if (!cage_socket.empty()) {
            BOOST_LOG(info) << "portal: Using direct screencopy from cage ("sv << cage_socket << ")"sv;
            auto result = cage_screencopy_capture(
              cage_socket, cfg_width, cfg_height,
              push_captured_image_cb, pull_free_image_cb, cursor,
              cfg_width, cfg_height);
            if (cfg_width > 0 && cfg_height > 0) {
              this->env_width = cfg_width;
              this->env_height = cfg_height;
              this->width = cfg_width;
              this->height = cfg_height;
            }
            return result;
          }
        }
        BOOST_LOG(warning) << "portal: Cage configured but not running after 5s — cannot capture"sv;
        return platf::capture_e::reinit;
      }
#endif

      // Fallback: portal D-Bus capture (only when cage is NOT configured)
      if (!ensure_global_session()) {
        BOOST_LOG(warning) << "portal: No capture session available"sv;
        return platf::capture_e::reinit;
      }

      auto *cap = ensure_global_capture(cfg_width, cfg_height);
      if (!cap) {
        BOOST_LOG(warning) << "portal: No capture available"sv;
        return platf::capture_e::reinit;
      }

      if (cap->negotiated && cap->frame_width > 0 && cap->frame_height > 0) {
        cfg_width = cap->frame_width;
        cfg_height = cap->frame_height;
        this->env_width = cfg_width;
        this->env_height = cfg_height;
        this->width = cfg_width;
        this->height = cfg_height;
      }

      while (cap && cap->running) {
        // Wait for a frame (up to 1 second)
        std::unique_lock lk(cap->frame_mtx);
        if (!cap->frame_available) {
          cap->frame_cv.wait_for(lk, 1s, [&] { return cap->frame_available; });
        }

        if (!cap->frame_available) {
          // Timeout — send empty frame
          std::shared_ptr<platf::img_t> dummy;
          if (!push_captured_image_cb(std::move(dummy), false)) {
            return platf::capture_e::ok;
          }
          continue;
        }

        std::shared_ptr<platf::img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        // Copy frame
        int copy_h = std::min(cap->frame_height, img_out->height);
        int copy_w = std::min(cap->frame_width, img_out->width);
        int src_stride = cap->frame_width * 4;
        int dst_stride = img_out->row_pitch;

        for (int y = 0; y < copy_h; ++y) {
          std::memcpy(
            img_out->data + y * dst_stride,
            cap->frame_data.data() + y * src_stride,
            copy_w * 4);
        }

        img_out->frame_timestamp = std::chrono::steady_clock::now();
        cap->frame_available = false;
        lk.unlock();

        if (!push_captured_image_cb(std::move(img_out), true)) {
          return platf::capture_e::ok;
        }
      }

      return platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t>
    alloc_img() override {
      auto img = std::make_shared<platf::img_t>();

      img->width = cfg_width;
      img->height = cfg_height;
      img->pixel_pitch = 4;
      img->row_pitch = cfg_width * 4;
      img->data = new uint8_t[cfg_height * img->row_pitch]();

      BOOST_LOG(info) << "portal: alloc_img "sv << img->width << "x"sv << img->height
                      << " env="sv << this->env_width << "x"sv << this->env_height;
      return img;
    }

    int
    dummy_img(platf::img_t *img) override {
      if (!img || !img->data) return -1;
      std::memset(img->data, 0, img->height * img->row_pitch);
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t>
    make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      int w = cfg_width;
      int h = cfg_height;

#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(w, h, false);
      }
#endif
#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (pix_fmt == platf::pix_fmt_e::nv12) {
          return cuda::make_avcodec_encode_device(w, h, false);
        }

        return std::make_unique<platf::avcodec_encode_device_t>();
      }
#endif
      return std::make_unique<platf::avcodec_encode_device_t>();
    }
  };

}  // namespace portal

namespace platf {

  std::shared_ptr<display_t>
  portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    auto disp = std::make_shared<portal::portal_display_t>();
    // Use probe mode for encoder detection at startup.
    // The real portal session (with window picker) is created lazily on first capture().
    disp->probe_only = true;
    if (disp->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }
    return disp;
  }

  std::vector<std::string>
  portal_display_names() {
    std::vector<std::string> names;

    if (!portal::is_portal_available()) {
      BOOST_LOG(debug) << "Portal: ScreenCast interface not available"sv;
      return names;
    }

    BOOST_LOG(info) << "Portal: XDG Desktop Portal ScreenCast interface detected"sv;
    names.emplace_back("0");
    return names;
  }

}  // namespace platf
