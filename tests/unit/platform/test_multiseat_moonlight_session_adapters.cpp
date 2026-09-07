/**
 * @file tests/unit/platform/test_multiseat_moonlight_session_adapters.cpp
 * @brief Offline tests for production multiseat Moonlight ownership adapters.
 */
#include "src/platform/linux/multiseat_moonlight_session_adapters.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat;
  using namespace multiseat::input;

  seat_handle_t handle_for(
    std::uint64_t generation,
    std::uint32_t slot = 0,
    std::string controller = "controller-a"
  ) {
    return {
      .controller_epoch = std::move(controller),
      .logical_gpu_id = "gpu-primary",
      .slot = slot,
      .generation = generation,
    };
  }

  moonlight_control_session_key_t key_for(std::uint64_t generation) {
    return {
      .launch_session_id = static_cast<std::uint32_t>(100 + generation),
      .session_generation = 1000 + generation,
    };
  }

  authenticated_moonlight_session_t binding_for(
    std::uint64_t generation,
    std::uint32_t slot = 0,
    bool feedback = true
  ) {
    return {
      .key = key_for(generation),
      .handle = handle_for(generation, slot),
      .input_permissions = {
        .keyboard = true,
        .mouse = true,
        .touch = true,
        .pen = true,
        .controller = true,
      },
      .controller_feedback = feedback,
    };
  }

  controller_feedback_t feedback_for(
    const seat_handle_t &handle,
    std::uint64_t sequence,
    std::uint32_t slot = 0,
    std::uint16_t low = 10,
    std::uint16_t high = 20
  ) {
    return {
      .handle = handle,
      .sequence = sequence,
      .event = feedback_event_t {
        .kind = feedback_kind_e::rumble,
        .gamepad_slot = slot,
        .low_frequency = low,
        .high_frequency = high,
      },
    };
  }

  moonlight_feedback_t converted_feedback(
    std::uint64_t sequence = 1,
    std::uint16_t slot = 0
  ) {
    return {
      .source_sequence = sequence,
      .message = platf::gamepad_feedback_msg_t::make_rumble(slot, 10, 20),
    };
  }

  template<class Predicate>
  bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

  class fake_mailbox_t final: public moonlight_session_feedback_mailbox_t {
  public:
    explicit fake_mailbox_t(authenticated_moonlight_session_t binding):
        binding_(std::move(binding)) {
    }

    const authenticated_moonlight_session_t &binding() const noexcept override {
      return binding_;
    }

    moonlight_feedback_mailbox_result_e submit(
      const moonlight_feedback_t &feedback
    ) override {
      std::scoped_lock lock {mutex_};
      calls_.push_back(feedback);
      if (throw_next_) {
        throw_next_ = false;
        throw std::runtime_error {"injected mailbox failure"};
      }
      if (results_.empty()) {
        return moonlight_feedback_mailbox_result_e::queued;
      }
      const auto result = results_.front();
      results_.pop_front();
      return result;
    }

    void enqueue(moonlight_feedback_mailbox_result_e result) {
      std::scoped_lock lock {mutex_};
      results_.push_back(result);
    }

    void enqueue_raw(int result) {
      enqueue(static_cast<moonlight_feedback_mailbox_result_e>(result));
    }

    void throw_next() {
      std::scoped_lock lock {mutex_};
      throw_next_ = true;
    }

    std::vector<moonlight_feedback_t> calls() const {
      std::scoped_lock lock {mutex_};
      return calls_;
    }

  private:
    const authenticated_moonlight_session_t binding_;
    mutable std::mutex mutex_;
    std::deque<moonlight_feedback_mailbox_result_e> results_;
    std::vector<moonlight_feedback_t> calls_;
    bool throw_next_ = false;
  };

  expectation_t expectation_for(std::uint64_t generation) {
    return {
      .handle = handle_for(generation),
      .input_seat = "polaris-input-controller-a-" +
                    std::to_string(generation),
      .plan = {.touch = true, .pen = true, .gamepad_slots = 2},
    };
  }

  allocation_t allocation_for(const expectation_t &expectation) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    std::vector<std::pair<device_kind_e, std::uint32_t>> required {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
      {device_kind_e::touch, 0},
      {device_kind_e::pen, 0},
      {device_kind_e::gamepad, 0},
      {device_kind_e::gamepad, 1},
    };
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto [kind, slot] = required[index];
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(index),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 41,
        .inode = 2000 + index,
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

  class bridge_backend_t final: public backend_t {
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
      const seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const input_event_t &
    ) override {
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      return current ? std::vector<allocation_t> {*current} :
                       std::vector<allocation_t> {};
    }

    std::optional<allocation_t> current;
  };

  TEST(MultiseatMoonlightSessionAdapters, RejectsInvalidDuplicateAndCollidingBindings) {
    moonlight_session_binding_registry_t registry;

    auto invalid = binding_for(1);
    invalid.key = {};
    auto result = registry.register_session(invalid);
    EXPECT_EQ(
      result.status,
      moonlight_session_registration_status_e::invalid_binding
    );

    invalid = binding_for(1);
    invalid.handle = {};
    result = registry.register_session(invalid);
    EXPECT_EQ(
      result.status,
      moonlight_session_registration_status_e::invalid_binding
    );

    invalid = binding_for(1);
    invalid.input_permissions.controller = false;
    result = registry.register_session(invalid);
    EXPECT_EQ(
      result.status,
      moonlight_session_registration_status_e::invalid_binding
    );

    auto first = registry.register_session(binding_for(1));
    ASSERT_EQ(
      first.status,
      moonlight_session_registration_status_e::registered
    );
    ASSERT_TRUE(first.registration);

    auto duplicate_key = binding_for(2, 1);
    duplicate_key.key = binding_for(1).key;
    result = registry.register_session(duplicate_key);
    EXPECT_EQ(
      result.status,
      moonlight_session_registration_status_e::duplicate_session
    );

    auto duplicate_seat = binding_for(2);
    duplicate_seat.handle.controller_epoch = "controller-replacement";
    result = registry.register_session(duplicate_seat);
    EXPECT_EQ(
      result.status,
      moonlight_session_registration_status_e::duplicate_seat
    );

    auto second = registry.register_session(binding_for(2, 1));
    EXPECT_EQ(
      second.status,
      moonlight_session_registration_status_e::registered
    );
    EXPECT_EQ(registry.registered_sessions(), 2U);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
  }

  TEST(MultiseatMoonlightSessionAdapters, ClaimsExclusivelyAndAllowsReattach) {
    moonlight_session_binding_registry_t registry;
    const auto binding = binding_for(3);
    auto registered = registry.register_session(binding);
    ASSERT_TRUE(registered.registration);

    auto first = registry.attach(binding.key);
    ASSERT_EQ(
      first.status,
      moonlight_session_authentication_status_e::authenticated
    );
    ASSERT_TRUE(first.lease);
    EXPECT_EQ(first.lease->binding(), binding);
    EXPECT_EQ(registry.claimed_sessions(), 1U);

    auto duplicate = registry.attach(binding.key);
    EXPECT_EQ(
      duplicate.status,
      moonlight_session_authentication_status_e::rejected
    );
    EXPECT_FALSE(duplicate.lease);

    first.lease->detach();
    first.lease->detach();
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    auto replacement = registry.attach(binding.key);
    ASSERT_EQ(
      replacement.status,
      moonlight_session_authentication_status_e::authenticated
    );
    replacement.lease.reset();
    EXPECT_EQ(registry.claimed_sessions(), 0U);

    registered.registration->close();
    EXPECT_FALSE(registered.registration->registered());
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(
      registry.attach(binding.key).status,
      moonlight_session_authentication_status_e::rejected
    );
  }

  TEST(MultiseatMoonlightSessionAdapters, RegistrationCloseWaitsForItsClaim) {
    moonlight_session_binding_registry_t registry;
    const auto binding = binding_for(4);
    auto registered = registry.register_session(binding);
    auto claimed = registry.attach(binding.key);
    ASSERT_TRUE(registered.registration);
    ASSERT_TRUE(claimed.lease);

    auto closing = std::async(
      std::launch::async,
      [registration = std::move(registered.registration)]() mutable {
        registration->close();
      }
    );
    ASSERT_TRUE(wait_until([&registry]() {
      return registry.registered_sessions() == 0;
    }));
    EXPECT_EQ(closing.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(
      registry.attach(binding.key).status,
      moonlight_session_authentication_status_e::rejected
    );

    claimed.lease.reset();
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    EXPECT_EQ(registry.claimed_sessions(), 0U);
  }

  TEST(MultiseatMoonlightSessionAdapters, RegistryCloseWaitsAndRejectsNewClaims) {
    moonlight_session_binding_registry_t registry;
    const auto first_binding = binding_for(5);
    const auto second_binding = binding_for(6, 1);
    auto first_registration = registry.register_session(first_binding);
    auto second_registration = registry.register_session(second_binding);
    auto claim = registry.attach(first_binding.key);
    ASSERT_TRUE(first_registration.registration);
    ASSERT_TRUE(second_registration.registration);
    ASSERT_TRUE(claim.lease);

    auto closing = std::async(std::launch::async, [&registry]() {
      registry.close();
    });
    ASSERT_TRUE(wait_until([&registry]() {
      return registry.closed();
    }));
    EXPECT_EQ(closing.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(registry.registered_sessions(), 0U);
    EXPECT_EQ(
      registry.attach(second_binding.key).status,
      moonlight_session_authentication_status_e::indeterminate
    );

    claim.lease.reset();
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    EXPECT_FALSE(first_registration.registration->registered());
    EXPECT_FALSE(second_registration.registration->registered());
  }

  TEST(MultiseatMoonlightSessionAdapters, BoundsTheRegistrationSet) {
    moonlight_session_binding_registry_t registry;
    std::vector<std::unique_ptr<moonlight_session_registration_t>> owners;
    owners.reserve(maximum_input_allocations);
    for (std::size_t index = 0; index < maximum_input_allocations; ++index) {
      auto binding = binding_for(index + 10, static_cast<std::uint32_t>(index));
      auto registered = registry.register_session(std::move(binding));
      ASSERT_EQ(
        registered.status,
        moonlight_session_registration_status_e::registered
      );
      owners.push_back(std::move(registered.registration));
    }
    EXPECT_EQ(registry.registered_sessions(), maximum_input_allocations);

    const auto overflow = registry.register_session(
      binding_for(1000, static_cast<std::uint32_t>(maximum_input_allocations))
    );
    EXPECT_EQ(
      overflow.status,
      moonlight_session_registration_status_e::capacity_reached
    );
    EXPECT_FALSE(overflow.registration);
  }

  TEST(MultiseatMoonlightSessionAdapters, RegistryDestructionRetiresAnActiveLeaseSafely) {
    const auto binding = binding_for(7);
    std::unique_ptr<moonlight_session_registration_t> registration;
    moonlight_session_authentication_result_t claim;
    {
      auto registry = std::make_unique<moonlight_session_binding_registry_t>();
      auto registered = registry->register_session(binding);
      registration = std::move(registered.registration);
      claim = registry->attach(binding.key);
      ASSERT_TRUE(registration);
      ASSERT_TRUE(claim.lease);
      registry.reset();
    }

    EXPECT_FALSE(registration->registered());
    EXPECT_EQ(claim.lease->binding(), binding);
    claim.lease.reset();
    registration.reset();
  }

  TEST(MultiseatMoonlightSessionAdapters, HubRoutesExactHandlesAndFencesSeatReuse) {
    moonlight_controller_feedback_hub_t hub;
    const auto first = handle_for(8);
    const auto replacement = handle_for(9);
    const auto second = handle_for(10, 1);
    std::vector<controller_feedback_t> first_received;
    std::vector<controller_feedback_t> second_received;

    auto first_subscription = hub.attach(
      first,
      [&first_received](const controller_feedback_t &feedback) {
        first_received.push_back(feedback);
      }
    );
    ASSERT_TRUE(first_subscription);
    EXPECT_FALSE(hub.attach(first, [](const auto &) {
    }));
    EXPECT_FALSE(hub.attach(replacement, [](const auto &) {
    }));
    auto second_subscription = hub.attach(
      second,
      [&second_received](const controller_feedback_t &feedback) {
        second_received.push_back(feedback);
      }
    );
    ASSERT_TRUE(second_subscription);
    EXPECT_EQ(hub.subscriptions(), 2U);

    EXPECT_EQ(
      hub.publish(feedback_for(first, 1)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(
      hub.publish(feedback_for(second, 1, 1)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(
      hub.publish(feedback_for(handle_for(99, 2), 1)),
      moonlight_feedback_publish_status_e::not_attached
    );
    auto invalid = feedback_for(first, 0);
    EXPECT_EQ(
      hub.publish(invalid),
      moonlight_feedback_publish_status_e::invalid_feedback
    );
    ASSERT_EQ(first_received.size(), 1U);
    ASSERT_EQ(second_received.size(), 1U);

    first_subscription->detach();
    first_subscription->detach();
    EXPECT_EQ(hub.subscriptions(), 1U);
    auto replacement_subscription = hub.attach(
      replacement,
      [](const controller_feedback_t &) {
      }
    );
    EXPECT_TRUE(replacement_subscription);
  }

  TEST(MultiseatMoonlightSessionAdapters, SubscriptionDetachWaitsForCallback) {
    moonlight_controller_feedback_hub_t hub;
    const auto handle = handle_for(11);
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    auto subscription = hub.attach(
      handle,
      [&entered_promise, release](const controller_feedback_t &) {
        entered_promise.set_value();
        release.wait();
      }
    );
    ASSERT_TRUE(subscription);

    auto publishing = std::async(std::launch::async, [&hub, handle]() {
      return hub.publish(feedback_for(handle, 1));
    });
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto detaching = std::async(
      std::launch::async,
      [subscription = std::move(subscription)]() mutable {
        subscription->detach();
      }
    );
    ASSERT_TRUE(wait_until([&hub]() {
      return hub.subscriptions() == 0;
    }));
    EXPECT_EQ(detaching.wait_for(0ms), std::future_status::timeout);

    release_promise.set_value();
    ASSERT_EQ(publishing.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(
      publishing.get(),
      moonlight_feedback_publish_status_e::delivered
    );
    ASSERT_EQ(detaching.wait_for(2s), std::future_status::ready);
    detaching.get();
    EXPECT_EQ(
      hub.publish(feedback_for(handle, 2)),
      moonlight_feedback_publish_status_e::not_attached
    );
  }

  TEST(MultiseatMoonlightSessionAdapters, HubCloseWaitsAcrossConcurrentDetach) {
    moonlight_controller_feedback_hub_t hub;
    const auto handle = handle_for(12);
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    auto subscription = hub.attach(
      handle,
      [&entered_promise, release](const controller_feedback_t &) {
        entered_promise.set_value();
        release.wait();
      }
    );
    ASSERT_TRUE(subscription);
    auto publishing = std::async(std::launch::async, [&hub, handle]() {
      return hub.publish(feedback_for(handle, 1));
    });
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto detaching = std::async(
      std::launch::async,
      [subscription = std::move(subscription)]() mutable {
        subscription->detach();
      }
    );
    ASSERT_TRUE(wait_until([&hub]() {
      return hub.subscriptions() == 0;
    }));
    auto closing = std::async(std::launch::async, [&hub]() {
      hub.close();
    });
    EXPECT_EQ(closing.wait_for(0ms), std::future_status::timeout);

    release_promise.set_value();
    ASSERT_EQ(publishing.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(
      publishing.get(),
      moonlight_feedback_publish_status_e::delivered
    );
    ASSERT_EQ(detaching.wait_for(2s), std::future_status::ready);
    detaching.get();
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    EXPECT_TRUE(hub.closed());
  }

  TEST(MultiseatMoonlightSessionAdapters, ReportsCallbackFailureAndRemainsUsable) {
    moonlight_controller_feedback_hub_t hub;
    const auto handle = handle_for(13);
    std::atomic<unsigned> calls {0};
    auto subscription = hub.attach(
      handle,
      [&calls](const controller_feedback_t &) {
        if (calls.fetch_add(1) == 0) {
          throw std::runtime_error {"injected callback failure"};
        }
      }
    );
    ASSERT_TRUE(subscription);

    EXPECT_EQ(
      hub.publish(feedback_for(handle, 1)),
      moonlight_feedback_publish_status_e::callback_failed
    );
    EXPECT_EQ(
      hub.publish(feedback_for(handle, 2)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(calls.load(), 2U);
  }

  TEST(MultiseatMoonlightSessionAdapters, RetainedSinkIsSafeAfterHubDestruction) {
    moonlight_controller_feedback_sink_t sink;
    const auto handle = handle_for(14);
    std::atomic<unsigned> calls {0};
    {
      moonlight_controller_feedback_hub_t hub;
      sink = hub.sink();
      auto subscription = hub.attach(
        handle,
        [&calls](const controller_feedback_t &) {
          calls.fetch_add(1);
        }
      );
      ASSERT_TRUE(subscription);
      sink(feedback_for(handle, 1));
      EXPECT_EQ(calls.load(), 1U);
    }

    sink(feedback_for(handle, 2));
    EXPECT_EQ(calls.load(), 1U);
  }

  TEST(MultiseatMoonlightSessionAdapters, HubHandlesConcurrentPublishers) {
    moonlight_controller_feedback_hub_t hub;
    const auto handle = handle_for(15);
    std::atomic<unsigned> calls {0};
    auto subscription = hub.attach(
      handle,
      [&calls](const controller_feedback_t &) {
        calls.fetch_add(1, std::memory_order_relaxed);
      }
    );
    ASSERT_TRUE(subscription);

    constexpr std::size_t publisher_count = 64;
    std::vector<std::future<moonlight_feedback_publish_status_e>> publishers;
    publishers.reserve(publisher_count);
    for (std::size_t index = 0; index < publisher_count; ++index) {
      publishers.push_back(std::async(std::launch::async, [&hub, handle, index]() {
        return hub.publish(feedback_for(handle, index + 1));
      }));
    }
    for (auto &publisher : publishers) {
      EXPECT_EQ(
        publisher.get(),
        moonlight_feedback_publish_status_e::delivered
      );
    }
    EXPECT_EQ(calls.load(), publisher_count);
  }

  TEST(MultiseatMoonlightSessionAdapters, MailboxSenderValidatesAndMapsResults) {
    const auto binding = binding_for(16);
    auto mailbox = std::make_shared<fake_mailbox_t>(binding);
    moonlight_session_mailbox_feedback_sender_t sender {mailbox};
    const auto feedback = converted_feedback();

    EXPECT_EQ(
      sender.send(binding, feedback),
      moonlight_feedback_send_result_e::sent
    );
    mailbox->enqueue(moonlight_feedback_mailbox_result_e::retry_later);
    EXPECT_EQ(
      sender.send(binding, feedback),
      moonlight_feedback_send_result_e::retry_later
    );
    mailbox->enqueue(moonlight_feedback_mailbox_result_e::closed);
    EXPECT_EQ(
      sender.send(binding, feedback),
      moonlight_feedback_send_result_e::closed
    );
    mailbox->enqueue_raw(0xFF);
    EXPECT_EQ(
      sender.send(binding, feedback),
      moonlight_feedback_send_result_e::closed
    );
    EXPECT_EQ(mailbox->calls().size(), 4U);

    auto wrong_binding = binding;
    wrong_binding.key.session_generation += 1;
    EXPECT_EQ(
      sender.send(wrong_binding, feedback),
      moonlight_feedback_send_result_e::closed
    );
    auto invalid_feedback = feedback;
    invalid_feedback.source_sequence = 0;
    EXPECT_EQ(
      sender.send(binding, invalid_feedback),
      moonlight_feedback_send_result_e::closed
    );
    invalid_feedback = feedback;
    invalid_feedback.message.id = maximum_gamepad_slots;
    EXPECT_EQ(
      sender.send(binding, invalid_feedback),
      moonlight_feedback_send_result_e::closed
    );
    EXPECT_EQ(mailbox->calls().size(), 4U);

    mailbox->throw_next();
    EXPECT_THROW(sender.send(binding, feedback), std::runtime_error);
  }

  TEST(MultiseatMoonlightSessionAdapters, ValidatesMailboxConstruction) {
    EXPECT_THROW(
      moonlight_session_mailbox_feedback_sender_t(nullptr),
      std::invalid_argument
    );
    auto invalid = binding_for(17);
    invalid.input_permissions.controller = false;
    auto invalid_mailbox = std::make_shared<fake_mailbox_t>(invalid);
    EXPECT_THROW(
      {
        moonlight_session_mailbox_feedback_sender_t sender {invalid_mailbox};
      },
      std::invalid_argument
    );

    auto unavailable = binding_for(17);
    unavailable.controller_feedback = false;
    auto mailbox = std::make_shared<fake_mailbox_t>(unavailable);
    EXPECT_NO_THROW({
      moonlight_session_mailbox_feedback_sender_t sender {mailbox};
    });
  }

  TEST(MultiseatMoonlightSessionAdapters, ProductionAdaptersDriveOneBridgeLifetime) {
    bridge_backend_t backend;
    authority_t authority {backend};
    const auto expectation = expectation_for(18);
    ASSERT_TRUE(authority.reconcile({}).admission_ready);
    ASSERT_TRUE(authority.prepare(expectation).prepared());

    const auto binding = authenticated_moonlight_session_t {
      .key = key_for(18),
      .handle = expectation.handle,
      .input_permissions = {
        .keyboard = true,
        .mouse = true,
        .touch = true,
        .pen = true,
        .controller = true,
      },
      .controller_feedback = true,
    };
    moonlight_session_binding_registry_t registry;
    auto registered = registry.register_session(binding);
    ASSERT_TRUE(registered.registration);
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    auto mailbox = std::make_shared<fake_mailbox_t>(binding);
    auto sender = std::make_shared<moonlight_session_mailbox_feedback_sender_t>(
      mailbox
    );

    auto opened = open_moonlight_session_bridge(
      authority,
      registry,
      binding.key,
      hub,
      sender
    );
    ASSERT_EQ(opened.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(opened.bridge);
    EXPECT_EQ(registry.claimed_sessions(), 1U);
    EXPECT_EQ(hub->subscriptions(), 1U);
    EXPECT_EQ(
      hub->publish(feedback_for(binding.handle, 1)),
      moonlight_feedback_publish_status_e::delivered
    );
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    ASSERT_EQ(mailbox->calls().size(), 1U);
    EXPECT_EQ(mailbox->calls()[0].source_sequence, 1U);

    auto retiring = std::async(
      std::launch::async,
      [registration = std::move(registered.registration)]() mutable {
        registration->close();
      }
    );
    ASSERT_TRUE(wait_until([&registry]() {
      return registry.registered_sessions() == 0;
    }));
    EXPECT_EQ(retiring.wait_for(0ms), std::future_status::timeout);
    opened.bridge->close();
    ASSERT_EQ(retiring.wait_for(2s), std::future_status::ready);
    retiring.get();
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_EQ(registry.claimed_sessions(), 0U);
    EXPECT_EQ(hub->subscriptions(), 0U);
    EXPECT_EQ(
      registry.attach(binding.key).status,
      moonlight_session_authentication_status_e::rejected
    );
  }

  TEST(MultiseatMoonlightSessionAdapters, ClosedHubFailsBridgeOpenAndReleasesClaim) {
    bridge_backend_t backend;
    authority_t authority {backend};
    const auto expectation = expectation_for(19);
    ASSERT_TRUE(authority.reconcile({}).admission_ready);
    ASSERT_TRUE(authority.prepare(expectation).prepared());
    const auto binding = binding_for(19);
    moonlight_session_binding_registry_t registry;
    auto registered = registry.register_session(binding);
    ASSERT_TRUE(registered.registration);
    auto hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    hub->close();
    auto mailbox = std::make_shared<fake_mailbox_t>(binding);
    auto sender = std::make_shared<moonlight_session_mailbox_feedback_sender_t>(
      mailbox
    );

    const auto opened = open_moonlight_session_bridge(
      authority,
      registry,
      binding.key,
      hub,
      sender
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::feedback_attach_failed
    );
    EXPECT_FALSE(opened.bridge);
    EXPECT_EQ(registry.claimed_sessions(), 0U);
  }
}  // namespace
