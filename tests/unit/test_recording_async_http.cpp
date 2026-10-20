/**
 * @file tests/unit/test_recording_async_http.cpp
 * @brief Exercise asynchronous host-admin replay saves through production recording routes:
 * request-executor responsiveness, admission, worker failure, shutdown and disconnect.
 *
 * Saved bytes are synthetic raw video; nothing here claims a playable clip, audio or container.
 */
#include "../tests_common.h"

#include <Simple-Web-Server/client_https.hpp>
#include <nlohmann/json.hpp>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/stream_recorder.h>
#include <src/utility.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace confighttp {
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
                                  const std::function<void(const std::string &)> &);
}

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using stream_recorder::detail::save_status_e;
  using stream_recorder::window_limit_e;

  constexpr std::uint64_t k_session = 402;
  constexpr int k_hevc = 1;
  constexpr auto k_api_key = "recording-status-fixture-key";
  constexpr auto k_csrf = "recording-status-fixture-csrf";
  constexpr auto k_stamp = "20261008_230000";
  constexpr auto k_bounded_wait = 10s;

  [[maybe_unused]] std::set<std::string> keys_of(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) {
      keys.insert(it.key());
    }
    return keys;
  }

  [[maybe_unused]] steady::duration from_us(std::int64_t microseconds) {
    return std::chrono::duration_cast<steady::duration>(std::chrono::microseconds {microseconds});
  }

  /// Holds a save inside storage until released. Release is idempotent.
  class barrier_t {
  public:
    void hold() {
      std::unique_lock lock {mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [this] { return released; });
    }

    bool wait_entered() {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, k_bounded_wait, [this] { return entered; });
    }

    void release() {
      {
        std::lock_guard lock {mutex};
        released = true;
      }
      changed.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
  };

  struct reply_t {
    int code = 0;
    nlohmann::json body;
  };

  /**
   * Each test starts from the recorder's process start state, in replay mode with one eligible
   * session, saving into a not-yet-created directory inside a parent created exclusively, with
   * fixture-owned admin credentials. Configuration is written only from the test thread, and
   * only while no listener runs. TearDown releases a held save and joins it before it restores
   * the recorder and configuration and removes the owned parent; listeners are stopped and
   * joined by with_server() on every path.
   */
  class RecordingAsyncHttpTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      saved_username = config::sunshine.username;
      saved_api_key = config::sunshine.api_key;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-recording-async-http-"} + info->name() + "-" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                       std::to_string(attempt));
        std::error_code ec;
        // create_directory() reports true only when this call made the directory.
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      const auto credentials = crypto::gen_creds("localhost", 2048);
      cert = owned_parent / "cert.pem";
      key = owned_parent / "key.pem";
      std::ofstream(cert) << credentials.x509;
      std::ofstream(key) << credentials.pkey;
      fs::permissions(key, fs::perms::owner_read | fs::perms::owner_write);
      config::sunshine.username = "recording-status-admin";
      config::sunshine.api_key = k_api_key;
      output_dir = owned_parent / "recordings";
      config::recording.output_dir = output_dir.string();
      config::recording.replay_buffer_minutes = 1;
      publish(true, true);
      stream_recorder::session_started(k_session, true);
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    void TearDown() override {
      barrier.release();
      confighttp::set_recording_post_for_tests({});
      confighttp::set_recording_commit_observer_for_tests({});
      confighttp::set_recording_before_launch_for_tests({});
      if (saver.joinable()) {
        saver.join();
      }
      if (stream_recorder::is_recording()) {
        stream_recorder::stop_recording();
      }
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_recording;
      config::sunshine.username = saved_username;
      config::sunshine.api_key = saved_api_key;
      stream_recorder::load_config();
      if (!owned_parent.empty()) {
        std::error_code ec;
        fs::remove_all(owned_parent, ec);
      }
    }

    static void publish(bool enabled, bool replay_buffer) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay_buffer;
      stream_recorder::load_config();
    }

    static void push(bool keyframe, std::size_t size = 10) {
      const std::vector<std::uint8_t> payload(size, 0x5a);
      stream_recorder::push_packet(payload.data(), payload.size(), true, keyframe, 0, k_session, k_hevc);
    }

    void install() {
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    /// Start a save on another thread that stops inside storage, after its snapshot, until released.
    void start_held_save() {
      hooks.before_write = [this](const fs::path &) {
        barrier.hold();
      };
      install();
      saver = std::thread([this] {
        held_result = stream_recorder::save_replay_result();
      });
    }

    stream_recorder::detail::save_result_t finish_held_save() {
      barrier.release();
      confighttp::set_recording_post_for_tests({});
      confighttp::set_recording_commit_observer_for_tests({});
      confighttp::set_recording_before_launch_for_tests({});
      if (saver.joinable()) {
        saver.join();
      }
      return held_result;
    }

    /**
     * Run `body` against the production recording routes on a loopback TLS listener, inside a
     * real web session with a scoped CSRF token. The listener is stopped and joined on every
     * path out, including a failed assertion in `body`.
     */
    void with_server(const std::function<void(const std::string &cookie)> &body) {
      confighttp::with_web_session_for_tests(owned_parent / "sessions.json", k_csrf, [&](const std::string &cookie) {
        std::mutex ready_mutex;
        std::condition_variable ready_changed;
        std::function<void()> stop;
        bool failed = false;
        port = 0;
        std::thread server([&] {
          try {
            confighttp::run_recording_routes_for_tests(cert.string(), key.string(),
              [&](unsigned short assigned, std::function<void()> stop_server) {
                std::lock_guard lock {ready_mutex};
                stop = [once = std::make_shared<std::atomic<bool>>(false), stop_server = std::move(stop_server)] {
                  if (!once->exchange(true)) stop_server();
                };
                stop_listener = stop;
                listener_thread = std::this_thread::get_id();
                port = assigned;
                ready_changed.notify_all();
              });
            listener_finished = true;
          } catch (...) {
            listener_finished = true;
            std::lock_guard lock {ready_mutex};
            failed = true;
            ready_changed.notify_all();
          }
        });
        auto shutdown = util::fail_guard([&] {
          barrier.release();
          std::function<void()> stop_now;
          {
            std::lock_guard lock {ready_mutex};
            stop_now = stop;
          }
          if (stop_now) {
            stop_now();
          }
          server.join();
          for (auto &client : clients) if (client.joinable()) client.join();
          clients.clear();
          stop_listener = {};
        });
        {
          std::unique_lock lock {ready_mutex};
          ASSERT_TRUE(ready_changed.wait_for(lock, k_bounded_wait, [&] { return port != 0 || failed; }));
          ASSERT_FALSE(failed) << "the recording routes listener did not start";
        }
        body(cookie);
      });
    }

    /// One request with a bounded timeout; a transport failure is a test failure, never a hang.
    reply_t request(const std::string &method, const std::string &path, SimpleWeb::CaseInsensitiveMultimap headers = {}, bool allow_transport_failure = false, int timeout = 5) {
      reply_t reply;
      try {
        SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
        client.config.timeout = timeout;
        client.config.timeout_connect = 5;
        auto response = client.request(method, path, "", headers);
        reply.code = std::stoi(response->status_code);
        reply.body = nlohmann::json::parse(response->content.string(), nullptr, false);
      } catch (const std::exception &error) {
        if (!allow_transport_failure) ADD_FAILURE() << method << ' ' << path << " failed: " << error.what();
      }
      return reply;
    }

    std::future<reply_t> start_request(bool allow_transport_failure = false, int timeout = 5) {
      auto task = std::make_shared<std::packaged_task<reply_t()>>([this, allow_transport_failure, timeout] {
        return request("POST", "/api/recording/save-replay", bearer(), allow_transport_failure, timeout);
      });
      auto result = task->get_future();
      clients.emplace_back([task] { (*task)(); });
      return result; // packaged_task futures never block destruction during a failed assertion.
    }

    static SimpleWeb::CaseInsensitiveMultimap bearer() {
      return {{"Authorization", std::string {"Bearer "} + k_api_key}};
    }

    config::recording_t saved_recording;
    std::string saved_username;
    std::string saved_api_key;
    fs::path owned_parent;
    fs::path output_dir;
    fs::path cert;
    fs::path key;
    stream_recorder::detail::save_hooks_t hooks;
    barrier_t barrier;
    std::thread saver;
    stream_recorder::detail::save_result_t held_result;
    unsigned short port = 0;
    std::vector<std::thread> clients;
    std::thread::id listener_thread;
    std::function<void()> stop_listener;
    std::atomic<bool> listener_finished {false};
  };
}  // namespace

