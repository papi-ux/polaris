/**
 * @file tests/unit/platform/test_multiseat_moonlight_activation.cpp
 * @brief Production-boundary tests for default-off multiseat activation.
 */
#include "src/platform/linux/multiseat_moonlight_activation.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat;
  using namespace multiseat::input;

  seat_handle_t handle_for(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .controller_epoch = "controller-activation",
      .logical_gpu_id = "gpu-primary",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t expectation_for(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .handle = handle_for(generation, slot),
      .input_seat = "polaris-input-activation-" +
                    std::to_string(generation),
      .plan = {.touch = true, .pen = true, .gamepad_slots = 1},
    };
  }

  allocation_t allocation_for(const expectation_t &expectation) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    const std::vector<std::pair<device_kind_e, std::uint32_t>> required {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
      {device_kind_e::touch, 0},
      {device_kind_e::pen, 0},
      {device_kind_e::gamepad, 0},
    };
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto [kind, slot] = required[index];
      const auto identity = expectation.handle.generation * 16 + index;
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(256 + identity),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 41,
        .inode = 5000 + identity,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(256 + identity),
        .kernel_name = expected_kernel_name(
          expectation.input_seat,
          kind,
          slot
        ),
        .host_seat = std::string {isolated_host_seat},
      });
    }
    return allocation;
  }

  class activation_backend_t final: public backend_t {
  public:
    backend_create_result_t create(const expectation_t &expectation) override {
      auto allocation = allocation_for(expectation);
      allocations.push_back(allocation);
      return {
        .result = backend_result_e::applied,
        .allocation = std::move(allocation),
      };
    }

    backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override {
      const auto found = std::find_if(
        allocations.begin(),
        allocations.end(),
        [&handle, input_seat](const auto &allocation) {
          return allocation.handle == handle &&
                 allocation.input_seat == input_seat;
        }
      );
      if (found == allocations.end()) {
        return backend_result_e::not_found;
      }
      allocations.erase(found);
      return backend_result_e::applied;
    }

    backend_result_e route(
      const seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const input_event_t &
    ) override {
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      return allocations;
    }

    std::vector<allocation_t> allocations;
  };

  struct prepared_activation_authority_t {
    activation_backend_t backend;
    authority_t authority {backend};

    prepared_activation_authority_t() {
      if (!authority.reconcile({}).admission_ready) {
        throw std::runtime_error {"failed to reconcile fake input authority"};
      }
    }

    seat_handle_t prepare(
      std::uint64_t generation,
      std::uint32_t slot = 0
    ) {
      const auto expectation = expectation_for(generation, slot);
      if (!authority.prepare(expectation).prepared()) {
        throw std::runtime_error {"failed to prepare fake input seat"};
      }
      return expectation.handle;
    }
  };

  moonlight_launch_selection_key_t key_for(
    std::uint32_t launch_session_id,
    std::uint64_t lifecycle_generation
  ) {
    return {
      .launch_session_id = launch_session_id,
      .lifecycle_generation = lifecycle_generation,
    };
  }

  std::shared_ptr<stream::session_t> stream_for(
    const moonlight_launch_selection_key_t &key
  ) {
    stream::config_t config {};
    rtsp_stream::launch_session_t launch {};
    launch.id = key.launch_session_id;
    launch.lifecycle_generation = key.lifecycle_generation;
    launch.gcm_key.resize(16);
    launch.iv.resize(16);
    launch.device_name = "activation-test-client";
    launch.unique_id = "activation-test-client-" +
                       std::to_string(key.launch_session_id) + "-" +
                       std::to_string(key.lifecycle_generation);
    launch.session_token = "test-token";
    launch.perm = crypto::PERM::_game_control;
    launch.watch_only = false;
    return stream::session::alloc(config, launch);
  }

  std::string read_source(const std::filesystem::path &path) {
    std::ifstream input {path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  std::size_t count_source_occurrences(std::string_view needle) {
    const auto source_root = std::filesystem::path {POLARIS_SOURCE_DIR} / "src";
    std::size_t occurrences = 0;
    for (const auto &entry:
         std::filesystem::recursive_directory_iterator {source_root}) {
      if (!entry.is_regular_file() ||
          (entry.path().extension() != ".h" &&
           entry.path().extension() != ".cpp")) {
        continue;
      }
      const auto contents = read_source(entry.path());
      for (auto offset = contents.find(needle); offset != std::string::npos;
           offset = contents.find(needle, offset + needle.size())) {
        ++occurrences;
      }
    }
    return occurrences;
  }

  TEST(MultiseatMoonlightActivation, DefaultStateDoesNotSelectAnySession) {
    ASSERT_FALSE(moonlight_session_activation_gate_installed());
    auto session = stream_for(key_for(200, 300));
    ASSERT_TRUE(session);

    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::not_installed
    );
    EXPECT_FALSE(stream::session::multiseat_input_bound(*session));
  }

  TEST(MultiseatMoonlightActivation, DisabledGateCannotRegisterOrInstall) {
    const auto invalid = install_moonlight_session_activation_gate(nullptr);
    EXPECT_EQ(
      invalid.status,
      moonlight_activation_install_status_e::invalid_gate
    );
    EXPECT_FALSE(invalid.installation);

    prepared_activation_authority_t prepared;
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      false,
      prepared.authority,
      registry,
      hub
    );

    const auto selected = gate->register_selection(
      key_for(201, 301),
      handle_for(1),
      expectation_for(1).input_seat,
      true
    );
    EXPECT_EQ(
      selected.status,
      moonlight_launch_selection_status_e::gate_disabled
    );
    const auto installed = install_moonlight_session_activation_gate(gate);
    EXPECT_EQ(
      installed.status,
      moonlight_activation_install_status_e::gate_disabled
    );
    EXPECT_FALSE(installed.installation);
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightActivation, InstallationIsExclusiveAndRaiiRemoved) {
    prepared_activation_authority_t prepared;
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto first_gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      hub
    );
    auto second_gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      hub
    );

    auto first = install_moonlight_session_activation_gate(first_gate);
    ASSERT_EQ(first.status, moonlight_activation_install_status_e::installed);
    ASSERT_TRUE(first.installation);
    EXPECT_TRUE(first.installation->installed());
    EXPECT_TRUE(moonlight_session_activation_gate_installed());

    auto duplicate = install_moonlight_session_activation_gate(second_gate);
    EXPECT_EQ(
      duplicate.status,
      moonlight_activation_install_status_e::already_installed
    );
    EXPECT_FALSE(duplicate.installation);

    first.installation.reset();
    EXPECT_FALSE(moonlight_session_activation_gate_installed());

    auto second = install_moonlight_session_activation_gate(second_gate);
    ASSERT_EQ(second.status, moonlight_activation_install_status_e::installed);
    ASSERT_TRUE(second.installation);
    second.installation->close();
    EXPECT_FALSE(second.installation->installed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightActivation, BindsSelectedAndLeavesUnselectedAlone) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(10);
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      hub
    );
    EXPECT_EQ(
      gate->register_selection(
        key_for(202, 302),
        handle,
        "polaris-input-wrong",
        true
      ).status,
      moonlight_launch_selection_status_e::seat_not_admitted
    );
    auto selected = gate->register_selection(
      key_for(202, 302),
      handle,
      expectation_for(10).input_seat,
      true
    );
    ASSERT_EQ(
      selected.status,
      moonlight_launch_selection_status_e::registered
    );
    ASSERT_TRUE(selected.selection);
    auto installed = install_moonlight_session_activation_gate(gate);
    ASSERT_EQ(installed.status, moonlight_activation_install_status_e::installed);

    auto selected_session = stream_for(key_for(202, 302));
    auto unselected_session = stream_for(key_for(203, 303));
    ASSERT_TRUE(selected_session);
    ASSERT_TRUE(unselected_session);
    EXPECT_EQ(
      activate_registered_moonlight_session(*unselected_session),
      moonlight_session_activation_status_e::unselected
    );
    EXPECT_FALSE(stream::session::multiseat_input_bound(*unselected_session));

    EXPECT_EQ(
      activate_registered_moonlight_session(*selected_session),
      moonlight_session_activation_status_e::bound
    );
    EXPECT_TRUE(stream::session::multiseat_input_bound(*selected_session));
    EXPECT_EQ(registry.registered_sessions(), 1U);
    EXPECT_EQ(registry.claimed_sessions(), 1U);
    EXPECT_EQ(hub->subscriptions(), 1U);
    EXPECT_EQ(
      activate_registered_moonlight_session(*selected_session),
      moonlight_session_activation_status_e::bound
    );

    stream::session::stop(*selected_session);
    // The closed owner remains attached as a tombstone so this selected
    // session can never fall through to singleton input if misused again.
    EXPECT_TRUE(stream::session::multiseat_input_bound(*selected_session));
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    EXPECT_EQ(hub->subscriptions(), 0U);
    selected.selection->close();
    installed.installation->close();
  }

  TEST(MultiseatMoonlightActivation, RejectsStaleLifecycleForReusedLaunchId) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(11);
    moonlight_session_binding_registry_t registry;
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      std::make_shared<moonlight_controller_feedback_hub_t>()
    );
    auto selected = gate->register_selection(
      key_for(204, 305),
      handle,
      expectation_for(11).input_seat,
      false
    );
    ASSERT_TRUE(selected.selection);
    auto installed = install_moonlight_session_activation_gate(gate);
    ASSERT_TRUE(installed.installation);

    auto stale = stream_for(key_for(204, 304));
    auto exact = stream_for(key_for(204, 305));
    EXPECT_EQ(
      activate_registered_moonlight_session(*stale),
      moonlight_session_activation_status_e::unselected
    );
    EXPECT_FALSE(stream::session::multiseat_input_bound(*stale));
    EXPECT_EQ(
      activate_registered_moonlight_session(*exact),
      moonlight_session_activation_status_e::bound
    );
    EXPECT_TRUE(stream::session::multiseat_input_bound(*exact));

    stream::session::stop(*exact);
    selected.selection->close();
    installed.installation->close();
  }

  TEST(MultiseatMoonlightActivation, ClaimDoesNotAcceptAnotherAlreadyBoundAllocation) {
    prepared_activation_authority_t prepared;
    const auto first_handle = prepared.prepare(31, 0);
    const auto other_handle = prepared.prepare(32, 1);
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    moonlight_session_activation_gate_t gate {true, prepared.authority, registry, hub};
    const auto key = key_for(231, 331);
    auto selected = gate.register_selection(key, first_handle, expectation_for(31).input_seat, false);
    ASSERT_TRUE(selected.selection);
    auto first = stream_for(key);
    auto other = stream_for(key);
    ASSERT_EQ(gate.activate(*first), moonlight_session_activation_status_e::bound);
    ASSERT_EQ(stream::session::bind_multiseat_input(*other, prepared.authority, registry,
      other_handle, hub, false), stream::session::multiseat_input_bind_status_e::bound);
    EXPECT_NE(stream::session::generation(*first), stream::session::generation(*other));
    EXPECT_EQ(gate.activate(*other), moonlight_session_activation_status_e::selected_binding_failed);
    EXPECT_EQ(gate.activate(*first), moonlight_session_activation_status_e::bound);
    stream::session::stop(*first);
    EXPECT_EQ(gate.activate(*first), moonlight_session_activation_status_e::selected_binding_failed);
    stream::session::stop(*other);
  }

  TEST(MultiseatMoonlightActivation, StopBeforeBindingClosesAdmission) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(33);
    moonlight_session_binding_registry_t registry;
    auto session = stream_for(key_for(233, 333));
    stream::session::stop(*session);
    EXPECT_EQ(stream::session::bind_multiseat_input(*session, prepared.authority, registry,
      handle, {}, false), stream::session::multiseat_input_bind_status_e::invalid_session_state);
    EXPECT_EQ(registry.registered_sessions(), 0U);
  }

  TEST(MultiseatMoonlightActivation, MissingRequiredConnectionNeverDowngradesToInputOnly) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(34);
    moonlight_session_binding_registry_t registry;
    moonlight_session_activation_gate_t gate {true, prepared.authority, registry, {}};
    const auto key = key_for(234, 334);
    EXPECT_EQ(gate.register_selection(key, handle, expectation_for(34).input_seat, false,
      {.required = true}).status, moonlight_launch_selection_status_e::invalid_selection);
    auto session = stream_for(key);
    EXPECT_EQ(stream::session::bind_multiseat_input(*session, prepared.authority, registry,
      handle, {}, false, {.required = true}), stream::session::multiseat_input_bind_status_e::worker_connection_rejected);
    EXPECT_FALSE(stream::session::multiseat_input_bound(*session));
  }

  TEST(MultiseatMoonlightActivation, SelectedBindFailureRemainsFailClosed) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(12);
    moonlight_session_binding_registry_t registry;
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      std::make_shared<moonlight_controller_feedback_hub_t>()
    );
    auto selected = gate->register_selection(
      key_for(205, 306),
      handle,
      expectation_for(12).input_seat,
      false
    );
    ASSERT_TRUE(selected.selection);
    auto installed = install_moonlight_session_activation_gate(gate);
    ASSERT_TRUE(installed.installation);
    ASSERT_EQ(prepared.authority.release(handle), status_e::applied);

    auto session = stream_for(key_for(205, 306));
    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::selected_binding_failed
    );
    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::selected_binding_failed
    );
    EXPECT_FALSE(stream::session::multiseat_input_bound(*session));
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);

    selected.selection->close();
    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::unselected
    );
    installed.installation->close();
  }

  TEST(MultiseatMoonlightActivation, CancelledSelectionRemainsFailClosed) {
    prepared_activation_authority_t prepared;
    const auto handle = prepared.prepare(16);
    moonlight_session_binding_registry_t registry;
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      std::make_shared<moonlight_controller_feedback_hub_t>()
    );
    auto selected = gate->register_selection(
      key_for(212, 312),
      handle,
      expectation_for(16).input_seat,
      false
    );
    ASSERT_TRUE(selected.selection);
    auto installed = install_moonlight_session_activation_gate(gate);
    ASSERT_TRUE(installed.installation);
    auto session = stream_for(key_for(212, 312));

    selected.selection->cancel();
    EXPECT_FALSE(selected.selection->active());
    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::selection_cancelled
    );
    EXPECT_EQ(
      gate->register_selection(
        key_for(212, 312),
        handle,
        expectation_for(16).input_seat,
        false
      ).status,
      moonlight_launch_selection_status_e::duplicate_session
    );

    selected.selection->close();
    EXPECT_EQ(
      activate_registered_moonlight_session(*session),
      moonlight_session_activation_status_e::unselected
    );
    installed.installation->close();
  }

  TEST(MultiseatMoonlightActivation, RejectsAmbiguousSelectionsWithoutResidue) {
    prepared_activation_authority_t prepared;
    const auto first_handle = prepared.prepare(13, 0);
    const auto second_handle = prepared.prepare(14, 1);
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      hub
    );
    auto first = gate->register_selection(
      key_for(206, 307),
      first_handle,
      expectation_for(13, 0).input_seat,
      true
    );
    ASSERT_TRUE(first.selection);
    EXPECT_TRUE(first.selection->active());
    EXPECT_EQ(first.selection->key(), key_for(206, 307));
    EXPECT_EQ(gate->active_selections(), 1U);

    EXPECT_EQ(
      gate->register_selection(
        key_for(206, 307),
        second_handle,
        expectation_for(14, 1).input_seat,
        true
      ).status,
      moonlight_launch_selection_status_e::duplicate_session
    );
    EXPECT_EQ(
      gate->register_selection(
        key_for(207, 308),
        first_handle,
        expectation_for(13, 0).input_seat,
        true
      ).status,
      moonlight_launch_selection_status_e::duplicate_seat
    );
    EXPECT_EQ(
      gate->register_selection(
        {},
        second_handle,
        expectation_for(14, 1).input_seat,
        true
      ).status,
      moonlight_launch_selection_status_e::invalid_selection
    );
    EXPECT_EQ(
      gate->register_selection(
        key_for(208, 309),
        handle_for(999, 2),
        expectation_for(999, 2).input_seat,
        true
      ).status,
      moonlight_launch_selection_status_e::seat_not_admitted
    );

    auto no_feedback_gate =
      std::make_shared<moonlight_session_activation_gate_t>(
        true,
        prepared.authority,
        registry,
        nullptr
      );
    EXPECT_EQ(
      no_feedback_gate->register_selection(
        key_for(210, 311),
        second_handle,
        expectation_for(14, 1).input_seat,
        true
      ).status,
      moonlight_launch_selection_status_e::missing_feedback_dependency
    );

    auto invalid_session = stream_for(key_for(211, 0));
    EXPECT_EQ(
      gate->activate(*invalid_session),
      moonlight_session_activation_status_e::invalid_session
    );

    first.selection.reset();
    EXPECT_EQ(gate->active_selections(), 0U);
    auto replacement = gate->register_selection(
      key_for(206, 307),
      first_handle,
      expectation_for(13, 0).input_seat,
      true
    );
    EXPECT_EQ(
      replacement.status,
      moonlight_launch_selection_status_e::registered
    );
    ASSERT_TRUE(replacement.selection);
    replacement.selection->close();
    EXPECT_EQ(gate->active_selections(), 0U);
  }

  TEST(MultiseatMoonlightActivation, ClosedGateRejectsInstallAndActivation) {
    prepared_activation_authority_t prepared;
    moonlight_session_binding_registry_t registry;
    auto gate = std::make_shared<moonlight_session_activation_gate_t>(
      true,
      prepared.authority,
      registry,
      std::make_shared<moonlight_controller_feedback_hub_t>()
    );
    gate->close();
    EXPECT_TRUE(gate->closed());
    EXPECT_EQ(
      gate->register_selection(
        key_for(209, 310),
        handle_for(15),
        expectation_for(15).input_seat,
        false
      ).status,
      moonlight_launch_selection_status_e::gate_closed
    );
    auto session = stream_for(key_for(209, 310));
    EXPECT_EQ(
      gate->activate(*session),
      moonlight_session_activation_status_e::gate_closed
    );
    EXPECT_EQ(
      install_moonlight_session_activation_gate(gate).status,
      moonlight_activation_install_status_e::gate_closed
    );
  }

  TEST(MultiseatMoonlightActivation, SourceKeepsConstructionOrderedAndDefaultOff) {
    const auto stream_source = read_source(
      std::filesystem::path {POLARIS_SOURCE_DIR} / "src/stream.cpp"
    );
    const auto start_begin = stream_source.find(
      "int start(session_t &session, const std::string &addr_string)"
    );
    const auto start_end = stream_source.find(
      "std::shared_ptr<session_t> alloc(",
      start_begin
    );
    ASSERT_NE(start_begin, std::string::npos);
    ASSERT_NE(start_end, std::string::npos);
    const auto start = stream_source.substr(start_begin, start_end - start_begin);
    const auto activate = start.find(
      "activate_registered_moonlight_session(session)"
    );
    const auto selection_closed = start.find(
      "multiseat_input_selection_closed = true"
    );
    const auto singleton_alloc = start.find("input::alloc(session.mail)");
    ASSERT_NE(activate, std::string::npos);
    ASSERT_NE(selection_closed, std::string::npos);
    ASSERT_NE(singleton_alloc, std::string::npos);
    EXPECT_LT(activate, selection_closed);
    EXPECT_LT(selection_closed, singleton_alloc);

    // The install API has its friend declaration, public declaration,
    // definition, and one call from the default-off coordinator factory.
    // No production runtime constructs that coordinator.
    EXPECT_EQ(
      count_source_occurrences("install_moonlight_session_activation_gate("),
      4U
    );
    // Declaration, definition, and the single session::start construction edge.
    EXPECT_EQ(
      count_source_occurrences("activate_registered_moonlight_session("),
      3U
    );
  }
}  // namespace
