/**
 * @file tests/unit/test_recording_continuous_http.cpp
 * @brief Production admin start/stop/status routes over fixture-owned loopback TLS.
 * Raw synthetic bytes exercise ownership and legacy compatibility, not playable media.
 */
#include "../tests_common.h"
#include <Simple-Web-Server/client_https.hpp>
#include <nlohmann/json.hpp>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/stream_recorder.h>
#include <src/utility.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace confighttp {
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
                                  const std::function<void(const std::string &)> &);
}

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  constexpr auto k_key = "continuous-http-fixture-key";
  constexpr auto k_csrf = "continuous-http-fixture-csrf";
  constexpr std::uint64_t k_owner = 501;
  constexpr std::uint64_t k_other = 502;

  std::set<std::string> keys_of(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
    return keys;
  }

  struct reply_t {
    int code = 0;
    nlohmann::json body;
  };

  class RecordingContinuousHttpTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      saved_username = config::sunshine.username;
      saved_key = config::sunshine.api_key;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      for (int attempt = 0; attempt < 100 && parent.empty(); ++attempt) {
        auto candidate = fs::temp_directory_path() /
          (std::string {"polaris-continuous-http-"} + info->name() + "-" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) parent = std::move(candidate);
      }
      ASSERT_FALSE(parent.empty());
      const auto credentials = crypto::gen_creds("localhost", 2048);
      cert = parent / "cert.pem";
      key = parent / "key.pem";
      std::ofstream(cert) << credentials.x509;
      std::ofstream(key) << credentials.pkey;
      fs::permissions(key, fs::perms::owner_read | fs::perms::owner_write);
      config::sunshine.username = "continuous-http-admin";
      config::sunshine.api_key = k_key;
      output = parent / "recordings";
      config::recording.output_dir = output.string();
      publish(false, false);
      // A stale global H.264 hint must not mislabel the selected HEVC owner.
      stream_recorder::set_active_video_format(0);
      stream_recorder::session_started(k_owner, true, 1);
    }

    void TearDown() override {
      if (stream_recorder::is_recording()) stream_recorder::stop_recording();
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_recording;
      config::sunshine.username = saved_username;
      config::sunshine.api_key = saved_key;
      stream_recorder::load_config();
      if (!parent.empty()) {
        std::error_code ec;
        fs::remove_all(parent, ec);
      }
    }

    static void publish(bool enabled, bool replay) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay;
      stream_recorder::load_config();
    }

    void push(std::uint64_t generation, bool keyframe, std::uint8_t value) {
      const std::vector<std::uint8_t> payload {value};
      stream_recorder::push_packet(payload.data(), payload.size(), true, keyframe,
                                   frame++, generation, 1);
    }

    static std::vector<std::uint8_t> bytes(const std::string &path) {
      std::ifstream input(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void with_server(const std::function<void(const std::string &)> &body) {
      confighttp::with_web_session_for_tests(parent / "sessions.json", k_csrf, [&](const std::string &cookie) {
        std::mutex mutex;
        std::condition_variable changed;
        bool failed = false;
        std::function<void()> stop;
        port = 0;
        std::thread server([&] {
          try {
            confighttp::run_recording_routes_for_tests(cert.string(), key.string(),
              [&](unsigned short assigned, std::function<void()> stop_server) {
                std::lock_guard lock {mutex};
                stop = [once = std::make_shared<std::atomic<bool>>(false), stop_server = std::move(stop_server)] {
                  if (!once->exchange(true)) stop_server();
                };
                port = assigned;
                changed.notify_all();
              });
          } catch (...) {
            std::lock_guard lock {mutex};
            failed = true;
            changed.notify_all();
          }
        });
        auto shutdown = util::fail_guard([&] {
          std::function<void()> stop_now;
          {
            std::lock_guard lock {mutex};
            stop_now = stop;
          }
          if (stop_now) stop_now();
          server.join();
        });
        {
          std::unique_lock lock {mutex};
          ASSERT_TRUE(changed.wait_for(lock, 10s, [&] { return port != 0 || failed; }));
          ASSERT_FALSE(failed);
        }
        body(cookie);
      });
    }

    reply_t request(const std::string &method, const std::string &path,
                    SimpleWeb::CaseInsensitiveMultimap headers = {}) {
      reply_t reply;
      try {
        SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
        client.config.timeout = 5;
        client.config.timeout_connect = 5;
        const auto response = client.request(method, path, "", headers);
        reply.code = std::stoi(response->status_code);
        reply.body = nlohmann::json::parse(response->content.string(), nullptr, false);
      } catch (const std::exception &error) {
        ADD_FAILURE() << path << ": " << error.what();
      }
      return reply;
    }

    static SimpleWeb::CaseInsensitiveMultimap bearer() {
      return {{"Authorization", std::string {"Bearer "} + k_key}};
    }

    config::recording_t saved_recording;
    std::string saved_username, saved_key;
    fs::path parent, output, cert, key;
    std::int64_t frame = 0;
    unsigned short port = 0;
  };
}