TEST_F(RecordingAsyncHttpTests, HeldHttpSaveLeavesStatusAndBusyRequestResponsive) {
  push(true, 4);
  push(false, 3);
  std::thread::id storage_thread;
  std::atomic<int> commits {0};
  std::atomic<bool> wrong_commit_thread {false};
  hooks.before_write = [&](const fs::path &) { storage_thread = std::this_thread::get_id(); barrier.hold(); };
  install();
  confighttp::set_recording_commit_observer_for_tests([&] {
    wrong_commit_thread = std::this_thread::get_id() != listener_thread;
    ++commits;
  });
  with_server([&](const std::string &) {
    auto first = start_request();
    ASSERT_TRUE(barrier.wait_entered());
    EXPECT_NE(storage_thread, listener_thread);
    auto status = request("GET", "/api/recording/status", bearer());
    ASSERT_EQ(status.code, 200);
    EXPECT_EQ(status.body["replay"]["save"]["in_progress"], true);
    EXPECT_EQ(status.body["replay"]["save"]["snapshot"]["packets"], 2);
    auto busy = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(busy.code, 200);
    EXPECT_EQ(busy.body["outcome"], "busy");
    EXPECT_TRUE(busy.body["captured"].is_null());
    EXPECT_EQ(first.wait_for(0ms), std::future_status::timeout);
    barrier.release();
    ASSERT_EQ(first.wait_for(k_bounded_wait), std::future_status::ready);
    auto saved = first.get();
    ASSERT_EQ(saved.code, 200);
    EXPECT_EQ(saved.body["outcome"], "saved");
    EXPECT_EQ(saved.body["status"], true);
    EXPECT_EQ(saved.body["kind"], "raw_video_bitstream");
    EXPECT_EQ(saved.body["captured"]["bytes"], 7);
    EXPECT_TRUE(fs::exists(saved.body["file"].get<std::string>()));
  });
  EXPECT_EQ(commits, 1);
  EXPECT_FALSE(wrong_commit_thread);
  auto status = stream_recorder::recorder_status().replay;
  EXPECT_EQ(status.saves_completed, 1u);
  EXPECT_EQ(status.saves_refused_busy, 1u);
  EXPECT_FALSE(status.save_in_progress);
  EXPECT_EQ(status.snapshot_packets, 0u);
}

