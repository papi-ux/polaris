/**
 * @file src/platform/linux/portal_session.cpp
 * @brief XDG Desktop Portal ScreenCast D-Bus session (CreateSession/SelectSources/Start).
 *
 * Extracted from portal_grab (structure S2). Capture wiring + portal_display_t stay in
 * portal_grab.cpp. session_media remains sole ordered media teardown owner.
 */

#include "portal_session.h"

#ifdef __linux__

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/pipewire_capture.h"

using namespace std::literals;

namespace portal {

  // -----------------------------------------------------------------------
  // Restore token persistence
  // -----------------------------------------------------------------------

  static std::string token_path() {
    std::string base = config::sunshine.config_file.empty()
      ? std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config/sunshine"
      : config::sunshine.config_file.substr(0, config::sunshine.config_file.rfind('/'));
    // Private gamescope bus tokens are not valid for host KDE ScreenCast (and vice
    // versa). Mixing them hangs Start with restore_token=yes and no PipeWire node.
    const char *private_bus = std::getenv("POLARIS_PORTAL_DBUS_ADDRESS");
    if (private_bus && *private_bus) {
      return base + "/portal_restore_token_private.txt";
    }
    const std::string host = base + "/portal_restore_token_host.txt";
    // One-time migration from the pre-split single token file (host only).
    const std::string legacy = base + "/portal_restore_token.txt";
    std::error_code ec;
    if (!std::filesystem::exists(host, ec) && std::filesystem::is_regular_file(legacy, ec)) {
      std::error_code copy_ec;
      std::filesystem::copy_file(legacy, host, copy_ec);
      if (!copy_ec) {
        BOOST_LOG(info) << "portal: migrated legacy restore token to "sv << host;
      }
    }
    return host;
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

  // Invalidate a bad/stale restore token so the next SelectSources can re-prompt
  // (or succeed without the token). Policy: keep restore_token *enabled* — never
  // disable restore tokens — only clear the on-disk cache on failure.
  static void clear_restore_token() {
    const auto path = token_path();
    std::error_code ec;
    if (std::filesystem::remove(path, ec)) {
      BOOST_LOG(warning) << "portal: Cleared invalid restore token at "sv << path;
    }
    else if (ec && ec != std::errc::no_such_file_or_directory) {
      BOOST_LOG(warning) << "portal: Failed to clear restore token "sv << path
                         << ": "sv << ec.message();
      // Best-effort truncate so a subsequent load returns empty.
      std::ofstream f(path, std::ios::trunc);
    }
  }

  // -----------------------------------------------------------------------
  // D-Bus introspection
  // -----------------------------------------------------------------------


  // Optional private portal bus (gamescope ScreenCast) while the rest of Polaris
  // stays on the session bus (Avahi / tray / KRDP host portal).
  // Set POLARIS_PORTAL_DBUS_ADDRESS=unix:path=.../polaris-portal/bus
  static GDBusConnection *portal_bus_connection(GError **gerr) {
    const char *addr = std::getenv("POLARIS_PORTAL_DBUS_ADDRESS");
    if (addr && *addr) {
      auto *conn = g_dbus_connection_new_for_address_sync(
        addr,
        static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        nullptr,
        gerr);
      if (conn) {
        BOOST_LOG(info) << "portal: using POLARIS_PORTAL_DBUS_ADDRESS for ScreenCast"sv;
      }
      return conn;
    }
    return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, gerr);
  }

