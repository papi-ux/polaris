/**
 * @file tests/unit/platform/test_multiseat_moonlight_live_session.cpp
 * @brief Offline tests for the explicitly activated live-session owner.
 */
#include "src/platform/linux/multiseat_moonlight_live_session.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat;
  using namespace multiseat::input;

  using bytes_t = std::vector<std::uint8_t>;

  void append_u16_le(bytes_t &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  }

  void append_u32_le(bytes_t &bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
  }

  bytes_t keyboard_packet(std::uint16_t key) {
    bytes_t body {0};
    append_u16_le(body, key);
    body.push_back(0);
    append_u16_le(body, 0);

    bytes_t result;
    const auto declared = static_cast<std::uint32_t>(body.size() + 4);
    result.push_back(static_cast<std::uint8_t>(declared >> 24U));
    result.push_back(static_cast<std::uint8_t>(declared >> 16U));
    result.push_back(static_cast<std::uint8_t>(declared >> 8U));
    result.push_back(static_cast<std::uint8_t>(declared));
    append_u32_le(result, KEY_DOWN_EVENT_MAGIC);
    result.insert(result.end(), body.begin(), body.end());
    return result;
  }

  seat_handle_t handle_for(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .controller_epoch = "controller-a",
      .logical_gpu_id = "gpu-primary",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t expectation_for(std::uint64_t generation) {
    return {
      .handle = handle_for(generation),
      .input_seat = "polaris-input-controller-a-" +
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
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(index),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 41,
        .inode = 3000 + index,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(64 + index),
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

  class live_backend_t final: public backend_t {
  public:
    backend_create_result_t create(const expectation_t &expectation) override {
      current = allocation_for(expectation);
      return {
        .result = backend_result_e::applied,
        .allocation = current,
      };
    }

    backend_result_e destroy(
      const seat_handle_t &,
      std::string_view
    ) override {
      current.reset();
      return backend_result_e::applied;
    }

    backend_result_e route(
      const seat_handle_t &handle,
      std::string_view input_seat,
      std::uint64_t sequence,
      const input_event_t &event
    ) override {
      std::scoped_lock lock {mutex};
      routes.push_back({handle, std::string {input_seat}, sequence, event});
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      return current ? std::vector<allocation_t> {*current} :
                       std::vector<allocation_t> {};
    }

    struct route_t {
      seat_handle_t handle;
      std::string input_seat;
      std::uint64_t sequence;
      input_event_t event;
    };

    std::optional<allocation_t> current;
    std::mutex mutex;
    std::vector<route_t> routes;
  };

  struct prepared_live_authority_t {
    live_backend_t backend;
    authority_t authority {backend};
    expectation_t expectation;

    explicit prepared_live_authority_t(std::uint64_t generation):
        expectation(expectation_for(generation)) {
      if (!authority.reconcile({}).admission_ready ||
          !authority.prepare(expectation).prepared()) {
        throw std::runtime_error {"failed to prepare fake input authority"};
      }
    }
  };

  moonlight_live_session_identity_t identity_for(std::uint64_t generation) {
    return {
      .key = {
        .launch_session_id = static_cast<std::uint32_t>(100 + generation),
        .session_generation = 1000 + generation,
      },
      .input_permissions = {
        .keyboard = true,
        .mouse = true,
        .touch = true,
        .pen = true,
        .controller = true,
      },
    };
  }

  controller_feedback_t feedback_for(
    const seat_handle_t &handle,
    std::uint64_t sequence
  ) {
    return {
      .handle = handle,
      .sequence = sequence,
      .event = {
        .kind = feedback_kind_e::rumble,
        .gamepad_slot = 0,
        .low_frequency = 10,
        .high_frequency = 20,
      },
    };
  }

  std::size_t count_text(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto offset = haystack.find(needle); offset != std::string_view::npos;
         offset = haystack.find(needle, offset + needle.size())) {
      ++count;
    }
    return count;
  }

  TEST(MultiseatMoonlightLiveSession, ProductionBindSeamHasOneGatedCallsite) {
    const auto source_root =
      std::filesystem::path {POLARIS_SOURCE_DIR} / "src";
    std::size_t occurrences = 0;
    for (const auto &entry:
         std::filesystem::recursive_directory_iterator {source_root}) {
      if (!entry.is_regular_file() ||
          (entry.path().extension() != ".h" &&
           entry.path().extension() != ".cpp")) {
        continue;
      }
      std::ifstream input {entry.path()};
      std::ostringstream contents;
      contents << input.rdbuf();
      occurrences += count_text(contents.str(), "bind_multiseat_input(");
    }

    // One declaration, one definition, and one call from the default-off
    // launch activation gate. Any other caller bypasses that selection fence.
    EXPECT_EQ(occurrences, 3U);
  }

  TEST(MultiseatMoonlightLiveSession, RejectsInvalidInputsWithoutResidue) {
    prepared_live_authority_t prepared {70};
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto identity = identity_for(70);

    auto invalid_identity = identity;
    invalid_identity.key = {};
    auto opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      invalid_identity,
      prepared.expectation.handle,
      hub,
      [](const moonlight_feedback_t &) {
        return moonlight_feedback_mailbox_result_e::queued;
      },
      true
    );
    EXPECT_EQ(
      opened.status,
      moonlight_live_session_open_status_e::invalid_identity
    );

    opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity,
      {},
      hub,
      [](const moonlight_feedback_t &) {
        return moonlight_feedback_mailbox_result_e::queued;
      },
      true
    );
    EXPECT_EQ(
      opened.status,
      moonlight_live_session_open_status_e::invalid_handle
    );

    opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity,
      prepared.expectation.handle,
      {},
      {},
      true
    );
    EXPECT_EQ(
      opened.status,
      moonlight_live_session_open_status_e::missing_feedback_dependency
    );
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    EXPECT_EQ(hub->subscriptions(), 0U);
  }

  TEST(MultiseatMoonlightLiveSession, OwnsRouteFeedbackAndCloseLifetime) {
    prepared_live_authority_t prepared {71};
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    std::vector<moonlight_feedback_t> submitted;
    auto opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity_for(71),
      prepared.expectation.handle,
      hub,
      [&submitted](const moonlight_feedback_t &feedback) {
        submitted.push_back(feedback);
        return moonlight_feedback_mailbox_result_e::queued;
      },
      true
    );

    ASSERT_EQ(opened.status, moonlight_live_session_open_status_e::opened);
    ASSERT_TRUE(opened.session);
    EXPECT_TRUE(opened.session->accepting());
    EXPECT_EQ(registry.registered_sessions(), 1U);
    EXPECT_EQ(registry.claimed_sessions(), 1U);
    EXPECT_EQ(hub->subscriptions(), 1U);

    const auto routed = opened.session->route_input(keyboard_packet(0x41));
    EXPECT_EQ(routed.status, moonlight_route_status_e::applied);
    ASSERT_EQ(prepared.backend.routes.size(), 1U);
    EXPECT_EQ(prepared.backend.routes.front().handle, prepared.expectation.handle);

    EXPECT_EQ(
      hub->publish(feedback_for(prepared.expectation.handle, 1)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(
      opened.session->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    ASSERT_EQ(submitted.size(), 1U);
    EXPECT_EQ(submitted.front().source_sequence, 1U);

    opened.session->close();
    opened.session->close();
    EXPECT_TRUE(opened.session->closed());
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    EXPECT_EQ(hub->subscriptions(), 0U);
    EXPECT_EQ(
      opened.session->route_input(keyboard_packet(0x42)).status,
      moonlight_route_status_e::session_closed
    );
  }

  TEST(MultiseatMoonlightLiveSession, RetainsFeedbackForRetry) {
    prepared_live_authority_t prepared {72};
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    std::size_t attempts = 0;
    auto opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity_for(72),
      prepared.expectation.handle,
      hub,
      [&attempts](const moonlight_feedback_t &) {
        ++attempts;
        return attempts == 1 ?
                 moonlight_feedback_mailbox_result_e::retry_later :
                 moonlight_feedback_mailbox_result_e::queued;
      },
      true
    );
    ASSERT_TRUE(opened.session);
    ASSERT_EQ(
      hub->publish(feedback_for(prepared.expectation.handle, 1)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(
      opened.session->drain_feedback(),
      moonlight_session_drain_result_e::retry_later
    );
    EXPECT_EQ(
      opened.session->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    EXPECT_EQ(attempts, 2U);
  }

  TEST(MultiseatMoonlightLiveSession, RejectsDuplicateSessionAndSeatClaims) {
    prepared_live_authority_t prepared {73};
    moonlight_session_binding_registry_t registry;
    auto first = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity_for(73),
      prepared.expectation.handle,
      {},
      {},
      false
    );
    ASSERT_TRUE(first.session);

    auto duplicate = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity_for(73),
      prepared.expectation.handle,
      {},
      {},
      false
    );
    EXPECT_EQ(
      duplicate.status,
      moonlight_live_session_open_status_e::registration_rejected
    );
    EXPECT_EQ(
      duplicate.registration_status,
      moonlight_session_registration_status_e::duplicate_session
    );

    auto other_identity = identity_for(74);
    duplicate = open_moonlight_live_session(
      prepared.authority,
      registry,
      other_identity,
      prepared.expectation.handle,
      {},
      {},
      false
    );
    EXPECT_EQ(
      duplicate.registration_status,
      moonlight_session_registration_status_e::duplicate_seat
    );
    first.session->close();
  }

  TEST(MultiseatMoonlightLiveSession, CleansRegistrationWhenBridgeOpenFails) {
    live_backend_t backend;
    authority_t unavailable {backend};
    moonlight_session_binding_registry_t registry;
    const auto opened = open_moonlight_live_session(
      unavailable,
      registry,
      identity_for(75),
      handle_for(75),
      {},
      {},
      false
    );
    EXPECT_EQ(
      opened.status,
      moonlight_live_session_open_status_e::bridge_open_failed
    );
    EXPECT_EQ(
      opened.bridge_status,
      moonlight_session_open_status_e::authority_unavailable
    );
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
  }

  TEST(MultiseatMoonlightLiveSession, CloseWaitsForActiveMailboxSubmit) {
    prepared_live_authority_t prepared {76};
    moonlight_session_binding_registry_t registry;
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    auto opened = open_moonlight_live_session(
      prepared.authority,
      registry,
      identity_for(76),
      prepared.expectation.handle,
      hub,
      [&](const moonlight_feedback_t &) {
        std::unique_lock lock {mutex};
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&]() {
          return release;
        });
        return moonlight_feedback_mailbox_result_e::queued;
      },
      true
    );
    ASSERT_TRUE(opened.session);
    ASSERT_EQ(
      hub->publish(feedback_for(prepared.expectation.handle, 1)),
      moonlight_feedback_publish_status_e::delivered
    );

    auto drain = std::async(std::launch::async, [&]() {
      return opened.session->drain_feedback();
    });
    {
      std::unique_lock lock {mutex};
      ASSERT_TRUE(changed.wait_for(lock, 2s, [&]() {
        return entered;
      }));
    }
    auto close = std::async(std::launch::async, [&]() {
      opened.session->close();
    });
    EXPECT_EQ(close.wait_for(25ms), std::future_status::timeout);
    {
      std::scoped_lock lock {mutex};
      release = true;
    }
    changed.notify_all();
    EXPECT_EQ(
      drain.get(),
      moonlight_session_drain_result_e::sent
    );
    close.get();
    EXPECT_TRUE(opened.session->closed());
    EXPECT_EQ(registry.registered_sessions(), 0U);
  }
}  // namespace
