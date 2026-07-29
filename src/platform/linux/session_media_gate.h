/**
 * @file src/platform/linux/session_media_gate.h
 * @brief Shared admission gate for media start and asynchronous teardown ownership.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace session_media {
  namespace detail {
    struct teardown_gate_state_t {
      std::mutex mutex;
      std::condition_variable changed;
      std::size_t start_owners = 0;
      std::size_t teardown_owners = 0;
    };
  }  // namespace detail

  class teardown_gate_t;

  class start_owner_t {
  public:
    start_owner_t() = default;
    ~start_owner_t() {
      reset();
    }

    start_owner_t(const start_owner_t &) = delete;
    start_owner_t &operator=(const start_owner_t &) = delete;

    start_owner_t(start_owner_t &&other) noexcept:
        state_ {std::exchange(other.state_, {})} {
    }

    start_owner_t &operator=(start_owner_t &&other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, {});
      }
      return *this;
    }

    bool owns_start() const {
      return static_cast<bool>(state_);
    }

    void reset() {
      if (!state_) {
        return;
      }
      auto state = std::exchange(state_, {});
      {
        std::lock_guard lock(state->mutex);
        if (state->start_owners > 0) {
          --state->start_owners;
        }
      }
      state->changed.notify_all();
    }

  private:
    friend class teardown_gate_t;
    explicit start_owner_t(std::shared_ptr<detail::teardown_gate_state_t> state):
        state_ {std::move(state)} {
    }

    std::shared_ptr<detail::teardown_gate_state_t> state_;
  };

  class teardown_owner_t {
  public:
    teardown_owner_t() = default;
    ~teardown_owner_t() {
      reset();
    }

    teardown_owner_t(const teardown_owner_t &) = delete;
    teardown_owner_t &operator=(const teardown_owner_t &) = delete;

    teardown_owner_t(teardown_owner_t &&other) noexcept:
        state_ {std::exchange(other.state_, {})} {
    }

    teardown_owner_t &operator=(teardown_owner_t &&other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, {});
      }
      return *this;
    }

    bool owns_teardown() const {
      return static_cast<bool>(state_);
    }

    void reset() {
      if (!state_) {
        return;
      }
      auto state = std::exchange(state_, {});
      {
        std::lock_guard lock(state->mutex);
        if (state->teardown_owners > 0) {
          --state->teardown_owners;
        }
      }
      state->changed.notify_all();
    }

  private:
    friend class teardown_gate_t;
    explicit teardown_owner_t(std::shared_ptr<detail::teardown_gate_state_t> state):
        state_ {std::move(state)} {
    }

    std::shared_ptr<detail::teardown_gate_state_t> state_;
  };

  class teardown_gate_t {
  public:
    teardown_gate_t():
        state_ {std::make_shared<detail::teardown_gate_state_t>()} {
    }

    start_owner_t begin_start() {
      std::unique_lock lock(state_->mutex);
      state_->changed.wait(lock, [this]() {
        return state_->teardown_owners == 0;
      });
      ++state_->start_owners;
      return start_owner_t {state_};
    }

    teardown_owner_t begin_teardown() {
      std::unique_lock lock(state_->mutex);
      ++state_->teardown_owners;
      state_->changed.notify_all();
      state_->changed.wait(lock, [this]() {
        return state_->start_owners == 0;
      });
      return teardown_owner_t {state_};
    }

    bool teardown_in_progress() const {
      std::lock_guard lock(state_->mutex);
      return state_->teardown_owners != 0;
    }

  private:
    std::shared_ptr<detail::teardown_gate_state_t> state_;
  };
}  // namespace session_media