  bool is_portal_available() {
    GError *gerr = nullptr;
    GDBusConnection *connection = portal_bus_connection(&gerr);
    if (!connection) {
      if (gerr) {
        BOOST_LOG(debug) << "Portal: could not connect to portal bus: "sv << gerr->message;
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

  portal_session_t::~portal_session_t() {
    if (pw_remote_fd >= 0) {
      close(pw_remote_fd);
      pw_remote_fd = -1;
    }
    if (conn) {
      if (!session_handle.empty()) {
        g_dbus_connection_call_sync(
          conn,
          "org.freedesktop.portal.Desktop",
          session_handle.c_str(),
          "org.freedesktop.portal.Session",
          "Close",
          nullptr,
          nullptr,
          G_DBUS_CALL_FLAGS_NONE,
          1000,
          nullptr,
          nullptr
        );
      }
      g_object_unref(conn);
      conn = nullptr;
    }
  }


  uint32_t capture_type_for_stream_display(bool headless_mode, bool use_cage_compositor,
                                           std::string_view stream_mode) {
    // Host-desktop modes stream a physical output after (optional) topology swap.
    // headless_dongle sets headless_mode for privacy swap but must NOT use window
    // SelectSources — KDE ScreenCast hangs on Start with type=window + restore_token.
    // host_virtual_display streams the created virtual MONITOR (EVDI connector),
    // never an app window — it sets headless_mode too, so without this entry it
    // fell through to portal_source_window: the same hang-prone combination, and
    // a source type that cannot name the virtual output at all.
    const auto mode = stream_mode.empty() ? std::string_view {config::video.linux_display.stream_mode} : stream_mode;
    if (mode == "headless_dongle" || mode == "desktop_display" || mode == "headless_evdi" ||
        mode == "host_virtual_display") {
      return portal_source_monitor;
    }

    // Plain desktop display mirroring should ask the portal for a monitor/source,
    // not a single app window. Windowed/headless cage paths keep the historical
    // window source because they stream an isolated compositor/window.
    if (!headless_mode && !use_cage_compositor) {
      return portal_source_monitor;
    }

    return portal_source_window;
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

  // XDG ScreenCast cursor modes (bitmask on AvailableCursorModes):
  // 1 = Hidden, 2 = Embedded, 4 = Metadata.
  // gamescope portal may report 0 until it is bound to a live compositor; never
  // hard-require Embedded (2) — that yields InvalidArgument and hangs handshake.
  static uint32_t portal_available_cursor_modes(GDBusConnection *conn) {
    GError *gerr = nullptr;
    auto *result = g_dbus_connection_call_sync(conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", "org.freedesktop.portal.ScreenCast", "AvailableCursorModes"),
      G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE,
      3000,
      nullptr,
      &gerr);
    if (!result) {
      if (gerr) {
        BOOST_LOG(warning) << "portal: AvailableCursorModes query failed: "sv << gerr->message;
        g_error_free(gerr);
      }
      return 0;
    }
    GVariant *inner = nullptr;
    g_variant_get(result, "(v)", &inner);
    g_variant_unref(result);
    uint32_t modes = 0;
    if (inner) {
      if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) {
        modes = g_variant_get_uint32(inner);
      }
      g_variant_unref(inner);
    }
    return modes;
  }

  // Prefer Embedded, then Metadata, then Hidden. Returns 0 → omit cursor_mode.
  static uint32_t portal_pick_cursor_mode(uint32_t available) {
    constexpr uint32_t kHidden = 1;
    constexpr uint32_t kEmbedded = 2;
    constexpr uint32_t kMetadata = 4;
    if (available & kEmbedded) {
      return kEmbedded;
    }
    if (available & kMetadata) {
      return kMetadata;
    }
    if (available & kHidden) {
      return kHidden;
    }
    return 0;
  }

#if defined(POLARIS_TESTS)
  uint32_t portal_pick_cursor_mode_for_tests(uint32_t available) {
    return portal_pick_cursor_mode(available);
  }
#endif

  // Brief wait for gamescope/xdg-desktop-portal-gamescope to advertise cursor
  // modes after bind. Avoids a cold-start AvailableCursorModes=0 path that ends
  // up omitting cursor_mode (which works but loses embedded cursors).
  static uint32_t portal_wait_cursor_modes(GDBusConnection *conn, int timeout_ms = 2000) {
    uint32_t modes = portal_available_cursor_modes(conn);
    if (modes != 0 || timeout_ms <= 0) {
      return modes;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100ms);
      modes = portal_available_cursor_modes(conn);
      if (modes != 0) {
        BOOST_LOG(info) << "portal: AvailableCursorModes became "sv << modes
                        << " after wait"sv;
        return modes;
      }
    }
    return modes;
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

  int open_pipewire_remote_fd(GDBusConnection *conn, const std::string &session_handle) {
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

  // XDG portal request object paths must be unique per connection. Reusing a
  // fixed handle_token after a failed/timed-out request races with the old
  // Response signal and can hang Start/SelectSources.
  static std::atomic<uint64_t> g_portal_handle_seq {0};
  static std::string next_handle_token(const char *prefix) {
    const auto n = g_portal_handle_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    return std::string(prefix) + std::to_string(n);
  }

  std::unique_ptr<portal_session_t> create_portal_session(uint32_t capture_type) {
    auto session = std::make_unique<portal_session_t>();

    GError *gerr = nullptr;
    session->conn = portal_bus_connection(&gerr);
    if (!session->conn) {
      BOOST_LOG(warning) << "portal: Cannot connect to portal bus"sv;
      if (gerr) g_error_free(gerr);
      session->failed = true;
      return session;
    }

    // Step 1: CreateSession
    BOOST_LOG(info) << "portal: Creating ScreenCast session..."sv;
    {
      const auto handle_token = next_handle_token("polaris_cs");
      const auto session_token = next_handle_token("polaris_sess");
      std::string req_path = make_request_path(session->conn, handle_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token",
        g_variant_new_string(session_token.c_str()));
      g_variant_builder_add(&builder, "{sv}", "handle_token",
        g_variant_new_string(handle_token.c_str()));

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
    // Keep restore_token enabled for headless auto-select. On failure
    // (InvalidArgument / non-zero Response / missing response) invalidate and
    // retry SelectSources once without it; Start will persist a fresh token.
    BOOST_LOG(info) << "portal: Selecting sources (type="sv << capture_type << ")..."sv;
    {
      auto try_select_sources = [&](const std::string &handle_token, bool use_restore_token) -> GVariant * {
        const uint32_t available_cursor = portal_wait_cursor_modes(session->conn);
        const uint32_t cursor_mode = portal_pick_cursor_mode(available_cursor);
        BOOST_LOG(info) << "portal: AvailableCursorModes="sv << available_cursor
                        << " selected_cursor_mode="sv << cursor_mode
                        << (cursor_mode == 0 ? " (omit — portal reports none)" : "")
                        << " restore_token="sv << (use_restore_token ? "yes" : "no") << ""sv;

        std::string req_path = make_request_path(session->conn, handle_token);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "types",
          g_variant_new_uint32(capture_type));
        // Only request a cursor mode the portal advertises. Hardcoding Embedded (2)
        // fails with InvalidArgument when AvailableCursorModes is 0 (cold portal /
        // gamescope not bound yet) and leaves Moonlight stuck in handshake.
        if (cursor_mode != 0) {
          g_variant_builder_add(&builder, "{sv}", "cursor_mode",
            g_variant_new_uint32(cursor_mode));
        }
        // Always request persist so Start can return a new restore_token after
        // an invalidate+retry path re-selects without the old token.
        g_variant_builder_add(&builder, "{sv}", "persist_mode",
          g_variant_new_uint32(2));
        g_variant_builder_add(&builder, "{sv}", "handle_token",
          g_variant_new_string(handle_token.c_str()));

        std::string saved_token;
        if (use_restore_token) {
          saved_token = load_restore_token();
          if (!saved_token.empty()) {
            BOOST_LOG(info) << "portal: Using saved restore token (auto-select)"sv;
            g_variant_builder_add(&builder, "{sv}", "restore_token",
              g_variant_new_string(saved_token.c_str()));
          }
        }

        auto *result = portal_call_sync(session->conn, "SelectSources",
          g_variant_new("(oa{sv})", session->session_handle.c_str(), &builder));
        if (!result) {
          // InvalidArgument / failed restore often fails the method call itself
          // (no Response signal). Treat as SelectSources failure immediately.
          BOOST_LOG(warning) << "portal: SelectSources D-Bus call failed"
                             << (use_restore_token && !saved_token.empty() ?
                                   " (restore token or cursor mode rejected)" :
                                   "")
                             << ""sv;
          return nullptr;
        }
        g_variant_unref(result);

        auto *resp = wait_for_response(session->conn, req_path, 10000);
        if (!resp) {
          BOOST_LOG(warning) << "portal: SelectSources response missing or non-zero"
                             << (use_restore_token && !saved_token.empty() ?
                                   " (stale restore token likely)" :
                                   "")
                             << ""sv;
        }
        return resp;
      };

      // Always prefer restore tokens when present. Host vs private gamescope portals
      // use separate on-disk files (token_path), so they cannot cross-contaminate.
      const bool had_saved_token = !load_restore_token().empty();
      auto *resp = try_select_sources(next_handle_token("polaris_ss"), true);
      if (!resp) {
        if (had_saved_token) {
          // Policy: always invalidate the on-disk token after SelectSources
          // failure, then retry once without restore_token. Cursor is re-queried
          // on the retry so a cold AvailableCursorModes=0 can recover.
          clear_restore_token();
        }
        BOOST_LOG(warning) << "portal: SelectSources failed — retry once without restore_token"sv;
        resp = try_select_sources(next_handle_token("polaris_ss"), false);
        if (!resp) {
          session->failed = true;
          return session;
        }
      }
      g_variant_unref(resp);
    }

    // Step 3: Start. With a host restore token this is non-interactive and must
    // finish under Moonlight's connect budget. Without a token KDE shows a picker
    // (desk must stay visible — see display_topology bootstrap); give the user
    // longer. Do NOT retry Start on the same session — KDE returns
    // "Can only start once" and wastes another timeout.
    const bool had_token_for_start = !load_restore_token().empty();
    const int start_timeout_ms = had_token_for_start ? 8000 : 45000;
    BOOST_LOG(info) << "portal: Starting capture"
                    << (had_token_for_start ? " (restore auto-select)"sv :
                                              " (ScreenCast picker may appear on host — approve once)"sv)
                    << " timeout_ms="sv << start_timeout_ms << ""sv;
    {
      auto try_start = [&](const std::string &handle_token) -> GVariant * {
        std::string req_path = make_request_path(session->conn, handle_token);

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "handle_token",
          g_variant_new_string(handle_token.c_str()));

        auto *result = portal_call_sync(session->conn, "Start",
          g_variant_new("(osa{sv})", session->session_handle.c_str(), "", &builder),
          start_timeout_ms);
        if (result) g_variant_unref(result);

        return wait_for_response(session->conn, req_path, start_timeout_ms);
      };

      auto *resp = try_start(next_handle_token("polaris_st"));
      if (!resp) {
        // Bad/stale restore token often hangs Start with no Response; drop it so
        // the next connect can re-bootstrap with a visible picker.
        clear_restore_token();
        // Nested↔idle gamescope handoff leaves a short window where
        // xdg-desktop-portal-gamescope cannot open WAYLAND_DISPLAY=gamescope-0
        // ("failed to connect to wayland socket" → Response code 2). One delayed
        // retry on a *fresh* session is owned by the caller; here we only clear
        // the token so the next CreateSession is clean.
        BOOST_LOG(warning) << "portal: Start timeout/failure — cleared restore_token (no Start retry; session is single-use)"sv;
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

}  // namespace portal

#endif  // __linux__
