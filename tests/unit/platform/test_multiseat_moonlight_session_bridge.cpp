/**
 * @file tests/unit/platform/test_multiseat_moonlight_session_bridge.cpp
 * @brief Offline lifecycle tests for authenticated multiseat Moonlight I/O.
 */
#include "src/platform/linux/multiseat_moonlight_session_bridge.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat;
  using namespace multiseat::input;

  using bytes_t = std::vector<std::uint8_t>;

  void append_u16_be(bytes_t &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void append_u16_le(bytes_t &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  }

  void append_u32_le(bytes_t &bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
  }

  bytes_t packet(std::uint32_t magic, bytes_t body) {
    bytes_t result;
    const auto declared = static_cast<std::uint32_t>(body.size() + 4);
    result.push_back(static_cast<std::uint8_t>(declared >> 24U));
    result.push_back(static_cast<std::uint8_t>(declared >> 16U));
    result.push_back(static_cast<std::uint8_t>(declared >> 8U));
    result.push_back(static_cast<std::uint8_t>(declared));
    append_u32_le(result, magic);
    result.insert(result.end(), body.begin(), body.end());
    return result;
  }

  bytes_t relative_packet(std::int16_t x, std::int16_t y) {
    bytes_t body;
    append_u16_be(body, static_cast<std::uint16_t>(x));
    append_u16_be(body, static_cast<std::uint16_t>(y));
    return packet(MOUSE_MOVE_REL_MAGIC_GEN5, std::move(body));
  }

  bytes_t keyboard_packet(std::uint16_t key) {
    bytes_t body {0};
    append_u16_le(body, key);
    body.push_back(0);
    append_u16_le(body, 0);
    return packet(KEY_DOWN_EVENT_MAGIC, std::move(body));
  }

  seat_handle_t handle_for(std::uint64_t generation) {
    return {
      .controller_epoch = "controller-a",
      .logical_gpu_id = "gpu-primary",
      .slot = 0,
      .generation = generation,
    };
  }

  moonlight_control_session_key_t key_for(std::uint64_t generation) {
    return {
      .launch_session_id = static_cast<std::uint32_t>(100 + generation),
      .session_generation = 1000 + generation,
    };
  }

  expectation_t expectation_for(
    std::uint64_t generation,
    plan_t plan = {.touch = true, .pen = true, .gamepad_slots = 2}
  ) {
    return {
      .handle = handle_for(generation),
      .input_seat = "polaris-input-controller-a-" +
                    std::to_string(generation),
      .plan = plan,
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
    };
    if (expectation.plan.touch) {
      required.emplace_back(device_kind_e::touch, 0);
    }
    if (expectation.plan.pen) {
      required.emplace_back(device_kind_e::pen, 0);
    }
    for (std::uint32_t slot = 0; slot < expectation.plan.gamepad_slots; ++slot) {
      required.emplace_back(device_kind_e::gamepad, slot);
    }
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
    struct route_call_t {
      seat_handle_t handle;
      std::string input_seat;
      std::uint64_t sequence = 0;
      input_event_t event;
    };

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
      std::function<void()> callback;
      backend_result_e result;
      {
        std::unique_lock lock {mutex_};
        calls_.push_back({
          .handle = handle,
          .input_seat = std::string {input_seat},
          .sequence = sequence,
          .event = event,
        });
        route_entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this]() {
          return !block_route_ || release_route_;
        });
        callback = on_route_;
        result = next_route_;
      }
      if (callback) {
        callback();
      }
      return result;
    }

    std::vector<allocation_t> inventory() override {
      return current ? std::vector<allocation_t> {*current} :
                       std::vector<allocation_t> {};
    }

    void block_route() {
      std::scoped_lock lock {mutex_};
      block_route_ = true;
      release_route_ = false;
      route_entered_ = false;
    }

    void wait_for_route() {
      std::unique_lock lock {mutex_};
      changed_.wait(lock, [this]() {
        return route_entered_;
      });
    }

    void release_route() {
      std::scoped_lock lock {mutex_};
      release_route_ = true;
      changed_.notify_all();
    }

    void set_on_route(std::function<void()> callback) {
      std::scoped_lock lock {mutex_};
      on_route_ = std::move(callback);
    }

    std::vector<route_call_t> calls() const {
      std::scoped_lock lock {mutex_};
      return calls_;
    }

    std::optional<allocation_t> current;

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<route_call_t> calls_;
    std::function<void()> on_route_;
    backend_result_e next_route_ = backend_result_e::applied;
    bool block_route_ = false;
    bool release_route_ = false;
    bool route_entered_ = false;
  };

  moonlight_input_permissions_t all_permissions() {
    return {
      .keyboard = true,
      .mouse = true,
      .touch = true,
      .pen = true,
      .controller = true,
    };
  }

  authenticated_moonlight_session_t binding_for(
    const expectation_t &expectation,
    bool controller_feedback = true,
    moonlight_input_permissions_t permissions = all_permissions()
  ) {
    return {
      .key = key_for(expectation.handle.generation),
      .handle = expectation.handle,
      .input_permissions = permissions,
      .controller_feedback = controller_feedback,
    };
  }

  class fake_binding_source_t final: public moonlight_session_binding_source_t {
  public:
    struct shared_state_t {
      std::mutex mutex;
      moonlight_session_authentication_status_e status =
        moonlight_session_authentication_status_e::indeterminate;
      std::optional<authenticated_moonlight_session_t> binding;
      std::vector<moonlight_control_session_key_t> observed;
      std::size_t calls = 0;
      std::size_t detach_count = 0;
      bool throw_on_attach = false;
      bool claimed = false;
    };

    class lease_t final: public moonlight_session_binding_lease_t {
    public:
      lease_t(
        std::shared_ptr<shared_state_t> state,
        authenticated_moonlight_session_t binding
      ):
          state_(std::move(state)),
          binding_(std::move(binding)) {
      }

      ~lease_t() override {
        detach();
      }

      const authenticated_moonlight_session_t &binding() const noexcept override {
        return binding_;
      }

      void detach() noexcept override {
        if (detached_) {
          return;
        }
        detached_ = true;
        std::scoped_lock lock {state_->mutex};
        state_->claimed = false;
        ++state_->detach_count;
      }

    private:
      std::shared_ptr<shared_state_t> state_;
      authenticated_moonlight_session_t binding_;
      bool detached_ = false;
    };

    moonlight_session_authentication_result_t attach(
      const moonlight_control_session_key_t &key
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->calls;
      state_->observed.push_back(key);
      if (state_->throw_on_attach) {
        throw std::runtime_error {"injected authentication failure"};
      }
      if (state_->status !=
          moonlight_session_authentication_status_e::authenticated) {
        return {.status = state_->status};
      }
      if (!state_->binding) {
        return {
          .status = moonlight_session_authentication_status_e::authenticated,
        };
      }
      if (state_->claimed) {
        return {
          .status = moonlight_session_authentication_status_e::rejected,
        };
      }
      state_->claimed = true;
      return {
        .status = moonlight_session_authentication_status_e::authenticated,
        .lease = std::make_unique<lease_t>(state_, *state_->binding),
      };
    }

    void configure(
      moonlight_session_authentication_status_e status,
      std::optional<authenticated_moonlight_session_t> binding = std::nullopt
    ) {
      std::scoped_lock lock {state_->mutex};
      state_->status = status;
      state_->binding = std::move(binding);
    }

    void set_throw_on_attach(bool enabled) {
      std::scoped_lock lock {state_->mutex};
      state_->throw_on_attach = enabled;
    }

    std::size_t calls() const {
      std::scoped_lock lock {state_->mutex};
      return state_->calls;
    }

    std::size_t detach_count() const {
      std::scoped_lock lock {state_->mutex};
      return state_->detach_count;
    }

    bool claimed() const {
      std::scoped_lock lock {state_->mutex};
      return state_->claimed;
    }

  private:
    std::shared_ptr<shared_state_t> state_ =
      std::make_shared<shared_state_t>();
  };

  class fake_feedback_source_t final:
      public moonlight_controller_feedback_source_t,
      public std::enable_shared_from_this<fake_feedback_source_t> {
  public:
    class subscription_t final: public moonlight_feedback_subscription_t {
    public:
      explicit subscription_t(std::shared_ptr<fake_feedback_source_t> source):
          source_(std::move(source)) {
      }

      ~subscription_t() override {
        detach();
      }

      void detach() noexcept override {
        if (detached_) {
          return;
        }
        detached_ = true;
        source_->detach();
      }

    private:
      std::shared_ptr<fake_feedback_source_t> source_;
      bool detached_ = false;
    };

    std::unique_ptr<moonlight_feedback_subscription_t> attach(
      const seat_handle_t &handle,
      moonlight_controller_feedback_callback_t callback
    ) override {
      std::optional<controller_feedback_t> immediate;
      {
        std::scoped_lock lock {mutex_};
        ++attach_count_;
        attached_handle_ = handle;
        if (throw_on_attach_) {
          throw std::runtime_error {"injected feedback attach failure"};
        }
        if (fail_attach_ || attached_) {
          return {};
        }
        callback_ = std::move(callback);
        attached_ = true;
        immediate = immediate_feedback_;
      }
      if (immediate) {
        (void) publish(*immediate);
      }
      return std::make_unique<subscription_t>(shared_from_this());
    }

    bool publish(const controller_feedback_t &feedback) {
      moonlight_controller_feedback_callback_t callback;
      {
        std::unique_lock lock {mutex_};
        if (!attached_ || !callback_) {
          return false;
        }
        ++callbacks_in_flight_;
        callback = callback_;
        if (pause_before_callback_) {
          callback_waiting_ = true;
          changed_.notify_all();
          changed_.wait(lock, [this]() {
            return release_callback_;
          });
        }
      }
      callback(feedback);
      {
        std::scoped_lock lock {mutex_};
        --callbacks_in_flight_;
        changed_.notify_all();
      }
      return true;
    }

    void configure_attach_failure(bool fail, bool throws = false) {
      std::scoped_lock lock {mutex_};
      fail_attach_ = fail;
      throw_on_attach_ = throws;
    }

    void set_immediate_feedback(controller_feedback_t feedback) {
      std::scoped_lock lock {mutex_};
      immediate_feedback_ = std::move(feedback);
    }

    void pause_next_callback() {
      std::scoped_lock lock {mutex_};
      pause_before_callback_ = true;
      callback_waiting_ = false;
      release_callback_ = false;
    }

    void wait_for_paused_callback() {
      std::unique_lock lock {mutex_};
      changed_.wait(lock, [this]() {
        return callback_waiting_;
      });
    }

    void wait_for_detach_start() {
      std::unique_lock lock {mutex_};
      changed_.wait(lock, [this]() {
        return detach_started_;
      });
    }

    void release_paused_callback() {
      std::scoped_lock lock {mutex_};
      release_callback_ = true;
      changed_.notify_all();
    }

    std::size_t attach_count() const {
      std::scoped_lock lock {mutex_};
      return attach_count_;
    }

    std::size_t detach_count() const {
      std::scoped_lock lock {mutex_};
      return detach_count_;
    }

    std::optional<seat_handle_t> attached_handle() const {
      std::scoped_lock lock {mutex_};
      return attached_handle_;
    }

  private:
    void detach() noexcept {
      std::unique_lock lock {mutex_};
      detach_started_ = true;
      attached_ = false;
      changed_.notify_all();
      changed_.wait(lock, [this]() {
        return callbacks_in_flight_ == 0;
      });
      callback_ = {};
      ++detach_count_;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    moonlight_controller_feedback_callback_t callback_;
    std::optional<seat_handle_t> attached_handle_;
    std::optional<controller_feedback_t> immediate_feedback_;
    std::size_t attach_count_ = 0;
    std::size_t detach_count_ = 0;
    std::size_t callbacks_in_flight_ = 0;
    bool attached_ = false;
    bool fail_attach_ = false;
    bool throw_on_attach_ = false;
    bool pause_before_callback_ = false;
    bool callback_waiting_ = false;
    bool release_callback_ = false;
    bool detach_started_ = false;
  };

  class fake_feedback_sender_t final: public moonlight_session_feedback_sender_t {
  public:
    struct call_t {
      authenticated_moonlight_session_t session;
      moonlight_feedback_t feedback;
    };

    moonlight_feedback_send_result_e send(
      const authenticated_moonlight_session_t &session,
      const moonlight_feedback_t &feedback
    ) override {
      std::function<void(std::size_t)> callback;
      moonlight_feedback_send_result_e result;
      bool throws;
      std::size_t call_index;
      {
        std::scoped_lock lock {mutex_};
        calls_.push_back({session, feedback});
        call_index = calls_.size();
        callback = on_send_;
        throws = throw_next_;
        throw_next_ = false;
        if (results_.empty()) {
          result = moonlight_feedback_send_result_e::sent;
        } else {
          result = results_.front();
          results_.pop_front();
        }
      }
      if (callback) {
        callback(call_index);
      }
      if (throws) {
        throw std::runtime_error {"injected feedback send failure"};
      }
      return result;
    }

    void enqueue_result(moonlight_feedback_send_result_e result) {
      std::scoped_lock lock {mutex_};
      results_.push_back(result);
    }

    void throw_next() {
      std::scoped_lock lock {mutex_};
      throw_next_ = true;
    }

    void set_on_send(std::function<void(std::size_t)> callback) {
      std::scoped_lock lock {mutex_};
      on_send_ = std::move(callback);
    }

    std::vector<call_t> calls() const {
      std::scoped_lock lock {mutex_};
      return calls_;
    }

  private:
    mutable std::mutex mutex_;
    std::deque<moonlight_feedback_send_result_e> results_;
    std::vector<call_t> calls_;
    std::function<void(std::size_t)> on_send_;
    bool throw_next_ = false;
  };

  controller_feedback_t feedback_for(
    const seat_handle_t &handle,
    std::uint64_t sequence,
    std::uint32_t slot,
    std::uint16_t low,
    std::uint16_t high
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

  struct prepared_authority_t {
    bridge_backend_t backend;
    authority_t authority {backend};
    expectation_t expectation;

    explicit prepared_authority_t(
      std::uint64_t generation,
      plan_t plan = {.touch = true, .pen = true, .gamepad_slots = 2}
    ):
        expectation(expectation_for(generation, plan)) {
      if (!authority.reconcile({}).admission_ready ||
          !authority.prepare(expectation).prepared()) {
        throw std::runtime_error {"failed to prepare fake input authority"};
      }
    }
  };

  moonlight_session_open_result_t open_bridge(
    prepared_authority_t &prepared,
    fake_binding_source_t &bindings,
    const std::shared_ptr<fake_feedback_source_t> &feedback_source,
    const std::shared_ptr<fake_feedback_sender_t> &feedback_sender,
    authenticated_moonlight_session_t binding
  ) {
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      binding
    );
    return open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key,
      feedback_source,
      feedback_sender
    );
  }

  TEST(MultiseatMoonlightSessionBridge, RejectsInvalidOrUnauthenticatedKeys) {
    prepared_authority_t prepared {40};
    fake_binding_source_t bindings;

    auto opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      {.launch_session_id = 0, .session_generation = 1}
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_key);
    EXPECT_FALSE(opened.bridge);
    EXPECT_EQ(bindings.calls(), 0U);

    const auto key = key_for(40);
    bindings.configure(moonlight_session_authentication_status_e::rejected);
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      key
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::authentication_rejected
    );

    bindings.configure(
      moonlight_session_authentication_status_e::indeterminate
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      key
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::authentication_indeterminate
    );

    bindings.set_throw_on_attach(true);
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      key
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::authentication_indeterminate
    );
    bindings.set_throw_on_attach(false);

    bindings.configure(
      moonlight_session_authentication_status_e::authenticated
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      key
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_binding);
    EXPECT_FALSE(opened.bridge);

    bindings.configure(
      static_cast<moonlight_session_authentication_status_e>(0xFF)
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      key
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_binding);
    EXPECT_FALSE(opened.bridge);
  }

  TEST(MultiseatMoonlightSessionBridge, RejectsClosedAuthorityAndReleasesLease) {
    bridge_backend_t backend;
    authority_t authority {backend};
    const auto expectation = expectation_for(41);
    const auto binding = binding_for(expectation, false);
    fake_binding_source_t bindings;
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      binding
    );

    const auto opened = open_moonlight_session_bridge(
      authority,
      bindings,
      binding.key
    );

    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::authority_unavailable
    );
    EXPECT_FALSE(opened.bridge);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);
  }

  TEST(MultiseatMoonlightSessionBridge, RejectsMismatchedUnpreparedOrOverbroadBindings) {
    prepared_authority_t prepared {
      41,
      {.touch = false, .pen = false, .gamepad_slots = 0}
    };
    fake_binding_source_t bindings;
    auto binding = binding_for(
      prepared.expectation,
      false,
      {.keyboard = true, .mouse = true}
    );

    auto mismatched = binding;
    mismatched.key.session_generation += 1;
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      mismatched
    );
    auto opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_binding);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);

    auto stale = binding;
    stale.handle = handle_for(42);
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      stale
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::authority_unavailable
    );
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 2U);

    auto overbroad = binding;
    overbroad.input_permissions.touch = true;
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      overbroad
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_binding);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 3U);

    overbroad = binding;
    overbroad.controller_feedback = true;
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      overbroad
    );
    opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    EXPECT_EQ(opened.status, moonlight_session_open_status_e::invalid_binding);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 4U);

    prepared_authority_t with_controller {43};
    const auto feedback_binding = binding_for(with_controller.expectation);
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      feedback_binding
    );
    opened = open_moonlight_session_bridge(
      with_controller.authority,
      bindings,
      feedback_binding.key
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::missing_feedback_dependency
    );
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 5U);
  }

  TEST(MultiseatMoonlightSessionBridge, RoutesOnlyTheResolvedSessionPermissions) {
    prepared_authority_t prepared {44};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(
      prepared.expectation,
      true,
      {.mouse = true, .controller = true}
    );
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );

    ASSERT_EQ(opened.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(opened.bridge);
    EXPECT_EQ(opened.bridge->binding(), binding);
    EXPECT_TRUE(opened.bridge->accepting());
    EXPECT_FALSE(opened.bridge->closed());
    EXPECT_EQ(bindings.calls(), 1U);
    EXPECT_EQ(feedback_source->attach_count(), 1U);
    EXPECT_EQ(feedback_source->attached_handle(), binding.handle);

    auto routed = opened.bridge->route_input(keyboard_packet(0x41));
    EXPECT_EQ(routed.status, moonlight_route_status_e::permission_denied);
    EXPECT_EQ(routed.sequence, 0U);
    routed = opened.bridge->route_input(relative_packet(3, -4));
    EXPECT_EQ(routed.status, moonlight_route_status_e::applied);
    EXPECT_EQ(routed.sequence, 1U);
    ASSERT_EQ(prepared.backend.calls().size(), 1U);
    EXPECT_EQ(prepared.backend.calls()[0].handle, binding.handle);

    opened.bridge->close();
    opened.bridge->close();
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_FALSE(opened.bridge->accepting());
    EXPECT_EQ(feedback_source->detach_count(), 1U);
    routed = opened.bridge->route_input(relative_packet(1, 0));
    EXPECT_EQ(routed.status, moonlight_route_status_e::session_closed);
  }

  TEST(MultiseatMoonlightSessionBridge, OmitsFeedbackAttachmentWhenNotAuthorized) {
    prepared_authority_t prepared {45};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    const auto binding = binding_for(prepared.expectation, false);
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      binding
    );
    auto opened = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key,
      feedback_source,
      {}
    );

    ASSERT_EQ(opened.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(opened.bridge);
    EXPECT_EQ(feedback_source->attach_count(), 0U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::empty
    );
    opened.bridge->close();
    EXPECT_EQ(feedback_source->detach_count(), 0U);
  }

  TEST(MultiseatMoonlightSessionBridge, HoldsOneExclusiveAuthenticatedLease) {
    prepared_authority_t prepared {57};
    fake_binding_source_t bindings;
    const auto binding = binding_for(prepared.expectation, false);
    bindings.configure(
      moonlight_session_authentication_status_e::authenticated,
      binding
    );

    auto first = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    ASSERT_EQ(first.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(first.bridge);
    EXPECT_TRUE(bindings.claimed());

    auto duplicate = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    EXPECT_EQ(
      duplicate.status,
      moonlight_session_open_status_e::authentication_rejected
    );
    EXPECT_FALSE(duplicate.bridge);

    first.bridge->close();
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);
    auto replacement = open_moonlight_session_bridge(
      prepared.authority,
      bindings,
      binding.key
    );
    ASSERT_EQ(replacement.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(replacement.bridge);
    EXPECT_TRUE(bindings.claimed());
    replacement.bridge.reset();
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 2U);
  }

  TEST(MultiseatMoonlightSessionBridge, AcceptsImmediateFeedbackDuringAttachment) {
    prepared_authority_t prepared {46};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    feedback_source->set_immediate_feedback(feedback_for(
      binding.handle,
      1,
      0,
      100,
      200
    ));

    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_EQ(opened.status, moonlight_session_open_status_e::opened);
    ASSERT_TRUE(opened.bridge);
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);
    EXPECT_EQ(opened.bridge->last_feedback_sequence(), 1U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    ASSERT_EQ(feedback_sender->calls().size(), 1U);
    EXPECT_EQ(feedback_sender->calls()[0].session, binding);
    EXPECT_EQ(feedback_sender->calls()[0].feedback.source_sequence, 1U);
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
  }

  TEST(MultiseatMoonlightSessionBridge, RetriesAndPreservesANewerSameSlotState) {
    prepared_authority_t prepared {47};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      10,
      20
    )));
    feedback_sender->enqueue_result(
      moonlight_feedback_send_result_e::retry_later
    );
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::retry_later
    );
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);

    feedback_sender->set_on_send([feedback_source, binding](std::size_t call) {
      if (call == 2) {
        EXPECT_TRUE(feedback_source->publish(feedback_for(
          binding.handle,
          2,
          0,
          30,
          40
        )));
      }
    });
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);
    EXPECT_EQ(opened.bridge->last_feedback_sequence(), 2U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);

    const auto calls = feedback_sender->calls();
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[0].feedback.source_sequence, 1U);
    EXPECT_EQ(calls[1].feedback.source_sequence, 1U);
    EXPECT_EQ(calls[2].feedback.source_sequence, 2U);
    EXPECT_EQ(calls[2].feedback.message.data.rumble.lowfreq, 30U);
  }

  TEST(MultiseatMoonlightSessionBridge, RetryStaysWithinTheAllocationBound) {
    prepared_authority_t prepared {
      59,
      {.touch = false, .pen = false, .gamepad_slots = maximum_gamepad_slots}
    };
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(
      prepared.expectation,
      true,
      {.controller = true}
    );
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      10,
      20
    )));
    feedback_sender->enqueue_result(
      moonlight_feedback_send_result_e::retry_later
    );
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::retry_later
    );

    for (std::uint32_t slot = 1; slot < maximum_gamepad_slots; ++slot) {
      ASSERT_TRUE(feedback_source->publish(feedback_for(
        binding.handle,
        slot + 1,
        slot,
        static_cast<std::uint16_t>(slot),
        static_cast<std::uint16_t>(slot + 1)
      )));
    }
    EXPECT_EQ(
      opened.bridge->pending_feedback(),
      maximum_pending_controller_feedback
    );
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      maximum_gamepad_slots + 1,
      0,
      30,
      40
    )));
    EXPECT_EQ(
      opened.bridge->pending_feedback(),
      maximum_pending_controller_feedback
    );

    for (std::size_t index = 0; index < maximum_gamepad_slots; ++index) {
      EXPECT_EQ(
        opened.bridge->drain_feedback(),
        moonlight_session_drain_result_e::sent
      );
    }
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
    const auto calls = feedback_sender->calls();
    ASSERT_EQ(calls.size(), maximum_gamepad_slots + 1);
    EXPECT_EQ(calls.front().feedback.source_sequence, 1U);
    for (std::size_t index = 1; index < calls.size(); ++index) {
      EXPECT_EQ(calls[index].feedback.source_sequence, index + 1);
    }
  }

  TEST(MultiseatMoonlightSessionBridge, SenderClosureFailsTheSessionClosed) {
    prepared_authority_t prepared {48};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      1,
      2
    )));
    feedback_sender->enqueue_result(moonlight_feedback_send_result_e::closed);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sender_closed
    );
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
    EXPECT_EQ(feedback_source->detach_count(), 1U);
    EXPECT_EQ(
      opened.bridge->route_input(relative_packet(1, 0)).status,
      moonlight_route_status_e::session_closed
    );
  }

  TEST(MultiseatMoonlightSessionBridge, SenderExceptionFailsTheSessionClosed) {
    prepared_authority_t prepared {49};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      1,
      2
    )));
    feedback_sender->throw_next();
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sender_failed
    );
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_EQ(feedback_source->detach_count(), 1U);
  }

  TEST(MultiseatMoonlightSessionBridge, AllowsFeedbackReentryDuringInputRoute) {
    prepared_authority_t prepared {50};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    prepared.backend.set_on_route([feedback_source, binding]() {
      EXPECT_TRUE(feedback_source->publish(feedback_for(
        binding.handle,
        1,
        0,
        100,
        200
      )));
    });

    auto routed = std::async(std::launch::async, [bridge = opened.bridge]() {
      return bridge->route_input(relative_packet(1, 0));
    });
    ASSERT_EQ(routed.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(routed.get().status, moonlight_route_status_e::applied);
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);
  }

  TEST(MultiseatMoonlightSessionBridge, CloseWaitsForInputAndRejectsNewWork) {
    prepared_authority_t prepared {51};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    ASSERT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      5,
      6
    )));
    prepared.backend.block_route();
    auto routed = std::async(std::launch::async, [bridge = opened.bridge]() {
      return bridge->route_input(relative_packet(1, 0));
    });
    prepared.backend.wait_for_route();
    auto closing = std::async(std::launch::async, [bridge = opened.bridge]() {
      bridge->close();
    });
    feedback_source->wait_for_detach_start();

    EXPECT_FALSE(opened.bridge->accepting());
    EXPECT_TRUE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 0U);
    EXPECT_EQ(closing.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(
      opened.bridge->route_input(relative_packet(2, 0)).status,
      moonlight_route_status_e::session_closed
    );
    prepared.backend.release_route();
    ASSERT_EQ(routed.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(routed.get().status, moonlight_route_status_e::applied);
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
    EXPECT_FALSE(feedback_source->publish(feedback_for(
      binding.handle,
      2,
      0,
      7,
      8
    )));
  }

  TEST(MultiseatMoonlightSessionBridge, CloseWaitsForAnInvokedFeedbackCallback) {
    prepared_authority_t prepared {52};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    feedback_source->pause_next_callback();

    auto publishing = std::async(std::launch::async, [feedback_source, binding]() {
      return feedback_source->publish(feedback_for(
        binding.handle,
        1,
        0,
        1,
        2
      ));
    });
    feedback_source->wait_for_paused_callback();
    auto closing = std::async(std::launch::async, [bridge = opened.bridge]() {
      bridge->close();
    });
    feedback_source->wait_for_detach_start();
    EXPECT_TRUE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 0U);
    EXPECT_EQ(closing.wait_for(0ms), std::future_status::timeout);

    feedback_source->release_paused_callback();
    ASSERT_EQ(publishing.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(publishing.get());
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
    EXPECT_EQ(opened.bridge->last_feedback_sequence(), 0U);
  }

  TEST(MultiseatMoonlightSessionBridge, ConcurrentCloseIsIdempotent) {
    prepared_authority_t prepared {58};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    prepared.backend.block_route();
    auto routed = std::async(std::launch::async, [bridge = opened.bridge]() {
      return bridge->route_input(relative_packet(1, 0));
    });
    prepared.backend.wait_for_route();

    constexpr std::size_t close_calls = 8;
    std::vector<std::future<void>> closes;
    closes.reserve(close_calls);
    for (std::size_t index = 0; index < close_calls; ++index) {
      closes.push_back(std::async(std::launch::async, [bridge = opened.bridge]() {
        bridge->close();
      }));
    }
    feedback_source->wait_for_detach_start();
    EXPECT_FALSE(opened.bridge->accepting());
    EXPECT_TRUE(bindings.claimed());
    prepared.backend.release_route();

    ASSERT_EQ(routed.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(routed.get().status, moonlight_route_status_e::applied);
    for (auto &closing : closes) {
      ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
      closing.get();
    }
    EXPECT_TRUE(opened.bridge->closed());
    EXPECT_EQ(feedback_source->detach_count(), 1U);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);
  }

  TEST(MultiseatMoonlightSessionBridge, DestructionDetachesTheWeakCallback) {
    prepared_authority_t prepared {53};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    EXPECT_EQ(feedback_source->attach_count(), 1U);
    opened.bridge.reset();
    EXPECT_EQ(feedback_source->detach_count(), 1U);
    EXPECT_FALSE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      1,
      2
    )));
  }

  TEST(MultiseatMoonlightSessionBridge, SerializesConcurrentFeedbackDrains) {
    prepared_authority_t prepared {
      54,
      {.touch = false, .pen = false, .gamepad_slots = maximum_gamepad_slots}
    };
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(
      prepared.expectation,
      true,
      {.controller = true}
    );
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);
    for (std::uint32_t slot = 0; slot < maximum_gamepad_slots; ++slot) {
      ASSERT_TRUE(feedback_source->publish(feedback_for(
        binding.handle,
        slot + 1,
        slot,
        static_cast<std::uint16_t>(slot),
        static_cast<std::uint16_t>(slot + 1)
      )));
    }
    EXPECT_EQ(
      opened.bridge->pending_feedback(),
      maximum_pending_controller_feedback
    );

    constexpr std::size_t drain_calls = maximum_gamepad_slots * 2;
    std::vector<std::future<moonlight_session_drain_result_e>> drains;
    drains.reserve(drain_calls);
    for (std::size_t index = 0; index < drain_calls; ++index) {
      drains.push_back(std::async(std::launch::async, [bridge = opened.bridge]() {
        return bridge->drain_feedback();
      }));
    }
    std::size_t sent = 0;
    std::size_t empty = 0;
    for (auto &drain : drains) {
      const auto status = drain.get();
      sent += status == moonlight_session_drain_result_e::sent ? 1 : 0;
      empty += status == moonlight_session_drain_result_e::empty ? 1 : 0;
    }
    EXPECT_EQ(sent, maximum_gamepad_slots);
    EXPECT_EQ(empty, maximum_gamepad_slots);
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);

    const auto calls = feedback_sender->calls();
    ASSERT_EQ(calls.size(), maximum_gamepad_slots);
    for (std::size_t index = 0; index < calls.size(); ++index) {
      EXPECT_EQ(calls[index].feedback.source_sequence, index + 1);
    }
  }

  TEST(MultiseatMoonlightSessionBridge, RejectsStaleAndGappedFeedbackBeforeEgress) {
    prepared_authority_t prepared {55};
    fake_binding_source_t bindings;
    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    ASSERT_TRUE(opened.bridge);

    EXPECT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      prepared.expectation.plan.gamepad_slots,
      1,
      2
    )));
    EXPECT_TRUE(feedback_source->publish(feedback_for(
      handle_for(54),
      1,
      0,
      1,
      2
    )));
    EXPECT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      2,
      0,
      1,
      2
    )));
    EXPECT_EQ(opened.bridge->pending_feedback(), 0U);
    EXPECT_EQ(opened.bridge->last_feedback_sequence(), 0U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::empty
    );

    EXPECT_TRUE(feedback_source->publish(feedback_for(
      binding.handle,
      1,
      0,
      3,
      4
    )));
    EXPECT_EQ(opened.bridge->pending_feedback(), 1U);
    EXPECT_EQ(
      opened.bridge->drain_feedback(),
      moonlight_session_drain_result_e::sent
    );
    ASSERT_EQ(feedback_sender->calls().size(), 1U);
    EXPECT_EQ(feedback_sender->calls()[0].feedback.source_sequence, 1U);
  }

  TEST(MultiseatMoonlightSessionBridge, FailsClosedWhenFeedbackCannotAttach) {
    prepared_authority_t prepared {56};
    fake_binding_source_t bindings;
    auto feedback_sender = std::make_shared<fake_feedback_sender_t>();
    const auto binding = binding_for(prepared.expectation);

    auto feedback_source = std::make_shared<fake_feedback_source_t>();
    feedback_source->configure_attach_failure(true);
    auto opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::feedback_attach_failed
    );
    EXPECT_FALSE(opened.bridge);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 1U);

    feedback_source = std::make_shared<fake_feedback_source_t>();
    feedback_source->configure_attach_failure(false, true);
    opened = open_bridge(
      prepared,
      bindings,
      feedback_source,
      feedback_sender,
      binding
    );
    EXPECT_EQ(
      opened.status,
      moonlight_session_open_status_e::feedback_attach_failed
    );
    EXPECT_FALSE(opened.bridge);
    EXPECT_FALSE(bindings.claimed());
    EXPECT_EQ(bindings.detach_count(), 2U);
  }
}  // namespace
