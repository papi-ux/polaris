/**
 * @file tests/unit/test_recording_status_http.cpp
 * @brief Test the host-admin recording status and replay-save results: the JSON the production
 * builders assemble, the one-lock recorder snapshot behind them, and the production recording
 * routes (CSRF wrapper and handler authentication) over a real loopback TLS listener.
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
#include <mutex>
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

  constexpr std::uint64_t k_session = 401;
  constexpr int k_hevc = 1;
  constexpr auto k_api_key = "recording-status-fixture-key";
  constexpr auto k_csrf = "recording-status-fixture-csrf";
  constexpr auto k_stamp = "20261008_230000";
  constexpr auto k_bounded_wait = 10s;

  std::set<std::string> keys_of(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) {
      keys.insert(it.key());
    }
    return keys;
  }

  steady::duration from_us(std::int64_t microseconds) {
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
  class RecordingStatusHttpTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      saved_username = config::sunshine.username;
      saved_api_key = config::sunshine.api_key;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-recording-http-"} + info->name() + "-" +
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
                stop = std::move(stop_server);
                port = assigned;
                ready_changed.notify_all();
              });
          } catch (...) {
            std::lock_guard lock {ready_mutex};
            failed = true;
            ready_changed.notify_all();
          }
        });
        auto shutdown = util::fail_guard([&] {
          std::function<void()> stop_now;
          {
            std::lock_guard lock {ready_mutex};
            stop_now = stop;
          }
          if (stop_now) {
            stop_now();
          }
          server.join();
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
    reply_t request(const std::string &method, const std::string &path, SimpleWeb::CaseInsensitiveMultimap headers = {}) {
      reply_t reply;
      try {
        SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
        client.config.timeout = 5;
        client.config.timeout_connect = 5;
        auto response = client.request(method, path, "", headers);
        reply.code = std::stoi(response->status_code);
        reply.body = nlohmann::json::parse(response->content.string(), nullptr, false);
      } catch (const std::exception &error) {
        ADD_FAILURE() << method << ' ' << path << " failed: " << error.what();
      }
      return reply;
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
  };
}  // namespace

TEST(RecordingStatusJsonTests, StatusKeepsTheLegacyKeysAndReportsTheWindowInWholeMilliseconds) {
  stream_recorder::recorder_status_t status;
  status.mode = stream_recorder::mode_t::replay_buffer;
  status.recording = true;
  status.file = "/recordings/polaris_continuous.h264";
  auto &replay = status.replay;
  replay.duration = 5min;
  replay.keyframe_start_run = true;
  replay.packets = 3;
  replay.bytes = 30;
  replay.run_keyframes = 2;
  replay.run_span = from_us(1'999'999);
  replay.run_front_age = from_us(2'500'999);
  replay.last_accepted_keyframe_age = from_us(1'000'500);
  replay.awaiting_keyframe = false;
  replay.last_window_limit = window_limit_e::payload_cap;
  replay.selected_generation = 77;
  replay.registered_sessions = 1;
  replay.save_in_progress = true;
  replay.snapshot_packets = 4;
  replay.snapshot_bytes = 40;
  replay.snapshot_keyframes = 1;
  replay.snapshot_span = from_us(3'000'999);
  replay.saves_completed = 5;
  replay.saves_failed = 6;
  replay.saves_refused_busy = 7;
  replay.refused_unselected = 4242;
  replay.max_bytes = 100;
  replay.max_packets = 8;
  replay.max_unique_payload_bytes = 200;
  replay.max_metadata_entries = 16;
  replay.unique_payload_bytes = 70;
  replay.metadata_entries = 7;

  const auto json = confighttp::build_recording_status_json(status);
  EXPECT_EQ(keys_of(json), (std::set<std::string> {"status", "recording", "mode", "file", "replay"}));
  EXPECT_EQ(json["status"], true);
  EXPECT_EQ(json["recording"], true);
  EXPECT_EQ(json["mode"], "replay_buffer");
  EXPECT_EQ(json["file"], "/recordings/polaris_continuous.h264");

  const auto &window = json["replay"];
  EXPECT_EQ(keys_of(window), (std::set<std::string> {"configured_duration_s", "buffered_run", "last_accepted_keyframe_age_ms",
                                                     "awaiting_keyframe", "last_window_limit", "session_selected", "save",
                                                     "budgets"}));
  EXPECT_EQ(window["configured_duration_s"], 300);
  EXPECT_TRUE(window["configured_duration_s"].is_number_integer());
  const auto &run = window["buffered_run"];
  EXPECT_EQ(keys_of(run), (std::set<std::string> {"present", "packets", "bytes", "keyframes", "arrival_span_ms", "front_age_ms"}));
  EXPECT_EQ(run["present"], true);
  EXPECT_EQ(run["packets"], 3);
  EXPECT_EQ(run["bytes"], 30);
  EXPECT_EQ(run["keyframes"], 2);
  // Whole milliseconds, floored.
  EXPECT_EQ(run["arrival_span_ms"], 1999);
  EXPECT_EQ(run["front_age_ms"], 2500);
  EXPECT_TRUE(run["arrival_span_ms"].is_number_integer());
  EXPECT_EQ(window["last_accepted_keyframe_age_ms"], 1000);
  EXPECT_EQ(window["awaiting_keyframe"], false);
  EXPECT_EQ(window["last_window_limit"], "payload_cap");
  EXPECT_EQ(window["session_selected"], true);

  const auto &save = window["save"];
  EXPECT_EQ(keys_of(save), (std::set<std::string> {"in_progress", "snapshot", "completed", "failed", "refused_busy"}));
  EXPECT_EQ(save["in_progress"], true);
  EXPECT_EQ(keys_of(save["snapshot"]), (std::set<std::string> {"packets", "bytes", "keyframes", "arrival_span_ms"}));
  EXPECT_EQ(save["snapshot"]["packets"], 4);
  EXPECT_EQ(save["snapshot"]["bytes"], 40);
  EXPECT_EQ(save["snapshot"]["keyframes"], 1);
  EXPECT_EQ(save["snapshot"]["arrival_span_ms"], 3000);
  EXPECT_EQ(save["completed"], 5);
  EXPECT_EQ(save["failed"], 6);
  EXPECT_EQ(save["refused_busy"], 7);

  const auto &budgets = window["budgets"];
  EXPECT_EQ(keys_of(budgets), (std::set<std::string> {"max_payload_bytes", "max_entries", "max_unique_payload_bytes",
                                                      "max_metadata_entries", "unique_payload_bytes", "metadata_entries"}));
  EXPECT_EQ(budgets["max_payload_bytes"], 100);
  EXPECT_EQ(budgets["max_entries"], 8);
  EXPECT_EQ(budgets["max_unique_payload_bytes"], 200);
  EXPECT_EQ(budgets["max_metadata_entries"], 16);
  EXPECT_EQ(budgets["unique_payload_bytes"], 70);
  EXPECT_EQ(budgets["metadata_entries"], 7);

  // No session identity, registry size, refusal counters, output directory or error text.
  const auto text = json.dump();
  for (const auto *absent : {"generation", "registered", "refused_unselected", "output_dir", "error", "77", "4242"}) {
    EXPECT_EQ(text.find(absent), std::string::npos) << absent;
  }

  // An unknown marker is null; it is the only nullable field.
  replay.last_accepted_keyframe_age.reset();
  replay.keyframe_start_run = false;
  replay.awaiting_keyframe = true;
  replay.packets = 0;
  replay.bytes = 0;
  replay.run_keyframes = 0;
  replay.run_span = steady::duration::zero();
  replay.run_front_age = steady::duration::zero();
  status.mode = stream_recorder::mode_t::replay_buffer;
  auto unknown = confighttp::build_recording_status_json(status);
  EXPECT_TRUE(unknown["replay"]["last_accepted_keyframe_age_ms"].is_null());
  EXPECT_EQ(unknown["mode"], "replay_buffer");
  status.mode = stream_recorder::mode_t::continuous;
  replay.awaiting_keyframe = false;
  EXPECT_EQ(confighttp::build_recording_status_json(status)["mode"], "continuous");
  status.mode = stream_recorder::mode_t::disabled;
  EXPECT_EQ(confighttp::build_recording_status_json(status)["mode"], "disabled");
}

TEST(RecordingStatusJsonTests, EveryWindowLimitHasItsOwnReasonString) {
  const std::pair<window_limit_e, const char *> reasons[] {
    {window_limit_e::none, "none"},
    {window_limit_e::duration, "duration"},
    {window_limit_e::payload_cap, "payload_cap"},
    {window_limit_e::entry_cap, "entry_cap"},
    {window_limit_e::configuration, "configuration"},
    {window_limit_e::selection, "selection"},
    {window_limit_e::codec, "codec"},
    {window_limit_e::discontinuity, "discontinuity"},
  };
  stream_recorder::recorder_status_t status;
  for (const auto &[limit, name] : reasons) {
    status.replay.last_window_limit = limit;
    EXPECT_EQ(confighttp::build_recording_status_json(status)["replay"]["last_window_limit"], name);
  }
}

TEST(RecordingStatusJsonTests, SaveOutcomesSayWhatWasCapturedAndOnlyASavedFileIsClaimed) {
  stream_recorder::detail::save_result_t captured;
  captured.admitted = true;
  captured.packets = 3;
  captured.bytes = 7;
  captured.keyframes = 2;
  captured.span = from_us(4'000'999);
  captured.video_format = k_hevc;
  captured.generation = 77;
  captured.error = "/secret/staging: Input/output error";

  auto saved = captured;
  saved.status = save_status_e::saved;
  saved.path = "/recordings/polaris_replay.hevc";
  saved.error.clear();
  auto json = confighttp::build_save_replay_json(saved);
  EXPECT_EQ(keys_of(json), (std::set<std::string> {"status", "file", "outcome", "captured", "kind"}));
  EXPECT_EQ(json["status"], true);
  EXPECT_EQ(json["file"], "/recordings/polaris_replay.hevc");
  EXPECT_EQ(json["outcome"], "saved");
  EXPECT_EQ(json["kind"], "raw_video_bitstream");
  EXPECT_EQ(keys_of(json["captured"]), (std::set<std::string> {"packets", "bytes", "keyframes", "arrival_span_ms", "codec"}));
  EXPECT_EQ(json["captured"]["packets"], 3);
  EXPECT_EQ(json["captured"]["bytes"], 7);
  EXPECT_EQ(json["captured"]["keyframes"], 2);
  EXPECT_EQ(json["captured"]["arrival_span_ms"], 4000);
  EXPECT_EQ(json["captured"]["codec"], "hevc");

  // A storage error after admission keeps what was captured, but claims no file.
  auto failed = captured;
  failed.status = save_status_e::storage_error;
  failed.path = "/recordings/never-published.hevc";
  json = confighttp::build_save_replay_json(failed);
  EXPECT_EQ(keys_of(json), (std::set<std::string> {"status", "file", "outcome", "captured"}));
  EXPECT_EQ(json["status"], false);
  EXPECT_EQ(json["file"], "");
  EXPECT_EQ(json["outcome"], "storage_error");
  EXPECT_EQ(json["captured"]["packets"], 3);
  EXPECT_EQ(json["captured"]["codec"], "hevc");
  const auto text = json.dump();
  for (const auto *absent : {"secret", "Input/output", "never-published", "generation", "77", "\"error\":", "\"errno\":"}) {
    EXPECT_EQ(text.find(absent), std::string::npos) << absent;
  }

  // Nothing captured: busy, disabled, empty, and a storage error during admission.
  for (const auto status : {save_status_e::busy, save_status_e::disabled, save_status_e::empty, save_status_e::storage_error}) {
    stream_recorder::detail::save_result_t refused;
    refused.status = status;
    json = confighttp::build_save_replay_json(refused);
    EXPECT_EQ(keys_of(json), (std::set<std::string> {"status", "file", "outcome", "captured"}));
    EXPECT_EQ(json["status"], false);
    EXPECT_EQ(json["file"], "");
    EXPECT_TRUE(json["captured"].is_null());
  }
  stream_recorder::detail::save_result_t busy;
  busy.status = save_status_e::busy;
  EXPECT_EQ(confighttp::build_save_replay_json(busy)["outcome"], "busy");
  busy.status = save_status_e::disabled;
  EXPECT_EQ(confighttp::build_save_replay_json(busy)["outcome"], "disabled");
  busy.status = save_status_e::empty;
  EXPECT_EQ(confighttp::build_save_replay_json(busy)["outcome"], "empty");

  // Every codec the recorder accepts has its own name.
  const std::pair<int, const char *> codecs[] {{0, "h264"}, {1, "hevc"}, {2, "av1"}, {3, "pyrowave"}};
  for (const auto &[format, name] : codecs) {
    saved.video_format = format;
    EXPECT_EQ(confighttp::build_save_replay_json(saved)["captured"]["codec"], name);
  }
}

TEST_F(RecordingStatusHttpTests, RecordingAndModeAreIndependentFactsFromOneSnapshot) {
  // A continuous writer may be open whatever the configured mode; the status says both.
  stream_recorder::start_recording();
  auto status = stream_recorder::recorder_status();
  EXPECT_EQ(status.mode, stream_recorder::mode_t::replay_buffer);
  EXPECT_TRUE(status.recording);
  ASSERT_FALSE(status.file.empty());
  EXPECT_EQ(fs::path {status.file}.parent_path(), output_dir);
  auto json = confighttp::build_recording_status_json(status);
  EXPECT_EQ(json["mode"], "replay_buffer");
  EXPECT_EQ(json["recording"], true);
  EXPECT_EQ(json["file"], status.file);

  publish(false, true);
  status = stream_recorder::recorder_status();
  EXPECT_EQ(status.mode, stream_recorder::mode_t::disabled);
  EXPECT_TRUE(status.recording);

  stream_recorder::stop_recording();
  status = stream_recorder::recorder_status();
  EXPECT_FALSE(status.recording);
  EXPECT_TRUE(status.file.empty());

  // While another thread opens and closes the writer, every snapshot pairs them consistently.
  // This exercises the lock; it does not prove the absence of a data race.
  std::atomic<bool> done {false};
  std::thread toggler([&] {
    for (int i = 0; i < 40; ++i) {
      stream_recorder::start_recording();
      stream_recorder::stop_recording();
    }
    done.store(true);
  });
  int inconsistent = 0;
  int reads = 0;
  do {
    const auto read = stream_recorder::recorder_status();
    inconsistent += read.recording == read.file.empty();
    ++reads;
  } while (!done.load());
  toggler.join();
  EXPECT_EQ(inconsistent, 0);
  EXPECT_GE(reads, 1);
}

TEST_F(RecordingStatusHttpTests, AnInFlightSaveStaysReportedAfterReplayIsTurnedOff) {
  push(true);
  push(false);
  start_held_save();
  ASSERT_TRUE(barrier.wait_entered());

  EXPECT_EQ(stream_recorder::save_replay_result().status, save_status_e::busy);
  const auto captured = stream_recorder::recorder_status().replay;
  publish(false, true);
  const auto status = stream_recorder::recorder_status();
  EXPECT_EQ(status.mode, stream_recorder::mode_t::disabled);
  EXPECT_FALSE(status.replay.keyframe_start_run);
  EXPECT_FALSE(status.replay.awaiting_keyframe);
  EXPECT_EQ(status.replay.last_window_limit, window_limit_e::configuration);
  EXPECT_TRUE(status.replay.save_in_progress);
  EXPECT_EQ(status.replay.snapshot_packets, 2u);
  EXPECT_EQ(status.replay.snapshot_keyframes, 1u);
  EXPECT_EQ(status.replay.snapshot_bytes, captured.snapshot_bytes);
  EXPECT_EQ(status.replay.snapshot_span, captured.snapshot_span);
  EXPECT_EQ(status.replay.saves_refused_busy, 1u);
  const auto json = confighttp::build_recording_status_json(status);
  EXPECT_EQ(json["replay"]["buffered_run"]["present"], false);
  EXPECT_EQ(json["replay"]["save"]["in_progress"], true);
  EXPECT_EQ(json["replay"]["save"]["snapshot"]["packets"], 2);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_TRUE(result.admitted);
  const auto after = stream_recorder::recorder_status();
  EXPECT_FALSE(after.replay.save_in_progress);
  EXPECT_EQ(after.replay.snapshot_packets, 0u);
  EXPECT_EQ(after.replay.saves_completed, 1u);
}

TEST_F(RecordingStatusHttpTests, StatusNeedsAuthenticationButNotCsrf) {
  with_server([&](const std::string &cookie) {
    EXPECT_EQ(request("GET", "/api/recording/status").code, 401);
    EXPECT_EQ(request("GET", "/api/recording/status", {{"Authorization", "Bearer not-the-key"}}).code, 401);
    EXPECT_EQ(request("GET", "/api/recording/status", {{"Cookie", "auth=not-a-session"}}).code, 401);

    const auto by_session = request("GET", "/api/recording/status", {{"Cookie", "auth=" + cookie}});
    EXPECT_EQ(by_session.code, 200);
    ASSERT_TRUE(by_session.body.is_object());
    EXPECT_EQ(keys_of(by_session.body), (std::set<std::string> {"status", "recording", "mode", "file", "replay"}));
    EXPECT_EQ(by_session.body["mode"], "replay_buffer");
    EXPECT_EQ(by_session.body["replay"]["awaiting_keyframe"], true);

    const auto by_key = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(by_key.code, 200);
    EXPECT_EQ(by_key.body["status"], true);
  });
}

TEST_F(RecordingStatusHttpTests, SaveNeedsCsrfForSessionsAndAuthenticationForEveryone) {
  with_server([&](const std::string &cookie) {
    const std::string path = "/api/recording/save-replay";
    // The CSRF wrapper answers first for anything that is not a valid key.
    EXPECT_EQ(request("POST", path).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "wrong"}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"Authorization", "Bearer not-the-key"}}).code, 403);
    // A valid token still needs the handler's own authentication.
    EXPECT_EQ(request("POST", path, {{"X-CSRF-Token", k_csrf}}).code, 401);
    EXPECT_EQ(request("POST", path, {{"Authorization", "Bearer not-the-key"}, {"X-CSRF-Token", k_csrf}}).code, 401);

    const auto by_session = request("POST", path, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", k_csrf}});
    EXPECT_EQ(by_session.code, 200);
    EXPECT_EQ(by_session.body["outcome"], "empty");
    const auto by_key = request("POST", path, bearer());
    EXPECT_EQ(by_key.code, 200);
    EXPECT_EQ(by_key.body["outcome"], "empty");
  });
}

TEST_F(RecordingStatusHttpTests, SaveReportsEachOutcomeOverHttp) {
  // Configuration and save seams change only between listener sessions, never while one runs.
  const std::string path = "/api/recording/save-replay";
  reply_t reply;

  publish(false, true);
  with_server([&](const std::string &) {
    reply = request("POST", path, bearer());
  });
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body["outcome"], "disabled");
  EXPECT_EQ(reply.body["status"], false);
  EXPECT_EQ(reply.body["file"], "");
  EXPECT_TRUE(reply.body["captured"].is_null());
  EXPECT_FALSE(reply.body.contains("kind"));

  publish(true, true);
  with_server([&](const std::string &) {
    reply = request("POST", path, bearer());
  });
  EXPECT_EQ(reply.body["outcome"], "empty");
  EXPECT_TRUE(reply.body["captured"].is_null());

  push(true, 4);
  push(false, 3);
  with_server([&](const std::string &) {
    reply = request("POST", path, bearer());
  });
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body["outcome"], "saved");
  EXPECT_EQ(reply.body["status"], true);
  EXPECT_EQ(reply.body["kind"], "raw_video_bitstream");
  EXPECT_EQ(reply.body["captured"]["packets"], 2);
  EXPECT_EQ(reply.body["captured"]["bytes"], 7);
  EXPECT_EQ(reply.body["captured"]["keyframes"], 1);
  EXPECT_EQ(reply.body["captured"]["codec"], "hevc");
  ASSERT_TRUE(reply.body["file"].is_string());
  const auto file = reply.body["file"].get<std::string>();
  EXPECT_EQ(fs::path {file}.parent_path(), output_dir);
  EXPECT_EQ(fs::file_size(file), 7u);

  // Storage fails after the run was captured: the capture is reported, no file is claimed.
  hooks.publish = [](const fs::path &, const fs::path &) {
    return std::make_error_code(std::errc::io_error);
  };
  install();
  with_server([&](const std::string &) {
    reply = request("POST", path, bearer());
  });
  EXPECT_EQ(reply.body["outcome"], "storage_error");
  EXPECT_EQ(reply.body["status"], false);
  EXPECT_EQ(reply.body["file"], "");
  ASSERT_TRUE(reply.body["captured"].is_object());
  EXPECT_EQ(reply.body["captured"]["packets"], 2);
  EXPECT_FALSE(reply.body.contains("kind"));
  EXPECT_EQ(reply.body.dump().find("Input/output"), std::string::npos);

  // Admission itself fails: nothing was captured.
  hooks.publish = {};
  hooks.after_snapshot_payloads = [] {
    throw std::bad_alloc {};
  };
  install();
  with_server([&](const std::string &) {
    reply = request("POST", path, bearer());
  });
  EXPECT_EQ(reply.body["outcome"], "storage_error");
  EXPECT_TRUE(reply.body["captured"].is_null());
  EXPECT_FALSE(reply.body.contains("kind"));
}

TEST_F(RecordingStatusHttpTests, StatusAndASecondSaveAnswerWhileARecorderSaveIsHeld) {
  // The held save is a direct recorder call on its own thread, not an HTTP request. The
  // production listener (like this one) keeps Simple-Web-Server's default single handler
  // thread, so a save requested over HTTP occupies it until storage finishes; this test
  // shows the recorder snapshot, status route and busy gate, not concurrent HTTP saves.
  push(true);
  push(false);
  start_held_save();
  ASSERT_TRUE(barrier.wait_entered());

  with_server([&](const std::string &) {
    const auto status = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(status.code, 200);
    EXPECT_EQ(status.body["replay"]["buffered_run"]["present"], true);
    EXPECT_TRUE(status.body["replay"]["last_accepted_keyframe_age_ms"].is_number_integer());
    EXPECT_GE(status.body["replay"]["last_accepted_keyframe_age_ms"].get<std::int64_t>(), 0);
    EXPECT_GE(status.body["replay"]["buffered_run"]["arrival_span_ms"].get<std::int64_t>(), 0);
    EXPECT_EQ(status.body["replay"]["save"]["in_progress"], true);
    EXPECT_EQ(status.body["replay"]["save"]["snapshot"]["packets"], 2);

    const auto second = request("POST", "/api/recording/save-replay", bearer());
    EXPECT_EQ(second.code, 200);
    EXPECT_EQ(second.body["outcome"], "busy");
    EXPECT_TRUE(second.body["captured"].is_null());
    EXPECT_FALSE(second.body.contains("kind"));
  });

  const auto result = finish_held_save();
  EXPECT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(stream_recorder::recorder_status().replay.saves_refused_busy, 1u);
}

TEST_F(RecordingStatusHttpTests, StatusAndRefusedSavesCreateNoFiles) {
  with_server([&](const std::string &cookie) {
    EXPECT_EQ(request("GET", "/api/recording/status", bearer()).code, 200);
    EXPECT_EQ(request("GET", "/api/recording/status", {{"Cookie", "auth=" + cookie}}).code, 200);
    EXPECT_EQ(request("POST", "/api/recording/save-replay", bearer()).body["outcome"], "empty");
  });
  EXPECT_FALSE(fs::exists(output_dir));

  publish(false, true);
  with_server([&](const std::string &) {
    EXPECT_EQ(request("GET", "/api/recording/status", bearer()).code, 200);
    EXPECT_EQ(request("POST", "/api/recording/save-replay", bearer()).body["outcome"], "disabled");
  });
  EXPECT_FALSE(fs::exists(output_dir));
}
