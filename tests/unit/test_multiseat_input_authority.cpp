/**
 * @file tests/unit/test_multiseat_input_authority.cpp
 * @brief Offline contract tests for host-brokered multiseat input authority.
 */
#include "src/platform/linux/multiseat_input_authority.h"

#include <gtest/gtest.h>
#include <limits>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using multiseat::input::allocation_t;
  using multiseat::input::authority_t;
  using multiseat::input::backend_create_result_t;
  using multiseat::input::backend_result_e;
  using multiseat::input::backend_t;
  using multiseat::input::device_kind_e;
  using multiseat::input::device_node_t;
  using multiseat::input::expectation_t;
  using multiseat::input::input_event_t;
  using multiseat::input::maximum_gamepad_slots;
  using multiseat::input::maximum_input_allocations;
  using multiseat::input::maximum_input_payload_bytes;
  using multiseat::input::plan_t;
  using multiseat::input::status_e;
  using multiseat::seat_handle_t;

  seat_handle_t handle_for(
    std::uint32_t slot,
    std::uint64_t generation,
    std::string controller = "controller-a"
  ) {
    return {
      .controller_epoch = std::move(controller),
      .logical_gpu_id = "gpu-primary",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t expectation_for(
    std::uint32_t slot,
    std::uint64_t generation,
    plan_t plan = {}
  ) {
    return {
      .handle = handle_for(slot, generation),
      .input_seat = "polaris-input-controller-a-" + std::to_string(generation),
      .plan = plan,
    };
  }

  std::vector<std::pair<device_kind_e, std::uint32_t>> nodes_for_plan(
    const plan_t &plan
  ) {
    std::vector<std::pair<device_kind_e, std::uint32_t>> result {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
    };
    if (plan.touch) {
      result.emplace_back(device_kind_e::touch, 0);
    }
    if (plan.pen) {
      result.emplace_back(device_kind_e::pen, 0);
    }
    for (std::uint32_t slot = 0; slot < plan.gamepad_slots; ++slot) {
      result.emplace_back(device_kind_e::gamepad, slot);
    }
    return result;
  }

  allocation_t allocation_for(
    const expectation_t &expectation,
    std::uint32_t first_minor
  ) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
      .nodes = {},
    };
    auto minor = first_minor;
    for (const auto &[kind, slot] : nodes_for_plan(expectation.plan)) {
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(minor < 96 ? minor - 64 : minor),
        .worker_path = multiseat::input::expected_worker_path(kind, slot),
        .filesystem_device = 41,
        .inode = 10000 + minor,
        .character_major = 13,
        .character_minor = minor,
        .kernel_name = multiseat::input::expected_kernel_name(
          expectation.input_seat,
          kind,
          slot
        ),
        .phys = multiseat::input::expected_phys(expectation.input_seat, kind, slot),
        .host_seat = std::string {multiseat::input::isolated_host_seat},
      });
      ++minor;
    }
    return allocation;
  }

  class fake_backend_t final : public backend_t {
  public:
    struct route_call_t {
      seat_handle_t handle;
      std::string input_seat;
      std::uint64_t sequence = 0;
      input_event_t event;
    };

    backend_create_result_t create(const expectation_t &expectation) override {
      ++create_calls;
      if (throw_create) {
        throw std::runtime_error {"private create detail"};
      }
      if (next_create != backend_result_e::applied &&
          next_create != backend_result_e::already_applied) {
        return {
          .result = next_create,
          .allocation = std::nullopt,
        };
      }
      const auto existing = find(expectation.handle);
      if (existing != allocations.end()) {
        return {
          .result = backend_result_e::already_applied,
          .allocation = *existing,
        };
      }
      auto allocation = allocation_for(expectation, next_minor);
      next_minor += static_cast<std::uint32_t>(allocation.nodes.size()) + 4;
      if (next_minor > 95 && next_minor < 256) {
        // The kernel has no minors between the static evdev range and 256.
        next_minor = 256;
      }
      if (mutate_created) {
        mutate_created(allocation);
      }
      allocations.push_back(allocation);
      return {
        .result = next_create,
        .allocation = std::move(allocation),
      };
    }

    backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override {
      destroyed.emplace_back(handle, input_seat);
      if (throw_destroy ||
          (throw_destroy_for && *throw_destroy_for == handle)) {
        throw std::runtime_error {"private destroy detail"};
      }
      if (next_destroy != backend_result_e::applied &&
          next_destroy != backend_result_e::already_applied &&
          next_destroy != backend_result_e::not_found) {
        return next_destroy;
      }
      const auto existing = std::find_if(
        allocations.begin(),
        allocations.end(),
        [&handle, input_seat](const auto &candidate) {
          return candidate.handle == handle && candidate.input_seat == input_seat;
        }
      );
      if (existing == allocations.end()) {
        return backend_result_e::not_found;
      }
      if (!retain_destroyed) {
        allocations.erase(existing);
      }
      return next_destroy == backend_result_e::not_found ?
               backend_result_e::not_found : next_destroy;
    }

    backend_result_e route(
      const seat_handle_t &handle,
      std::string_view input_seat,
      std::uint64_t sequence,
      const input_event_t &event
    ) override {
      if (throw_route) {
        throw std::runtime_error {"private route detail"};
      }
      routes.push_back({
        .handle = handle,
        .input_seat = std::string {input_seat},
        .sequence = sequence,
        .event = event,
      });
      return next_route;
    }

    std::vector<allocation_t> inventory() override {
      ++inventory_calls;
      if (throw_inventory) {
        throw std::runtime_error {"private inventory detail"};
      }
      return allocations;
    }

    void inject(allocation_t allocation) {
      allocations.push_back(std::move(allocation));
    }

    std::vector<allocation_t>::iterator find(const seat_handle_t &handle) {
      return std::find_if(
        allocations.begin(),
        allocations.end(),
        [&handle](const auto &candidate) {
          return candidate.handle == handle;
        }
      );
    }

    std::vector<allocation_t> allocations;
    std::vector<std::pair<seat_handle_t, std::string>> destroyed;
    std::vector<route_call_t> routes;
    std::function<void(allocation_t &)> mutate_created;
    backend_result_e next_create = backend_result_e::applied;
    backend_result_e next_destroy = backend_result_e::applied;
    backend_result_e next_route = backend_result_e::applied;
    std::uint32_t next_minor = 64;
    std::size_t create_calls = 0;
    std::size_t inventory_calls = 0;
    std::optional<seat_handle_t> throw_destroy_for;
    bool throw_create = false;
    bool throw_destroy = false;
    bool throw_route = false;
    bool throw_inventory = false;
    bool retain_destroyed = false;
  };

  void open_admission(authority_t &authority) {
    const auto report = authority.reconcile({});
    ASSERT_TRUE(report.inventory_authoritative);
    ASSERT_TRUE(report.admission_ready);
    ASSERT_TRUE(authority.admission_ready());
  }

  TEST(MultiseatInputAuthority, CanonicalManifestRejectsCreationEndpointsAndAmbiguity) {
    const auto expectation = expectation_for(
      0,
      1,
      {.touch = true, .pen = true, .gamepad_slots = 2}
    );
    const auto canonical = allocation_for(expectation, 64);
    EXPECT_TRUE(multiseat::input::valid_plan(expectation.plan));
    EXPECT_TRUE(multiseat::input::valid_allocation(canonical, expectation));
    EXPECT_EQ(canonical.nodes.size(), 7U);
    for (const auto &node : canonical.nodes) {
      EXPECT_EQ(node.host_path.native().find("/dev/input/event"), 0U);
      EXPECT_NE(node.host_path, "/dev/uinput");
      EXPECT_NE(node.host_path, "/dev/uhid");
    }

    auto invalid = canonical;
    invalid.nodes[0].host_path = "/dev/uinput";
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[1].host_path = invalid.nodes[0].host_path;
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].phys = "unisolated";
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].phys.clear();
    EXPECT_TRUE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].kernel_name = "Polaris multiseat wrong-generation keyboard";
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].host_seat = "seat0";
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].character_major = 10;
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].character_minor += 1;
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    invalid = canonical;
    invalid.nodes[0].host_path = "/dev/input/event00";
    EXPECT_FALSE(multiseat::input::valid_allocation(invalid, expectation));
    EXPECT_FALSE(multiseat::input::valid_plan({
      .gamepad_slots = maximum_gamepad_slots + 1,
    }));
  }

  TEST(MultiseatInputAuthority, ReconciliationGatesPreparationAndPrepareIsIdempotent) {
    fake_backend_t backend;
    authority_t authority {backend};
    const auto expectation = expectation_for(0, 1);

    EXPECT_EQ(
      authority.prepare(expectation).status,
      status_e::reconciliation_required
    );
    EXPECT_EQ(backend.create_calls, 0U);
    open_admission(authority);

    const auto prepared = authority.prepare(expectation);
    ASSERT_TRUE(prepared.prepared());
    EXPECT_EQ(prepared.status, status_e::applied);
    ASSERT_TRUE(prepared.allocation.has_value());
    EXPECT_TRUE(multiseat::input::valid_allocation(*prepared.allocation, expectation));
    EXPECT_EQ(backend.create_calls, 1U);

    const auto repeated = authority.prepare(expectation);
    ASSERT_TRUE(repeated.prepared());
    EXPECT_EQ(repeated.status, status_e::already_applied);
    EXPECT_EQ(repeated.allocation, prepared.allocation);
    EXPECT_EQ(backend.create_calls, 1U);
  }

  TEST(MultiseatInputAuthority, TwoSeatsRouteStrictSequencesWithoutCrossing) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto first = expectation_for(0, 1, {.gamepad_slots = 2});
    const auto second = expectation_for(1, 2, {.touch = true, .gamepad_slots = 1});
    ASSERT_TRUE(authority.prepare(first).prepared());
    ASSERT_TRUE(authority.prepare(second).prepared());

    const auto first_event = input_event_t {
      .slot = 1,
      .payload = multiseat::input::gamepad_state_event_t {
        .buttons = 0x1000,
        .left_trigger = 12,
        .right_trigger = 34,
        .left_stick_x = -200,
        .left_stick_y = 300,
        .right_stick_x = 400,
        .right_stick_y = -500,
      },
    };
    const auto second_event = input_event_t {
      .payload = multiseat::input::touch_contact_event_t {
        .pointer_id = 7,
        .action = multiseat::input::touch_action_e::down,
        .orientation_degrees = -30,
        .x = 1000,
        .y = 2000,
        .pressure = 3000,
      },
    };
    const auto followup_event = input_event_t {
      .payload = multiseat::input::mouse_relative_event_t {
        .delta_x = 4,
        .delta_y = -5,
      },
    };
    const auto first_payload = multiseat::input::encode_input_event(first_event);
    const auto second_payload = multiseat::input::encode_input_event(second_event);
    const auto followup_payload = multiseat::input::encode_input_event(followup_event);
    EXPECT_EQ(authority.route(first.handle, 1, first_payload), status_e::applied);
    EXPECT_EQ(authority.route(second.handle, 1, second_payload), status_e::applied);
    EXPECT_EQ(authority.route(first.handle, 3, first_payload), status_e::invalid_request);
    auto malformed = followup_payload;
    malformed[5] = 1;
    EXPECT_EQ(authority.route(first.handle, 2, malformed), status_e::invalid_request);
    EXPECT_EQ(authority.route(first.handle, 2, followup_payload), status_e::applied);
    EXPECT_EQ(
      authority.route(handle_for(0, 99), 1, first_payload),
      status_e::stale_authority
    );

    std::vector<std::uint8_t> oversized(maximum_input_payload_bytes + 1, 1);
    EXPECT_EQ(authority.route(second.handle, 2, oversized), status_e::invalid_request);
    ASSERT_EQ(backend.routes.size(), 3U);
    EXPECT_EQ(backend.routes[0].handle, first.handle);
    EXPECT_EQ(backend.routes[0].event, first_event);
    EXPECT_EQ(backend.routes[1].handle, second.handle);
    EXPECT_EQ(backend.routes[1].event, second_event);
    EXPECT_EQ(backend.routes[2].handle, first.handle);
    EXPECT_EQ(backend.routes[2].sequence, 2U);
    EXPECT_EQ(backend.routes[2].event, followup_event);
  }

  TEST(MultiseatInputAuthority, CollidingOrMalformedCreateClosesAdmissionAndRollsBack) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto first = expectation_for(0, 1);
    const auto second = expectation_for(1, 2);
    const auto first_result = authority.prepare(first);
    ASSERT_TRUE(first_result.prepared());
    ASSERT_TRUE(first_result.allocation.has_value());
    const auto first_node = first_result.allocation->nodes.front();

    backend.mutate_created = [first_node](allocation_t &allocation) {
      allocation.nodes.front().host_path = first_node.host_path;
      allocation.nodes.front().filesystem_device = first_node.filesystem_device;
      allocation.nodes.front().inode = first_node.inode;
      allocation.nodes.front().character_major = first_node.character_major;
      allocation.nodes.front().character_minor = first_node.character_minor;
    };
    const auto rejected = authority.prepare(second);
    EXPECT_EQ(rejected.status, status_e::backend_protocol_error);
    EXPECT_FALSE(rejected.allocation.has_value());
    EXPECT_FALSE(authority.admission_ready());
    ASSERT_FALSE(backend.destroyed.empty());
    EXPECT_EQ(backend.destroyed.back().first, second.handle);
    EXPECT_EQ(backend.allocations.size(), 1U);
  }

  TEST(MultiseatInputAuthority, BackendOutcomesFailClosedWithoutLosingExactLease) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto rejected = expectation_for(0, 1);
    backend.next_create = backend_result_e::rejected;
    EXPECT_EQ(authority.prepare(rejected).status, status_e::backend_rejected);
    EXPECT_TRUE(authority.admission_ready());

    backend.next_create = backend_result_e::indeterminate;
    EXPECT_EQ(
      authority.prepare(expectation_for(1, 2)).status,
      status_e::backend_indeterminate
    );
    EXPECT_FALSE(authority.admission_ready());

    backend.next_create = backend_result_e::applied;
    const auto reset = authority.reconcile({});
    ASSERT_TRUE(reset.admission_ready);
    const auto prepared = authority.prepare(rejected);
    ASSERT_TRUE(prepared.prepared());
    backend.next_route = backend_result_e::indeterminate;
    const auto payload = multiseat::input::encode_input_event({
      .payload = multiseat::input::keyboard_key_event_t {
        .key_code = 0x41,
        .state = multiseat::input::button_state_e::pressed,
      },
    });
    EXPECT_EQ(
      authority.route(rejected.handle, 1, payload),
      status_e::backend_indeterminate
    );
    EXPECT_FALSE(authority.admission_ready());
    EXPECT_TRUE(authority.allocation(rejected.handle).has_value());
  }

  TEST(MultiseatInputAuthority, ReleaseIsExactAndRetainsIndeterminateAllocation) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto expectation = expectation_for(0, 1);
    ASSERT_TRUE(authority.prepare(expectation).prepared());

    EXPECT_EQ(authority.release(handle_for(0, 2)), status_e::stale_authority);
    EXPECT_TRUE(authority.allocation(expectation.handle).has_value());
    backend.next_destroy = backend_result_e::indeterminate;
    EXPECT_EQ(authority.release(expectation.handle), status_e::backend_indeterminate);
    EXPECT_TRUE(authority.allocation(expectation.handle).has_value());
    EXPECT_FALSE(authority.admission_ready());

    backend.next_destroy = backend_result_e::applied;
    ASSERT_TRUE(authority.reconcile({expectation}).admission_ready);
    EXPECT_EQ(authority.release(expectation.handle), status_e::applied);
    EXPECT_FALSE(authority.allocation(expectation.handle).has_value());
    EXPECT_EQ(authority.release(expectation.handle), status_e::not_found);
  }

  TEST(MultiseatInputAuthority, KernelMinorMappingCoversStaticAndDynamicRanges) {
    using multiseat::input::expected_event_minor;
    using multiseat::input::expected_joystick_minor;
    EXPECT_EQ(expected_event_minor(0), 64U);
    EXPECT_EQ(expected_event_minor(31), 95U);
    EXPECT_FALSE(expected_event_minor(32).has_value());
    EXPECT_FALSE(expected_event_minor(255).has_value());
    EXPECT_EQ(expected_event_minor(256), 256U);
    EXPECT_EQ(expected_event_minor(258), 258U);
    EXPECT_EQ(
      expected_event_minor(std::numeric_limits<std::uint32_t>::max()),
      std::numeric_limits<std::uint32_t>::max()
    );
    EXPECT_FALSE(expected_event_minor(
      std::uint64_t {std::numeric_limits<std::uint32_t>::max()} + 1
    ).has_value());
    EXPECT_EQ(expected_joystick_minor(0), 0U);
    EXPECT_EQ(expected_joystick_minor(15), 15U);
    EXPECT_FALSE(expected_joystick_minor(16).has_value());
    EXPECT_FALSE(expected_joystick_minor(255).has_value());
    EXPECT_EQ(expected_joystick_minor(256), 256U);
  }

  TEST(MultiseatInputAuthority, ValidAllocationAcceptsDynamicMinorsAndRejectsTheGap) {
    const auto expectation = expectation_for(0, 1);
    auto dynamic = allocation_for(expectation, 64);
    ASSERT_TRUE(multiseat::input::valid_allocation(dynamic, expectation));
    for (std::size_t index = 0; index < dynamic.nodes.size(); ++index) {
      auto &node = dynamic.nodes[index];
      node.host_path = "/dev/input/event" + std::to_string(256 + index);
      node.character_minor = static_cast<std::uint32_t>(256 + index);
      node.inode = 20000 + index;
    }
    EXPECT_TRUE(multiseat::input::valid_allocation(dynamic, expectation));

    auto gap = allocation_for(expectation, 64);
    gap.nodes.front().host_path = "/dev/input/event40";
    gap.nodes.front().character_minor = 104;
    EXPECT_FALSE(multiseat::input::valid_allocation(gap, expectation));

    auto shifted = allocation_for(expectation, 64);
    shifted.nodes.front().host_path = "/dev/input/event256";
    shifted.nodes.front().character_minor = 320;
    EXPECT_FALSE(multiseat::input::valid_allocation(shifted, expectation));
  }

  TEST(MultiseatInputAuthority, ReleaseAllAttemptsEveryAllocationAndRetainsFailuresForRetry) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto first = expectation_for(0, 1);
    const auto second = expectation_for(1, 2);
    ASSERT_TRUE(authority.prepare(first).prepared());
    ASSERT_TRUE(authority.prepare(second).prepared());

    backend.throw_destroy_for = first.handle;
    const auto partial = authority.release_all();
    EXPECT_EQ(partial.released_allocations, 1U);
    EXPECT_EQ(partial.cleanup_failures, 1U);
    EXPECT_TRUE(authority.allocation(first.handle).has_value());
    EXPECT_FALSE(authority.allocation(second.handle).has_value());
    ASSERT_EQ(backend.destroyed.size(), 2U);
    EXPECT_EQ(backend.destroyed[0].first, first.handle);
    EXPECT_EQ(backend.destroyed[1].first, second.handle);

    backend.throw_destroy_for.reset();
    const auto retry = authority.release_all();
    EXPECT_EQ(retry.released_allocations, 1U);
    EXPECT_EQ(retry.cleanup_failures, 0U);
    EXPECT_FALSE(authority.allocation(first.handle).has_value());
    EXPECT_EQ(backend.allocations.size(), 0U);
  }

  TEST(MultiseatInputAuthority, ReleaseAllRetainsUnknownBackendResultsWithoutSpinning) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    const auto expectation = expectation_for(0, 1);
    ASSERT_TRUE(authority.prepare(expectation).prepared());

    backend.next_destroy = static_cast<backend_result_e>(0x7f);
    const auto unknown = authority.release_all();
    EXPECT_EQ(unknown.released_allocations, 0U);
    EXPECT_EQ(unknown.cleanup_failures, 1U);
    EXPECT_TRUE(authority.allocation(expectation.handle).has_value());

    backend.next_destroy = backend_result_e::applied;
    const auto retry = authority.release_all();
    EXPECT_EQ(retry.released_allocations, 1U);
    EXPECT_EQ(retry.cleanup_failures, 0U);
    EXPECT_FALSE(authority.allocation(expectation.handle).has_value());
  }

  TEST(MultiseatInputAuthority, ReconcileRetainsCurrentAndRemovesExactOrphan) {
    fake_backend_t backend;
    const auto current = expectation_for(0, 10);
    const auto orphan = expectation_for(1, 9);
    backend.inject(allocation_for(current, 64));
    backend.inject(allocation_for(orphan, 80));
    authority_t authority {backend};

    const auto report = authority.reconcile({current});
    EXPECT_TRUE(report.inventory_authoritative);
    EXPECT_EQ(report.observations, 2U);
    EXPECT_EQ(report.current, 1U);
    EXPECT_EQ(report.orphans, 1U);
    EXPECT_EQ(report.removed_orphans, 1U);
    EXPECT_TRUE(report.admission_ready);
    ASSERT_EQ(backend.destroyed.size(), 1U);
    EXPECT_EQ(backend.destroyed[0].first, orphan.handle);
    EXPECT_TRUE(authority.allocation(current.handle).has_value());
    EXPECT_FALSE(authority.allocation(orphan.handle).has_value());
    EXPECT_GE(backend.inventory_calls, 2U);
  }

  TEST(MultiseatInputAuthority, ReconcileRequiresObservedOrphanRemoval) {
    fake_backend_t backend;
    const auto current = expectation_for(0, 10);
    const auto orphan = expectation_for(1, 9);
    backend.inject(allocation_for(current, 64));
    backend.inject(allocation_for(orphan, 80));
    backend.retain_destroyed = true;
    authority_t authority {backend};

    const auto report = authority.reconcile({current});
    EXPECT_TRUE(report.inventory_authoritative);
    EXPECT_EQ(report.orphans, 1U);
    EXPECT_EQ(report.removed_orphans, 1U);
    EXPECT_EQ(report.current, 1U);
    EXPECT_EQ(report.protocol_errors, 1U);
    EXPECT_FALSE(report.admission_ready);
    EXPECT_FALSE(authority.admission_ready());
    EXPECT_EQ(backend.inventory_calls, 2U);
  }

  TEST(MultiseatInputAuthority, AmbiguousInventoryIsRetainedForInspection) {
    fake_backend_t backend;
    const auto first = expectation_for(0, 1);
    const auto second = expectation_for(1, 2);
    auto first_allocation = allocation_for(first, 64);
    auto second_allocation = allocation_for(second, 80);
    second_allocation.nodes.front().host_path = first_allocation.nodes.front().host_path;
    backend.inject(std::move(first_allocation));
    backend.inject(std::move(second_allocation));
    authority_t authority {backend};

    const auto report = authority.reconcile({first, second});
    EXPECT_EQ(report.protocol_errors, 1U);
    EXPECT_FALSE(report.admission_ready);
    EXPECT_FALSE(authority.admission_ready());
    EXPECT_TRUE(backend.destroyed.empty());
    EXPECT_EQ(backend.allocations.size(), 2U);
  }

  TEST(MultiseatInputAuthority, MissingExpectedAllocationKeepsAdmissionClosed) {
    fake_backend_t backend;
    authority_t authority {backend};
    const auto report = authority.reconcile({expectation_for(0, 1)});
    EXPECT_TRUE(report.inventory_authoritative);
    EXPECT_EQ(report.missing, 1U);
    EXPECT_FALSE(report.admission_ready);
    EXPECT_FALSE(authority.admission_ready());
  }

  TEST(MultiseatInputAuthority, ReconciliationBoundsExpectedAndObservedAllocations) {
    fake_backend_t backend;
    authority_t authority {backend};
    std::vector<expectation_t> expected;
    expected.reserve(maximum_input_allocations + 1);
    for (std::size_t index = 0; index <= maximum_input_allocations; ++index) {
      expected.push_back(expectation_for(
        static_cast<std::uint32_t>(index),
        index + 1
      ));
    }

    auto report = authority.reconcile(expected);
    EXPECT_EQ(report.protocol_errors, 1U);
    EXPECT_FALSE(report.inventory_authoritative);
    EXPECT_EQ(backend.inventory_calls, 0U);

    expected.clear();
    for (std::size_t index = 0; index <= maximum_input_allocations; ++index) {
      const auto expectation = expectation_for(
        static_cast<std::uint32_t>(index),
        index + 1
      );
      backend.inject(allocation_for(
        expectation,
        64 + static_cast<std::uint32_t>(index * 8)
      ));
    }
    report = authority.reconcile(expected);
    EXPECT_EQ(report.observations, maximum_input_allocations + 1);
    EXPECT_EQ(report.protocol_errors, 1U);
    EXPECT_FALSE(report.inventory_authoritative);
    EXPECT_FALSE(report.admission_ready);
  }

  TEST(MultiseatInputAuthority, BackendExceptionsBecomeBoundedIndeterminateState) {
    fake_backend_t backend;
    authority_t authority {backend};
    backend.throw_inventory = true;
    auto report = authority.reconcile({});
    EXPECT_EQ(report.backend_failures, 1U);
    EXPECT_FALSE(report.inventory_authoritative);

    backend.throw_inventory = false;
    open_admission(authority);
    backend.throw_create = true;
    EXPECT_EQ(
      authority.prepare(expectation_for(0, 1)).status,
      status_e::backend_indeterminate
    );
    EXPECT_FALSE(authority.admission_ready());
  }

  TEST(MultiseatInputAuthority, ConcurrentPreparationIsSerializedPerAuthority) {
    fake_backend_t backend;
    authority_t authority {backend};
    open_admission(authority);
    std::vector<std::future<status_e>> futures;
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
      futures.push_back(std::async(std::launch::async, [&authority, slot]() {
        return authority.prepare(expectation_for(slot, slot + 1)).status;
      }));
    }
    for (auto &future : futures) {
      EXPECT_EQ(future.get(), status_e::applied);
    }
    EXPECT_EQ(authority.allocations().size(), 8U);
    EXPECT_EQ(backend.allocations.size(), 8U);
  }
}
