/**
 * @file src/stream_packet_owner.h
 * @brief Session ownership across encoded packet queues and synchronous sends.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace stream_packets {
  namespace detail {
    struct state_t {
      explicit state_t(void *session): session {session} {}

      void *const session;
      std::mutex mutex;
      std::condition_variable changed;
      std::size_t deliveries = 0;
      bool closed = false;
    };
  }

  class destination_t;
  class owner_t;

  /** Holds one admitted send through its last access to the session. */
  class delivery_t {
  public:
    delivery_t() = default;
    ~delivery_t() { reset(); }
    delivery_t(const delivery_t &) = delete;
    delivery_t &operator=(const delivery_t &) = delete;
    delivery_t(delivery_t &&other) noexcept:
        state_ {std::move(other.state_)} {}
    delivery_t &operator=(delivery_t &&other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::move(other.state_);
      }
      return *this;
    }

    [[nodiscard]] void *get() const noexcept {
      return state_ ? state_->session : nullptr;
    }
    explicit operator bool() const noexcept { return get() != nullptr; }

    void reset() noexcept {
      if (auto state = std::exchange(state_, {})) {
        std::lock_guard lock {state->mutex};
        if (--state->deliveries == 0) {
          state->changed.notify_all();
        }
      }
    }

  private:
    friend class destination_t;
    explicit delivery_t(std::shared_ptr<detail::state_t> state):
        state_ {std::move(state)} {}
    std::shared_ptr<detail::state_t> state_;
  };

  /**
   * Copyable packet destination. Queued copies do not keep a session alive.
   * Only acquire() grants access, and closing the owner permanently rejects
   * new deliveries, even if a later session occupies the same address.
   */
  class destination_t {
  public:
    destination_t() = default;
    destination_t(std::nullptr_t) noexcept {}

    [[nodiscard]] delivery_t acquire() const {
      if (!state_) {
        return {};
      }
      std::lock_guard lock {state_->mutex};
      if (state_->closed || !state_->session) {
        return {};
      }
      ++state_->deliveries;
      return delivery_t {state_};
    }

    // Opaque equality tag for capture-start cancellation only. Never dereference
    // it: the session may already be closed or destroyed.
    [[nodiscard]] const void *capture_owner_tag() const noexcept {
      return state_ ? state_->session : nullptr;
    }

  private:
    friend class owner_t;
    explicit destination_t(std::shared_ptr<detail::state_t> state):
        state_ {std::move(state)} {}
    std::shared_ptr<detail::state_t> state_;
  };

  /**
   * One owner per session allocation; there is deliberately no reopen/reset.
   * The owner must close and wait before destroying any session resources.
   * close() never waits, so capture/control callbacks may retire a session.
   * close_and_wait() belongs to teardown, outside an admitted delivery.
   */
  class owner_t {
  public:
    explicit owner_t(void *session):
        state_ {std::make_shared<detail::state_t>(session)} {}
    ~owner_t() { close_and_wait(); }
    owner_t(const owner_t &) = delete;
    owner_t &operator=(const owner_t &) = delete;
    owner_t(owner_t &&) = delete;
    owner_t &operator=(owner_t &&) = delete;

    [[nodiscard]] destination_t destination() const { return destination_t {state_}; }

    void close() noexcept {
      std::lock_guard lock {state_->mutex};
      state_->closed = true;
    }

    void close_and_wait() noexcept {
      std::unique_lock lock {state_->mutex};
      state_->closed = true;
      state_->changed.wait(lock, [this] { return state_->deliveries == 0; });
    }

  private:
    const std::shared_ptr<detail::state_t> state_;
  };
}
