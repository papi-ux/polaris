#include "src/platform/linux/spaces_setup_service.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/utility.h"
#include <Simple-Web-Server/server_https.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <unistd.h>

#ifdef __linux__
namespace confighttp {
  void registerSpacesSetupRoutes(SimpleWeb::Server<SimpleWeb::HTTPS> &);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
    const std::function<void(const std::string &)> &);
}
namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  namespace psf = private_state_file;
  using namespace std::chrono_literals;
  const spaces::setup_request_t request {"start", "12345678-1234-1234-1234-123456789abc", "steam-test", "Living room"};
  spaces::runtime_t runtime() {
    return {"steam-test", "default", std::string(40, 'a'), "sha256:" + std::string(64, 'b'),
      "sha256:" + std::string(64, 'c'), ""};
  }
  class SpacesSetupService : public ::testing::Test {
  protected:
    std::filesystem::path root, journal;
    std::atomic<unsigned> installs {0}, homes {0};
    void SetUp() override {
      std::array<char, 64> pattern {};
      const std::string value = "/tmp/polaris-spaces-job-XXXXXX";
      std::copy(value.begin(), value.end(), pattern.begin());
      ASSERT_NE(mkdtemp(pattern.data()), nullptr);
      root = pattern.data(); journal = root / "setup.json";
    }
    void TearDown() override {
      psf::set_write_fault_for_tests(psf::write_fault_e::none);
      std::filesystem::remove_all(root);
    }
    spaces::setup_operations_t operations() {
      return {
        .install = [this](std::string_view id, std::stop_token) {
          ++installs; EXPECT_EQ(id, request.runtime_id);
          return spaces::runtime_install_result_t {true, "runtime_ready", {}, runtime().config_digest};
        },
        .prepare = [this](const profiles::first_steam_request_t &r, std::string_view image, std::stop_token) {
          ++homes; EXPECT_EQ(r.request_id, request.request_id); EXPECT_EQ(r.name, request.name);
          EXPECT_EQ(image, runtime().config_digest);
          return true;
        },
      };
    }
    std::unique_ptr<spaces::setup_service_t> service(spaces::setup_operations_t ops) {
      return std::make_unique<spaces::setup_service_t>(journal, std::vector {runtime()}, true, std::move(ops));
    }
    bool wait_state(spaces::setup_service_t &job, std::string_view state) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (job.snapshot()["job"]["state"] == state) return true;
        std::this_thread::sleep_for(1ms);
      }
      return false;
    }
  };
}

TEST_F(SpacesSetupService, RejectsAmbiguousRequestsAndClientSuppliedPaths) {
  const json start {{"operation", "start"}, {"request_id", request.request_id}, {"runtime_id", request.runtime_id}, {"name", request.name}};
  EXPECT_TRUE(spaces::decode_setup_request(start.dump()));
  EXPECT_TRUE(spaces::decode_setup_request(json {{"operation", "cancel"}, {"request_id", request.request_id}}.dump()));
  for (const auto &[key, value] : std::vector<std::pair<std::string, json>> {
      {"operation", "activate"}, {"request_id", "../another-home"}, {"runtime_id", "image:latest"},
      {"name", " trailing "}, {"name", "bad\nname"}, {"catalog", "/tmp/catalog"}, {"name", json::object()}}) {
    auto bad = start; bad[key] = value;
    EXPECT_FALSE(spaces::decode_setup_request(bad.dump())) << key;
  }
  auto duplicate = start.dump(); duplicate.insert(1, "\"operation\":\"cancel\",");
  EXPECT_FALSE(spaces::decode_setup_request(duplicate));
  EXPECT_FALSE(spaces::decode_setup_request(std::string(4097, ' ')));
}

TEST_F(SpacesSetupService, NavigationAndRestartRetainOneCompletedRequestWithoutReprovisioning) {
  {
    auto job = service(operations());
    ASSERT_EQ(job->submit(request), 202);
    ASSERT_TRUE(wait_state(*job, "prepared"));
    EXPECT_EQ(job->submit(request), 200);
    const auto saved = job->snapshot();
    EXPECT_EQ(saved["job"]["request_id"], request.request_id);
    EXPECT_FALSE(saved["job"]["can_retry"]);
    EXPECT_FALSE(saved["job"]["can_cancel"]);
  }
  auto resumed = service(operations());
  EXPECT_EQ(resumed->snapshot()["job"]["state"], "prepared");
  EXPECT_EQ(resumed->submit(request), 200);
  EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 1U);
}