TEST_F(RecordingAsyncHttpTests, SubmissionFailureKeepsCapturedFieldsWithoutStartingStorage) {
  push(true, 4);
  std::atomic<int> attempts {0}, writes {0};
  confighttp::set_recording_before_launch_for_tests([&] { if (++attempts == 1) throw std::bad_alloc {}; });
  hooks.before_write = [&](const fs::path &) { ++writes; };
  install();
  with_server([&](const std::string &) {
    auto failed = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(failed.code, 200);
    EXPECT_EQ(failed.body["outcome"], "storage_error");
    EXPECT_EQ(failed.body["status"], false);
    EXPECT_EQ(failed.body["file"], "");
    EXPECT_EQ(failed.body["captured"]["bytes"], 4);
    EXPECT_FALSE(failed.body.contains("kind"));
    EXPECT_EQ(writes, 0);
    EXPECT_FALSE(fs::exists(output_dir));
    auto status = stream_recorder::recorder_status().replay;
    EXPECT_FALSE(status.save_in_progress);
    EXPECT_EQ(status.snapshot_packets, 0u);
    EXPECT_EQ(status.saves_failed, 1u);
    auto retry = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(retry.code, 200);
    EXPECT_EQ(retry.body["outcome"], "saved");
  });
  EXPECT_EQ(attempts, 2);
  EXPECT_EQ(writes, 1);
}

