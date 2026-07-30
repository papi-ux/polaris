/**
 * @file src/platform/gamepad_feedback_router.h
 * @brief Stable feedback routing for virtual gamepads that outlive a client session queue.
 */
#pragma once

#include <chrono>
#include <mutex>
#include <utility>

#include "src/platform/common.h"

namespace platf {
  /**
   * Keeps virtual-device callbacks bound to one stable object while allowing their
   * destination mail queue to change when a preallocated gamepad is adopted.
   */
  class gamepad_feedback_router_t {
  public:
    explicit gamepad_feedback_router_t(feedback_queue_t queue):
        queue_ {std::move(queue)} {
    }

    void raise(gamepad_feedback_msg_t message) {
      std::scoped_lock lock {mutex_};
      if (queue_) {
        queue_->raise(std::move(message));
      }
    }

    void rebind(feedback_queue_t queue) {
      std::scoped_lock lock {mutex_};
      if (queue_ == queue) {
        return;
      }

      if (queue) {
        while (queue_) {
          auto message = queue_->pop(std::chrono::milliseconds {0});
          if (!message) {
            break;
          }
          queue->raise(std::move(*message));
        }
      }
      queue_ = std::move(queue);
    }

  private:
    std::mutex mutex_;
    feedback_queue_t queue_;
  };
}  // namespace platf
