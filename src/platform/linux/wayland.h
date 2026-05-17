/**
 * @file src/platform/linux/wayland.h
 * @brief Declarations for Wayland capture.
 */
#pragma once

// standard includes
#include <bitset>
#include <sys/types.h>
#include <string_view>

#ifdef POLARIS_BUILD_WAYLAND
  #include <ext-image-capture-source-v1.h>
  #include <ext-image-copy-capture-v1.h>
  #include <linux-dmabuf-unstable-v1.h>
  #include <wlr-screencopy-unstable-v1.h>
  #include <xdg-output-unstable-v1.h>
#endif

struct gbm_bo;
struct gbm_device;

// local includes
#include "graphics.h"

/**
 * The classes defined in this macro block should only be used by
 * cpp files whose compilation depends on POLARIS_BUILD_WAYLAND
 */
#ifdef POLARIS_BUILD_WAYLAND

namespace wl {
  struct dmabuf_feedback_format_modifier_t {
    std::uint32_t format {};
    std::uint64_t modifier {};
  };

  struct dmabuf_feedback_tranche_t {
    bool target_device_valid {false};
    dev_t target_device {};
    std::uint32_t flags {0};
    std::vector<dmabuf_feedback_format_modifier_t> format_modifiers;
  };

  struct dmabuf_feedback_t {
    bool available {false};
    bool main_device_valid {false};
    dev_t main_device {};
    std::vector<dmabuf_feedback_tranche_t> tranches;
  };

  using display_internal_t = util::safe_ptr<wl_display, wl_display_disconnect>;

  class frame_t {
  public:
    frame_t();
    ~frame_t() = default;
    void destroy();
    void reset_capture_descriptor();
    void reset_buffer();

    egl::surface_descriptor_t sd;
    ::gbm_bo *bo {nullptr};
    ::wl_buffer *buffer {nullptr};
    std::uint64_t buffer_key {0};
    std::uint32_t buffer_width {0};
    std::uint32_t buffer_height {0};
    std::uint32_t buffer_format {0};
    std::uint64_t buffer_modifier {0};
  };

  class dmabuf_t {
  public:
    enum status_e {
      WAITING,  ///< Waiting for a frame
      READY,  ///< Frame is ready
      REINIT,  ///< Reinitialize the frame
    };

    dmabuf_t();
    ~dmabuf_t();

    dmabuf_t(dmabuf_t &&) = delete;
    dmabuf_t(const dmabuf_t &) = delete;
    dmabuf_t &operator=(const dmabuf_t &) = delete;
    dmabuf_t &operator=(dmabuf_t &&) = delete;

