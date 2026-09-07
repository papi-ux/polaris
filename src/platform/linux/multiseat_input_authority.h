/**
 * @file src/platform/linux/multiseat_input_authority.h
 * @brief Generation-fenced host authority for multiseat virtual input.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_input_protocol.h"
  #include "src/multiseat_runtime.h"

  #include <cstddef>
  #include <cstdint>
  #include <filesystem>
  #include <mutex>
  #include <optional>
  #include <span>
  #include <string>
  #include <string_view>
  #include <vector>

namespace multiseat::input {

  inline constexpr std::size_t maximum_input_payload_bytes =
    maximum_encoded_input_event_bytes;
  inline constexpr std::size_t maximum_input_allocations = 256;
  inline constexpr std::size_t maximum_kernel_device_name_bytes = 79;
  inline constexpr std::string_view isolated_host_seat = "seat-polaris";
  inline constexpr std::string_view multiseat_kernel_device_prefix =
    "Polaris multiseat ";

  enum class device_kind_e {
    keyboard,
    mouse_relative,
    mouse_absolute,
    touch,
    pen,
    gamepad,
  };

  /** Devices created before a worker starts. Keyboard and mouse are mandatory. */
  struct plan_t {
    bool touch = false;
    bool pen = false;
    std::uint32_t gamepad_slots = 1;

    bool operator==(const plan_t &) const = default;
  };

  /**
   * One exact host node mapped into one worker's private device namespace.
   *
   * Creation endpoints such as /dev/uinput and /dev/uhid are deliberately not
   * representable. The host broker retains those capabilities; an untrusted
   * launcher receives only the event nodes allocated to its exact generation.
   */
  struct device_node_t {
    device_kind_e kind = device_kind_e::keyboard;
    std::uint32_t slot = 0;
    std::filesystem::path host_path;
    std::filesystem::path worker_path;
    std::uint64_t filesystem_device = 0;
    std::uint64_t inode = 0;
    std::uint32_t character_major = 0;
    std::uint32_t character_minor = 0;
    std::string kernel_name;
    std::string phys;
    std::string host_seat;

    bool operator==(const device_node_t &) const = default;
  };

  struct allocation_t {
    seat_handle_t handle;
    std::string input_seat;
    plan_t plan;
    std::vector<device_node_t> nodes;

    bool operator==(const allocation_t &) const = default;
  };

  struct expectation_t {
    seat_handle_t handle;
    std::string input_seat;
    plan_t plan;

    bool operator==(const expectation_t &) const = default;
  };

  enum class backend_result_e {
    applied,
    already_applied,
    not_found,
    rejected,
    indeterminate,
  };

  struct backend_create_result_t {
    backend_result_e result = backend_result_e::rejected;
    std::optional<allocation_t> allocation;
  };

  /** Injectable host boundary. Tests never open uinput, uhid, or evdev. */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    virtual backend_create_result_t create(const expectation_t &expectation) = 0;
    virtual backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) = 0;
    virtual backend_result_e route(
      const seat_handle_t &handle,
      std::string_view input_seat,
      std::uint64_t sequence,
      const input_event_t &event
    ) = 0;
    virtual std::vector<allocation_t> inventory() = 0;
  };

  enum class status_e {
    applied,
    already_applied,
    reconciliation_required,
    invalid_request,
    not_found,
    stale_authority,
    backend_rejected,
    backend_indeterminate,
    backend_protocol_error,
  };

  struct prepare_result_t {
    status_e status = status_e::invalid_request;
    std::optional<allocation_t> allocation;

    [[nodiscard]] bool prepared() const {
      return (status == status_e::applied || status == status_e::already_applied) &&
             allocation.has_value();
    }
  };

  struct reconciliation_report_t {
    std::size_t observations = 0;
    std::size_t expected = 0;
    std::size_t current = 0;
    std::size_t orphans = 0;
    std::size_t removed_orphans = 0;
    std::size_t missing = 0;
    std::size_t protocol_errors = 0;
    std::size_t backend_failures = 0;
    bool inventory_authoritative = false;
    bool admission_ready = false;
  };

  struct cleanup_report_t {
    std::size_t released_allocations = 0;
    std::size_t cleanup_failures = 0;
  };

  [[nodiscard]] bool valid_plan(const plan_t &plan);
  [[nodiscard]] std::filesystem::path expected_worker_path(
    device_kind_e kind,
    std::uint32_t slot
  );
  [[nodiscard]] std::string expected_phys(
    std::string_view input_seat,
    device_kind_e kind,
    std::uint32_t slot
  );
  /**
   * Exact uinput name used for udev selection and generation readback.
   *
   * The opaque input-seat value is represented by a SHA-256 prefix so the
   * resulting name remains within Linux's 79-byte uinput name limit. The
   * digest is a namespace label, not an authentication credential.
   */
  [[nodiscard]] std::string expected_kernel_name(
    std::string_view input_seat,
    device_kind_e kind,
    std::uint32_t slot
  );
  [[nodiscard]] bool valid_allocation(
    const allocation_t &allocation,
    const expectation_t &expectation
  );

  /**
   * Serializes exact-generation input lifecycle and routing.
   *
   * Admission starts closed and reopens only after an authoritative inventory
   * pass. Ambiguous inventory or an indeterminate create/destroy/route result
   * closes admission until another clean reconciliation.
   */
  class authority_t {
  public:
    explicit authority_t(backend_t &backend);

    [[nodiscard]] prepare_result_t prepare(const expectation_t &expectation);
    [[nodiscard]] status_e release(const seat_handle_t &handle);
    [[nodiscard]] status_e route(
      const seat_handle_t &handle,
      std::uint64_t sequence,
      std::span<const std::uint8_t> payload
    );
    [[nodiscard]] reconciliation_report_t reconcile(
      const std::vector<expectation_t> &expected
    );

    [[nodiscard]] std::optional<allocation_t> allocation(
      const seat_handle_t &handle
    ) const;
    [[nodiscard]] std::vector<allocation_t> allocations() const;
    /** Attempt every active teardown without allocating a snapshot vector. */
    [[nodiscard]] cleanup_report_t release_all();
    [[nodiscard]] bool admission_ready() const;

  private:
    struct active_t {
      allocation_t allocation;
      std::uint64_t last_sequence = 0;
    };

    [[nodiscard]] std::vector<active_t>::iterator find_exact_locked(
      const seat_handle_t &handle
    );
    [[nodiscard]] std::vector<active_t>::const_iterator find_exact_locked(
      const seat_handle_t &handle
    ) const;
    [[nodiscard]] status_e missing_status_locked(const seat_handle_t &handle) const;
    [[nodiscard]] bool collides_locked(const allocation_t &allocation) const;

    backend_t &backend_;
    mutable std::mutex mutex_;
    std::vector<active_t> active_;
    bool admission_ready_ = false;
  };

}  // namespace multiseat::input

#endif
