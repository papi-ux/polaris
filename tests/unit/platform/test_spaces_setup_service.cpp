#include "src/platform/linux/spaces_setup_service.h"
#include "src/config.h"
#include "src/configuration_store.h"
#include "src/platform/linux/spaces_activation.h"
#include "src/platform/linux/multiseat_launch_service.h"
#include "src/platform/linux/multiseat_worker_authority.h"
#include "src/crypto.h"
#include "src/utility.h"
#include <Simple-Web-Server/server_https.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <set>
#include <unistd.h>

#ifdef __linux__
namespace confighttp {
  void registerSpacesSetupRoutes(SimpleWeb::ServerBase<SimpleWeb::HTTPS> &);
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
        .graphics = [](const auto &) { return json::array({{{"id", "gpu-0"}, {"label", "Test graphics"}}}); },
        .activate = [](const auto &, const auto &, auto, auto) { return true; },
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

TEST_F(SpacesSetupService, SavesContainerdIdentityBeforeProvisioningAndRetainsItAcrossRestart) {
  auto ops = operations();
  ops.install = [&](auto, auto) {
    ++installs;
    return spaces::runtime_install_result_t {true, "runtime_ready", {}, runtime().registry_digest};
  };
  ops.prepare = [&](const auto &, std::string_view image, auto) {
    ++homes;
    EXPECT_EQ(image, runtime().registry_digest);
    const auto saved = psf::read_secure(journal, 4096, false, false);
    if (!saved) { ADD_FAILURE() << "Missing durable setup record"; return false; }
    const auto body = json::parse(saved.payload);
    EXPECT_EQ(body["state"], "preparing");
    EXPECT_EQ(body["image"], image);
    return true;
  };
  {
    auto job = service(ops);
    ASSERT_EQ(job->submit(request), 202);
    ASSERT_TRUE(wait_state(*job, "prepared"));
    EXPECT_TRUE(job->snapshot()["job"]["can_activate"]);
  }
  auto resumed = service(ops);
  EXPECT_EQ(resumed->submit(request), 200);
  EXPECT_TRUE(resumed->snapshot()["job"]["can_activate"]);
  EXPECT_EQ(resumed->submit({"activate", request.request_id, {}, {}, "gpu-0"}), 202);
  ASSERT_TRUE(wait_state(*resumed, "restart_required"));
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
    const auto activate = json {{"operation", "activate"}, {"request_id", request.request_id}, {"gpu_id", "gpu-0"}}.dump();
    EXPECT_EQ(call("POST", activate, {}), 403);
    EXPECT_EQ(call("POST", activate, {{"Cookie", "auth=" + cookie}}), 403);
    EXPECT_EQ(job->snapshot()["job"]["state"], "prepared");
    EXPECT_EQ(call("POST", activate, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "setup-csrf"}}), 202);
    ASSERT_TRUE(wait_state(*job, "restart_required"));
    EXPECT_EQ(installs, 1U); EXPECT_EQ(homes, 1U);
  });
}

TEST_F(SpacesSetupService, ActivationRequiresPreparedHomeAndExactGpuAndSurvivesRestart) {
  const spaces::setup_request_t activate {"activate", request.request_id, {}, {}, "gpu-0"};
  auto job = service(operations());
  EXPECT_EQ(job->submit(activate), 409);
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_TRUE(wait_state(*job, "prepared"));
  auto wrong = activate; wrong.gpu_id = "not-discovered";
  EXPECT_EQ(job->submit(wrong), 409);
  EXPECT_TRUE(job->snapshot()["job"]["can_activate"]);
  ASSERT_EQ(job->submit(activate), 202);
  ASSERT_TRUE(wait_state(*job, "restart_required"));
  EXPECT_EQ(job->submit(activate), 200);
  EXPECT_EQ(job->submit(wrong), 409);
  job.reset();
  auto resumed = service(operations());
  EXPECT_EQ(resumed->snapshot()["job"]["state"], "restart_required");
  EXPECT_EQ(resumed->submit(activate), 200);
  EXPECT_EQ(homes, 1U); EXPECT_EQ(installs, 1U);
}

TEST_F(SpacesSetupService, ActivationInterruptionRequiresSameSelectionAndNeverRecreatesHome) {
  std::promise<void> entered;
  auto ops = operations();
  ops.activate = [&](const auto &, const auto &, auto, std::stop_token stop) {
    entered.set_value();
    std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    return false;
  };
  const spaces::setup_request_t activate {"activate", request.request_id, {}, {}, "gpu-0"};
  auto job = service(ops);
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_TRUE(wait_state(*job, "prepared"));
  ASSERT_EQ(job->submit(activate), 202);
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  EXPECT_EQ(job->submit({"cancel", request.request_id}), 409);
  job->shutdown(); job.reset();
  auto resumed = service(operations());
  EXPECT_EQ(resumed->snapshot()["job"]["state"], "activation_failed");
  EXPECT_FALSE(resumed->snapshot()["job"]["can_retry"]);
  auto wrong = activate; wrong.gpu_id = "gpu-1";
  EXPECT_EQ(resumed->submit(wrong), 409);
  ASSERT_EQ(resumed->submit(activate), 202);
  ASSERT_TRUE(wait_state(*resumed, "restart_required"));
  EXPECT_EQ(homes, 1U); EXPECT_EQ(installs, 1U);
}