TEST_F(RecordingAsyncHttpTests, CompletionBacklogCanFillWorkersAfterSnapshotsWereReleased) {
  push(true, 4);
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::function<void()>> queued;
  std::atomic<int> writes {0};
  hooks.before_write = [&](const fs::path &) { ++writes; };
  install();
  // Controlled completion queue only for saturation. Default executor affinity
  // is verified separately by HeldHttpSaveLeavesStatusAndBusyRequestResponsive.
  confighttp::set_recording_post_for_tests([&](auto callback) {
    std::lock_guard lock(mutex);
    queued.push_back(std::move(callback));
    changed.notify_all();
  });
  auto wait_queued = [&](std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, k_bounded_wait, [&] { return queued.size() >= count; });
  };
  with_server([&](const std::string &) {
    auto first = start_request();
    ASSERT_TRUE(wait_queued(1));
    EXPECT_FALSE(stream_recorder::recorder_status().replay.save_in_progress);
    auto second = start_request();
    ASSERT_TRUE(wait_queued(2));
    auto status = request("GET", "/api/recording/status", bearer());
    ASSERT_EQ(status.code, 200);
    EXPECT_EQ(status.body["replay"]["save"]["in_progress"], false);
    auto third = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(third.code, 200);
    EXPECT_EQ(third.body["outcome"], "storage_error");
    EXPECT_EQ(third.body["captured"]["bytes"], 4);
    EXPECT_EQ(writes, 2);
    auto snapshot = stream_recorder::recorder_status().replay;
    EXPECT_FALSE(snapshot.save_in_progress);
    EXPECT_EQ(snapshot.snapshot_packets, 0u);
    EXPECT_EQ(snapshot.saves_completed, 2u);
    EXPECT_EQ(snapshot.saves_failed, 1u);
    std::vector<std::function<void()>> callbacks;
    { std::lock_guard lock(mutex); callbacks.swap(queued); }
    for (auto &callback : callbacks) callback();
    ASSERT_EQ(first.wait_for(k_bounded_wait), std::future_status::ready);
    ASSERT_EQ(second.wait_for(k_bounded_wait), std::future_status::ready);
    EXPECT_EQ(first.get().body["outcome"], "saved");
    EXPECT_EQ(second.get().body["outcome"], "saved");
  });
}

TEST_F(RecordingAsyncHttpTests, ShutdownFencesCompletionsThenDrainsAlreadyAdmittedStorage) {
  push(true, 4);
  std::atomic<int> commits {0};
  hooks.before_write = [&](const fs::path &) { barrier.hold(); };
  install();
  confighttp::set_recording_commit_observer_for_tests([&] { ++commits; });
  with_server([&](const std::string &) {
    auto first = start_request(true);
    ASSERT_TRUE(barrier.wait_entered());
    stop_listener();
    EXPECT_TRUE(stream_recorder::recorder_status().replay.save_in_progress);
    EXPECT_FALSE(listener_finished);
    barrier.release();
    ASSERT_EQ(first.wait_for(k_bounded_wait), std::future_status::ready);
    first.get(); // Delivery after listener stop is intentionally not asserted.
  });
  EXPECT_TRUE(listener_finished);
  EXPECT_EQ(commits, 0);
  auto status = stream_recorder::recorder_status().replay;
  EXPECT_FALSE(status.save_in_progress);
  EXPECT_EQ(status.snapshot_packets, 0u);
  EXPECT_EQ(status.saves_completed, 1u);
  EXPECT_TRUE(fs::exists(output_dir));
}

