#pragma once

#ifdef __linux__
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <condition_variable>

#include "src/capture_producer_binding.h"
#include "src/game_mode_adaptive_sync.h"

namespace platf::game_mode_feedback {
  using graph_reader_t = std::function<capture_producer::binding_t(std::chrono::milliseconds)>;
  struct probe_t {
    std::filesystem::path proc_root = "/proc";
    std::filesystem::path socket_dir = "/tmp/.X11-unix";
    std::filesystem::path os_release = "/etc/os-release";
  };

  // Performs two graph barriers around a process-generation and X-property snapshot. No writes to
  // properties/preferences, no settings changes and no subprocesses. Unknown is a normal result.
  game_mode_adaptive_sync::observation_t collect_once(const graph_reader_t &graph, const probe_t &probe = {});

  /** One joinable observer owned by the capture call. Stops publishing before it is destroyed. */
  class collector_t {
  public:
    explicit collector_t(graph_reader_t graph, probe_t probe = {});
    ~collector_t();
    collector_t(const collector_t &) = delete;
    collector_t &operator=(const collector_t &) = delete;
    std::shared_ptr<const game_mode_adaptive_sync::store_t> reader() const { return store_; }
  private:
    std::shared_ptr<game_mode_adaptive_sync::store_t> store_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stop_ = false;
    std::thread worker_;
  };
}
#endif