TEST_F(RecordingContinuousHttpTests, CsrfAndAuthenticationRejectionsCannotStartAWriter) {
  with_server([&](const std::string &cookie) {
    const std::string path = "/api/recording/start";
    EXPECT_EQ(request("POST", path).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "wrong"}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"Authorization", "Bearer wrong"}}).code, 403);
    EXPECT_EQ(request("POST", path, {{"X-CSRF-Token", k_csrf}}).code, 401);
    EXPECT_EQ(request("POST", path, {{"Authorization", "Bearer wrong"}, {"X-CSRF-Token", k_csrf}}).code, 401);
    EXPECT_FALSE(stream_recorder::is_recording());
    EXPECT_TRUE(stream_recorder::current_file().empty());
    EXPECT_FALSE(fs::exists(output));
  });
}

TEST_F(RecordingContinuousHttpTests, AuthorizedManualStartAndStopKeepLegacyShapeInDisabledMode) {
  std::string file;
  with_server([&](const std::string &cookie) {
    const auto start = request("POST", "/api/recording/start", bearer());
    ASSERT_EQ(start.code, 200);
    ASSERT_TRUE(start.body.is_object());
    EXPECT_EQ(keys_of(start.body), (std::set<std::string> {"status", "recording", "file"}));
    EXPECT_EQ(start.body["status"], true);
    EXPECT_EQ(start.body["recording"], true);
    file = start.body["file"].get<std::string>();
    EXPECT_EQ(fs::path(file).extension(), ".hevc");
    EXPECT_EQ(fs::path(file).parent_path(), output);
    EXPECT_TRUE(fs::is_regular_file(file));
    const auto status = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(status.code, 200);
    EXPECT_EQ(keys_of(status.body), (std::set<std::string> {"status", "recording", "mode", "file", "replay"}));
    EXPECT_EQ(status.body["mode"], "disabled");
    EXPECT_EQ(status.body["recording"], true);
    EXPECT_EQ(status.body["file"], file);
    EXPECT_EQ(request("POST", "/api/recording/stop", {{"Cookie", "auth=" + cookie}}).code, 403);
    EXPECT_TRUE(stream_recorder::is_recording());
    const auto stop = request("POST", "/api/recording/stop", {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", k_csrf}});
    EXPECT_EQ(stop.code, 200);
    EXPECT_EQ(keys_of(stop.body), (std::set<std::string> {"status", "recording"}));
    EXPECT_EQ(stop.body["status"], true);
    EXPECT_EQ(stop.body["recording"], false);
    EXPECT_TRUE(stream_recorder::current_file().empty());
  });
  EXPECT_TRUE(bytes(file).empty());
}

TEST_F(RecordingContinuousHttpTests, OverlapAndOwnerEndKeepStatusButNeverAppendOtherSessions) {
  std::string file;
  with_server([&](const std::string &) {
    const auto start = request("POST", "/api/recording/start", bearer());
    ASSERT_EQ(start.code, 200);
    file = start.body["file"].get<std::string>();
    push(k_owner, false, 0x10);
    push(k_owner, true, 0x11);
    stream_recorder::session_started(k_other, true, 1);
    push(k_owner, true, 0x12);
    push(k_other, true, 0x13);
    auto status = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(status.body["recording"], true);
    EXPECT_EQ(status.body["file"], file);
    EXPECT_EQ(keys_of(status.body), (std::set<std::string> {"status", "recording", "mode", "file", "replay"}));
    stream_recorder::session_ended(k_other);
    push(k_owner, false, 0x14);
    push(k_owner, true, 0x15);
    stream_recorder::session_ended(k_owner);
    stream_recorder::session_started(k_other, true, 1);
    push(k_other, true, 0x16);
    push(k_owner, true, 0x17);
    status = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(status.body["recording"], true);
    EXPECT_EQ(status.body["file"], file);
    EXPECT_EQ(request("POST", "/api/recording/stop", bearer()).code, 200);
  });
  EXPECT_EQ(bytes(file), (std::vector<std::uint8_t> {0x11, 0x15}));
}

TEST_F(RecordingContinuousHttpTests, ModeAndOutputReloadDoNotRedirectAnOpenWriter) {
  std::string file;
  with_server([&](const std::string &cookie) {
    const auto start = request("POST", "/api/recording/start", {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", k_csrf}});
    ASSERT_EQ(start.code, 200);
    file = start.body["file"].get<std::string>();
    push(k_owner, true, 0x21);
  });
  // Configure only between owned listener lifetimes, with the writer still open.
  const auto new_output = parent / "elsewhere";
  config::recording.output_dir = new_output.string();
  publish(true, false);
  with_server([&](const std::string &) {
    const auto status = request("GET", "/api/recording/status", bearer());
    EXPECT_EQ(status.body["mode"], "continuous");
    EXPECT_EQ(status.body["recording"], true);
    EXPECT_EQ(status.body["file"], file);
    push(k_owner, false, 0x22);
    EXPECT_EQ(request("POST", "/api/recording/stop", bearer()).code, 200);
  });
  EXPECT_FALSE(fs::exists(new_output));
  EXPECT_EQ(bytes(file), (std::vector<std::uint8_t> {0x21, 0x22}));
}

TEST_F(RecordingContinuousHttpTests, RepeatedStartKeepsExistingFileAndAcceptedBytes) {
  std::string file;
  with_server([&](const std::string &) {
    const auto first = request("POST", "/api/recording/start", bearer());
    ASSERT_EQ(first.code, 200);
    file = first.body["file"].get<std::string>();
    push(k_owner, true, 0x31);
    const auto second = request("POST", "/api/recording/start", bearer());
    EXPECT_EQ(second.body["file"], file);
    EXPECT_EQ(second.body["recording"], true);
    push(k_owner, false, 0x32);
    const auto first_stop = request("POST", "/api/recording/stop", bearer());
    const auto second_stop = request("POST", "/api/recording/stop", bearer());
    EXPECT_EQ(first_stop.body, second_stop.body);
  });
  EXPECT_EQ(bytes(file), (std::vector<std::uint8_t> {0x31, 0x32}));
}

TEST_F(RecordingContinuousHttpTests, OpenFailureKeepsTheLegacyFalseRecordingResult) {
  std::ofstream(output) << "owned regular file, not a directory";
  with_server([&](const std::string &) {
    const auto start = request("POST", "/api/recording/start", bearer());
    ASSERT_EQ(start.code, 200);
    EXPECT_EQ(keys_of(start.body), (std::set<std::string> {"status", "recording", "file"}));
    EXPECT_EQ(start.body["status"], true);
    EXPECT_EQ(start.body["recording"], false);
    EXPECT_EQ(start.body["file"], "");
    EXPECT_FALSE(stream_recorder::is_recording());
  });
  EXPECT_EQ(fs::file_size(output), std::string {"owned regular file, not a directory"}.size());
}
