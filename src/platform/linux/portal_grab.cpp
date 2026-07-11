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
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <wayland-client.h>
#include <wlr-screencopy-unstable-v1.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/graphics.h"
#include "src/video.h"
#include "src/stream_stats.h"

#include "src/platform/linux/cage_display_router.h"
#include "src/platform/linux/pipewire_capture.h"

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
    uint64_t pw_node_serial = 0;
    std::optional<std::string> capture_render_node;
    int pw_remote_fd = -1;
    std::string restore_token;
    bool ready = false;
    bool failed = false;

    ~portal_session_t() {
      if (pw_remote_fd >= 0) {
        close(pw_remote_fd);
        pw_remote_fd = -1;
      }
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

  constexpr uint32_t portal_source_monitor = 1;
  constexpr uint32_t portal_source_window = 2;

  static uint32_t capture_type_for_stream_display(bool headless_mode, bool use_cage_compositor) {
    // Plain desktop display mirroring should ask the portal for a monitor/source,
    // not a single app window. Windowed/headless cage paths keep the historical
    // window source because they stream an isolated compositor/window.
    if (!headless_mode && !use_cage_compositor) {
      return portal_source_monitor;
    }

    return portal_source_window;
  }

#if defined(POLARIS_TESTS)
  uint32_t capture_type_for_stream_display_for_tests(bool headless_mode, bool use_cage_compositor) {
    return capture_type_for_stream_display(headless_mode, use_cage_compositor);
  }
#endif

  static uint32_t capture_type_for_current_config() {
    return capture_type_for_stream_display(
      config::video.linux_display.headless_mode,
      config::video.linux_display.use_cage_compositor);
  }

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

  static uint64_t lookup_uint64_property(GVariant *props, const char *name) {
    if (!props) {
      return 0;
    }

    GVariant *value = g_variant_lookup_value(props, name, nullptr);
    if (!value) {
      return 0;
    }

    uint64_t result = 0;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) {
      result = g_variant_get_uint64(value);
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
      result = g_variant_get_uint32(value);
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) {
      auto signed_value = g_variant_get_int64(value);
      if (signed_value > 0) {
        result = static_cast<uint64_t>(signed_value);
      }
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
      auto signed_value = g_variant_get_int32(value);
      if (signed_value > 0) {
        result = static_cast<uint64_t>(signed_value);
      }
    }

    g_variant_unref(value);
    return result;
  }

  static std::optional<std::string> lookup_string_property(GVariant *props, const char *name) {
    if (!props) {
      return std::nullopt;
    }

    GVariant *value = g_variant_lookup_value(props, name, G_VARIANT_TYPE_STRING);
    if (!value) {
      return std::nullopt;
    }

    std::string result = g_variant_get_string(value, nullptr);
    g_variant_unref(value);
    return result;
  }

  static std::optional<std::string> lookup_capture_render_node(GVariant *props) {
    for (const auto *key : {"render-node", "pipewire.render-node", "device.path"}) {
      const auto value = lookup_string_property(props, key);
      if (!value) {
        continue;
      }
      if (auto canonical = pipewire_capture::canonical_render_node(*value)) {
        return canonical;
      }
      BOOST_LOG(warning) << "portal: ignoring non-canonical capture render-node property "sv
                         << key << "=["sv << *value << ']';
    }

    return std::nullopt;
  }

  static int open_pipewire_remote_fd(GDBusConnection *conn, const std::string &session_handle) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    GError *gerr = nullptr;
    GUnixFDList *out_fd_list = nullptr;
    auto *result = g_dbus_connection_call_with_unix_fd_list_sync(
      conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast",
      "OpenPipeWireRemote",
      g_variant_new("(oa{sv})", session_handle.c_str(), &builder),
      G_VARIANT_TYPE("(h)"),
      G_DBUS_CALL_FLAGS_NONE,
      10000,
      nullptr,
      &out_fd_list,
      nullptr,
      &gerr);

    if (!result) {
      if (gerr) {
        BOOST_LOG(warning) << "portal: OpenPipeWireRemote failed: "sv << gerr->message;
        g_error_free(gerr);
      }
      return -1;
    }

    int fd_index = -1;
    g_variant_get(result, "(h)", &fd_index);
    g_variant_unref(result);

    int fd = -1;
    if (out_fd_list) {
      fd = g_unix_fd_list_get(out_fd_list, fd_index, &gerr);
      g_object_unref(out_fd_list);
    }

    if (fd < 0) {
      if (gerr) {
        BOOST_LOG(warning) << "portal: failed to extract PipeWire remote fd: "sv << gerr->message;
        g_error_free(gerr);
      } else {
        BOOST_LOG(warning) << "portal: OpenPipeWireRemote returned no usable fd"sv;
      }
    }

    return fd;
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

  static std::unique_ptr<portal_session_t> create_portal_session(uint32_t capture_type = portal_source_window) {
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
          session->pw_node_serial = lookup_uint64_property(props, "pipewire-serial");
          if (session->pw_node_serial == 0) {
            session->pw_node_serial = lookup_uint64_property(props, "pipewire.serial");
          }
          session->capture_render_node = lookup_capture_render_node(props);
          BOOST_LOG(info) << "portal: PipeWire node ID: "sv << node_id
                          << " serial="sv << session->pw_node_serial
                          << " render_node="sv << session->capture_render_node.value_or("none");
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
        session->pw_remote_fd = open_pipewire_remote_fd(session->conn, session->session_handle);
        if (session->pw_remote_fd < 0) {
          session->failed = true;
          return session;
        }
        session->ready = true;
      }
    }

    return session;
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
  static std::shared_ptr<pipewire_capture::capture_t> g_capture;
  static std::mutex g_capture_mtx;
  static uint32_t g_node_id = 0;
  static int g_capture_requested_width = 0;
  static int g_capture_requested_height = 0;
  static platf::mem_type_e g_capture_mem_type = platf::mem_type_e::system;
  static std::string g_capture_adapter;

  static bool ensure_global_session() {
    // Create the portal D-Bus session once (shows picker on first use)
    if (!g_portal || !g_portal->ready || g_portal->pw_node_id == 0) {
      g_portal.reset();
      g_portal = create_portal_session(capture_type_for_current_config());
      if (!g_portal || g_portal->failed || !g_portal->ready || g_portal->pw_node_id == 0) {
        BOOST_LOG(warning) << "portal: Failed to create global session"sv;
        g_portal.reset();
        return false;
      }
      g_node_id = g_portal->pw_node_id;
      BOOST_LOG(info) << "portal: Global session ready, node "sv << g_node_id
                      << " serial="sv << g_portal->pw_node_serial;
    }
    return true;
  }

  static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(int width, int height, platf::mem_type_e mem_type) {
    std::lock_guard lock(g_capture_mtx);

    const auto requested_adapter = config::video.adapter_name;
    if (g_capture && g_capture->running()) {
      const auto compatible = g_capture_requested_width == width &&
                              g_capture_requested_height == height &&
                              g_capture_mem_type == mem_type &&
                              g_capture_adapter == requested_adapter;
      if (compatible) {
        return g_capture;
      }

      BOOST_LOG(info) << "portal: capture configuration changed; reconnecting PipeWire before encoder selection"sv;
      g_capture.reset();

      // A PipeWire remote FD is a protocol connection, not a reusable device
      // handle. Request a fresh remote for the existing portal session so an
      // encoder retry does not reopen the picker or reuse a consumed socket.
      if (g_portal && g_portal->conn && !g_portal->session_handle.empty()) {
        if (g_portal->pw_remote_fd >= 0) {
          close(g_portal->pw_remote_fd);
          g_portal->pw_remote_fd = -1;
        }
        g_portal->pw_remote_fd = open_pipewire_remote_fd(g_portal->conn, g_portal->session_handle);
      }
      if (!g_portal || g_portal->pw_remote_fd < 0) {
        g_portal.reset();
        g_node_id = 0;
      }
    } else if (g_capture) {
      // A dead stream invalidates the node on this private remote. Recreate the
      // portal session rather than trying to reuse a stale target ID.
      g_capture.reset();
      g_portal.reset();
      g_node_id = 0;
    }

    if (!ensure_global_session()) {
      return nullptr;
    }

    const auto encoder_render_node = pipewire_capture::canonical_render_node(config::video.adapter_name);
    std::vector<pipewire_capture::dmabuf_format_modifier_t> dmabuf_formats;
    bool may_use_dmabuf = false;
    if (!g_portal->capture_render_node) {
      BOOST_LOG(info) << "portal: DMA-BUF disabled because the portal stream did not provide an explicit capture render node"sv;
    } else if (!encoder_render_node) {
      BOOST_LOG(info) << "portal: DMA-BUF disabled because config::video.adapter_name is not an explicit canonical render node"sv;
    } else if (*g_portal->capture_render_node != *encoder_render_node) {
      BOOST_LOG(info) << "portal: DMA-BUF disabled because capture render node ["sv << *g_portal->capture_render_node
                      << "] does not match encoder adapter ["sv << *encoder_render_node << ']';
    } else if (mem_type != platf::mem_type_e::vaapi && mem_type != platf::mem_type_e::cuda) {
      BOOST_LOG(info) << "portal: DMA-BUF disabled because encoder memory type is not VAAPI or CUDA"sv;
    } else {
      const auto egl_formats = pipewire_capture::query_egl_dmabuf_import_formats(*encoder_render_node);
      std::vector<std::uint64_t> importable_modifiers;
      for (const auto &format : egl_formats) {
        for (const auto modifier : format.modifiers) {
          if (std::find(format.external_only_modifiers.begin(), format.external_only_modifiers.end(), modifier) == format.external_only_modifiers.end()) {
            importable_modifiers.push_back(modifier);
          }
        }
      }
      const auto portal_formats = pipewire_capture::task1_packed_dmabuf_formats(std::move(importable_modifiers));
      dmabuf_formats = pipewire_capture::filter_importable_dmabuf_formats(portal_formats, egl_formats);
      const pipewire_capture::dmabuf_eligibility_t eligibility {
        .capture_render_node = g_portal->capture_render_node,
        .encoder_render_node = *encoder_render_node,
        .mem_type = mem_type,
        .egl_import_supported = !dmabuf_formats.empty(),
      };
      may_use_dmabuf = pipewire_capture::may_offer_dmabuf(eligibility);
      if (!may_use_dmabuf) {
        BOOST_LOG(info) << "portal: DMA-BUF disabled because EGL reported no importable packed-RGB modifiers for encoder render node ["sv
                        << *encoder_render_node << ']';
      }
    }

    auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {
      .remote_fd = g_portal->pw_remote_fd,
      .node_id = g_portal->pw_node_id,
      .node_serial = g_portal->pw_node_serial,
      .requested_width = width,
      .requested_height = height,
      .capture_render_node = g_portal->capture_render_node,
      .dmabuf_formats = std::move(dmabuf_formats),
      .mem_type = mem_type,
      .may_use_dmabuf = may_use_dmabuf,
    });
    if (!capture->start()) {
      BOOST_LOG(warning) << "portal: Failed to start PipeWire capture; invalidating portal session"sv;
      capture.reset();
      g_portal.reset();
      g_node_id = 0;
      return nullptr;
    }

    // The capture transport determines whether the encoder factory must use
    // RAM or GPU-resident input, so never select a factory before negotiation.
    for (int i = 0; i < 100 && capture->running() && !capture->negotiated(); ++i) {
      std::this_thread::sleep_for(100ms);
    }
    if (!capture->negotiated()) {
      BOOST_LOG(warning) << "portal: PipeWire format negotiation did not complete; invalidating portal session"sv;
      capture.reset();
      g_portal.reset();
      g_node_id = 0;
      return nullptr;
    }

    g_capture_requested_width = width;
    g_capture_requested_height = height;
    g_capture_mem_type = mem_type;
    g_capture_adapter = requested_adapter;
    g_capture = capture;
    return g_capture;
  }

  // -----------------------------------------------------------------------
  // Display backend
  // -----------------------------------------------------------------------

  class portal_display_t: public platf::display_t {
  public:
    ~portal_display_t() override {
      g_screencopy_stop = true;
    }

    int requested_width = 0;
    int requested_height = 0;
    int cfg_width = 0;
    int cfg_height = 0;
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    bool pipewire_dmabuf_negotiated = false;

    bool probe_only = false;  // true during encoder probe (skip portal session)

    int
    init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      requested_width = config.width;
      requested_height = config.height;
      cfg_width = requested_width;
      cfg_height = requested_height;
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

          auto cap = ensure_global_capture(requested_width, requested_height, mem_type);
          if (!cap) {
            return -1;
          }

          const auto info = cap->frame_info();
          pipewire_dmabuf_negotiated = cap->negotiated_dmabuf();
          if (cap->negotiated() && info.width > 0 && info.height > 0) {
            cfg_width = info.width;
            cfg_height = info.height;
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

      auto cap = ensure_global_capture(requested_width, requested_height, mem_type);
      if (!cap) {
        BOOST_LOG(warning) << "portal: No capture available"sv;
        return platf::capture_e::reinit;
      }

      auto info = cap->frame_info();
      pipewire_dmabuf_negotiated = cap->negotiated_dmabuf();
      if (cap->negotiated() && info.width > 0 && info.height > 0) {
        cfg_width = info.width;
        cfg_height = info.height;
        this->env_width = cfg_width;
        this->env_height = cfg_height;
        this->width = cfg_width;
        this->height = cfg_height;
      }

      bool capture_transport_logged = false;
      while (cap) {
        const auto capture_start = std::chrono::steady_clock::now();
        const auto wait_result = cap->wait_for_frame(1s);
        const auto dispatch_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start);

        switch (wait_result) {
          case pipewire_capture::wait_result_e::timeout: {
            std::shared_ptr<platf::img_t> dummy;
            if (!push_captured_image_cb(std::move(dummy), false)) {
              return platf::capture_e::ok;
            }
            continue;
          }
          case pipewire_capture::wait_result_e::reinit:
            return platf::capture_e::reinit;
          case pipewire_capture::wait_result_e::error:
            return platf::capture_e::error;
          case pipewire_capture::wait_result_e::frame:
            break;
        }

        std::shared_ptr<platf::img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        if (!cap->fill_frame(img_out)) {
          return platf::capture_e::reinit;
        }

        stream_stats::update_capture_metadata(img_out->frame_metadata);
        if (!capture_transport_logged) {
          capture_transport_logged = true;
          BOOST_LOG(warning) << "portal: capture_transport="sv << platf::from_frame_transport(img_out->frame_metadata.transport)
                             << " frame_residency="sv << platf::from_frame_residency(img_out->frame_metadata.residency)
                             << " frame_format="sv << platf::from_frame_format(img_out->frame_metadata.format)
                             << "; capture will incur an extra CPU-side copy/conversion path"sv;
        }
        if (config::video.linux_display.capture_profile) {
          const auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - capture_start);
          stream_stats::update_capture_profile({
            .transport = img_out->frame_metadata.transport,
            .dispatch_time = dispatch_time,
            .ingest_time = total_time - dispatch_time,
            .total_time = total_time,
          });
        }

        if (!push_captured_image_cb(std::move(img_out), true)) {
          return platf::capture_e::ok;
        }
      }

      return platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t>
    alloc_img() override {
      struct portal_img_t: egl::img_descriptor_t {
      };

      auto img = std::make_shared<portal_img_t>();

      img->width = cfg_width;
      img->height = cfg_height;
      img->pixel_pitch = 4;
      img->row_pitch = cfg_width * 4;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->dmabuf_buffer_key = 0;
      img->sd = {};
      std::fill_n(img->sd.fds, 4, -1);
      if (pipewire_dmabuf_negotiated) {
        img->data = nullptr;
      } else {
        img->buffer.assign(static_cast<std::size_t>(cfg_height) * static_cast<std::size_t>(img->row_pitch), 0);
        img->data = img->buffer.data();
      }

      BOOST_LOG(info) << "portal: alloc_img "sv << img->width << "x"sv << img->height
                      << " env="sv << this->env_width << "x"sv << this->env_height;
      return img;
    }

    int
    dummy_img(platf::img_t *img) override {
      if (!img) return -1;
      if (pipewire_dmabuf_negotiated) {
        auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(img);
        if (!descriptor) return -1;
        descriptor->sequence = 0;
        return 0;
      }
      if (!img->data) {
        if (auto *cursor = dynamic_cast<egl::cursor_t *>(img); cursor && !cursor->buffer.empty()) {
          img->data = cursor->buffer.data();
        }
      }
      if (!img->data) return -1;
      std::memset(img->data, 0, img->height * img->row_pitch);
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t>
    make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      int w = cfg_width;
      int h = cfg_height;

#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        if (pipewire_dmabuf_negotiated) {
          return va::make_avcodec_encode_device(w, h, 0, 0, true);
        }
        return va::make_avcodec_encode_device(w, h, false);
      }
#endif
#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (pipewire_dmabuf_negotiated) {
          return cuda::make_avcodec_gl_encode_device(w, h, 0, 0);
        }
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
    // Encoder validation must remain picker-free and use the RAM factory.
    // Real capture construction negotiates before image-pool/factory selection.
    disp->probe_only = video::encoder_probe_active();
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
