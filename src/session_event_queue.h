/**
 * @file src/session_event_queue.h
 * @brief Small sequence-aware backlog for Polaris session SSE events.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace session_event_queue {
  class event_queue {
  public:
    explicit event_queue(std::size_t max_events = 128):
        max_events_ {max_events == 0 ? 1 : max_events} {
    }

    std::uint64_t cursor() const {
      return next_seq_;
    }

    std::uint64_t push(std::string payload) {
      const auto seq = ++next_seq_;
      events_.emplace_back(seq, std::move(payload));
      prune();
      return seq;
    }

    std::vector<std::string> events_after(std::uint64_t cursor) const {
      std::vector<std::string> result;
      for (const auto &entry : events_) {
        if (entry.first > cursor) {
          result.push_back(entry.second);
        }
      }
      return result;
    }

  private:
    void prune() {
      while (events_.size() > max_events_) {
        events_.erase(events_.begin());
      }
    }

    std::size_t max_events_;
    std::uint64_t next_seq_ {0};
    std::vector<std::pair<std::uint64_t, std::string>> events_;
  };
}
