#include "src/platform/linux/spaces_setup_service.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <unistd.h>

#ifdef __linux__
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
#endif