TEST_F(SpacesSetupService, ActivationDecoderRejectsPathsAndAdditionalAuthority) {
  json body {{"operation", "activate"}, {"request_id", request.request_id}, {"gpu_id", "gpu-0"}};
  ASSERT_TRUE(spaces::decode_setup_request(body.dump()));
  auto extra = body; extra["max_seats"] = 16;
  EXPECT_FALSE(spaces::decode_setup_request(extra.dump()));
  for (const auto *value : {"/dev/dri/renderD128", "", "../device"}) {
    body["gpu_id"] = value;
    EXPECT_FALSE(spaces::decode_setup_request(body.dump()));
  }
}

namespace {
  class activation_host_t : public container::host_t {
  public:
    std::set<std::string> denied;
    std::uint64_t effective_uid() const override { return geteuid(); }
    std::uint64_t effective_gid() const override { return getegid(); }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return true; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return true; }
    bool private_readable_file(const std::filesystem::path &) const override { return true; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &path) const override {
      if (denied.contains(path)) return {};
      return container::character_device_identity_t {1, 1, 226, static_cast<unsigned>(std::hash<std::string>{}(path.string()))};
    }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return {}; }
    container::command_result_t run(const std::vector<std::string> &, std::chrono::milliseconds, std::size_t) override {
      ADD_FAILURE() << "Configuration must not run Docker or start a game";
      return {};
    }
  };
}

TEST_F(SpacesSetupService, ConfigurationCommitsLastPreservesSettingsAndRecreatesOnlyManagedIpc) {
  activation_host_t host;
  const spaces::activation_paths_t paths {root / "polaris.conf", root / "controller.json", root / "profiles.json", root / "ipc"};
  ASSERT_TRUE(psf::write_atomic(paths.native, "# Keep my settings\nport = 47989\nbitrate = 8000\n"));
  const profiles::catalog_t catalog {static_cast<unsigned>(geteuid()), static_cast<unsigned>(getegid()), {{
    .storage = {request.request_id, "pv-" + request.request_id, runtime_profile_e::steam, runtime().config_digest},
    .name = request.name, .workload = {workload_kind_e::steam, "big-picture-v1"}, .client_keys = {},
  }}};
  const auto original = profiles::encode(catalog);
  ASSERT_TRUE(psf::write_atomic(paths.profiles, original));
  const spaces::graphics_t graphics {{"gpu-0", "/dev/dri/renderD128", {"/dev/dri/renderD128", "/dev/dri/card0"}, 1, 1}, "Test", "default"};
  auto configure = [&] { return spaces::configure_first_space(paths, {request.request_id, request.name}, runtime().config_digest, graphics, "", host); };
  ASSERT_TRUE(configure());
  const auto committed = configuration_store::read(paths.native);
  ASSERT_TRUE(committed);
  EXPECT_TRUE(committed->contents.starts_with("# Keep my settings\nport = 47989\nbitrate = 8000\n"));
  EXPECT_EQ(config::parse_config(committed->contents).at("multiseat_enabled"), "true");
  EXPECT_EQ(psf::read_secure(paths.profiles, 4096).payload, original);
  const auto options = load_controller_options(paths.controller);
  ASSERT_TRUE(options); ASSERT_EQ(options->gpus.size(), 1U);
  EXPECT_EQ(options->gpus[0].max_seats, 1U);
  EXPECT_EQ(options->gpus[0].devices, graphics.gpu.devices);
  EXPECT_TRUE(configure());
  EXPECT_EQ(configuration_store::read(paths.native)->contents, committed->contents);
  worker_ipc::authority_store_t authority(paths.ipc);
  EXPECT_EQ(authority.status(), worker_ipc::authority_status_e::applied);
  std::filesystem::remove_all(paths.ipc);
  EXPECT_TRUE(spaces::prepare_managed_ipc(paths));
  auto wrong = paths; wrong.ipc = root / "unrelated";
  EXPECT_FALSE(spaces::prepare_managed_ipc(wrong));
  EXPECT_FALSE(std::filesystem::exists(wrong.ipc));
}

