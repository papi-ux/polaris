#pragma once

#include <cstdint>
#include <unordered_map>

namespace pipewire_capture {
  // Used only while holding the PipeWire loop lock. A buffer address identifies
  // storage, not an individual delivery; a producer may resend held storage.
  class buffer_generation_t {
  public:
    std::uint64_t acquire(const void *buffer) {
      const auto generation = ++next_;
      generations_[buffer] = generation;
      return generation;
    }

    bool complete(const void *buffer, std::uint64_t generation) {
      const auto found = generations_.find(buffer);
      if (!generation || found == generations_.end() || found->second != generation) {
        return false;
      }
      found->second = 0;
      return true;
    }

    void remove(const void *buffer) {
      generations_.erase(buffer);
    }

    // Keep the serial monotonic across renegotiation and address reuse.
    void clear() {
      generations_.clear();
    }

  private:
    std::uint64_t next_ = 0;
    std::unordered_map<const void *, std::uint64_t> generations_;
  };
}
