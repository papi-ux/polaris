/**
 * @file src/platform/linux/input/inputtino_gamepad_readiness.h
 * @brief Bounded wait for a virtual gamepad's own input nodes, and the retry back-off after they never appear.
 *
 * A virtual DualSense is a UHID device. inputtino's create() returns once the device is written to
 * /dev/uhid; hid-playstation then registers the gamepad, motion and touchpad inputs asynchronously,
 * so a node list read as create() returns can be empty. Strict bubblewrap isolation binds exactly
 * the registered nodes at launch, which would leave a private game with no controller.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::gamepad::readiness {
  using time_point_t = std::chrono::steady_clock::time_point;

  /**
   * Once hid-playstation is loaded it registers all of a pad's inputs within milliseconds of
   * create(). The timeout also covers udev loading the driver for the first pad after boot.
   */
  struct policy_t {
    std::chrono::milliseconds timeout {2000};
    std::chrono::milliseconds poll_interval {20};
    std::chrono::milliseconds settle {50};  ///< How long the node set must stay the same to be complete.
  };

  enum class status_e {
    ready,  ///< The owned nodes, including an event node, stayed the same for the settle time.
    partial,  ///< Owned nodes existed at the timeout but had not settled.
    empty,  ///< No owned node existed at the timeout.
    error,  ///< The last probe failed, so no node could be confirmed.
  };

  struct result_t {
    status_e status = status_e::empty;
    std::vector<std::string> nodes;  ///< Sorted owned nodes that existed at the last successful probe.
    std::chrono::milliseconds waited {0};
    int probes = 0;
    std::string detail;

    bool usable() const {
      return (status == status_e::ready || status == status_e::partial) && !nodes.empty();
    }
  };

  struct environment_t {
    std::function<std::vector<std::string>()> probe;  ///< Lists the device's own nodes; may throw.
    std::function<bool(const std::string &)> node_exists;
    std::function<time_point_t()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
  };

  inline bool has_numeric_suffix(std::string_view node, std::string_view prefix) {
    if (node.size() <= prefix.size() || node.substr(0, prefix.size()) != prefix) {
      return false;
    }

    return std::all_of(node.begin() + static_cast<std::ptrdiff_t>(prefix.size()), node.end(), [](unsigned char ch) {
      return ch >= '0' && ch <= '9';
    });
  }

  inline bool is_event_node(std::string_view node) {
    return has_numeric_suffix(node, "/dev/input/event");
  }

  /// The node kinds the isolation registry accepts.
  inline bool is_device_node(std::string_view node) {
    return is_event_node(node) || has_numeric_suffix(node, "/dev/input/js") || has_numeric_suffix(node, "/dev/hidraw");
  }

  /// The listed nodes that are device nodes and exist, sorted and unique. Nothing outside the list is considered.
  inline std::vector<std::string> present_owned_nodes(const std::vector<std::string> &listed, const std::function<bool(const std::string &)> &node_exists) {
    std::vector<std::string> present;
    for (const auto &node : listed) {
      if (is_device_node(node) && node_exists(node)) {
        present.push_back(node);
      }
    }

    std::sort(present.begin(), present.end());
    present.erase(std::unique(present.begin(), present.end()), present.end());
    return present;
  }

  /**
   * Probe the device's own nodes until they include an event node and stay the same for
   * `policy.settle`, or until `policy.timeout` has passed. Returns early once ready.
   */
  inline result_t wait_for_owned_nodes(const environment_t &env, const policy_t &policy) {
    using std::chrono::milliseconds;

    const auto start = env.now();
    const auto deadline = start + policy.timeout;
    const auto interval = std::max(policy.poll_interval, milliseconds {1});
    // Bounded by probe count as well, so a clock that does not advance cannot extend the wait.
    const auto max_probes = policy.timeout / interval + 2;

    result_t result;
    std::vector<std::string> seen;
    auto unchanged_since = start;
    bool last_probe_failed = false;

    for (;;) {
      ++result.probes;
      std::vector<std::string> present;
      try {
        present = present_owned_nodes(env.probe(), env.node_exists);
        last_probe_failed = false;
      } catch (const std::exception &e) {
        last_probe_failed = true;
        result.detail = e.what();
      } catch (...) {
        last_probe_failed = true;
        result.detail = "unknown error while listing the device's nodes";
      }

      const auto now = env.now();
      result.waited = std::chrono::duration_cast<milliseconds>(now - start);
      if (last_probe_failed) {
        unchanged_since = now;
      } else if (present != seen) {
        seen = std::move(present);
        unchanged_since = now;
      }

      if (!last_probe_failed && std::any_of(seen.begin(), seen.end(), is_event_node) && now - unchanged_since >= policy.settle) {
        result.status = status_e::ready;
        result.nodes = seen;
        return result;
      }

      if (now >= deadline || result.probes >= max_probes) {
        break;
      }
      env.sleep(std::min(interval, std::chrono::ceil<milliseconds>(deadline - now)));
    }

    if (last_probe_failed) {
      result.status = status_e::error;
    } else if (!seen.empty()) {
      result.status = status_e::partial;
      result.nodes = seen;
      result.detail = std::any_of(seen.begin(), seen.end(), is_event_node) ? "its input nodes were still changing" : "it has no event node";
    } else {
      result.status = status_e::empty;
      result.detail = "none of its input nodes appeared";
    }
    return result;
  }

  /**
   * Spaces out new attempts after a device's nodes never appeared. Client input retries allocation
   * for every controller report, so each attempt would otherwise stall input for the whole timeout.
   * The delay doubles with each consecutive failure, up to `longest`, and a success clears it.
   */
  class backoff_t {
  public:
    explicit backoff_t(std::chrono::seconds first = std::chrono::seconds {30}, std::chrono::seconds longest = std::chrono::minutes {5}):
        first_ {first},
        longest_ {longest} {
    }

    bool blocks(time_point_t now) const {
      return failures_ > 0 && now < retry_at_;
    }

    time_point_t retry_at() const {
      return retry_at_;
    }

    void failed(time_point_t now) {
      auto delay = first_;
      for (int i = 0; i < failures_ && delay < longest_; ++i) {
        delay *= 2;
      }
      retry_at_ = now + std::min(delay, longest_);
      ++failures_;
    }

    void succeeded() {
      failures_ = 0;
    }

  private:
    std::chrono::seconds first_;
    std::chrono::seconds longest_;
    int failures_ = 0;
    time_point_t retry_at_ {};
  };
}  // namespace platf::gamepad::readiness