TEST_F(RecordingAsyncHttpTests, DisconnectedAdmittedClientDoesNotCancelStorageOrBlockLaterSave) {
  push(true, 4);
  std::atomic<int> commits {0};
  std::mutex mutex;
  std::condition_variable changed;
  hooks.before_write = [&](const fs::path &) { barrier.hold(); };
  install();
  confighttp::set_recording_commit_observer_for_tests([&] {
    std::lock_guard lock(mutex);
    ++commits;
    changed.notify_all();
  });
  with_server([&](const std::string &) {
    auto first = start_request(true, 1); // Owned client closes its socket on timeout.
    ASSERT_TRUE(barrier.wait_entered());
    ASSERT_EQ(first.wait_for(k_bounded_wait), std::future_status::ready);
    EXPECT_EQ(first.get().code, 0);
    barrier.release();
    {
      std::unique_lock lock(mutex);
      ASSERT_TRUE(changed.wait_for(lock, k_bounded_wait, [&] { return commits.load() >= 1; }));
    }
    auto retry = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(retry.code, 200);
    EXPECT_EQ(retry.body["outcome"], "saved");
  });
  EXPECT_EQ(stream_recorder::recorder_status().replay.saves_completed, 2u);
  EXPECT_FALSE(stream_recorder::recorder_status().replay.save_in_progress);
}

TEST_F(RecordingAsyncHttpTests, FailedCompletionPostCannotReclassifyPublishedStorage) {
  push(true, 4);
  std::promise<void> post_attempted;
  auto posted = post_attempted.get_future();
  confighttp::set_recording_post_for_tests([&](auto) {
    post_attempted.set_value();
    throw std::bad_alloc {};
  });
  with_server([&](const std::string &) {
    auto first = start_request(true, 1);
    ASSERT_EQ(posted.wait_for(k_bounded_wait), std::future_status::ready);
    ASSERT_EQ(first.wait_for(k_bounded_wait), std::future_status::ready);
    first.get(); // No structured failure reply is invented after publication.
    auto status = request("GET", "/api/recording/status", bearer());
    ASSERT_EQ(status.code, 200);
    auto replay = stream_recorder::recorder_status().replay;
    EXPECT_EQ(replay.saves_completed, 1u);
    EXPECT_EQ(replay.saves_failed, 0u);
    EXPECT_FALSE(replay.save_in_progress);
    EXPECT_TRUE(fs::exists(output_dir));
  });
}

TEST_F(RecordingAsyncHttpTests, AdmissionFailureNeverSubmitsAndHasNullCapturedFields) {
  push(true, 4);
  std::atomic<int> launches {0};
  hooks.after_snapshot_payloads = [] { throw std::bad_alloc {}; };
  install();
  confighttp::set_recording_before_launch_for_tests([&] { ++launches; });
  with_server([&](const std::string &) {
    auto failed = request("POST", "/api/recording/save-replay", bearer());
    ASSERT_EQ(failed.code, 200);
    EXPECT_EQ(failed.body["outcome"], "storage_error");
    EXPECT_TRUE(failed.body["captured"].is_null());
    EXPECT_EQ(launches, 0);
    EXPECT_FALSE(fs::exists(output_dir));
    EXPECT_FALSE(stream_recorder::recorder_status().replay.save_in_progress);
  });
}

TEST_F(RecordingAsyncHttpTests, RejectedAuthAndCsrfNeverAdmitButValidBearerCanSave) {
  push(true, 4);
  std::atomic<int> launches {0};
  confighttp::set_recording_before_launch_for_tests([&] { ++launches; });
  with_server([&](const std::string &cookie) {
    const std::string path = "/api/recording/save-replay";
    EXPECT_EQ(request("POST", path).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "wrong"}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"X-CSRF-Token", k_csrf}}).code, 401);
    EXPECT_EQ(request("POST", path, {{"Authorization", "Bearer wrong"}}).code, 403);
    EXPECT_EQ(request("GET", "/api/recording/status").code, 401);
    EXPECT_EQ(launches, 0);
    EXPECT_FALSE(fs::exists(output_dir));
    auto saved = request("POST", path, bearer());
    ASSERT_EQ(saved.code, 200);
    EXPECT_EQ(saved.body["outcome"], "saved");
    EXPECT_EQ(launches, 1);
    auto session_save = request("POST", path, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", k_csrf}});
    ASSERT_EQ(session_save.code, 200);
    EXPECT_EQ(session_save.body["outcome"], "saved");
    EXPECT_EQ(launches, 2);
  });
}