TEST_F(SpacesSetupService, SerializesDuplicatesAndFencesStaleCancellation) {
  std::promise<void> entered;
  auto ops = operations();
  ops.install = [&](std::string_view, std::stop_token stop) {
    ++installs; entered.set_value();
    std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    return spaces::runtime_install_result_t {};
  };
  auto job = service(ops);
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  auto newer = request; newer.request_id.back() = 'd';
  EXPECT_EQ(job->submit(newer), 409);
  EXPECT_EQ(job->submit({"cancel", newer.request_id, {}, {}}), 409);
  EXPECT_EQ(job->submit(request), 202);
  EXPECT_EQ(job->submit({"cancel", request.request_id, {}, {}}), 202);
  ASSERT_TRUE(wait_state(*job, "cancelled"));
  EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 0U);
}

TEST_F(SpacesSetupService, ShutdownStopsTheDownloadAndRestartRequiresExplicitRetry) {
  std::promise<void> entered;
  auto ops = operations();
  ops.install = [&](std::string_view, std::stop_token stop) {
    ++installs; entered.set_value();
    std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    return spaces::runtime_install_result_t {};
  };
  {
    auto job = service(ops);
    ASSERT_EQ(job->submit(request), 202);
    ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
    job->shutdown();
    EXPECT_EQ(job->submit(request), 503);
  }
  auto resumed = service(operations());
  EXPECT_TRUE(resumed->snapshot()["job"]["can_retry"]);
  EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 0U);
  ASSERT_EQ(resumed->submit(request), 202);
  ASSERT_TRUE(wait_state(*resumed, "prepared"));
  EXPECT_EQ(installs, 2U); EXPECT_EQ(homes, 1U);
}

TEST_F(SpacesSetupService, CannotCancelOrReplaceAHomeBeingCommitted) {
  std::promise<void> entered;
  auto ops = operations();
  ops.prepare = [&](const auto &, std::string_view, std::stop_token stop) {
    ++homes; entered.set_value();
    std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    return true; // A durable commit wins over a concurrent shutdown.
  };
  auto job = service(ops);
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  EXPECT_EQ(job->snapshot()["job"]["state"], "preparing");
  EXPECT_FALSE(job->snapshot()["job"]["can_cancel"]);
  EXPECT_EQ(job->submit({"cancel", request.request_id, {}, {}}), 409);
  auto other = request; other.name = "Other";
  EXPECT_EQ(job->submit(other), 409);
  job->shutdown();
  EXPECT_EQ(job->snapshot()["job"]["state"], "prepared");
  EXPECT_EQ(homes, 1U);
}

TEST_F(SpacesSetupService, RefusesASecondOwnerAndUnpublishedOrAlreadyConfiguredSetup) {
  auto first = service(operations());
  auto second = service(operations());
  EXPECT_FALSE(second->snapshot()["available"]);
  EXPECT_EQ(second->submit(request), 503);
  spaces::setup_service_t unpublished(root / "unpublished.json", {}, true, operations());
  spaces::setup_service_t configured(root / "configured.json", {runtime()}, false, operations());
  EXPECT_EQ(unpublished.submit(request), 503);
  EXPECT_EQ(configured.submit(request), 503);
  EXPECT_FALSE(std::filesystem::exists(root / "unpublished.json.owner"));
  EXPECT_FALSE(std::filesystem::exists(root / "configured.json.owner"));
  EXPECT_EQ(installs, 0U); EXPECT_EQ(homes, 0U);
}

TEST_F(SpacesSetupService, FailedDurabilityCannotAdmitEffectsOrOverwriteUncertainState) {
  auto job = service(operations());
  psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
  EXPECT_EQ(job->submit(request), 503);
  psf::set_write_fault_for_tests(psf::write_fault_e::none);
  EXPECT_EQ(job->submit(request), 503);
  EXPECT_EQ(installs, 0U); EXPECT_EQ(homes, 0U);
  EXPECT_EQ(job->snapshot()["job"]["state"], "recovery_required");
  job.reset();
  auto resumed = service(operations());
  EXPECT_EQ(resumed->snapshot()["job"]["state"], "interrupted");
  EXPECT_EQ(installs, 0U);
  ASSERT_EQ(resumed->submit(request), 202);
  ASSERT_TRUE(wait_state(*resumed, "prepared"));
}

