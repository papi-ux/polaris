/** @file Host Virtual Display capture protocol evidence, independent of display creation. */
#pragma once
#include <cstdint>
#include <string_view>

namespace virtual_display {
  enum class registry_state_e { no_wayland, unknown, complete };
  struct capture_provider_snapshot_t {
    registry_state_e state = registry_state_e::no_wayland;
    bool kwin_identity = false;
    std::uint32_t kwin_screencast_version = 0;
    bool xdg_output = false;
    bool wlr_export_dmabuf = false;

    void note_global(std::string_view name, std::uint32_t version) {
      if (name.starts_with("kde_output_") || name.starts_with("org_kde_plasma_") ||
          name == "org_kde_kwin_outputdevice") kwin_identity = true;
      if (name == "zkde_screencast_unstable_v1") {
        kwin_identity = true;
        kwin_screencast_version = version;
      }
      if (name == "zxdg_output_manager_v1") xdg_output = version > 0;
      if (name == "zwlr_export_dmabuf_manager_v1") wlr_export_dmabuf = version > 0;
    }
  };
  /// No permission writes/bindings/sessions. One 500ms connect+registry deadline;
  /// capability reads cache for 2s by transport, fresh launch reads bypass cache.
  capture_provider_snapshot_t probe_capture_provider(bool fresh = false);

  enum class capture_provider_state_e { no_wayland, unknown, unsupported, kwin, recoverable_withheld, native_wlr };
  inline capture_provider_state_e classify_capture_provider(const capture_provider_snapshot_t &p) {
    if (p.state == registry_state_e::no_wayland) return capture_provider_state_e::no_wayland;
    if (p.state != registry_state_e::complete) return capture_provider_state_e::unknown;
    if (p.kwin_screencast_version > 0) return capture_provider_state_e::kwin;
    if (p.kwin_identity) return capture_provider_state_e::recoverable_withheld;
    if (p.xdg_output && p.wlr_export_dmabuf) return capture_provider_state_e::native_wlr;
    return capture_provider_state_e::unsupported;
  }
}