TEST_F(SpacesSetupService, ConfigurationRefusesExistingAuthoritySymlinksAndUncertainWrites) {
  activation_host_t host;
  const spaces::activation_paths_t paths {root / "polaris.conf", root / "controller.json", root / "profiles.json", root / "ipc"};
  const profiles::catalog_t catalog {static_cast<unsigned>(geteuid()), static_cast<unsigned>(getegid()), {{
    .storage = {request.request_id, "pv-" + request.request_id, runtime_profile_e::steam, runtime().config_digest},
    .name = request.name, .workload = {workload_kind_e::steam, "big-picture-v1"}, .client_keys = {},
  }}};
  ASSERT_TRUE(psf::write_atomic(paths.profiles, profiles::encode(catalog)));
  const spaces::graphics_t graphics {{"gpu-0", "/dev/dri/renderD128", {"/dev/dri/renderD128"}, 1, 1}, "Test", "default"};
  auto configure = [&] { return spaces::configure_first_space(paths, {request.request_id, request.name}, runtime().config_digest, graphics, "", host); };
  for (const auto &contents : {"multiseat_enabled = true\n", "multiseat_config = /existing/controller.json\n", "multiseat_moonlight_input = true\n"}) {
    ASSERT_TRUE(psf::write_atomic(paths.native, contents));
    EXPECT_FALSE(configure());
    EXPECT_EQ(psf::read_secure(paths.native, 4096).payload, contents);
    EXPECT_FALSE(std::filesystem::exists(paths.controller));
  }
  ASSERT_TRUE(psf::write_atomic(paths.native, "port = 47989\n"));
  std::filesystem::create_symlink(root / "another", paths.controller);
  EXPECT_FALSE(configure());
  EXPECT_FALSE(std::filesystem::exists(root / "another"));
  std::filesystem::remove(paths.controller);
  psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
  EXPECT_FALSE(configure());
  psf::set_write_fault_for_tests(psf::write_fault_e::none);
  EXPECT_EQ(psf::read_secure(paths.native, 4096).payload, "port = 47989\n");
  EXPECT_TRUE(configure());
}

TEST_F(SpacesSetupService, GraphicsDiscoveryPairsPhysicalNodesAndRejectsMissingAccess) {
  namespace fs = std::filesystem;
  activation_host_t host;
  spaces::graphics_roots_t roots {root / "class", root / "nvidia"};
  const auto device = root / "devices/0000:01:00.0";
  fs::create_directories(device / "drm/card2");
  fs::create_directories(roots.drm / "renderD128");
  fs::create_directory_symlink(device, roots.drm / "renderD128/device");
  std::ofstream(device / "vendor") << "0x1002\n";
  auto cards = spaces::discover_graphics(host, roots);
  ASSERT_EQ(cards.size(), 1U);
  EXPECT_EQ(cards[0].gpu.devices, (std::vector<fs::path> {"/dev/dri/renderD128", "/dev/dri/card2"}));
  EXPECT_EQ(cards[0].gpu.logical_gpu_id, "pci-0000_01_00.0");
  host.denied.insert("/dev/dri/card2");
  EXPECT_TRUE(spaces::discover_graphics(host, roots).empty());
  host.denied.clear();
  std::ofstream(device / "vendor") << "0x10de\n";
  EXPECT_TRUE(spaces::discover_graphics(host, roots).empty());
  fs::create_directories(roots.nvidia / "0000:01:00.0");
  std::ofstream(roots.nvidia / "0000:01:00.0/information") << "Device Minor: \t 3\n";
  cards = spaces::discover_graphics(host, roots);
  ASSERT_EQ(cards.size(), 1U);
  EXPECT_EQ(cards[0].gpu.devices[2], "/dev/nvidia3");
  fs::create_directories(roots.drm / "renderD129");
  fs::create_directory_symlink(device, roots.drm / "renderD129/device");
  EXPECT_EQ(spaces::discover_graphics(host, roots).size(), 1U);
}
TEST_F(SpacesSetupService, ManagedSocketPathsLeaveRoomForGenerationGrowth) {
  const auto paths = spaces::activation_paths(root, root / "polaris.conf");
  const auto socket = paths.ipc / ("polaris-runtime-" + request.request_id + "-1000000") / "ipc/control.sock";
  EXPECT_LT(socket.string().size(), 108U);
}

TEST_F(SpacesSetupService, NewJournalSchemaRequiresTheExactGraphicsField) {
  auto job = service(operations());
  ASSERT_EQ(job->submit(request), 202);
  ASSERT_TRUE(wait_state(*job, "prepared"));
  job.reset();
  auto saved = json::parse(psf::read_secure(journal, 4096).payload);
  ASSERT_EQ(saved["schema"], 2);
  saved.erase("gpu_id"); saved["unknown"] = "";
  ASSERT_TRUE(psf::write_atomic(journal, saved.dump()));
  auto resumed = service(operations());
  EXPECT_EQ(resumed->submit(request), 503);
  EXPECT_EQ(homes, 1U); EXPECT_EQ(installs, 1U);
}

#endif