    void listen(zwlr_screencopy_manager_v1 *screencopy_manager, zwp_linux_dmabuf_v1 *dmabuf_interface, wl_output *output, bool blend_cursor = false, wl_shm *shm = nullptr);
    void set_feedback(const dmabuf_feedback_t *feedback) {
      this->feedback = feedback;
    }
    static void buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *wl_buffer);
    static void buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *params);
    void buffer(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride);
    void linux_dmabuf(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height);
    void buffer_done(zwlr_screencopy_frame_v1 *frame);
    void flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags);
    void damage(zwlr_screencopy_frame_v1 *frame, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    void ready(zwlr_screencopy_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    void failed(zwlr_screencopy_frame_v1 *frame);

    frame_t *get_next_frame() {
      return current_frame == &frames[0] ? &frames[1] : &frames[0];
    }

    status_e status;
    std::array<frame_t, 2> frames;
    frame_t *current_frame;
    zwlr_screencopy_frame_v1_listener listener;

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t stride;
    } shm_info;

  private:
    bool init_gbm();
    void cleanup_gbm();
    void create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame);

    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    wl_shm *shm_global {nullptr};
    const dmabuf_feedback_t *feedback {nullptr};

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
    } dmabuf_info;

  public:
    // SHM capture state (accessed by wlgrab)
    void *shm_data {nullptr};
    bool shm_frame_ready {false};  ///< Set by ready(), cleared after copy
    bool prefer_shm {false};
    bool prefer_linear_dmabuf {false};

  private:
    ::gbm_device *gbm_device {nullptr};
    bool y_invert {false};

    int shm_fd {-1};
    std::size_t shm_size {0};
    ::wl_shm_pool *shm_pool {nullptr};
    ::wl_buffer *shm_buffer {nullptr};
    struct {
      bool valid {false};
      std::uint32_t format {0};
      std::uint32_t width {0};
      std::uint32_t height {0};
      std::uint32_t stride {0};
    } shm_buffer_info;
    bool logged_preferred_shm_capture {false};
    bool logged_shm_fallback_capture {false};
    bool logged_linear_dmabuf_attempt {false};
    bool logged_linear_dmabuf_unsupported {false};
    std::uint64_t next_buffer_key {1};

    void create_and_copy_shm(zwlr_screencopy_frame_v1 *frame);
    void cleanup_shm();
  };

  class display_t;

  class extcopy_t {
  public:
    enum status_e {
      WAITING,  ///< Waiting for constraints or a frame
      READY,  ///< Frame is ready
      REINIT,  ///< Reinitialize the frame/session
    };

    extcopy_t();
    ~extcopy_t();

    extcopy_t(extcopy_t &&) = delete;
    extcopy_t(const extcopy_t &) = delete;
    extcopy_t &operator=(const extcopy_t &) = delete;
    extcopy_t &operator=(extcopy_t &&) = delete;

    int init(
      display_t &display,
      ext_image_copy_capture_manager_v1 *copy_capture_manager,
      ext_output_image_capture_source_manager_v1 *output_capture_source_manager,
      zwp_linux_dmabuf_v1 *dmabuf_interface,
      wl_output *output,
      bool blend_cursor = false
    );
    void capture(display_t &display);
    static void buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *wl_buffer);
    static void buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *params);

    frame_t *get_next_frame() {
      return current_frame == &frames[0] ? &frames[1] : &frames[0];
    }

    status_e status;
    std::array<frame_t, 2> frames;
    frame_t *current_frame;

  private:
    struct format_constraints_t {
      std::uint32_t format {};
      bool implicit_modifier {false};
      std::vector<std::uint64_t> modifiers;
    };

    bool init_gbm_for_device(dev_t device);
    void cleanup_gbm();
    void destroy_capture_frame();
    void destroy_session();
    bool wait_for_status(display_t &display, std::chrono::milliseconds timeout);
    bool ensure_session(display_t &display);
    bool choose_format();
    bool populate_capture_descriptor(frame_t &target);
    bool allocate_buffer(display_t &display, frame_t &target);
    void reset_constraints();

    void session_buffer_size(ext_image_copy_capture_session_v1 *session, std::uint32_t width, std::uint32_t height);
    void session_shm_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format);
    void session_dmabuf_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format, wl_array *modifiers);
    void session_dmabuf_device(ext_image_copy_capture_session_v1 *session, wl_array *device);
    void session_done(ext_image_copy_capture_session_v1 *session);
    void session_stopped(ext_image_copy_capture_session_v1 *session);
    void frame_transform(ext_image_copy_capture_frame_v1 *frame, std::uint32_t transform);
    void frame_damage(ext_image_copy_capture_frame_v1 *frame, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    void frame_presentation_time(ext_image_copy_capture_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    void frame_ready(ext_image_copy_capture_frame_v1 *frame);
    void frame_failed(ext_image_copy_capture_frame_v1 *frame, std::uint32_t reason);

    ext_image_copy_capture_session_v1_listener session_listener;
    ext_image_copy_capture_frame_v1_listener frame_listener;
    ext_image_copy_capture_manager_v1 *copy_capture_manager {nullptr};
    ext_output_image_capture_source_manager_v1 *output_capture_source_manager {nullptr};
    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    wl_output *output {nullptr};
    ext_image_capture_source_v1 *capture_source {nullptr};
    ext_image_copy_capture_session_v1 *capture_session {nullptr};
    ext_image_copy_capture_frame_v1 *capture_frame_object {nullptr};
    ::gbm_device *gbm_device {nullptr};
    dev_t gbm_device_id {};
    bool gbm_device_id_valid {false};
    bool blend_cursor {false};
    bool constraints_ready {false};
    bool buffer_size_valid {false};
    bool device_valid {false};
    bool chosen_format_valid {false};
    bool buffer_create_done {false};
    bool buffer_create_success {false};
    bool probe_failed_unknown_ {false};  ///< frame_failed(reason=0) fired during init probe
    std::uint32_t frame_width {0};
    std::uint32_t frame_height {0};
    dev_t device_id {};
    format_constraints_t chosen_format;
    std::vector<format_constraints_t> dmabuf_formats;
    std::uint64_t next_buffer_key {1};
  };

  class monitor_t {
  public:
    explicit monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);
    void xdg_name(zxdg_output_v1 *, const char *name);
    void xdg_description(zxdg_output_v1 *, const char *description);
    void xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y);
    void xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height);

    void xdg_done(zxdg_output_v1 *) {}

    void wl_geometry(wl_output *wl_output, std::int32_t x, std::int32_t y, std::int32_t physical_width, std::int32_t physical_height, std::int32_t subpixel, const char *make, const char *model, std::int32_t transform) {}

    void wl_mode(wl_output *wl_output, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh);

    void wl_done(wl_output *wl_output) {}

    void wl_scale(wl_output *wl_output, std::int32_t factor) {}

    wl_output *output;
    std::string name;
    std::string description;
    platf::touch_port_t viewport;
    wl_output_listener wl_listener;
    zxdg_output_v1_listener xdg_listener;
  };

  class interface_t {
    struct bind_t {
      std::uint32_t id;
      std::uint32_t version;
    };

  public:
    enum interface_e {
      XDG_OUTPUT,  ///< xdg-output
      WLR_EXPORT_DMABUF,  ///< screencopy manager
      LINUX_DMABUF,  ///< linux-dmabuf protocol
      EXT_OUTPUT_CAPTURE_SOURCE,  ///< ext output image capture source manager
      EXT_IMAGE_COPY_CAPTURE,  ///< ext image copy capture manager
      MAX_INTERFACES,  ///< Maximum number of interfaces
    };

    interface_t() noexcept;
    ~interface_t();

    interface_t(interface_t &&) = delete;
    interface_t(const interface_t &) = delete;
    interface_t &operator=(const interface_t &) = delete;
    interface_t &operator=(interface_t &&) = delete;

    void listen(wl_registry *registry);

    bool operator[](interface_e bit) const {
      return interface[bit];
    }

    std::vector<std::unique_ptr<monitor_t>> monitors;
    zwlr_screencopy_manager_v1 *screencopy_manager {nullptr};
    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    ext_output_image_capture_source_manager_v1 *output_capture_source_manager {nullptr};
    ext_image_copy_capture_manager_v1 *copy_capture_manager {nullptr};
    dmabuf_feedback_t dmabuf_feedback;
    zxdg_output_manager_v1 *output_manager {nullptr};
    wl_shm *shm {nullptr};

  private:
    void begin_feedback_update();
    void feedback_done(zwp_linux_dmabuf_feedback_v1 *feedback);
    void feedback_format_table(zwp_linux_dmabuf_feedback_v1 *feedback, int32_t fd, std::uint32_t size);
    void feedback_main_device(zwp_linux_dmabuf_feedback_v1 *feedback, wl_array *device);
    void feedback_tranche_done(zwp_linux_dmabuf_feedback_v1 *feedback);
    void feedback_tranche_target_device(zwp_linux_dmabuf_feedback_v1 *feedback, wl_array *device);
    void feedback_tranche_formats(zwp_linux_dmabuf_feedback_v1 *feedback, wl_array *indices);
    void feedback_tranche_flags(zwp_linux_dmabuf_feedback_v1 *feedback, std::uint32_t flags);
    void add_interface(wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t version);
    void del_interface(wl_registry *registry, uint32_t id);

    std::bitset<MAX_INTERFACES> interface;
    wl_registry_listener listener;
    zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback_object {nullptr};
    zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener;
    dmabuf_feedback_t pending_dmabuf_feedback;
    dmabuf_feedback_tranche_t pending_dmabuf_feedback_tranche;
    std::vector<dmabuf_feedback_format_modifier_t> dmabuf_feedback_table;
    bool dmabuf_feedback_update_in_progress {false};
    bool dmabuf_feedback_tranche_active {false};
  };

  class display_t {
  public:
    /**
     * @brief Initialize display.
     * If display_name == nullptr -> display_name = std::getenv("WAYLAND_DISPLAY")
     * @param display_name The name of the display.
     * @return 0 on success, -1 on failure.
     */
    int init(const char *display_name = nullptr);

    // Roundtrip with Wayland connection
    void roundtrip();

    // Wait up to the timeout to read and dispatch new events
    bool dispatch(std::chrono::milliseconds timeout);

    // Get the registry associated with the display
    // No need to manually free the registry
    wl_registry *registry();

    inline display_internal_t::pointer get() {
      return display_internal.get();
    }

  private:
    display_internal_t display_internal;
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr);
  int init();
}  // namespace wl
#else

struct wl_output;
struct zxdg_output_manager_v1;

namespace wl {
  class monitor_t {
  public:
    monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);

    wl_output *output;
    std::string name;
    std::string description;
    platf::touch_port_t viewport;
  };

  inline std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr) {
    return {};
  }

  inline int init() {
    return -1;
  }
}  // namespace wl
#endif
