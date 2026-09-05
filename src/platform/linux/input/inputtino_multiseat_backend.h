/**
 * @file src/platform/linux/input/inputtino_multiseat_backend.h
 * @brief Trusted host lifecycle backend for multiseat virtual input.
 */
#pragma once

#ifdef __linux__

#include "src/platform/linux/multiseat_input_authority.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiseat::input {

  inline constexpr std::size_t maximum_kernel_metadata_bytes = 64 * 1024;

  enum class managed_device_kind_e {
    keyboard,
    mouse,
    touch,
    pen,
    gamepad,
  };

  struct expected_event_node_t {
    device_kind_e kind = device_kind_e::keyboard;
    std::uint32_t slot = 0;
    std::string kernel_name;
    std::filesystem::path worker_path;

    bool operator==(const expected_event_node_t &) const = default;
  };

  /** Pure inputtino creation policy derived from one authority expectation. */
  struct device_spec_t {
    managed_device_kind_e kind = managed_device_kind_e::keyboard;
    std::uint32_t slot = 0;
    std::string kernel_name;
    std::string expected_phys;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint16_t version = 0;
    bool permits_joystick_node = false;
    std::vector<expected_event_node_t> event_nodes;

    bool operator==(const device_spec_t &) const = default;
  };

  [[nodiscard]] std::optional<std::vector<device_spec_t>> build_device_specs(
    const expectation_t &expectation
  );

  class managed_device_t {
  public:
    virtual ~managed_device_t() = default;
    virtual std::vector<std::filesystem::path> nodes() const = 0;
  };

  class device_factory_t {
  public:
    virtual ~device_factory_t() = default;
    virtual std::unique_ptr<managed_device_t> create(
      const device_spec_t &spec
    ) = 0;
  };

  /** Production factory. Constructing it is inert; create() opens uinput. */
  class inputtino_device_factory_t final : public device_factory_t {
  public:
    std::unique_ptr<managed_device_t> create(
      const device_spec_t &spec
    ) override;
  };

  enum class node_io_status_e {
    ok,
    absent,
    retryable,
    unsafe,
  };

  struct input_node_metadata_t {
    std::uint64_t filesystem_device = 0;
    std::uint64_t inode = 0;
    std::uint32_t character_major = 0;
    std::uint32_t character_minor = 0;
    bool character_device = false;

    bool operator==(const input_node_metadata_t &) const = default;
  };

  struct input_node_read_t {
    node_io_status_e status = node_io_status_e::unsafe;
    std::optional<input_node_metadata_t> metadata;
  };

  struct trusted_text_read_t {
    node_io_status_e status = node_io_status_e::unsafe;
    std::string text;
  };

  /** Injectable syscall boundary; fake implementations keep tests offline. */
  class kernel_node_io_t {
  public:
    virtual ~kernel_node_io_t() = default;
    virtual input_node_read_t inspect_input_node(
      const std::filesystem::path &path
    ) = 0;
    virtual trusted_text_read_t read_trusted_text(
      const std::filesystem::path &path,
      std::size_t maximum_bytes
    ) = 0;
  };

  /** Linux O_PATH/fstat and root-owned, non-writable text-file adapter. */
  class posix_kernel_node_io_t final : public kernel_node_io_t {
  public:
    input_node_read_t inspect_input_node(
      const std::filesystem::path &path
    ) override;
    trusted_text_read_t read_trusted_text(
      const std::filesystem::path &path,
      std::size_t maximum_bytes
    ) override;
  };

  enum class node_observation_status_e {
    observed,
    absent,
    retryable,
    unsafe,
  };

  struct kernel_node_snapshot_t {
    std::filesystem::path host_path;
    std::uint64_t filesystem_device = 0;
    std::uint64_t inode = 0;
    std::uint32_t character_major = 0;
    std::uint32_t character_minor = 0;
    std::string kernel_name;
    std::string phys;
    std::string host_seat;

    bool operator==(const kernel_node_snapshot_t &) const = default;
  };

  struct node_observation_t {
    node_observation_status_e status = node_observation_status_e::unsafe;
    std::optional<kernel_node_snapshot_t> snapshot;
  };

  class kernel_node_probe_t {
  public:
    virtual ~kernel_node_probe_t() = default;
    virtual node_observation_t observe(
      const std::filesystem::path &path
    ) = 0;
  };

  struct linux_kernel_probe_paths_t {
    std::filesystem::path device_root = "/dev/input";
    std::filesystem::path sys_class_input_root = "/sys/class/input";
    std::filesystem::path udev_data_root = "/run/udev/data";
  };

  /** Derives kernel identity from fstat, sysfs, and the udev database. */
  class linux_kernel_node_probe_t final : public kernel_node_probe_t {
  public:
    explicit linux_kernel_node_probe_t(
      kernel_node_io_t &io,
      linux_kernel_probe_paths_t paths = {}
    );

    node_observation_t observe(
      const std::filesystem::path &path
    ) override;

  private:
    kernel_node_io_t &io_;
    linux_kernel_probe_paths_t paths_;
  };

  struct inputtino_host_backend_options_t {
    std::size_t discovery_attempts = 20;
    // The pinned Xbox implementation can retain its uinput fd for one 500-ms
    // feedback poll after its wrapper is released.
    std::size_t cleanup_attempts = 100;
    std::size_t stable_observations = 2;
    std::chrono::milliseconds retry_delay {10};
  };

  using inputtino_backend_waiter_t =
    std::function<void(std::chrono::milliseconds)>;

  /**
   * Owns inputtino devices on the trusted host and returns exact event nodes.
   *
   * This backend intentionally rejects route(): the typed worker input decoder
   * and feedback transport are a later boundary. It is not wired to Podman or
   * the singleton runtime by this checkpoint.
   */
  class inputtino_host_backend_t final : public backend_t {
  public:
    inputtino_host_backend_t(
      device_factory_t &factory,
      kernel_node_probe_t &probe,
      inputtino_host_backend_options_t options = {},
      inputtino_backend_waiter_t waiter = {}
    );
    ~inputtino_host_backend_t() override;

    inputtino_host_backend_t(const inputtino_host_backend_t &) = delete;
    inputtino_host_backend_t &operator=(const inputtino_host_backend_t &) = delete;
    inputtino_host_backend_t(inputtino_host_backend_t &&) = delete;
    inputtino_host_backend_t &operator=(inputtino_host_backend_t &&) = delete;

    backend_create_result_t create(const expectation_t &expectation) override;
    backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override;
    backend_result_e route(
      const seat_handle_t &handle,
      std::string_view input_seat,
      std::uint64_t sequence,
      std::span<const std::uint8_t> payload
    ) override;
    std::vector<allocation_t> inventory() override;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

}  // namespace multiseat::input

#endif