TEST_F(SpacesSetupService, InvalidJournalAndChangedRuntimeCannotBeSilentlyReplaced) {
  ASSERT_TRUE(psf::write_atomic(journal, "{invalid"));
  auto corrupt = service(operations());
  EXPECT_EQ(corrupt->submit(request), 503);
  EXPECT_EQ(psf::read_secure(journal, 4096).payload, "{invalid");
  corrupt.reset();
  std::filesystem::remove(journal);
  auto ops = operations();
  ops.install = [&](auto, auto) { ++installs; return spaces::runtime_install_result_t {false, "download_incomplete", {}, {}}; };
  {
    auto job = service(ops);
    ASSERT_EQ(job->submit(request), 202);
    ASSERT_TRUE(wait_state(*job, "failed"));
  }
  auto changed = runtime(); changed.registry_digest.back() = 'd';
  spaces::setup_service_t upgraded(journal, {changed}, true, operations());
  EXPECT_EQ(upgraded.submit(request), 409);
  EXPECT_FALSE(upgraded.snapshot()["job"]["can_retry"]);
  EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 0U);
}

TEST_F(SpacesSetupService, FailedVerificationNeverCreatesAHome) {
  auto ops = operations();
  ops.install = [&](auto, auto) {
    ++installs;
    return spaces::runtime_install_result_t {true, "runtime_ready", {}, "sha256:" + std::string(64, 'd')};
  };
  auto job = service(ops);
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_TRUE(wait_state(*job, "failed"));
  EXPECT_EQ(homes, 0U);
}

TEST_F(SpacesSetupService, ProductionTlsRoutesRequireAdminAuthenticationAndCookieCsrf) {
  const auto old_config = config::sunshine;
  auto restore = util::fail_guard([&] { config::sunshine = old_config; });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-setup-test-key";
  const auto credentials = crypto::gen_creds("localhost", 2048);
  ASSERT_TRUE(psf::write_atomic(root / "cert.pem", credentials.x509));
  ASSERT_TRUE(psf::write_atomic(root / "key.pem", credentials.pkey));
  auto job = std::make_shared<spaces::setup_service_t>(journal, std::vector {runtime()}, true, operations());
  ASSERT_TRUE(spaces::install_setup_service(job));
  auto release = util::fail_guard([&] {
    job->shutdown();
    spaces::uninstall_setup_service(job);
  });
  confighttp::with_web_session_for_tests(root / "sessions.json", "setup-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((root / "cert.pem").string(), (root / "key.pem").string());
    server.config.address = "127.0.0.1"; server.config.port = 0;
    server.config.timeout_request = 5; server.config.timeout_content = 5;
    // Exercise the same registration used by confighttp::start, including its
    // CSRF wrapper, rather than wrapping a test-only copy of the handler.
    confighttp::registerSpacesSetupRoutes(server);
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    const auto start = json {{"operation", "start"}, {"request_id", request.request_id},
      {"runtime_id", request.runtime_id}, {"name", request.name}}.dump();
    auto call = [&](const std::string &method, const std::string &body, SimpleWeb::CaseInsensitiveMultimap headers) {
      headers.emplace("Content-Type", "application/json");
      return std::stoi(client.request(method, "/api/spaces/setup/job", body, headers)->status_code);
    };
    EXPECT_EQ(call("GET", "", {}), 401);
    EXPECT_EQ(call("POST", start, {}), 403);
    EXPECT_EQ(call("POST", start, {{"X-CSRF-Token", "setup-csrf"}}), 401);
    EXPECT_EQ(call("POST", start, {{"Cookie", "auth=" + cookie}}), 403);
    EXPECT_EQ(call("POST", start, {{"Cookie", "auth=" + cookie}, {"Authorization", "Bearer wrong"}}), 403);
    EXPECT_EQ(call("GET", "", {{"Cookie", "auth=" + cookie}}), 200);
    EXPECT_EQ(installs, 0U); EXPECT_EQ(homes, 0U);
    EXPECT_EQ(call("POST", "{}", {{"Authorization", "Bearer isolated-setup-test-key"}}), 400);
    ASSERT_EQ(call("POST", start, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "setup-csrf"}}), 202);
    ASSERT_TRUE(wait_state(*job, "prepared"));
    EXPECT_EQ(call("POST", start, {{"Authorization", "Bearer isolated-setup-test-key"}}), 200);
    EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 1U);
  });
}
#endif
