/**
 * @file src/platform/linux/misc.h
 * @brief Miscellaneous declarations for Linux.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <unistd.h>
#include <vector>

// local includes
#include "src/utility.h"

KITTY_USING_MOVE_T(file_t, int, -1, {
  if (el >= 0) {
    close(el);
  }
});

enum class window_system_e {
  NONE,  ///< No window system
  X11,  ///< X11
  WAYLAND,  ///< Wayland
};

extern window_system_e window_system;

namespace dyn {
  typedef void (*apiproc)(void);

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn

namespace platf {

  /**
   * @brief One /dev/dri/renderD* node with the sysfs facts the default-device
   *        choice is made from.
   */
  struct render_device_candidate_t {
    std::string path;  ///< Render node path (e.g. /dev/dri/renderD128)
    std::string driver;  ///< Kernel driver bound to the parent PCI device (amdgpu, nvidia, i915, xe, ...)
    long long vram_total_bytes = 0;  ///< amdgpu mem_info_vram_total or i915/xe lmem_total_bytes; 0 when neither is exposed
    bool boot_vga = false;  ///< Parent PCI function is the firmware boot display device
  };

  /**
   * @brief True for DRM drivers that expose a render node but no GPU.
   * @details Virtual displays (evdi, vkms), other streaming stacks' virtual displays
   *          (hermes-kms, vibeshine_drm) and USB display adapters (udl) all register a
   *          renderD* node. None of them can encode, and a laptop that carries one next to
   *          an iGPU and an NVIDIA card enumerates three "GPUs" where it has two.
   */
  bool is_virtual_display_driver(std::string_view driver);

  /**
   * @brief Drop virtual display nodes from an encoder candidate list.
   */
  std::vector<render_device_candidate_t> without_virtual_display_nodes(std::vector<render_device_candidate_t> candidates);

  /**
   * @brief The argv a launch command yields when it carries POSIX shell quoting.
   *
   * Boost.Process splits a command string on unquoted spaces, strips only double quotes and
   * ignores single quotes, so a single-quoted path reaches the child with its quotes attached
   * and a path with a space arrives in two pieces. A command that uses single quotes or
   * backslashes is split the way a shell would instead; anything else is left to Boost so
   * plain commands keep their exact behaviour.
   * @return The tokens, or nothing when the command needs no shell-style split or cannot be split.
   */
  std::optional<std::vector<std::string>> posix_command_argv(const std::string &cmd);

  /**
   * @brief The render node the encoder actually runs on: adapter_name when it is set,
   *        otherwise the automatic choice. Empty when neither names a node.
   */
  std::string effective_encoder_render_device();

#ifdef POLARIS_TESTS
  /**
   * @brief Pin what the automatic choice returns; the test build defaults it to empty so a
   *        host's real render nodes never leak into a fixture, nullopt restores the probe.
   */
  void set_effective_encoder_render_device_for_tests(std::optional<std::string> node);
#endif

  /**
   * @brief Pick the render node games and capture should default to when
   *        adapter_name does not say (issue #367).
   *
   * Node numbering is probe order, not importance: on an APU + dGPU host the
   * first node wlroots or a hardcoded renderD128 fallback lands on may be the
   * integrated GPU. Preference: discrete over integrated (an NVIDIA driver —
   * nvidia or nouveau — or a dedicated memory pool of at least 1 GiB; APU
   * carve-outs usually report less), then larger dedicated memory, then the
   * firmware boot display, then the lowest path for stability. Known gap: an
   * Intel Arc dGPU whose driver exposes no lmem sysfs ranks as integrated —
   * such hosts should set adapter_name, which overrides all of this.
   */
  std::string choose_default_render_device(std::vector<render_device_candidate_t> candidates);

  /**
   * @brief Enumerate /dev/dri/renderD* with sysfs metadata and choose, caching
   *        the answer for the process lifetime. Empty when no render node exists.
   */
  std::string default_render_device();

  /**
   * @brief Every render node that can encode, with the sysfs facts the default choice uses.
   * @details Virtual display nodes are left out. Enumerated once per process.
   */
  std::vector<render_device_candidate_t> render_devices();

  /**
   * @brief Return the kernel driver bound to an enumerated render node.
   * @param render_device Exact /dev/dri/renderD* path, or empty to inspect the
   *        shared default render device.
   * @return Driver name such as amdgpu, nvidia, i915, or xe; empty if unknown.
   */
  std::string render_device_driver(std::string_view render_device = {});

  /**
   * @brief The default render node for VAAPI specifically (issue #367).
   *
   * Same choice as default_render_device(), but NVIDIA-bound nodes (nvidia,
   * nouveau) are excluded first: no usable VA driver exists there, so handing
   * VAAPI the general default on an iGPU + NVIDIA host would trade a working
   * encoder for a guaranteed init failure. Empty when no VAAPI-capable node
   * exists; the caller keeps its own last-resort.
   */
  std::string default_vaapi_render_device();

  /**
   * @brief Run a program with an explicit argument vector, without a shell.
   * @return The exit status, or 128 + signal for a signaled child.
   */
  int run_process_argv(const std::vector<std::string> &argv);

  /**
   * @brief Result from a bounded, shell-free child process invocation.
   */
  struct process_output_t {
    int exit_status = 127;
    bool timed_out = false;
    bool truncated = false;
    std::string output;
    bool cancelled = false;
  };

  /**
   * @brief Run a program without a shell and capture its standard output.
   *
   * The child is killed when @p timeout expires. Output beyond @p max_output_bytes
   * is drained but not retained, so an unexpected helper response cannot grow
   * Polaris without bound.
   */
  process_output_t run_process_argv_capture(
    const std::vector<std::string> &argv,
    std::chrono::milliseconds timeout = std::chrono::seconds {2},
    std::size_t max_output_bytes = 1024 * 1024,
    std::stop_token stop = {}
  );

}  // namespace platf
