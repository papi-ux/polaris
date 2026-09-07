/**
 * @file src/platform/linux/multiseat_input_authority.cpp
 * @brief Generation-fenced host authority for multiseat virtual input.
 */
#include "multiseat_input_authority.h"

#ifdef __linux__

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace multiseat::input {
  namespace {
    constexpr std::string_view input_phys_prefix =
      "polaris/client-input-seat-isolated/";
    constexpr std::string_view gamepad_phys_prefix =
      "polaris/client-gamepad-seat-isolated/";

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool valid_name_token(std::string_view value, std::size_t maximum = 128) {
      return !value.empty() && value.size() <= maximum &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) || character == '-' ||
                        character == '_' || character == '.';
               }
             );
    }

    std::string_view kind_name(device_kind_e kind) {
      switch (kind) {
        case device_kind_e::keyboard:
          return "keyboard";
        case device_kind_e::mouse_relative:
        case device_kind_e::mouse_absolute:
          return "mouse";
        case device_kind_e::touch:
          return "touch";
        case device_kind_e::pen:
          return "pen";
        case device_kind_e::gamepad:
          return "gamepad";
      }
      return {};
    }

    std::string_view kernel_role_name(device_kind_e kind) {
      switch (kind) {
        case device_kind_e::keyboard:
          return "keyboard";
        case device_kind_e::mouse_relative:
          return "mouse";
        case device_kind_e::mouse_absolute:
          return "mouse (absolute)";
        case device_kind_e::touch:
          return "touch";
        case device_kind_e::pen:
          return "pen";
        case device_kind_e::gamepad:
          return "gamepad";
      }
      return {};
    }

    std::optional<std::uint32_t> canonical_event_number(
      const std::filesystem::path &path
    ) {
      const auto value = path.native();
      constexpr std::string_view prefix = "/dev/input/event";
      if (value.size() <= prefix.size() ||
          std::string_view {value}.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
      }
      const auto suffix = std::string_view {value}.substr(prefix.size());
      if (suffix.size() > 1 && suffix.front() == '0') {
        return std::nullopt;
      }
      std::uint32_t parsed = 0;
      const auto result = std::from_chars(
        suffix.data(),
        suffix.data() + suffix.size(),
        parsed
      );
      if (result.ec != std::errc {} ||
          result.ptr != suffix.data() + suffix.size() ||
          parsed > std::numeric_limits<std::uint32_t>::max() - 64) {
        return std::nullopt;
      }
      return parsed;
    }

    std::optional<std::string> input_seat_digest_prefix(
      std::string_view input_seat
    ) {
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
      unsigned int digest_size = 0;
      if (EVP_Digest(
            input_seat.data(),
            input_seat.size(),
            digest.data(),
            &digest_size,
            EVP_sha256(),
            nullptr
          ) != 1 ||
          digest_size < 16) {
        return std::nullopt;
      }
      constexpr std::array<char, 16> hex {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
      };
      std::string result;
      result.reserve(32);
      for (std::size_t index = 0; index < 16; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 0x0f]);
      }
      return result;
    }

    std::vector<std::pair<device_kind_e, std::uint32_t>> expected_nodes(
      const plan_t &plan
    ) {
      std::vector<std::pair<device_kind_e, std::uint32_t>> nodes {
        {device_kind_e::keyboard, 0},
        {device_kind_e::mouse_relative, 0},
        {device_kind_e::mouse_absolute, 0},
      };
      if (plan.touch) {
        nodes.emplace_back(device_kind_e::touch, 0);
      }
      if (plan.pen) {
        nodes.emplace_back(device_kind_e::pen, 0);
      }
      for (std::uint32_t slot = 0; slot < plan.gamepad_slots; ++slot) {
        nodes.emplace_back(device_kind_e::gamepad, slot);
      }
      return nodes;
    }

    bool same_slot(const seat_handle_t &left, const seat_handle_t &right) {
      return left.logical_gpu_id == right.logical_gpu_id && left.slot == right.slot;
    }

    bool resources_collide(const allocation_t &left, const allocation_t &right) {
      if (left.input_seat == right.input_seat) {
        return true;
      }
      for (const auto &left_node : left.nodes) {
        for (const auto &right_node : right.nodes) {
          if (left_node.host_path == right_node.host_path ||
              left_node.kernel_name == right_node.kernel_name ||
              std::tie(left_node.filesystem_device, left_node.inode) ==
                std::tie(right_node.filesystem_device, right_node.inode) ||
              std::tie(left_node.character_major, left_node.character_minor) ==
                std::tie(right_node.character_major, right_node.character_minor)) {
            return true;
          }
        }
      }
      return false;
    }

    bool valid_expectations(const std::vector<expectation_t> &expected) {
      if (expected.size() > maximum_input_allocations) {
        return false;
      }
      for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!expected[index].handle.valid() ||
            !valid_name_token(expected[index].input_seat) ||
            !valid_plan(expected[index].plan)) {
          return false;
        }
        for (std::size_t other = index + 1; other < expected.size(); ++other) {
          if (expected[index].handle == expected[other].handle ||
              expected[index].input_seat == expected[other].input_seat ||
              same_slot(expected[index].handle, expected[other].handle)) {
            return false;
          }
        }
      }
      return true;
    }

    const expectation_t *find_expectation(
      const std::vector<expectation_t> &expected,
      const allocation_t &allocation
    ) {
      const auto found = std::find_if(
        expected.begin(),
        expected.end(),
        [&allocation](const auto &candidate) {
          return candidate.handle == allocation.handle;
        }
      );
      return found == expected.end() ? nullptr : &*found;
    }

    bool valid_inventory(const std::vector<allocation_t> &inventory) {
      if (inventory.size() > maximum_input_allocations) {
        return false;
      }
      for (std::size_t index = 0; index < inventory.size(); ++index) {
        const expectation_t self {
          .handle = inventory[index].handle,
          .input_seat = inventory[index].input_seat,
          .plan = inventory[index].plan,
        };
        if (!valid_allocation(inventory[index], self)) {
          return false;
        }
        for (std::size_t other = index + 1; other < inventory.size(); ++other) {
          if (inventory[index].handle == inventory[other].handle ||
              same_slot(inventory[index].handle, inventory[other].handle) ||
              resources_collide(inventory[index], inventory[other])) {
            return false;
          }
        }
      }
      return true;
    }
  }  // namespace

  bool valid_plan(const plan_t &plan) {
    return plan.gamepad_slots <= maximum_gamepad_slots;
  }

  std::filesystem::path expected_worker_path(
    device_kind_e kind,
    std::uint32_t slot
  ) {
    switch (kind) {
      case device_kind_e::keyboard:
        return slot == 0 ?
                 std::filesystem::path {"/dev/input/polaris-keyboard"} :
                 std::filesystem::path {};
      case device_kind_e::mouse_relative:
        return slot == 0 ?
                 std::filesystem::path {"/dev/input/polaris-mouse-relative"} :
                 std::filesystem::path {};
      case device_kind_e::mouse_absolute:
        return slot == 0 ?
                 std::filesystem::path {"/dev/input/polaris-mouse-absolute"} :
                 std::filesystem::path {};
      case device_kind_e::touch:
        return slot == 0 ?
                 std::filesystem::path {"/dev/input/polaris-touch"} :
                 std::filesystem::path {};
      case device_kind_e::pen:
        return slot == 0 ?
                 std::filesystem::path {"/dev/input/polaris-pen"} :
                 std::filesystem::path {};
      case device_kind_e::gamepad:
        if (slot < maximum_gamepad_slots) {
          return "/dev/input/polaris-gamepad-" + std::to_string(slot);
        }
        return {};
    }
    return {};
  }

  std::string expected_phys(
    std::string_view input_seat,
    device_kind_e kind,
    std::uint32_t slot
  ) {
    if (!valid_name_token(input_seat) || expected_worker_path(kind, slot).empty()) {
      return {};
    }
    const auto prefix = kind == device_kind_e::gamepad ?
                          gamepad_phys_prefix : input_phys_prefix;
    auto result = std::string {prefix} + std::string {input_seat} + "/" +
                  std::string {kind_name(kind)};
    if (kind == device_kind_e::gamepad) {
      result += "/" + std::to_string(slot);
    }
    return result;
  }

  std::string expected_kernel_name(
    std::string_view input_seat,
    device_kind_e kind,
    std::uint32_t slot
  ) {
    if (!valid_name_token(input_seat) || expected_worker_path(kind, slot).empty()) {
      return {};
    }
    const auto digest = input_seat_digest_prefix(input_seat);
    const auto role = kernel_role_name(kind);
    if (!digest || role.empty()) {
      return {};
    }
    auto result = std::string {multiseat_kernel_device_prefix} + *digest + " " +
                  std::string {role};
    if (kind == device_kind_e::gamepad) {
      result += "-" + std::to_string(slot);
    }
    return result.size() <= maximum_kernel_device_name_bytes ? result : std::string {};
  }

  bool valid_allocation(
    const allocation_t &allocation,
    const expectation_t &expectation
  ) {
    if (!expectation.handle.valid() ||
        !valid_name_token(expectation.input_seat) ||
        !valid_plan(expectation.plan) ||
        allocation.handle != expectation.handle ||
        allocation.input_seat != expectation.input_seat ||
        allocation.plan != expectation.plan) {
      return false;
    }
    const auto required = expected_nodes(expectation.plan);
    if (allocation.nodes.size() != required.size()) {
      return false;
    }
    std::set<std::filesystem::path> host_paths;
    std::set<std::filesystem::path> worker_paths;
    std::set<std::pair<std::uint64_t, std::uint64_t>> inode_identities;
    std::set<std::pair<std::uint32_t, std::uint32_t>> character_identities;
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto &[expected_kind, expected_slot] = required[index];
      const auto &node = allocation.nodes[index];
      const auto worker_path = expected_worker_path(expected_kind, expected_slot);
      const auto event_number = canonical_event_number(node.host_path);
      const auto expected_node_phys = expected_phys(
        expectation.input_seat,
        expected_kind,
        expected_slot
      );
      if (node.kind != expected_kind || node.slot != expected_slot ||
          !event_number ||
          node.worker_path != worker_path ||
          node.filesystem_device == 0 || node.inode == 0 ||
          node.character_major != 13 ||
          node.character_minor != 64 + *event_number ||
          node.kernel_name != expected_kernel_name(
            expectation.input_seat,
            expected_kind,
            expected_slot
          ) ||
          (!node.phys.empty() && node.phys != expected_node_phys) ||
          node.host_seat != isolated_host_seat ||
          !host_paths.emplace(node.host_path).second ||
          !worker_paths.emplace(node.worker_path).second ||
          !inode_identities.emplace(node.filesystem_device, node.inode).second ||
          !character_identities.emplace(
            node.character_major,
            node.character_minor
          ).second) {
        return false;
      }
    }
    return true;
  }

  authority_t::authority_t(backend_t &backend) :
      backend_(backend) {
  }

  std::vector<authority_t::active_t>::iterator authority_t::find_exact_locked(
    const seat_handle_t &handle
  ) {
    return std::find_if(
      active_.begin(),
      active_.end(),
      [&handle](const auto &candidate) {
        return candidate.allocation.handle == handle;
      }
    );
  }

  std::vector<authority_t::active_t>::const_iterator authority_t::find_exact_locked(
    const seat_handle_t &handle
  ) const {
    return std::find_if(
      active_.begin(),
      active_.end(),
      [&handle](const auto &candidate) {
        return candidate.allocation.handle == handle;
      }
    );
  }

  status_e authority_t::missing_status_locked(const seat_handle_t &handle) const {
    const auto related = std::find_if(
      active_.begin(),
      active_.end(),
      [&handle](const auto &candidate) {
        return same_slot(candidate.allocation.handle, handle);
      }
    );
    return related == active_.end() ? status_e::not_found : status_e::stale_authority;
  }

  bool authority_t::collides_locked(const allocation_t &allocation) const {
    return std::any_of(
      active_.begin(),
      active_.end(),
      [&allocation](const auto &candidate) {
        return resources_collide(candidate.allocation, allocation);
      }
    );
  }

  prepare_result_t authority_t::prepare(const expectation_t &expectation) {
    std::scoped_lock lock {mutex_};
    if (!admission_ready_) {
      return {
        .status = status_e::reconciliation_required,
        .allocation = std::nullopt,
      };
    }
    if (!expectation.handle.valid() ||
        !valid_name_token(expectation.input_seat) ||
        !valid_plan(expectation.plan)) {
      return {
        .status = status_e::invalid_request,
        .allocation = std::nullopt,
      };
    }
    const auto existing = find_exact_locked(expectation.handle);
    if (existing != active_.end()) {
      if (existing->allocation.input_seat != expectation.input_seat ||
          existing->allocation.plan != expectation.plan) {
        return {
          .status = status_e::invalid_request,
          .allocation = std::nullopt,
        };
      }
      return {
        .status = status_e::already_applied,
        .allocation = existing->allocation,
      };
    }
    if (std::any_of(
          active_.begin(),
          active_.end(),
          [&expectation](const auto &candidate) {
            return same_slot(candidate.allocation.handle, expectation.handle);
          }
        )) {
      return {
        .status = status_e::stale_authority,
        .allocation = std::nullopt,
      };
    }

    backend_create_result_t created;
    try {
      created = backend_.create(expectation);
    } catch (...) {
      admission_ready_ = false;
      return {
        .status = status_e::backend_indeterminate,
        .allocation = std::nullopt,
      };
    }
    switch (created.result) {
      case backend_result_e::not_found:
      case backend_result_e::rejected:
        return {
          .status = status_e::backend_rejected,
          .allocation = std::nullopt,
        };
      case backend_result_e::indeterminate:
        admission_ready_ = false;
        return {
          .status = status_e::backend_indeterminate,
          .allocation = std::nullopt,
        };
      case backend_result_e::applied:
      case backend_result_e::already_applied:
        break;
    }
    if (!created.allocation ||
        !valid_allocation(*created.allocation, expectation) ||
        collides_locked(*created.allocation)) {
      try {
        (void) backend_.destroy(expectation.handle, expectation.input_seat);
      } catch (...) {
      }
      admission_ready_ = false;
      return {
        .status = status_e::backend_protocol_error,
        .allocation = std::nullopt,
      };
    }
    active_.push_back({.allocation = *created.allocation});
    return {
      .status = created.result == backend_result_e::already_applied ?
                  status_e::already_applied : status_e::applied,
      .allocation = std::move(created.allocation),
    };
  }

  status_e authority_t::release(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    if (!handle.valid()) {
      return status_e::invalid_request;
    }
    const auto existing = find_exact_locked(handle);
    if (existing == active_.end()) {
      return missing_status_locked(handle);
    }
    backend_result_e result;
    try {
      result = backend_.destroy(handle, existing->allocation.input_seat);
    } catch (...) {
      result = backend_result_e::indeterminate;
    }
    switch (result) {
      case backend_result_e::applied:
      case backend_result_e::already_applied:
      case backend_result_e::not_found:
        active_.erase(existing);
        return status_e::applied;
      case backend_result_e::rejected:
        admission_ready_ = false;
        return status_e::backend_rejected;
      case backend_result_e::indeterminate:
        admission_ready_ = false;
        return status_e::backend_indeterminate;
    }
    admission_ready_ = false;
    return status_e::backend_indeterminate;
  }

  status_e authority_t::route(
    const seat_handle_t &handle,
    std::uint64_t sequence,
    std::span<const std::uint8_t> payload
  ) {
    std::scoped_lock lock {mutex_};
    if (!admission_ready_) {
      return status_e::reconciliation_required;
    }
    if (!handle.valid() || sequence == 0 || payload.empty() ||
        payload.size() > maximum_input_payload_bytes) {
      return status_e::invalid_request;
    }
    const auto event = decode_input_event(payload);
    if (!event) {
      return status_e::invalid_request;
    }
    const auto existing = find_exact_locked(handle);
    if (existing == active_.end()) {
      return missing_status_locked(handle);
    }
    if (existing->last_sequence == std::numeric_limits<std::uint64_t>::max() ||
        sequence != existing->last_sequence + 1) {
      return status_e::invalid_request;
    }
    backend_result_e result;
    try {
      result = backend_.route(
        handle,
        existing->allocation.input_seat,
        sequence,
        *event
      );
    } catch (...) {
      result = backend_result_e::indeterminate;
    }
    switch (result) {
      case backend_result_e::applied:
      case backend_result_e::already_applied:
        existing->last_sequence = sequence;
        return status_e::applied;
      case backend_result_e::not_found:
      case backend_result_e::rejected:
        admission_ready_ = false;
        return status_e::backend_rejected;
      case backend_result_e::indeterminate:
        admission_ready_ = false;
        return status_e::backend_indeterminate;
    }
    admission_ready_ = false;
    return status_e::backend_indeterminate;
  }

  reconciliation_report_t authority_t::reconcile(
    const std::vector<expectation_t> &expected
  ) {
    std::scoped_lock lock {mutex_};
    reconciliation_report_t report {.expected = expected.size()};
    admission_ready_ = false;
    if (!valid_expectations(expected)) {
      ++report.protocol_errors;
      return report;
    }

    std::vector<allocation_t> observed;
    try {
      observed = backend_.inventory();
    } catch (...) {
      ++report.backend_failures;
      return report;
    }
    report.observations = observed.size();
    if (!valid_inventory(observed)) {
      ++report.protocol_errors;
      return report;
    }
    report.inventory_authoritative = true;

    bool removed_any = false;
    for (const auto &candidate : observed) {
      const auto *wanted = find_expectation(expected, candidate);
      if (wanted && valid_allocation(candidate, *wanted)) {
        continue;
      }
      if (wanted) {
        ++report.protocol_errors;
        return report;
      }
      ++report.orphans;
      report.inventory_authoritative = false;
      backend_result_e result;
      try {
        result = backend_.destroy(candidate.handle, candidate.input_seat);
      } catch (...) {
        result = backend_result_e::indeterminate;
      }
      if (result != backend_result_e::applied &&
          result != backend_result_e::already_applied &&
          result != backend_result_e::not_found) {
        ++report.backend_failures;
        return report;
      }
      ++report.removed_orphans;
      removed_any = true;
    }

    if (removed_any) {
      try {
        observed = backend_.inventory();
      } catch (...) {
        ++report.backend_failures;
        return report;
      }
      if (!valid_inventory(observed)) {
        ++report.protocol_errors;
        return report;
      }
      report.inventory_authoritative = true;
    }

    std::vector<active_t> reconciled;
    reconciled.reserve(expected.size());
    for (const auto &wanted : expected) {
      const auto found = std::find_if(
        observed.begin(),
        observed.end(),
        [&wanted](const auto &candidate) {
          return candidate.handle == wanted.handle;
        }
      );
      if (found == observed.end()) {
        ++report.missing;
        continue;
      }
      if (!valid_allocation(*found, wanted)) {
        ++report.protocol_errors;
        return report;
      }
      ++report.current;
      std::uint64_t prior_sequence = 0;
      const auto prior = find_exact_locked(found->handle);
      if (prior != active_.end() && prior->allocation == *found) {
        prior_sequence = prior->last_sequence;
      }
      reconciled.push_back({
        .allocation = *found,
        .last_sequence = prior_sequence,
      });
    }
    if (report.missing != 0) {
      return report;
    }
    if (observed.size() != expected.size()) {
      ++report.protocol_errors;
      return report;
    }
    active_ = std::move(reconciled);
    admission_ready_ = true;
    report.admission_ready = true;
    return report;
  }

  std::optional<allocation_t> authority_t::allocation(
    const seat_handle_t &handle
  ) const {
    std::scoped_lock lock {mutex_};
    const auto found = find_exact_locked(handle);
    return found == active_.end() ?
             std::nullopt : std::optional<allocation_t> {found->allocation};
  }

  std::vector<allocation_t> authority_t::allocations() const {
    std::scoped_lock lock {mutex_};
    std::vector<allocation_t> result;
    result.reserve(active_.size());
    for (const auto &active : active_) {
      result.push_back(active.allocation);
    }
    return result;
  }

  cleanup_report_t authority_t::release_all() {
    std::scoped_lock lock {mutex_};
    cleanup_report_t report;
    for (auto active = active_.begin(); active != active_.end();) {
      backend_result_e result;
      try {
        result = backend_.destroy(
          active->allocation.handle,
          active->allocation.input_seat
        );
      } catch (...) {
        result = backend_result_e::indeterminate;
      }
      switch (result) {
        case backend_result_e::applied:
        case backend_result_e::already_applied:
        case backend_result_e::not_found:
          active = active_.erase(active);
          ++report.released_allocations;
          break;
        case backend_result_e::rejected:
        case backend_result_e::indeterminate:
          admission_ready_ = false;
          ++report.cleanup_failures;
          ++active;
          break;
        default:
          admission_ready_ = false;
          ++report.cleanup_failures;
          ++active;
          break;
      }
    }
    return report;
  }

  bool authority_t::admission_ready() const {
    std::scoped_lock lock {mutex_};
    return admission_ready_;
  }

}  // namespace multiseat::input

#endif
