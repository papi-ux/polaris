/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// platform includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xf86drm.h>

// local includes
#include "graphics.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {

  namespace {
    struct format_modifier_blob_t {
      std::uint32_t format;
      std::uint32_t pad;
      std::uint64_t modifier;
    };

    struct pending_buffer_create_t {
      dmabuf_t *self {};
      frame_t *target {};
      zwlr_screencopy_frame_v1 *frame {};
      int wl_buffer_fd {-1};
    };

    std::string path_for_fd(int fd) {
      if (fd < 0) {
        return {};
      }

      char path[64] {};
      std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

      char resolved[PATH_MAX] {};
      auto bytes = readlink(path, resolved, sizeof(resolved) - 1);
      if (bytes < 0) {
        return {};
      }

      resolved[bytes] = '\0';
      return resolved;
    }

    bool decode_dev_id(const wl_array *device_array, dev_t &device_id) {
      if (!device_array || device_array->size != sizeof(device_id) || !device_array->data) {
        return false;
      }

      std::memcpy(&device_id, device_array->data, sizeof(device_id));
      return true;
    }

    bool drm_devices_equal_by_devid(dev_t lhs, dev_t rhs) {
      drmDevicePtr lhs_device = nullptr;
      drmDevicePtr rhs_device = nullptr;

      if (drmGetDeviceFromDevId(lhs, 0, &lhs_device) != 0) {
        return lhs == rhs;
      }

      if (drmGetDeviceFromDevId(rhs, 0, &rhs_device) != 0) {
        drmFreeDevice(&lhs_device);
        return lhs == rhs;
      }

      const bool equal = drmDevicesEqual(lhs_device, rhs_device);
      drmFreeDevice(&lhs_device);
      drmFreeDevice(&rhs_device);
      return equal;
    }

    std::string modifier_list_string(const std::vector<std::uint64_t> &modifiers) {
      std::string formatted;

      for (std::size_t i = 0; i < modifiers.size(); ++i) {
        if (i != 0) {
          formatted += ", ";
        }

        formatted += std::to_string(modifiers[i]);
      }

      return formatted;
    }
  }  // namespace

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  // Define buffer params listener
  static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_t::buffer_params_created,
    .failed = dmabuf_t::buffer_params_failed
  };

  int display_t::init(const char *display_name) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }

    if (!display_name) {
      BOOST_LOG(error) << "Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout The timeout in milliseconds.
   * @return `true` if new events were dispatched or `false` if the timeout expired.
   */
  bool display_t::dispatch(std::chrono::milliseconds timeout) {
    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      if (poll(&pfd, 1, timeout.count()) == 1 && (pfd.revents & POLLIN)) {
        // Read the new event(s)
        wl_display_read_events(display_internal.get());
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events
    wl_display_dispatch_pending(display_internal.get());
    return true;
  }

  wl_registry *display_t::registry() {
    return wl_display_get_registry(display_internal.get());
  }

  inline monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        &CLASS_CALL(monitor_t, wl_geometry),
        &CLASS_CALL(monitor_t, wl_mode),
        &CLASS_CALL(monitor_t, wl_done),
        &CLASS_CALL(monitor_t, wl_scale),
      },
      xdg_listener {
        &CLASS_CALL(monitor_t, xdg_position),
        &CLASS_CALL(monitor_t, xdg_size),
        &CLASS_CALL(monitor_t, xdg_done),
        &CLASS_CALL(monitor_t, xdg_name),
        &CLASS_CALL(monitor_t, xdg_description)
      } {
  }

  inline void monitor_t::xdg_name(zxdg_output_v1 *, const char *name) {
    this->name = name;

    BOOST_LOG(info) << "Name: "sv << this->name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    this->description = description;

    BOOST_LOG(info) << "Found monitor: "sv << this->description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    viewport.offset_x = x;
    viewport.offset_y = y;

    BOOST_LOG(info) << "Offset: "sv << x << 'x' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    BOOST_LOG(info) << "Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    viewport.width = width;
    viewport.height = height;

    BOOST_LOG(info) << "Resolution: "sv << width << 'x' << height;
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    auto xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      },
      dmabuf_feedback_listener {
        &CLASS_CALL(interface_t, feedback_done),
        &CLASS_CALL(interface_t, feedback_format_table),
        &CLASS_CALL(interface_t, feedback_main_device),
        &CLASS_CALL(interface_t, feedback_tranche_done),
        &CLASS_CALL(interface_t, feedback_tranche_target_device),
        &CLASS_CALL(interface_t, feedback_tranche_formats),
        &CLASS_CALL(interface_t, feedback_tranche_flags),
      } {
  }

  interface_t::~interface_t() {
    if (dmabuf_feedback_object) {
      zwp_linux_dmabuf_feedback_v1_destroy(dmabuf_feedback_object);
      dmabuf_feedback_object = nullptr;
    }
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::begin_feedback_update() {
    if (dmabuf_feedback_update_in_progress) {
      return;
    }

    pending_dmabuf_feedback = {};
    pending_dmabuf_feedback_tranche = {};
    dmabuf_feedback_update_in_progress = true;
    dmabuf_feedback_tranche_active = false;
  }

  void interface_t::feedback_done(zwp_linux_dmabuf_feedback_v1 *feedback) {
    if (!dmabuf_feedback_update_in_progress) {
      return;
    }

    if (dmabuf_feedback_tranche_active) {
      pending_dmabuf_feedback.tranches.emplace_back(std::move(pending_dmabuf_feedback_tranche));
      pending_dmabuf_feedback_tranche = {};
      dmabuf_feedback_tranche_active = false;
    }

    pending_dmabuf_feedback.available =
      pending_dmabuf_feedback.main_device_valid ||
      !pending_dmabuf_feedback.tranches.empty();

    dmabuf_feedback = std::move(pending_dmabuf_feedback);
    pending_dmabuf_feedback = {};
    dmabuf_feedback_update_in_progress = false;

    BOOST_LOG(info) << "Wayland DMA-BUF feedback ready: main_device_valid="sv
                    << dmabuf_feedback.main_device_valid
                    << " tranches="sv << dmabuf_feedback.tranches.size()
                    << " format_table_entries="sv << dmabuf_feedback_table.size();
  }

  void interface_t::feedback_format_table(
    zwp_linux_dmabuf_feedback_v1 *feedback,
    int32_t fd,
    std::uint32_t size
  ) {
    begin_feedback_update();
    dmabuf_feedback_table.clear();

    if (fd < 0 || size == 0 || (size % sizeof(format_modifier_blob_t)) != 0) {
      if (fd >= 0) {
        close(fd);
      }

      BOOST_LOG(warning) << "Wayland DMA-BUF feedback provided an invalid format table (fd="sv
                         << fd << ", size="sv << size << ')';
      return;
    }

    void *mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
      BOOST_LOG(warning) << "Wayland DMA-BUF feedback format table mmap failed"sv;
      return;
    }

    const auto *entries = static_cast<const format_modifier_blob_t *>(mapping);
    const std::size_t count = size / sizeof(format_modifier_blob_t);
    dmabuf_feedback_table.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      dmabuf_feedback_table.emplace_back(dmabuf_feedback_format_modifier_t {
        .format = entries[i].format,
        .modifier = entries[i].modifier,
      });
    }

    munmap(mapping, size);
  }

  void interface_t::feedback_main_device(
    zwp_linux_dmabuf_feedback_v1 *feedback,
    wl_array *device
  ) {
    begin_feedback_update();

    dev_t main_device {};
    if (!decode_dev_id(device, main_device)) {
      BOOST_LOG(warning) << "Wayland DMA-BUF feedback main_device payload was invalid"sv;
      return;
    }

    pending_dmabuf_feedback.main_device_valid = true;
    pending_dmabuf_feedback.main_device = main_device;
  }

  void interface_t::feedback_tranche_done(zwp_linux_dmabuf_feedback_v1 *feedback) {
    begin_feedback_update();

    if (!dmabuf_feedback_tranche_active) {
      return;
    }

    pending_dmabuf_feedback.tranches.emplace_back(std::move(pending_dmabuf_feedback_tranche));
    pending_dmabuf_feedback_tranche = {};
    dmabuf_feedback_tranche_active = false;
  }

  void interface_t::feedback_tranche_target_device(
    zwp_linux_dmabuf_feedback_v1 *feedback,
    wl_array *device
  ) {
    begin_feedback_update();

    if (!dmabuf_feedback_tranche_active) {
      pending_dmabuf_feedback_tranche = {};
      dmabuf_feedback_tranche_active = true;
    }

    dev_t target_device {};
    if (!decode_dev_id(device, target_device)) {
      BOOST_LOG(warning) << "Wayland DMA-BUF feedback tranche_target_device payload was invalid"sv;
      pending_dmabuf_feedback_tranche = {};
      dmabuf_feedback_tranche_active = false;
      return;
    }

    pending_dmabuf_feedback_tranche.target_device_valid = true;
    pending_dmabuf_feedback_tranche.target_device = target_device;
  }

  void interface_t::feedback_tranche_formats(
    zwp_linux_dmabuf_feedback_v1 *feedback,
    wl_array *indices
  ) {
    begin_feedback_update();

    if (!dmabuf_feedback_tranche_active) {
      pending_dmabuf_feedback_tranche = {};
      dmabuf_feedback_tranche_active = true;
    }

    if (!indices || !indices->data || (indices->size % sizeof(std::uint16_t)) != 0) {
      BOOST_LOG(warning) << "Wayland DMA-BUF feedback tranche_formats payload was invalid"sv;
      return;
    }

    const auto *format_indices = static_cast<const std::uint16_t *>(indices->data);
    const auto count = indices->size / sizeof(std::uint16_t);
    for (std::size_t i = 0; i < count; ++i) {
      const auto index = format_indices[i];
      if (index >= dmabuf_feedback_table.size()) {
        BOOST_LOG(warning) << "Wayland DMA-BUF feedback tranche_formats referenced out-of-range index ["sv
                           << index << ']';
        continue;
      }

      pending_dmabuf_feedback_tranche.format_modifiers.emplace_back(dmabuf_feedback_table[index]);
    }
  }

  void interface_t::feedback_tranche_flags(
    zwp_linux_dmabuf_feedback_v1 *feedback,
    std::uint32_t flags
  ) {
    begin_feedback_update();

    if (!dmabuf_feedback_tranche_active) {
      pending_dmabuf_feedback_tranche = {};
      dmabuf_feedback_tranche_active = true;
    }

    pending_dmabuf_feedback_tranche.flags |= flags;
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "Available interface: "sv << interface << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface, wl_output_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 2)
        )
      );
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, version);

      this->interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);

      this->interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, version);

      this->interface[LINUX_DMABUF] = true;

      if (zwp_linux_dmabuf_v1_get_version(dmabuf_interface) >= 4) {
        dmabuf_feedback_object = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf_interface);
        if (dmabuf_feedback_object) {
          zwp_linux_dmabuf_feedback_v1_add_listener(dmabuf_feedback_object, &dmabuf_feedback_listener, this);
        }
      }
    } else if (!std::strcmp(interface, wl_shm_interface.name)) {
      shm = (wl_shm *) wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "Delete: "sv << id;
  }

  // Initialize GBM
  bool dmabuf_t::init_gbm() {
    if (gbm_device) {
      return true;
    }

    // Find render node
    drmDevice *devices[16];
    int n = drmGetDevices2(0, devices, 16);
    if (n <= 0) {
      BOOST_LOG(error) << "No DRM devices found"sv;
      return false;
    }

    int drm_fd = -1;
    for (int i = 0; i < n; i++) {
      if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
        drm_fd = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR);
        if (drm_fd >= 0) {
          break;
        }
      }
    }
    drmFreeDevices(devices, n);

    if (drm_fd < 0) {
      BOOST_LOG(error) << "Failed to open DRM render node"sv;
      return false;
    }

    gbm_device = gbm_create_device(drm_fd);
    if (!gbm_device) {
      close(drm_fd);
      BOOST_LOG(error) << "Failed to create GBM device"sv;
      return false;
    }

    static bool logged_gbm_backend = false;
    if (!logged_gbm_backend) {
      BOOST_LOG(info) << "Wayland DMA-BUF capture using GBM backend ["sv
                      << gbm_device_get_backend_name(gbm_device)
                      << "] render_node=["sv << path_for_fd(gbm_device_get_fd(gbm_device)) << ']';
      logged_gbm_backend = true;
    }

    return true;
  }

  // Cleanup GBM
  void dmabuf_t::cleanup_gbm() {
    for (auto &frame : frames) {
      frame.reset_buffer();
    }
  }

  dmabuf_t::dmabuf_t():
      status {READY},
      frames {},
      current_frame {&frames[0]},
      listener {
        &CLASS_CALL(dmabuf_t, buffer),
        &CLASS_CALL(dmabuf_t, flags),
        &CLASS_CALL(dmabuf_t, ready),
        &CLASS_CALL(dmabuf_t, failed),
        &CLASS_CALL(dmabuf_t, damage),
        &CLASS_CALL(dmabuf_t, linux_dmabuf),
        &CLASS_CALL(dmabuf_t, buffer_done),
      } {
  }

  // Start capture
  void dmabuf_t::listen(
    zwlr_screencopy_manager_v1 *screencopy_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    wl_output *output,
    bool blend_cursor,
    wl_shm *shm
  ) {
    this->dmabuf_interface = dmabuf_interface;
    this->shm_global = shm;
    // Reset state
    shm_info.supported = false;
    dmabuf_info.supported = false;

    // Create new frame
    auto frame = zwlr_screencopy_manager_v1_capture_output(
      screencopy_manager,
      blend_cursor ? 1 : 0,
      output
    );

    // Store frame data pointer for callbacks
    zwlr_screencopy_frame_v1_set_user_data(frame, this);

    // Add listener
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
  }

  dmabuf_t::~dmabuf_t() {
    cleanup_gbm();

    for (auto &frame : frames) {
      frame.destroy();
    }

    if (gbm_device) {
      // We should close the DRM FD, but it's owned by GBM
      gbm_device_destroy(gbm_device);
      gbm_device = nullptr;
    }
  }

  // Buffer format callback
  void dmabuf_t::buffer(
    zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride
  ) {
    shm_info.supported = true;
    shm_info.format = format;
    shm_info.width = width;
    shm_info.height = height;
    shm_info.stride = stride;

    BOOST_LOG(debug) << "Screencopy supports SHM format: "sv << format;
  }

  // DMA-BUF format callback
  void dmabuf_t::linux_dmabuf(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height
  ) {
    dmabuf_info.supported = true;
    dmabuf_info.format = format;
    dmabuf_info.width = width;
    dmabuf_info.height = height;

    BOOST_LOG(debug) << "Screencopy supports DMA-BUF format: "sv << format;
  }

  // Flags callback
  void dmabuf_t::flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags) {
    y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    BOOST_LOG(debug) << "Frame flags: "sv << flags << (y_invert ? " (y_invert)" : "");
  }

  // DMA-BUF creation helper
  void dmabuf_t::create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame) {
    if (!init_gbm()) {
      BOOST_LOG(error) << "Failed to initialize GBM"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    auto next_frame = get_next_frame();
    next_frame->sd.fourcc = dmabuf_info.format;
    next_frame->sd.width = dmabuf_info.width;
    next_frame->sd.height = dmabuf_info.height;

    auto populate_capture_descriptor = [&](frame_t &target) -> bool {
      target.reset_capture_descriptor();

      auto fd = gbm_bo_get_fd(target.bo);
      if (fd < 0) {
        BOOST_LOG(error) << "Failed to get buffer FD"sv;
        return false;
      }

      target.sd.fds[0] = fd;
      target.sd.pitches[0] = gbm_bo_get_stride(target.bo);
      target.sd.offsets[0] = 0;
      target.sd.modifier = gbm_bo_get_modifier(target.bo);
      target.buffer_modifier = target.sd.modifier;
      return true;
    };

    struct modifier_selection_t {
      bool from_feedback {false};
      bool implicit_supported {false};
      std::vector<std::uint64_t> explicit_modifiers;
    };

    auto select_feedback_modifiers = [&]() -> modifier_selection_t {
      modifier_selection_t selection;

      if (!feedback || !feedback->available) {
        return selection;
      }

      struct stat render_node_stat {};
      if (fstat(gbm_device_get_fd(gbm_device), &render_node_stat) != 0) {
        BOOST_LOG(warning) << "Wayland DMA-BUF feedback selection could not stat render node ["sv
                           << path_for_fd(gbm_device_get_fd(gbm_device)) << ']';
        return selection;
      }

      for (const auto &tranche : feedback->tranches) {
        if (!tranche.target_device_valid ||
            !drm_devices_equal_by_devid(render_node_stat.st_rdev, tranche.target_device)) {
          continue;
        }

        for (const auto &entry : tranche.format_modifiers) {
          if (entry.format != dmabuf_info.format) {
            continue;
          }

          selection.from_feedback = true;
          if (entry.modifier == DRM_FORMAT_MOD_INVALID) {
            selection.implicit_supported = true;
          } else if (std::find(selection.explicit_modifiers.begin(), selection.explicit_modifiers.end(), entry.modifier) == selection.explicit_modifiers.end()) {
            selection.explicit_modifiers.emplace_back(entry.modifier);
          }
        }

        if (selection.from_feedback) {
          BOOST_LOG(info) << "Wayland DMA-BUF feedback selected tranche for format="sv
                          << dmabuf_info.format
                          << " explicit_modifiers=["sv << modifier_list_string(selection.explicit_modifiers)
                          << "] implicit_modifier="sv << selection.implicit_supported;
          return selection;
        }
      }

      BOOST_LOG(info) << "Wayland DMA-BUF feedback had no tranche for format="sv
                      << dmabuf_info.format
                      << " on render_node=["sv << path_for_fd(gbm_device_get_fd(gbm_device)) << ']';
      return selection;
    };

    const bool needs_new_buffer =
      !next_frame->bo ||
      !next_frame->buffer ||
      next_frame->buffer_width != dmabuf_info.width ||
      next_frame->buffer_height != dmabuf_info.height ||
      next_frame->buffer_format != dmabuf_info.format;

    if (!needs_new_buffer) {
      if (!populate_capture_descriptor(*next_frame)) {
        next_frame->destroy();
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      BOOST_LOG(debug) << "Reusing DMA-BUF screencopy buffer for "
                       << dmabuf_info.width << 'x' << dmabuf_info.height;
      zwlr_screencopy_frame_v1_copy(frame, next_frame->buffer);
      return;
    }

    next_frame->destroy();
    std::uint32_t bo_flags = GBM_BO_USE_RENDERING;
    bool using_linear_dmabuf = false;
    bool use_implicit_modifier_protocol = false;
    const auto feedback_selection = select_feedback_modifiers();
    if (!feedback_selection.explicit_modifiers.empty()) {
      auto requested_modifiers = feedback_selection.explicit_modifiers;

      if (prefer_linear_dmabuf) {
        auto linear_it = std::find(requested_modifiers.begin(), requested_modifiers.end(), DRM_FORMAT_MOD_LINEAR);
        if (linear_it != requested_modifiers.end()) {
          const std::uint64_t linear_modifier = *linear_it;
          requested_modifiers.erase(linear_it);
          if (!logged_linear_dmabuf_attempt) {
            BOOST_LOG(info) << "Wayland DMA-BUF capture: preferring linear modifier advertised by feedback for nested labwc probe"sv;
            logged_linear_dmabuf_attempt = true;
          }

          next_frame->bo = gbm_bo_create_with_modifiers2(
            gbm_device,
            dmabuf_info.width,
            dmabuf_info.height,
            dmabuf_info.format,
            &linear_modifier,
            1,
            GBM_BO_USE_RENDERING
          );
          using_linear_dmabuf = next_frame->bo != nullptr;
        }
      }

      if (!next_frame->bo && !requested_modifiers.empty()) {
        next_frame->bo = gbm_bo_create_with_modifiers2(
          gbm_device,
          dmabuf_info.width,
          dmabuf_info.height,
          dmabuf_info.format,
          requested_modifiers.data(),
          requested_modifiers.size(),
          GBM_BO_USE_RENDERING
        );
      }

      if (!next_frame->bo) {
        BOOST_LOG(info) << "Wayland DMA-BUF capture: feedback-constrained GBM allocation failed for format ["sv
                        << dmabuf_info.format
                        << "] modifiers=["sv << modifier_list_string(feedback_selection.explicit_modifiers)
                        << "]; retrying with legacy GBM allocation heuristics"sv;
      }
    }

    if (!next_frame->bo && prefer_linear_dmabuf) {
      const auto linear_flags = static_cast<std::uint32_t>(GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
      if (gbm_device_is_format_supported(gbm_device, dmabuf_info.format, linear_flags)) {
        bo_flags = linear_flags;
        using_linear_dmabuf = true;
        if (!logged_linear_dmabuf_attempt) {
          BOOST_LOG(info) << "Wayland DMA-BUF capture: preferring linear GBM allocation for nested labwc probe"sv;
          logged_linear_dmabuf_attempt = true;
        }
      } else if (!logged_linear_dmabuf_unsupported) {
        BOOST_LOG(info) << "Wayland DMA-BUF capture: compositor-facing format ["sv
                        << dmabuf_info.format
                        << "] does not report GBM linear support on render_node=["sv
                        << path_for_fd(gbm_device_get_fd(gbm_device))
                        << "]; retrying with driver default modifiers"sv;
        logged_linear_dmabuf_unsupported = true;
      }
    }

    if (!next_frame->bo) {
      next_frame->bo = gbm_bo_create(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, bo_flags);
    }
    if (!next_frame->bo && using_linear_dmabuf) {
      BOOST_LOG(info) << "Wayland DMA-BUF capture: linear GBM allocation failed for format ["sv
                      << dmabuf_info.format
                      << "] on render_node=["sv << path_for_fd(gbm_device_get_fd(gbm_device))
                      << "]; retrying with driver default modifiers"sv;
      using_linear_dmabuf = false;
      bo_flags = GBM_BO_USE_RENDERING;
      next_frame->bo = gbm_bo_create(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, bo_flags);
    }
    if (!next_frame->bo) {
      BOOST_LOG(error) << "Failed to create GBM buffer"sv
                       << " format="sv << dmabuf_info.format
                       << " flags="sv << bo_flags
                       << " render_node=["sv << path_for_fd(gbm_device_get_fd(gbm_device)) << ']';
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    if (!feedback_selection.explicit_modifiers.empty()) {
      using_linear_dmabuf = gbm_bo_get_modifier(next_frame->bo) == DRM_FORMAT_MOD_LINEAR;
    }
    use_implicit_modifier_protocol =
      feedback_selection.from_feedback &&
      feedback_selection.implicit_supported &&
      feedback_selection.explicit_modifiers.empty();

    next_frame->buffer_width = dmabuf_info.width;
    next_frame->buffer_height = dmabuf_info.height;
    next_frame->buffer_format = dmabuf_info.format;
    next_frame->buffer_key = next_buffer_key++;

    if (!populate_capture_descriptor(*next_frame)) {
      next_frame->destroy();
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    auto modifier = next_frame->sd.modifier;
    auto protocol_modifier = use_implicit_modifier_protocol ? DRM_FORMAT_MOD_INVALID : modifier;
    auto stride = next_frame->sd.pitches[0];
    auto wl_buffer_fd = gbm_bo_get_fd(next_frame->bo);
    if (wl_buffer_fd < 0) {
      BOOST_LOG(error) << "Failed to get buffer FD for wl_buffer creation"sv;
      next_frame->destroy();
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);
    zwp_linux_buffer_params_v1_add(params, wl_buffer_fd, 0, 0, stride, protocol_modifier >> 32, protocol_modifier & 0xffffffff);

    BOOST_LOG(info) << "Wayland DMA-BUF capture target: size="sv
                    << dmabuf_info.width << 'x' << dmabuf_info.height
                    << " format="sv << dmabuf_info.format
                    << " stride="sv << stride
                    << " modifier="sv << modifier
                    << " protocol_modifier="sv << protocol_modifier
                    << " linear="sv << using_linear_dmabuf;

    auto *pending = new pending_buffer_create_t {
      .self = this,
      .target = next_frame,
      .frame = frame,
      .wl_buffer_fd = wl_buffer_fd,
    };
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, pending);
    zwp_linux_buffer_params_v1_create(params, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, 0);
  }

  // Buffer done callback - time to create buffer
  void dmabuf_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();

    const bool can_use_shm = shm_info.supported && shm_global;
    const bool can_use_dmabuf = dmabuf_info.supported && dmabuf_interface;

    if (prefer_shm && can_use_shm) {
      if (!logged_preferred_shm_capture) {
        BOOST_LOG(info) << "Using SHM screencopy (preferred for this runtime)"sv;
        logged_preferred_shm_capture = true;
      }
      create_and_copy_shm(frame);
    } else if (can_use_dmabuf) {
      // Prefer DMA-BUF if supported unless SHM was explicitly requested.
      // Store format info first
      next_frame->sd.fourcc = dmabuf_info.format;
      next_frame->sd.width = dmabuf_info.width;
      next_frame->sd.height = dmabuf_info.height;

      // Create and start copy
      create_and_copy_dmabuf(frame);
    } else if (can_use_shm) {
      // SHM fallback — works on all drivers including NVIDIA
      if (!logged_shm_fallback_capture) {
        BOOST_LOG(info) << "Using SHM screencopy (DMA-BUF not available)"sv;
        logged_shm_fallback_capture = true;
      }
      create_and_copy_shm(frame);
    } else {
      BOOST_LOG(error) << "No supported buffer types"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    }
  }

  // Buffer params created callback
  void dmabuf_t::buffer_params_created(
    void *data,
    struct zwp_linux_buffer_params_v1 *params,
    struct wl_buffer *buffer
  ) {
    auto pending = std::unique_ptr<pending_buffer_create_t>(static_cast<pending_buffer_create_t *>(data));
    zwp_linux_buffer_params_v1_destroy(params);

    pending->target->buffer = buffer;
    if (pending->wl_buffer_fd >= 0) {
      close(pending->wl_buffer_fd);
      pending->wl_buffer_fd = -1;
    }

    zwlr_screencopy_frame_v1_copy(pending->frame, buffer);
  }

  // Buffer params failed callback
  void dmabuf_t::buffer_params_failed(
    void *data,
    struct zwp_linux_buffer_params_v1 *params
  ) {
    auto pending = std::unique_ptr<pending_buffer_create_t>(static_cast<pending_buffer_create_t *>(data));
    zwp_linux_buffer_params_v1_destroy(params);
    BOOST_LOG(error) << "Failed to create buffer from params"sv;
    if (pending->wl_buffer_fd >= 0) {
      close(pending->wl_buffer_fd);
      pending->wl_buffer_fd = -1;
    }
    pending->target->destroy();

    zwlr_screencopy_frame_v1_destroy(pending->frame);
    pending->self->status = REINIT;
  }

  // SHM buffer creation and copy
  void dmabuf_t::create_and_copy_shm(zwlr_screencopy_frame_v1 *frame) {
    if (!shm_global) {
      BOOST_LOG(error) << "SHM: No wl_shm global available"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    const bool needs_new_buffer =
      !shm_buffer ||
      !shm_data ||
      !shm_buffer_info.valid ||
      shm_buffer_info.format != shm_info.format ||
      shm_buffer_info.width != shm_info.width ||
      shm_buffer_info.height != shm_info.height ||
      shm_buffer_info.stride != shm_info.stride;

    if (needs_new_buffer) {
      cleanup_shm();
      shm_size = shm_info.stride * shm_info.height;

      shm_fd = memfd_create("polaris-screencopy", MFD_CLOEXEC);
      if (shm_fd < 0) {
        BOOST_LOG(error) << "Failed to create memfd for SHM screencopy"sv;
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      if (ftruncate(shm_fd, shm_size) < 0) {
        BOOST_LOG(error) << "Failed to resize SHM buffer"sv;
        cleanup_shm();
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      shm_data = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
      if (shm_data == MAP_FAILED) {
        shm_data = nullptr;
        BOOST_LOG(error) << "Failed to mmap SHM buffer"sv;
        cleanup_shm();
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      shm_pool = wl_shm_create_pool(shm_global, shm_fd, shm_size);
      if (!shm_pool) {
        BOOST_LOG(error) << "Failed to create wl_shm_pool"sv;
        cleanup_shm();
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      shm_buffer = wl_shm_pool_create_buffer(shm_pool, 0,
        shm_info.width, shm_info.height, shm_info.stride, shm_info.format);
      if (!shm_buffer) {
        BOOST_LOG(error) << "Failed to create wl_buffer from SHM pool"sv;
        cleanup_shm();
        zwlr_screencopy_frame_v1_destroy(frame);
        status = REINIT;
        return;
      }

      shm_buffer_info = {
        .valid = true,
        .format = shm_info.format,
        .width = shm_info.width,
        .height = shm_info.height,
        .stride = shm_info.stride,
      };

      BOOST_LOG(info) << "SHM screencopy buffer allocated: "sv << shm_info.width << "x"sv << shm_info.height
                      << " stride="sv << shm_info.stride << " format="sv << shm_info.format;
    } else {
      BOOST_LOG(debug) << "Reusing SHM screencopy buffer for "
                       << shm_info.width << 'x' << shm_info.height;
    }

    // Request the compositor to copy the frame into our SHM buffer
    zwlr_screencopy_frame_v1_copy(frame, shm_buffer);
  }

  void dmabuf_t::cleanup_shm() {
    if (shm_buffer) {
      wl_buffer_destroy(shm_buffer);
      shm_buffer = nullptr;
    }
    if (shm_pool) {
      wl_shm_pool_destroy(shm_pool);
      shm_pool = nullptr;
    }
    if (shm_data) {
      munmap(shm_data, shm_size);
      shm_data = nullptr;
    }
    if (shm_fd >= 0) {
      close(shm_fd);
      shm_fd = -1;
    }
    shm_size = 0;
    shm_frame_ready = false;
    shm_buffer_info = {};
  }

  // Ready callback
  void dmabuf_t::ready(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {
    BOOST_LOG(debug) << "Frame ready"sv;

    if (shm_data && shm_size > 0) {
      // SHM frame is ready — data is in shm_data
      shm_frame_ready = true;
    } else {
      // DMA-BUF frame is ready — GBM buffer contains screen content
      current_frame->reset_capture_descriptor();
      current_frame = get_next_frame();
    }

    zwlr_screencopy_frame_v1_destroy(frame);
    status = READY;
  }

  // Failed callback
  void dmabuf_t::failed(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();
    BOOST_LOG(error) << "Frame capture failed"sv
                     << " dmabuf_supported="sv << dmabuf_info.supported
                     << " shm_supported="sv << shm_info.supported
                     << " prefer_linear_dmabuf="sv << prefer_linear_dmabuf
                     << " target_size="sv << next_frame->buffer_width << 'x' << next_frame->buffer_height
                     << " target_format="sv << next_frame->buffer_format
                     << " target_modifier="sv << next_frame->buffer_modifier;
    next_frame->destroy();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = REINIT;
  }

  void dmabuf_t::damage(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height
  ) {};

  void frame_t::destroy() {
    reset_capture_descriptor();
    reset_buffer();
  }

  void frame_t::reset_capture_descriptor() {
    for (auto x = 0; x < 4; ++x) {
      if (sd.fds[x] >= 0) {
        close(sd.fds[x]);

        sd.fds[x] = -1;
      }
    }
  }

  void frame_t::reset_buffer() {
    if (buffer) {
      wl_buffer_destroy(buffer);
      buffer = nullptr;
    }

    if (bo) {
      gbm_bo_destroy(bo);
      bo = nullptr;
    }

    buffer_key = 0;
    buffer_width = 0;
    buffer_height = 0;
    buffer_format = 0;
    buffer_modifier = 0;
  }

  frame_t::frame_t() {
    // File descriptors aren't open
    std::fill_n(sd.fds, 4, -1);
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  static bool validate() {
    display_t display;

    return display.init() == 0;
  }

  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop
