#include "src/platform/linux/multiseat_launch_service.h"
#include "src/config.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/private_state_file.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <thread>

namespace {
  using namespace multiseat;
  using namespace std::chrono_literals;

  TEST(MultiseatLaunchCapabilities, NativeTabletSupportFollowsTheSelectedConsumer) {
    rtsp_stream::launch_session_t launch;
    launch.perm = crypto::PERM::_all;
    constexpr auto tablet = platf::platform_caps::pen_touch;
    constexpr auto controller_touch = platf::platform_caps::controller_touch;
    EXPECT_EQ(rtsp_stream::session_feature_flags(launch, tablet | controller_touch), tablet | controller_touch);
    launch.require_worker_connection();
    EXPECT_EQ(rtsp_stream::session_feature_flags(launch, tablet | controller_touch), controller_touch);
    EXPECT_EQ(rtsp_stream::session_feature_flags(launch, tablet), 0U);
    EXPECT_EQ(rtsp_stream::session_feature_flags(launch, 0), 0U);
    launch.cancel();
    EXPECT_EQ(rtsp_stream::session_feature_flags(launch, tablet | controller_touch), controller_touch);
    EXPECT_EQ(launch.perm, crypto::PERM::_all);
  }

  struct controller_state_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::thread::id> owners;
    std::atomic<unsigned> begins {0}, reconciles {0}, polls {0}, shutdowns {0};
    std::atomic<bool> select {true}, close {true}, fail {false};
    void called() { std::lock_guard lock(mutex); owners.push_back(std::this_thread::get_id()); }
    bool await_begin() {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, 2s, [&] { return begins.load() != 0; });
    }
  };

  class controller_t final : public profile_controller_t {
  public:
    explicit controller_t(std::shared_ptr<controller_state_t> state) : state_(std::move(state)) {}
    bool routes_client(std::string_view client) const override { return client == "client-a" || client == "client-b"; }
    void reconcile() override { state_->called(); ++state_->reconciles; }
    profile_begin_result_t begin(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) override {
      state_->called();
      { std::lock_guard lock(state_->mutex); ++state_->begins; }
      state_->changed.notify_all();
      if (state_->fail) return {{409, "No capacity"}, {}};
      return {{200, "Starting"}, seat_handle_t {"test-epoch", "gpu-a", 0, *launch->lifecycle_generation}};
    }
    profile_poll_e poll(const std::shared_ptr<rtsp_stream::launch_session_t> &, const seat_handle_t &) override {
      state_->called();
      ++state_->polls;
      return state_->select ? profile_poll_e::selected : profile_poll_e::pending;
    }
    bool shutdown() override { state_->called(); ++state_->shutdowns; return state_->close; }
  private:
    std::shared_ptr<controller_state_t> state_;
  };

  class MultiseatLaunchService : public ::testing::Test {
  protected:
    std::shared_ptr<controller_state_t> state = std::make_shared<controller_state_t>();
    std::shared_ptr<profile_launch_service_t> service;
    void SetUp() override {
      service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state), 2s);
    }
    void TearDown() override {
      state->close = true;
      EXPECT_TRUE(service->shutdown(2s));
      uninstall_profile_launch_service(service);
      service.reset();
    }
    std::shared_ptr<rtsp_stream::launch_session_t> launch(std::string client = "client-a") {
      auto value = std::make_shared<rtsp_stream::launch_session_t>();
      value->unique_id = std::move(client);
      value->perm = crypto::PERM::_game_control;
      value->width = 1920;
      value->height = 1080;
      value->fps = 60000;
      return value;
    }
  };

  TEST_F(MultiseatLaunchService, ResourceOperationsHaveOneOwnerAndIndependentLaunchIdentities) {
    const auto a = launch(), b = launch("client-b");
    a->client_do_cmds.push_back({"host command", false});
    a->client_undo_cmds.push_back({"host undo", false});
    auto first = std::async(std::launch::async, [&] { return service->prepare(a); });
    auto second = std::async(std::launch::async, [&] { return service->prepare(b); });
    ASSERT_EQ(first.get().status, 200);
    ASSERT_EQ(second.get().status, 200);
    EXPECT_TRUE(a->worker_connection_requirement()->load());
    ASSERT_TRUE(a->lifecycle_generation);
    EXPECT_GE(*a->lifecycle_generation, 1ULL << 63);
    EXPECT_NE(a->lifecycle_generation, b->lifecycle_generation);
    EXPECT_NE(a->session_token, b->session_token);
    EXPECT_FALSE(a->session_token.empty());
    EXPECT_TRUE(a->client_do_cmds.empty());
    EXPECT_TRUE(a->client_undo_cmds.empty());
    EXPECT_TRUE(service->shutdown(2s));
    std::lock_guard lock(state->mutex);
    ASSERT_FALSE(state->owners.empty());
    for (const auto &owner : state->owners) {
      EXPECT_EQ(owner, state->owners.front());
      EXPECT_NE(owner, std::this_thread::get_id());
    }
  }

  TEST_F(MultiseatLaunchService, StaleCancellationCannotCancelReplacementOrAnotherClient) {
    const auto old = launch(), other = launch("client-b");
    ASSERT_TRUE(service->prepare(old).prepared());
    ASSERT_TRUE(service->prepare(other).prepared());
    ASSERT_TRUE(service->cancel_client("client-a", old->session_token));
    const auto replacement = launch();
    ASSERT_TRUE(service->prepare(replacement).prepared());
    EXPECT_FALSE(service->cancel_client("client-a", old->session_token));
    EXPECT_FALSE(service->cancel_client("client-a", other->session_token));
    EXPECT_FALSE(replacement->is_cancelled());
    EXPECT_FALSE(other->is_cancelled());
  }

  TEST_F(MultiseatLaunchService, SessionInfoOnlyExposesOwnStartedLaunch) {
    const auto a = launch(), b = launch("client-b");
    ASSERT_TRUE(service->prepare(a).prepared());
    ASSERT_TRUE(service->prepare(b).prepared());
    EXPECT_FALSE(service->session_token("client-a"));
    ASSERT_TRUE(a->try_begin_setup_handoff());
    ASSERT_TRUE(a->commit_setup_start());
    EXPECT_EQ(service->session_token("client-a"), a->session_token);
    EXPECT_FALSE(service->session_token("client-b"));
    a->cancel();
    EXPECT_FALSE(service->session_token("client-a"));
  }

  TEST_F(MultiseatLaunchService, CancellationDuringStartupKeepsWorkerRequirement) {
    state->select = false;
    const auto value = launch();
    auto pending = std::async(std::launch::async, [&] { return service->prepare(value); });
    ASSERT_TRUE(state->await_begin());
    EXPECT_TRUE(service->cancel_client(value->unique_id));
    EXPECT_EQ(pending.get().status, 409);
    EXPECT_TRUE(value->is_cancelled());
    EXPECT_TRUE(value->worker_connection_requirement()->load());
    EXPECT_GT(state->reconciles, 0U);
  }

  TEST_F(MultiseatLaunchService, TimeoutCancelsLaunchWithoutHostFallback) {
    ASSERT_TRUE(service->shutdown(2s));
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state), 100ms);
    state->select = false;
    const auto value = launch();
    EXPECT_EQ(service->prepare(value).status, 504);
    EXPECT_TRUE(value->is_cancelled());
    EXPECT_TRUE(value->worker_connection_requirement()->load());
  }

  TEST_F(MultiseatLaunchService, FailedAdmissionIsStickyAndDoesNotPoll) {
    state->fail = true;
    const auto value = launch();
    EXPECT_EQ(service->prepare(value).status, 409);
    EXPECT_TRUE(value->is_cancelled());
    EXPECT_TRUE(value->worker_connection_requirement()->load());
    EXPECT_EQ(state->polls, 0U);
  }

  TEST_F(MultiseatLaunchService, ShutdownRejectsPendingAndKeepsRoutingUntilAuthorityCloses) {
    ASSERT_TRUE(install_profile_launch_service(service));
    state->select = false;
    state->close = false;
    const auto value = launch();
    auto pending = std::async(std::launch::async, [&] { return service->prepare(value); });
    ASSERT_TRUE(state->await_begin());
    EXPECT_FALSE(service->shutdown(100ms));
    EXPECT_EQ(pending.get().status, 503);
    EXPECT_TRUE(value->is_cancelled());
    EXPECT_EQ(profile_service_for("client-a"), service);
    const auto retry = launch();
    EXPECT_EQ(service->prepare(retry).status, 503);
    EXPECT_TRUE(retry->worker_connection_requirement()->load());
    state->close = true;
    EXPECT_TRUE(service->shutdown(2s));
  }

  TEST_F(MultiseatLaunchService, UnmappedDeviceKeepsOrdinaryLaunchState) {
    const auto value = launch("unassigned");
    EXPECT_EQ(service->prepare(value).status, 404);
    EXPECT_FALSE(value->worker_connection_requirement()->load());
    EXPECT_FALSE(value->lifecycle_generation);
    EXPECT_EQ(state->begins, 0U);
  }

  TEST_F(MultiseatLaunchService, RejectsInvalidOrReusedRequestsBeforeStartingResources) {
    for (unsigned kind = 0; kind != 7; ++kind) {
      auto value = launch();
      switch (kind) {
        case 0: value->temporary_authorization = true; break;
        case 1: value->watch_only = true; break;
        case 2: value->input_only = true; break;
        case 3: value->perm = crypto::PERM::_default; break;
        case 4: value->enable_hdr = true; break;
        case 5: value->fps = 59940; break;
        case 6: value->lifecycle_generation = 5; break;
      }
      EXPECT_EQ(service->prepare(value).status, 400) << kind;
      EXPECT_TRUE(value->worker_connection_requirement()->load());
    }
    EXPECT_EQ(state->begins, 0U);
  }

  TEST_F(MultiseatLaunchService, WorkerStreamsSurviveHostExitIncludingLateStickyRequirement) {
    const auto ordinary = launch(), worker = launch();
    ordinary->iv.resize(16); ordinary->gcm_key.resize(16);
    worker->iv.resize(16); worker->gcm_key.resize(16);
    stream::config_t options {};
    auto host_session = stream::session::alloc(options, *ordinary);
    auto worker_session = stream::session::alloc(options, *worker);
    worker->require_worker_connection();
    EXPECT_TRUE(stream::session::uses_host_process(*host_session));
    EXPECT_FALSE(stream::session::uses_host_process(*worker_session));
    EXPECT_TRUE(stream::session::stops_when_host_exits(*host_session, false, true));
    EXPECT_FALSE(stream::session::stops_when_host_exits(*host_session, false, false));
    EXPECT_FALSE(stream::session::stops_when_host_exits(*host_session, true, true));
    EXPECT_FALSE(stream::session::stops_when_host_exits(*worker_session, false, true));
  }

  TEST_F(MultiseatLaunchService, WorkerRtspSetupKeepsCancellationAndFailureRollbackWithoutHostGeneration) {
    const auto value = launch();
    ASSERT_TRUE(service->prepare(value).prepared());
    // A failed start is reached only after the real handoff commits. A host
    // generation check would cancel this high-range worker generation first.
    EXPECT_EQ(rtsp_stream::run_setup_insert_for_tests(*value, false, 1),
      rtsp_stream::setup_insert_result_e::failed);
    EXPECT_TRUE(value->is_cancelled());
    EXPECT_EQ(rtsp_stream::session_snapshot(value->unique_id).active_sessions, 0U);
    const auto cancelled = launch();
    ASSERT_TRUE(service->prepare(cancelled).prepared());
    EXPECT_EQ(rtsp_stream::run_setup_insert_for_tests(*cancelled, true, 0),
      rtsp_stream::setup_insert_result_e::cancelled);
    EXPECT_TRUE(cancelled->is_cancelled());
    EXPECT_EQ(rtsp_stream::session_snapshot(cancelled->unique_id).active_sessions, 0U);
    const auto missing = launch();
    missing->require_worker_connection();
    EXPECT_EQ(rtsp_stream::run_setup_insert_for_tests(*missing, false, 1),
      rtsp_stream::setup_insert_result_e::cancelled);
  }

  TEST_F(MultiseatLaunchService, RtspRequiresExactWorkerCadencePixelFormatAndAudioPacketDuration) {
    const auto value = launch();
    stream::config_t config {};
    config.audio.channels = 2;
    config.audio.packetDuration = 5;
    config.monitor.width = value->width; config.monitor.height = value->height;
    ASSERT_TRUE(video::configure_announced_rates(config.monitor, 60, 6000, value->fps, true));
    EXPECT_TRUE(rtsp_stream::worker_media_matches_launch_for_tests(*value, config));
    for (unsigned kind = 0; kind != 11; ++kind) {
      auto bad = config;
      switch (kind) {
        case 0: bad.audio.packetDuration = 10; break;
        case 1: bad.audio.channels = 6; break;
        case 2: bad.monitor.videoFormat = 1; break;
        case 3: bad.monitor.dynamicRange = 1; break;
        case 4: bad.monitor.width = 1280; break;
        case 5: bad.monitor.height = 720; break;
        case 6: bad.monitor.chromaSamplingType = 1; break;
        case 7: bad.monitor.enableIntraRefresh = 1; break;
        case 8: bad.monitor.framerate = std::numeric_limits<int>::max(); break;
        case 9: bad.monitor.stream_rate = {60000, 1001}; break;
        case 10: bad.monitor.encode_rate = {60000, 1001}; break;
      }
      EXPECT_FALSE(rtsp_stream::worker_media_matches_launch_for_tests(*value, bad)) << kind;
    }
  }

  class MultiseatLaunchConfiguration : public ::testing::Test {
  protected:
    std::filesystem::path root, path;
    void SetUp() override {
      char pattern[] = "/tmp/polaris-launch-config-XXXXXX";
      const auto created = ::mkdtemp(pattern);
      ASSERT_NE(created, nullptr);
      root = created; path = root / "controller.json";
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    nlohmann::json sample() {
      return {{"schema", 1}, {"deployment_id", "profile-test"},
        {"profile_catalog", (root / "profiles.json").string()}, {"ipc_root", (root / "ipc").string()},
        {"selinux_type", ""}, {"gpus", nlohmann::json::array({{
          {"id", "gpu-a"}, {"render_node", "/dev/dri/renderD128"},
          {"devices", {"/dev/dri/renderD128"}}, {"max_seats", 2}, {"max_encoder_sessions", 2}
        }})}};
    }
    void save(std::string contents) { ASSERT_TRUE(private_state_file::write_atomic(path, contents)); }
  };

  TEST_F(MultiseatLaunchConfiguration, DefaultOffAndExplicitPrivateConfiguration) {
    EXPECT_FALSE(config::multiseat.enabled);
    save(sample().dump());
    const auto options = load_controller_options(path);
    ASSERT_TRUE(options);
    EXPECT_TRUE(options->enabled);
    EXPECT_TRUE(options->container.media_enabled);
    ASSERT_EQ(options->gpus.size(), 1U);
    EXPECT_EQ(options->gpus.front().max_seats, 2U);
    EXPECT_EQ(options->profile_catalog, root / "profiles.json");
  }

  TEST_F(MultiseatLaunchConfiguration, RejectsDuplicateUnknownAndInvalidGpuAuthority) {
    auto value = sample();
    auto duplicate = value.dump();
    duplicate.insert(1, "\"schema\":1,");
    save(duplicate);
    EXPECT_FALSE(load_controller_options(path));
    for (unsigned kind = 0; kind != 9; ++kind) {
      value = sample();
      switch (kind) {
        case 0: value["unexpected"] = true; break;
        case 1: value["gpus"][0]["max_seats"] = 0; break;
        case 2: value["gpus"][0]["max_encoder_sessions"] = 17; break;
        case 3: value["gpus"][0]["max_seats"] = 2.5; break;
        case 4: value["gpus"][0]["devices"] = {"/tmp/device"}; break;
        case 5: value["gpus"].push_back(value["gpus"][0]); break;
        case 6: value["selinux_type"] = "unconfined_t"; break;
        case 7: value["profile_catalog"] = (root / "ipc" / "profiles.json").string(); break;
        case 8: value["ipc_root"] = root.string(); break;
      }
      save(value.dump());
      EXPECT_FALSE(load_controller_options(path)) << kind;
    }
  }

  TEST_F(MultiseatLaunchConfiguration, RejectsPublicOrSymlinkConfiguration) {
    save(sample().dump());
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    EXPECT_FALSE(load_controller_options(path));
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    const auto link = root / "link.json";
    std::filesystem::create_symlink(path, link);
    EXPECT_FALSE(load_controller_options(link));
  }

  class MultiseatProfileHttp : public MultiseatLaunchService {
  protected:
    std::filesystem::path root;
    std::string old_path;
    crypto::p_named_cert_t client;
    void SetUp() override {
      MultiseatLaunchService::SetUp();
      char pattern[] = "/tmp/polaris-profile-http-XXXXXX";
      const auto created = ::mkdtemp(pattern);
      ASSERT_NE(created, nullptr);
      root = created; old_path = config::nvhttp.file_state;
      config::nvhttp.file_state = (root / "state.json").string();
      nvhttp::reset_pairing_state_for_tests();
      auto original = std::make_shared<crypto::named_cert_t>();
      original->uuid = "client-a";
      original->name = "Profile test client";
      static const auto cert = crypto::gen_creds("Profile launch integration", 2048).x509;
      original->cert = cert;
      ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(original, crypto::PERM::_all));
      client = nvhttp::resolve_authorized_client(original);
      ASSERT_TRUE(client);
      ASSERT_TRUE(install_profile_launch_service(service));
    }
    void TearDown() override {
      MultiseatLaunchService::TearDown();
      nvhttp::reset_pairing_state_for_tests();
      config::nvhttp.file_state = old_path;
      std::filesystem::remove_all(root);
    }
    nvhttp::args_t args() {
      return {{"rikey", std::string(32, 'a')}, {"rikeyid", "1"}, {"corever", "1"},
        {"appid", std::to_string(profile_app_id)}, {"mode", "1920x1080x60"}};
    }
  };

  TEST_F(MultiseatProfileHttp, UsesCurrentPairedIdentityAndPublishesWorkerOnlyLaunch) {
    auto request = args();
    request.emplace("uniqueid", "client-b");
    request.emplace("profile", "someone-else");
    unsigned published = 0;
    auto result = nvhttp::launch_profile_request(client, request, false, [&](const auto &value) {
      ++published;
      EXPECT_EQ(value->unique_id, client->uuid);
      EXPECT_EQ(value->perm, crypto::PERM::_game_control);
      EXPECT_TRUE(value->worker_connection_requirement()->load());
      EXPECT_TRUE(value->rtsp_cipher);
      EXPECT_FALSE(value->host_audio);
      EXPECT_TRUE(value->client_do_cmds.empty());
      return true;
    });
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(published, 1U);
    EXPECT_EQ(state->begins, 1U);
  }

  TEST_F(MultiseatProfileHttp, RevocationDuringStartupPreventsPublication) {
    state->select = false;
    std::atomic<unsigned> published {0};
    auto pending = std::async(std::launch::async, [&] {
      return nvhttp::launch_profile_request(client, args(), false, [&](const auto &) { ++published; return true; });
    });
    ASSERT_TRUE(state->await_begin());
    EXPECT_TRUE(nvhttp::unpair_client(client->uuid));
    state->select = true;
    const auto result = pending.get();
    ASSERT_TRUE(result);
    EXPECT_NE(result->status, 200);
    EXPECT_EQ(published, 0U);
    ASSERT_TRUE(result->launch);
    EXPECT_TRUE(result->launch->is_cancelled());
    EXPECT_TRUE(result->launch->worker_connection_requirement()->load());
  }

  TEST_F(MultiseatProfileHttp, PermissionReplacementDuringStartupPreventsPublication) {
    state->select = false;
    std::atomic<unsigned> published {0};
    auto pending = std::async(std::launch::async, [&] {
      return nvhttp::launch_profile_request(client, args(), false, [&](const auto &) { ++published; return true; });
    });
    ASSERT_TRUE(state->await_begin());
    auto replacement = std::make_shared<crypto::named_cert_t>();
    replacement->uuid = client->uuid; replacement->name = client->name; replacement->cert = client->cert;
    EXPECT_TRUE(nvhttp::add_authorized_client_for_tests(replacement, crypto::PERM::_default));
    state->select = true;
    const auto result = pending.get();
    ASSERT_TRUE(result);
    EXPECT_NE(result->status, 200);
    EXPECT_EQ(published, 0U);
    ASSERT_TRUE(result->launch);
    EXPECT_TRUE(result->launch->is_cancelled());
  }

  TEST_F(MultiseatProfileHttp, BusyRtspPublicationCancelsPreparedWorker) {
    const auto result = nvhttp::launch_profile_request(client, args(), false, [](const auto &) { return false; });
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 409);
    ASSERT_TRUE(result->launch);
    EXPECT_TRUE(result->launch->is_cancelled());
  }

  TEST_F(MultiseatProfileHttp, InvalidAppKeyAndUnsupportedMediaNeverStartWorker) {
    const std::vector<std::pair<std::string, std::string>> invalid {
      {"appid", "1"}, {"appuuid", "other"}, {"rikey", "aa"}, {"corever", "0"},
      {"mode", "1920x1080x59.94"}, {"mode", "1920x1080x60000x"}, {"mode", "-1x1080x60"},
      {"mode", "999999999999999999x1080x60"}, {"mode", "1920x1080x59940"},
      {"hdrMode", "1"}, {"resolvedProfile", "1"}, {"encoderBackend", "nvenc"},
      {"surroundAudioInfo", "393222"}, {"watch", "1"}, {"expectedEncoder", "h264_nvenc"}
    };
    for (const auto &[key, value] : invalid) {
      auto request = args(); request.erase(key); request.emplace(key, value);
      const auto result = nvhttp::launch_profile_request(client, request, false, [](const auto &) { return true; });
      ASSERT_TRUE(result) << key;
      EXPECT_NE(result->status, 200) << key << '=' << value;
    }
    EXPECT_EQ(state->begins, 0U);
  }

  TEST_F(MultiseatProfileHttp, UnmappedDeviceUsesOrdinaryRequestPath) {
    uninstall_profile_launch_service(service);
    const auto result = nvhttp::launch_profile_request(client, args(), false, [](const auto &) { return true; });
    EXPECT_FALSE(result);
    EXPECT_EQ(state->begins, 0U);
  }
}  // namespace
