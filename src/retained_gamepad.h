/**
 * @file src/retained_gamepad.h
 * @brief Keep a private game's bound controller alive between stream connections.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace input {
  class retained_gamepad_t {
  public:
    using callback_t = std::function<void(int)>;

    retained_gamepad_t(int id, std::string owner, callback_t neutralize, callback_t destroy):
        id_(id), owner_(std::move(owner)), neutralize_(std::move(neutralize)),
        destroy_(std::move(destroy)) {}

    ~retained_gamepad_t() {
      destroy_(id_);
    }

    retained_gamepad_t(const retained_gamepad_t &) = delete;
    retained_gamepad_t &operator=(const retained_gamepad_t &) = delete;

    // Only one authenticated owner's stream can drive this game's controller.
    std::uint64_t acquire(std::string_view owner) {
      std::lock_guard lock(mutex_);
      if (retired_ || owner.empty() || owner != owner_ || active_ != 0) {
        return 0;
      }
      active_ = ++generation_;
      return active_;
    }

    class access_t {
    public:
      access_t() = default;  // Ordinary, stream-owned controller.
      access_t(std::unique_lock<std::mutex> lock, bool valid):
          lock_(std::move(lock)), valid_(valid) {}
      explicit operator bool() const { return valid_; }
    private:
      std::unique_lock<std::mutex> lock_;
      bool valid_ = true;
    };

    access_t access(std::uint64_t generation) {
      std::unique_lock lock(mutex_);
      const bool valid = !retired_ && generation != 0 && generation == active_;
      return {std::move(lock), valid};
    }

    void release(std::uint64_t generation) {
      std::lock_guard lock(mutex_);
      if (generation != 0 && generation == active_) {
        neutralize_(id_);
        active_ = 0;
      }
    }

    void retire() {
      std::lock_guard lock(mutex_);
      if (!retired_) {
        retired_ = true;
        neutralize_(id_);
        active_ = 0;
      }
    }

    int id() const { return id_; }

  private:
    const int id_;
    const std::string owner_;
    const callback_t neutralize_;
    const callback_t destroy_;
    std::mutex mutex_;
    std::uint64_t generation_ = 0;
    std::uint64_t active_ = 0;
    bool retired_ = false;
  };
}
