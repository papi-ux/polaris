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
#include <fstream>
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
    std::atomic<bool> select {true}, close {true}, fail {false}, idle {true};
    std::atomic<unsigned> destroyed {0};
    spaces::library_reader_t library;
    std::vector<std::string> desktops;
    void called() { std::lock_guard lock(mutex); owners.push_back(std::this_thread::get_id()); }
    bool await_begin() {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, 2s, [&] { return begins.load() != 0; });
    }
  };

  class controller_t final : public profile_controller_t {
  public:
    explicit controller_t(std::shared_ptr<controller_state_t> state,
      std::vector<profile_summary_t> catalog = {{"12345678-1234-4234-8234-123456789abc", "Primary", {"client-a", "client-b"}}}) :
      state_(std::move(state)), catalog_(std::move(catalog)) {}
    ~controller_t() override { ++state_->destroyed; }
    bool routes_client(std::string_view client) const override { return profile_for_client(client).has_value(); }
    std::optional<std::string> profile_for_client(std::string_view client) const override {
      for (const auto &profile : catalog_)
        if (std::find(profile.clients.begin(), profile.clients.end(), client) != profile.clients.end()) return profile.id;
      return std::nullopt;
    }
    std::vector<profile_summary_t> profile_catalog() const override { return catalog_; }
    spaces::library_reader_t library_reader() const override { return state_->library; }
    std::vector<std::string> desktop_clients() const override { return state_->desktops; }
    bool idle() const override { return state_->idle; }
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
    const std::vector<profile_summary_t> catalog_;
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

  class MultiseatAssignments : public MultiseatLaunchService {
  protected:
    std::vector<profile_summary_t> catalog {{"profile-a", "Alex", {"client-a"}, true}, {"profile-b", "Sam", {"client-b"}}};
    std::atomic<unsigned> writes {0}, reloads {0}, creates {0}, edits {0};
    private_state_file::write_status_e write_status = private_state_file::write_status_e::committed;
    bool reload_fails = false;
    std::function<void()> before_write;
    void SetUp() override {
      service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state, catalog), 2s,
        profile_admin_options_t {
          .reload = [&]() -> std::unique_ptr<profile_controller_t> {
            ++reloads;
            if (reload_fails) return {};
            return std::make_unique<controller_t>(state, catalog);
          },
          .persist = [&](std::string_view profile, std::string_view client) {
            state->called();
            ++writes;
            EXPECT_GT(state->destroyed.load(), 0U);
            if (before_write) before_write();
            if (write_status != private_state_file::write_status_e::not_committed) {
              for (auto &entry : catalog) std::erase(entry.clients, client);
              for (auto &entry : catalog) if (entry.id == profile) entry.clients.emplace_back(client);
            }
            return profiles::change_result_t {.status = write_status};
          },
          .create = [&](const profiles::steam_create_request_t &request) {
            state->called();
            ++creates;
            EXPECT_GT(state->destroyed.load(), 0U);
            if (before_write) before_write();
            if (write_status != private_state_file::write_status_e::not_committed)
              catalog.push_back({request.request_id, request.name, {}, true});
            return profiles::change_result_t {.status = write_status, .profile_key = request.request_id};
          },
          .edit = [&](const profiles::edit_request_t &request) {
            state->called(); ++edits;
            EXPECT_GT(state->destroyed.load(), 0U);
            if (before_write) before_write();
            if (write_status != private_state_file::write_status_e::not_committed) {
              for (auto &entry : catalog) if (entry.id == request.profile_id) {
                if (request.operation == profiles::edit_operation_e::rename) entry.name = request.name;
                else { entry.archived = request.operation == profiles::edit_operation_e::remove; if (entry.archived) entry.clients.clear(); }
              }
            }
            return profiles::change_result_t {.status = write_status};
          }
        });
    }
  };

  const profiles::steam_create_request_t create_request {
    "12345678-1234-4234-8234-123456789abc", "profile-a", "Second player"
  };

  const profiles::edit_request_t remove_request {profiles::edit_operation_e::remove, "profile-a", ""};

  TEST_F(MultiseatAssignments, RemovalClearsOnlyItsRoutesAndRestorationRequiresNewAssignment) {
    EXPECT_TRUE(service->admin_snapshot().management_available);
    ASSERT_EQ(service->edit_profile(remove_request).status, 200);
    EXPECT_FALSE(service->routes_client("client-a"));
    EXPECT_TRUE(service->routes_client("client-b"));
    EXPECT_TRUE(service->admin_snapshot().profiles[0].archived);
    EXPECT_EQ(service->set_assignment("profile-a", "client-a").status, 404);
    ASSERT_EQ(service->edit_profile({profiles::edit_operation_e::restore, "profile-a", ""}).status, 200);
    EXPECT_FALSE(service->routes_client("client-a"));
    EXPECT_FALSE(service->admin_snapshot().profiles[0].archived);
    ASSERT_EQ(service->set_assignment("profile-a", "client-a").status, 200);
    ASSERT_EQ(service->edit_profile({profiles::edit_operation_e::rename, "profile-a", "Player one"}).status, 200);
    EXPECT_EQ(service->profile_name_for_client("client-a"), "Player one");
    EXPECT_EQ(edits, 3U);
    std::lock_guard lock(state->mutex);
    for (const auto owner : state->owners) EXPECT_EQ(owner, state->owners.front());
  }

  TEST_F(MultiseatAssignments, RemovalRefusesActiveStreamsAndUnfinishedCleanupWithoutCancellation) {
    const auto active = launch();
    ASSERT_EQ(service->prepare(active, "profile-a").status, 200);
    EXPECT_EQ(service->edit_profile(remove_request).status, 409);
    EXPECT_FALSE(active->is_cancelled());
    active->cancel(); state->idle = false;
    EXPECT_EQ(service->edit_profile(remove_request).status, 409);
    EXPECT_EQ(edits, 0U); EXPECT_EQ(state->shutdowns, 0U);
  }

  TEST_F(MultiseatAssignments, ConcurrentRemovalRetriesKeepRoutesFencedUntilOneCommit) {
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    before_write = [&] { entered.set_value(); released.wait(); };
    auto first = std::async(std::launch::async, [&] { return service->edit_profile(remove_request); });
    EXPECT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 503);
    EXPECT_EQ(service->edit_profile(remove_request).status, 202);
    EXPECT_EQ(edits, 1U);
    release.set_value(); first.get();
    for (int i = 0; i < 100 && service->admin_snapshot().changing; ++i) std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(service->admin_snapshot().changing);
    EXPECT_FALSE(service->routes_client("client-a"));
    EXPECT_EQ(edits, 1U);
  }

  TEST_F(MultiseatAssignments, FailedRemovalRestoresRoutesAndUncertainDurabilityFailsClosed) {
    write_status = private_state_file::write_status_e::not_committed;
    EXPECT_EQ(service->edit_profile(remove_request).status, 409);
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-a");
    write_status = private_state_file::write_status_e::durability_uncertain;
    EXPECT_EQ(service->edit_profile(remove_request).status, 503);
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 503);
    EXPECT_EQ(reloads, 1U);
  }

  TEST_F(MultiseatAssignments, RemovalCannotMutateAfterUnprovenShutdown) {
    state->close = false;
    EXPECT_EQ(service->edit_profile(remove_request).status, 503);
    EXPECT_EQ(edits, 0U);
    EXPECT_TRUE(service->routes_client("client-a"));
  }

  TEST_F(MultiseatAssignments, RemovalWithFailedReloadRetainsTheOldRoutingFence) {
    reload_fails = true;
    EXPECT_EQ(service->edit_profile(remove_request).status, 503);
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->edit_profile(remove_request).status, 503);
  }

  TEST_F(MultiseatAssignments, CreatesAnUnassignedSteamProfileUnderTheControllerOwner) {
    EXPECT_TRUE(service->admin_snapshot().creation_available);
    ASSERT_EQ(service->create_steam_profile(create_request).status, 200);
    const auto snapshot = service->admin_snapshot();
    ASSERT_EQ(snapshot.profiles.size(), 3U);
    EXPECT_EQ(snapshot.profiles.back().id, create_request.request_id);
    EXPECT_TRUE(snapshot.profiles.back().clients.empty());
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-a");
    EXPECT_EQ(creates, 1U);
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(reloads, 1U);
    std::lock_guard lock(state->mutex);
    ASSERT_FALSE(state->owners.empty());
    for (const auto owner : state->owners) {
      EXPECT_EQ(owner, state->owners.front());
      EXPECT_NE(owner, std::this_thread::get_id());
    }
  }

  TEST_F(MultiseatAssignments, CreationCannotCancelActiveStreamsOrSkipCleanup) {
    const auto active = launch();
    ASSERT_EQ(service->prepare(active, "profile-a").status, 200);
    EXPECT_EQ(service->create_steam_profile(create_request).status, 409);
    EXPECT_FALSE(active->is_cancelled());
    active->cancel();
    state->idle = false;
    EXPECT_EQ(service->create_steam_profile(create_request).status, 409);
    EXPECT_EQ(creates, 0U);
    EXPECT_EQ(state->shutdowns, 0U);
  }

  TEST_F(MultiseatAssignments, CreationRejectsInvalidAndUnsupportedSourcesBeforeShutdown) {
    auto request = create_request; request.source_profile_id = "profile-b";
    EXPECT_EQ(service->create_steam_profile(request).status, 404);
    request.source_profile_id = "unknown";
    EXPECT_EQ(service->create_steam_profile(request).status, 404);
    request = create_request; request.name = "\n";
    EXPECT_EQ(service->create_steam_profile(request).status, 400);
    EXPECT_EQ(creates, 0U);
    EXPECT_EQ(state->shutdowns, 0U);
  }

  TEST_F(MultiseatAssignments, ConcurrentCreationRetriesJoinOneTransactionAndRetainExistingRoutes) {
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    before_write = [&] { entered.set_value(); released.wait(); };
    auto first = std::async(std::launch::async, [&] { return service->create_steam_profile(create_request); });
    EXPECT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(service->admin_snapshot().changing);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 503);
    EXPECT_NE(service->set_assignment("profile-a", "new-client").status, 200);
    auto changed = create_request; changed.name = "Different player";
    EXPECT_EQ(service->create_steam_profile(changed).status, 409);
    // Let a retry reach its bounded response while the original remains owned.
    EXPECT_EQ(service->create_steam_profile(create_request).status, 202);
    EXPECT_EQ(creates, 1U);
    release.set_value();
    const auto status = first.get().status;
    EXPECT_TRUE(status == 200 || status == 202);
    for (int i = 0; i < 100 && service->admin_snapshot().changing; ++i) std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(service->admin_snapshot().changing);
    EXPECT_FALSE(service->admin_snapshot().failed);
    EXPECT_EQ(service->admin_snapshot().profiles.size(), 3U);
    EXPECT_EQ(creates, 1U);
    EXPECT_EQ(reloads, 1U);
  }

  TEST_F(MultiseatAssignments, FailedCreationRestoresExistingAssignmentsWithoutPublishingAProfile) {
    write_status = private_state_file::write_status_e::not_committed;
    EXPECT_EQ(service->create_steam_profile(create_request).status, 409);
    EXPECT_FALSE(service->admin_snapshot().failed);
    EXPECT_EQ(service->admin_snapshot().profiles.size(), 2U);
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-a");
  }

  TEST_F(MultiseatAssignments, UncertainCreationDurabilityKeepsExistingRoutesUnavailable) {
    write_status = private_state_file::write_status_e::durability_uncertain;
    EXPECT_EQ(service->create_steam_profile(create_request).status, 503);
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 503);
    EXPECT_EQ(reloads, 0U);
    EXPECT_EQ(service->create_steam_profile(create_request).status, 503);
    EXPECT_EQ(creates, 1U);
  }

  TEST_F(MultiseatAssignments, CreationCannotProceedAfterUnprovenShutdownOrFailedReload) {
    state->close = false;
    EXPECT_EQ(service->create_steam_profile(create_request).status, 503);
    EXPECT_EQ(creates, 0U);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_TRUE(service->admin_snapshot().failed);
  }

  TEST_F(MultiseatAssignments, CreatedProfileWithFailedReloadIsNotReportedReady) {
    reload_fails = true;
    EXPECT_EQ(service->create_steam_profile(create_request).status, 503);
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_EQ(service->create_steam_profile(create_request).status, 503);
    EXPECT_EQ(creates, 1U);
  }

  TEST_F(MultiseatAssignments, ProfileNamesAreLimitedToTheAssignedDevice) {
    EXPECT_EQ(service->profile_name_for_client("client-a"), "Alex");
    EXPECT_EQ(service->profile_name_for_client("client-b"), "Sam");
    EXPECT_FALSE(service->profile_name_for_client("unknown-client"));
    service->stop_admission();
    EXPECT_FALSE(service->profile_name_for_client("client-a"));
  }

  TEST_F(MultiseatAssignments, MovesAndUnassignsWithoutRetainingStaleLaunchAuthority) {
    ASSERT_EQ(service->set_assignment("profile-b", "client-a").status, 200);
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-b");
    EXPECT_EQ(service->profile_for_client("client-b"), "profile-b");
    EXPECT_EQ(service->profile_name_for_client("client-a"), "Sam");
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 409);
    EXPECT_EQ(state->begins.load(), 0U);
    ASSERT_EQ(service->set_assignment("", "client-a").status, 200);
    EXPECT_FALSE(service->routes_client("client-a"));
    EXPECT_TRUE(service->routes_client("client-b"));
    EXPECT_FALSE(service->profile_name_for_client("client-a"));
    EXPECT_EQ(service->profile_name_for_client("client-b"), "Sam");
    EXPECT_EQ(service->admin_snapshot().profiles[1].clients, std::vector<std::string> {"client-b"});
    EXPECT_EQ(writes, 2U);
    EXPECT_EQ(reloads, 2U);
  }

  TEST_F(MultiseatAssignments, ActiveAndStartingLaunchesRejectChangesWithoutCancellation) {
    const auto active = launch();
    ASSERT_EQ(service->prepare(active, "profile-a").status, 200);
    EXPECT_EQ(service->set_assignment("profile-b", "client-a").status, 409);
    EXPECT_FALSE(active->is_cancelled());
    EXPECT_EQ(writes, 0U);
    active->cancel();
    state->select = false;
    state->begins = 0;
    const auto starting = launch();
    auto pending = std::async(std::launch::async, [&] { return service->prepare(starting, "profile-a"); });
    ASSERT_TRUE(state->await_begin());
    EXPECT_EQ(service->set_assignment("profile-b", "client-a").status, 409);
    EXPECT_FALSE(starting->is_cancelled());
    starting->cancel();
    EXPECT_EQ(pending.get().status, 409);
    EXPECT_EQ(writes, 0U);
  }

  TEST_F(MultiseatAssignments, CleanupAndUnknownTargetsCannotMutateTheCatalog) {
    EXPECT_EQ(service->set_assignment("missing", "client-a").status, 404);
    state->idle = false;
    EXPECT_EQ(service->set_assignment("profile-b", "new-client").status, 409);
    EXPECT_FALSE(service->routes_client("new-client"));
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(state->shutdowns, 0U);
  }

  TEST_F(MultiseatAssignments, ConcurrentLaunchesAndEditsStayBlockedUntilReplacementIsPublished) {
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    before_write = [&] { entered.set_value(); released.wait(); };
    auto change = std::async(std::launch::async, [&] { return service->set_assignment("profile-a", "new-client"); });
    const auto reached = entered.get_future().wait_for(2s);
    EXPECT_EQ(reached, std::future_status::ready);
    EXPECT_TRUE(service->admin_snapshot().changing);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_TRUE(service->routes_client("new-client"));
    EXPECT_FALSE(service->profile_for_client("new-client"));
    EXPECT_FALSE(service->profile_name_for_client("new-client"));
    EXPECT_FALSE(service->profile_name_for_client("client-a"));
    EXPECT_EQ(service->prepare(launch("new-client"), "profile-a").status, 503);
    EXPECT_NE(service->set_assignment("profile-b", "client-a").status, 200);
    release.set_value();
    ASSERT_EQ(change.get().status, 200);
    EXPECT_FALSE(service->admin_snapshot().changing);
    EXPECT_EQ(service->profile_for_client("new-client"), "profile-a");
  }

  TEST_F(MultiseatAssignments, FailedWriteRestoresThePreviouslyConfirmedCatalog) {
    write_status = private_state_file::write_status_e::not_committed;
    EXPECT_EQ(service->set_assignment("profile-b", "client-a").status, 409);
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-a");
    EXPECT_FALSE(service->admin_snapshot().failed);
    EXPECT_EQ(reloads, 1U);
  }

  TEST_F(MultiseatAssignments, UncertainDurabilityRetainsOldAndNewRoutesWithoutHostFallback) {
    write_status = private_state_file::write_status_e::durability_uncertain;
    EXPECT_EQ(service->set_assignment("profile-a", "new-client").status, 503);
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_TRUE(service->routes_client("new-client"));
    EXPECT_FALSE(service->profile_for_client("client-a"));
    EXPECT_FALSE(service->profile_name_for_client("client-a"));
    EXPECT_FALSE(service->profile_name_for_client("new-client"));
    EXPECT_EQ(service->prepare(launch(), "profile-a").status, 503);
    EXPECT_EQ(reloads, 0U);
  }

  TEST_F(MultiseatAssignments, FailedReloadRetainsRoutesAndDisablesFurtherEdits) {
    reload_fails = true;
    EXPECT_EQ(service->set_assignment("", "client-a").status, 503);
    EXPECT_TRUE(service->routes_client("client-a"));
    EXPECT_TRUE(service->admin_snapshot().failed);
    EXPECT_EQ(service->set_assignment("profile-b", "client-a").status, 503);
    EXPECT_EQ(writes, 1U);
  }

  TEST_F(MultiseatAssignments, UnprovenShutdownCannotWriteOrLoseTheRequestedDeviceRoute) {
    state->close = false;
    EXPECT_EQ(service->set_assignment("profile-b", "new-client").status, 503);
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(reloads, 0U);
    EXPECT_TRUE(service->routes_client("new-client"));
    EXPECT_TRUE(service->admin_snapshot().failed);
  }

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
    EXPECT_EQ(state->begins.load(), 0U);
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
    EXPECT_EQ(state->begins.load(), 0U);
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

  TEST_F(MultiseatProfileHttp, StatusContainsOnlyTheRequestingDevicesWorkerSettings) {
    const auto other = launch("client-b");
    ASSERT_TRUE(service->prepare(other).prepared());
    ASSERT_TRUE(other->try_begin_setup_handoff());
    ASSERT_TRUE(other->commit_setup_start());
    auto idle = nvhttp::profile_session_status(client);
    ASSERT_TRUE(idle);
    EXPECT_FALSE(idle->body["streaming_active"]);
    EXPECT_EQ(idle->body["session_token"], "");
    EXPECT_EQ(idle->body["game_id"], 0);
    EXPECT_FALSE(idle->body.contains("doctor"));
    EXPECT_FALSE(idle->body.contains("owner_device_name"));
    EXPECT_FALSE(idle->body["capture"].contains("cpu_copy"));
    EXPECT_EQ(idle->body["display_mode"]["label"], "Primary");
    const auto own = launch();
    ASSERT_TRUE(service->prepare(own).prepared());
    ASSERT_TRUE(own->try_begin_setup_handoff());
    ASSERT_TRUE(own->commit_setup_start());
    const auto status = nvhttp::profile_session_status(client);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->body["session_token"], own->session_token);
    EXPECT_NE(status->body["session_token"], other->session_token);
    EXPECT_TRUE(status->body["owned_by_client"]);
    EXPECT_TRUE(status->body["streaming_active"]);
    EXPECT_EQ(status->body["game"], "Primary");
    EXPECT_EQ(status->body["capture"]["resolution"], "1920x1080");
    EXPECT_EQ(status->body["encoder"]["session_target_fps"], 60);
    EXPECT_FALSE(status->body["encoder"].contains("fps"));
    EXPECT_FALSE(status->body["controls"]["host_tuning_allowed"]);
    own->cancel();
    EXPECT_FALSE(nvhttp::profile_session_status(client)->body["streaming_active"]);
    EXPECT_FALSE(other->is_cancelled());
  }

  TEST_F(MultiseatProfileHttp, ProfileStopRequiresCurrentAuthorizationAndExactOwnToken) {
    const auto own = launch(), other = launch("client-b");
    ASSERT_TRUE(service->prepare(own).prepared());
    ASSERT_TRUE(service->prepare(other).prepared());
    EXPECT_EQ(nvhttp::stop_profile_session(client, "")->status, 400);
    EXPECT_EQ(nvhttp::stop_profile_session(client, other->session_token)->status, 409);
    EXPECT_FALSE(own->is_cancelled());
    EXPECT_FALSE(other->is_cancelled());
    EXPECT_EQ(nvhttp::stop_profile_session(client, own->session_token)->status, 200);
    EXPECT_TRUE(own->is_cancelled());
    EXPECT_FALSE(other->is_cancelled());
    const auto replacement = launch();
    ASSERT_TRUE(service->prepare(replacement).prepared());
    EXPECT_EQ(nvhttp::stop_profile_session(client, own->session_token)->status, 409);
    EXPECT_FALSE(replacement->is_cancelled());
    nvhttp::reset_pairing_state_for_tests();
    EXPECT_EQ(nvhttp::stop_profile_session(client, replacement->session_token)->status, 401);
    EXPECT_EQ(nvhttp::profile_session_status(client)->status, 401);
    EXPECT_FALSE(replacement->is_cancelled());
  }

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

  TEST_F(MultiseatProfileHttp, ResolverReturnsOnlyTheAuthenticatedWorkerContractWithoutStartingResources) {
    nvhttp::args_t request {{"game", std::string(profile_app_uuid)}, {"width", "1920"}, {"height", "1080"},
      {"fps", "120.0"}, {"client_max_fps", "60"}, {"hdr", "1"}, {"device", "client-b"}};
    const auto result = nvhttp::resolve_profile_request(client, request);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200);
    EXPECT_EQ(result->body["source"], "worker_profile_v1");
    EXPECT_EQ(result->body["worker_profile"]["id"], *service->profile_for_client(client->uuid));
    const auto &fields = result->body["resolved_profile"]["fields"];
    EXPECT_EQ(fields["target_fps"]["value"], 60);
    EXPECT_EQ(fields["target_fps"]["normalized"], true);
    EXPECT_EQ(fields["hdr"]["value"], false);
    EXPECT_EQ(fields["hdr"]["normalized"], true);
    EXPECT_EQ(fields["target_bitrate_kbps"]["value"], 8000);
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, HighRefreshProfileLaunchKeepsTheOtherSessionAtItsOwnRate) {
    uninstall_profile_launch_service(service);
    ASSERT_TRUE(service->shutdown(2s));
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state,
      std::vector<profile_summary_t> {{"profile-a", "First", {"client-a"}},
        {"profile-b", "Second", {"client-b"}}}), 2s);
    ASSERT_TRUE(install_profile_launch_service(service));
    const auto other = launch("client-b");
    ASSERT_TRUE(service->prepare(other).prepared());
    ASSERT_TRUE(other->try_begin_setup_handoff());
    ASSERT_TRUE(other->commit_setup_start());
    nvhttp::args_t request {{"game", std::string(profile_app_uuid)}, {"width", "1920"}, {"height", "1080"},
      {"fps", "120"}, {"client_max_fps", "120"}};
    const auto resolved = nvhttp::resolve_profile_request(client, request);
    ASSERT_TRUE(resolved);
    ASSERT_EQ(resolved->status, 200);
    const auto &fields = resolved->body["resolved_profile"]["fields"];
    EXPECT_EQ(fields["display_mode"]["value"], "1920x1080x120");
    EXPECT_EQ(fields["target_fps"]["value"], 120);
    EXPECT_EQ(fields["target_fps"]["normalized"], false);
    EXPECT_EQ(state->begins, 1U);

    auto launch_args = args();
    launch_args.erase("mode"); launch_args.emplace("mode", "1920x1080x120");
    launch_args.emplace("workerProfile", *service->profile_for_client(client->uuid));
    launch_args.emplace("resolvedProfile", "1"); launch_args.emplace("expectedTopology", "gamescope_stream");
    launch_args.emplace("resolvedHdr", "0"); launch_args.emplace("bitrateKbps", "8000");
    const auto launched = nvhttp::launch_profile_request(client, launch_args, false,
      [](const auto &value) {
        EXPECT_EQ(value->fps, 120000);
        EXPECT_TRUE(value->worker_connection_requirement()->load());
        return true;
      });
    ASSERT_TRUE(launched);
    ASSERT_EQ(launched->status, 200);
    ASSERT_TRUE(launched->launch);
    ASSERT_TRUE(launched->launch->try_begin_setup_handoff());
    ASSERT_TRUE(launched->launch->commit_setup_start());
    const auto status = nvhttp::profile_session_status(client);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->body["encoder"]["session_target_fps"], 120);
    EXPECT_EQ(other->fps, 60000);
    EXPECT_FALSE(other->is_cancelled());
    launched->launch->cancel();
    EXPECT_FALSE(other->is_cancelled());
  }

  TEST_F(MultiseatProfileHttp, ResolverRejectsUnsupportedLocksAndMalformedRequests) {
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>> {
      {"game", "host-game"}, {"encoder", "software"}, {"width", "1920.5"}, {"width", "nan"},
      {"height", "0"}, {"fps", "0"}, {"fps", "inf"}, {"client_max_fps", "0"},
      {"hdr", "2"}, {"mirrorDesktop", "1"}, {"closeDesktopSteamForPrivate", "1"},
      {"bitrate_locked", "true"}}) {
      nvhttp::args_t request {{"game", std::string(profile_app_uuid)}};
      request.erase(key); request.emplace(key, value);
      const auto result = nvhttp::resolve_profile_request(client, request);
      ASSERT_TRUE(result);
      EXPECT_EQ(result->status, 400) << key << "=" << value;
    }
    for (const auto &extra : std::vector<nvhttp::args_t> {
      {{"display_locked", "1"}, {"width", "1921"}}}) {
      auto request = extra; request.emplace("game", std::string(profile_app_uuid));
      EXPECT_EQ(nvhttp::resolve_profile_request(client, request)->status, 409);
    }
    nvhttp::args_t duplicate {{"game", std::string(profile_app_uuid)}, {"fps", "60"}, {"fps", "120"}};
    EXPECT_EQ(nvhttp::resolve_profile_request(client, duplicate)->status, 400);
    EXPECT_EQ(nvhttp::resolve_profile_request({}, {})->status, 401);
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, LowerLockedBitrateIsResolvedAndCarriedToTheLaunch) {
    nvhttp::args_t request {{"game", std::string(profile_app_uuid)},
      {"bitrate_locked", "1"}, {"bitrate_kbps", "4000"}};
    const auto resolved = nvhttp::resolve_profile_request(client, request);
    ASSERT_TRUE(resolved);
    ASSERT_EQ(resolved->status, 200);
    EXPECT_EQ(resolved->body["resolved_profile"]["fields"]["target_bitrate_kbps"]["value"], 4000);
    EXPECT_EQ(state->begins.load(), 0U);
    auto launch_args = args();
    launch_args.emplace("workerProfile", *service->profile_for_client(client->uuid));
    launch_args.emplace("resolvedProfile", "1"); launch_args.emplace("expectedTopology", "gamescope_stream");
    launch_args.emplace("resolvedHdr", "0"); launch_args.emplace("bitrateKbps", "4000");
    const auto launched = nvhttp::launch_profile_request(client, launch_args, false,
      [](const auto &value) { EXPECT_EQ(value->target_bitrate_kbps, 4000); return true; });
    ASSERT_TRUE(launched);
    EXPECT_EQ(launched->status, 200);
    if (launched->launch) launched->launch->cancel();
  }

  TEST_F(MultiseatProfileHttp, WorkerAssertionMustMatchAssignmentAndMediaBeforeStartup) {
    auto request = args();
    request.emplace("workerProfile", *service->profile_for_client(client->uuid));
    request.emplace("resolvedProfile", "1"); request.emplace("expectedTopology", "gamescope_stream");
    request.emplace("resolvedHdr", "0"); request.emplace("bitrateKbps", "8000");
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>> {
      {"workerProfile", "another-profile"}, {"expectedTopology", "desktop_display"},
      {"bitrateKbps", "8001"}, {"bitrateKbps", "0"}, {"bitrateKbps", "4000.5"}, {"resolvedHdr", "1"}}) {
      auto bad = request; bad.erase(key); bad.emplace(key, value);
      const auto result = nvhttp::launch_profile_request(client, bad, false, [](const auto &) { return true; });
      ASSERT_TRUE(result);
      EXPECT_NE(result->status, 200) << key;
    }
    EXPECT_EQ(state->begins.load(), 0U);
    const auto valid = nvhttp::launch_profile_request(client, request, false, [](const auto &) { return true; });
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->status, 200);
    valid->launch->cancel();
    uninstall_profile_launch_service(service);
    const auto unassigned = nvhttp::launch_profile_request(client, request, false, [](const auto &) { return true; });
    ASSERT_TRUE(unassigned);
    EXPECT_EQ(unassigned->status, 409);
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
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, UnmappedDeviceUsesOrdinaryRequestPath) {
    uninstall_profile_launch_service(service);
    const auto result = nvhttp::launch_profile_request(client, args(), false, [](const auto &) { return true; });
    EXPECT_FALSE(result);
    EXPECT_EQ(state->begins.load(), 0U);
  }
  TEST_F(MultiseatLaunchService, ClientSelectionOnlyUsesGrantedSpacesAndRejectsStaleChoices) {
    ASSERT_TRUE(service->shutdown(2s));
    std::vector<profile_summary_t> catalog {{"profile-a", "Default", {"client-a"}},
      {"profile-b", "Shared", {"client-b"}, true, false, {"client-a"}},
      {"private", "Private", {"client-c"}}};
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state, catalog), 2s);
    EXPECT_EQ(service->client_spaces("client-a").spaces.size(), 2U);
    EXPECT_EQ(service->select_space("client-a", "private", "profile-a").status, 404);
    EXPECT_EQ(service->select_space("client-a", "profile-b", "profile-a").status, 200);
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-b");
    EXPECT_EQ(service->select_space("client-a", "profile-a", "profile-a").status, 409);
    auto owned = launch();
    EXPECT_EQ(service->prepare(owned, "profile-a").status, 409);
    EXPECT_EQ(service->prepare(owned, "profile-b").status, 200);
    EXPECT_EQ(owned->worker_profile_key, "profile-b");
    EXPECT_EQ(service->select_space("client-a", "profile-a", "profile-b").status, 409);
    owned->cancel();
  }

  TEST_F(MultiseatLaunchService, ChoosingASpaceDoesNotRestartAnotherDevicesController) {
    ASSERT_TRUE(service->shutdown(2s));
    std::vector<profile_summary_t> catalog {{"profile-a", "Default", {"client-a"}},
      {"profile-b", "Shared", {"client-b"}, true, false, {"client-a"}}};
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state, catalog), 2s);
    auto other = launch("client-b"); ASSERT_EQ(service->prepare(other, "profile-b").status, 200);
    const auto shutdowns = state->shutdowns.load();
    EXPECT_EQ(service->select_space("client-a", "profile-b", "profile-a").status, 200);
    EXPECT_FALSE(other->is_cancelled()); EXPECT_EQ(state->shutdowns.load(), shutdowns);
    const auto visible = service->client_spaces("client-a");
    ASSERT_EQ(visible.spaces.size(), 2U); EXPECT_EQ(visible.spaces[1].state, "in_use");
    other->cancel();
  }


  TEST_F(MultiseatProfileHttp, SpaceListUsesPairedIdentityAndHidesOtherDevices) {
    const auto response = nvhttp::profile_spaces_request(client);
    ASSERT_EQ(response.status, 200);
    ASSERT_EQ(response.body.at("spaces").size(), 1U);
    EXPECT_EQ(response.body.at("spaces")[0].at("name"), "Primary");
    EXPECT_EQ(response.body.dump().find("client-b"), std::string::npos);
    EXPECT_EQ(response.body.dump().find("clients"), std::string::npos);
    EXPECT_EQ(nvhttp::profile_spaces_request(nullptr).status, 401);
    EXPECT_EQ(nvhttp::profile_spaces_request(client, R"({"space_id":"private","previous_space_id":"12345678-1234-4234-8234-123456789abc"})").status, 404);
    for (const auto payload : {R"({"space_id":"a","space_id":"b","previous_space_id":"a"})",
      R"({"space_id":"a","previous_space_id":"a","client_id":"client-b"})", R"({"space_id":true,"previous_space_id":"a"})"})
      EXPECT_EQ(nvhttp::profile_spaces_request(client, payload).status, 400);
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, SpaceSelectionRejectsReplacedPermissions) {
    auto replacement = std::make_shared<crypto::named_cert_t>();
    replacement->uuid = client->uuid; replacement->name = client->name; replacement->cert = client->cert;
    ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(replacement, crypto::PERM::_default));
    EXPECT_EQ(nvhttp::profile_spaces_request(client).status, 403);
    EXPECT_EQ(nvhttp::profile_spaces_request(client, R"({"space_id":"a","previous_space_id":"a"})").status, 403);
  }

  TEST_F(MultiseatLaunchService, SelectionSurvivesRestartButNeverRestoresRevokedAccess) {
    char pattern[] = "/tmp/polaris-space-selection-XXXXXX";
    const auto created = ::mkdtemp(pattern); ASSERT_NE(created, nullptr);
    const std::filesystem::path path = std::filesystem::path(created) / "catalog.json";
    const auto cleanup = util::fail_guard([&] { std::filesystem::remove_all(path.parent_path()); });
    std::vector<profile_summary_t> catalog {{"profile-a", "Default", {"client-a"}},
      {"profile-b", "Shared", {"client-b"}, true, false, {"client-a"}}};
    auto restart = [&] {
      ASSERT_TRUE(service->shutdown(2s));
      service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state, catalog), 2s,
        profile_admin_options_t {.catalog = path});
    };
    restart();
    ASSERT_EQ(service->select_space("client-a", "profile-b", "profile-a").status, 200);
    restart(); EXPECT_EQ(service->profile_for_client("client-a"), "profile-b");
    struct stat info {}; ASSERT_EQ(::stat((path.string() + ".selections").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
    catalog[1].access_clients.clear(); restart();
    EXPECT_EQ(service->profile_for_client("client-a"), "profile-a");
    EXPECT_EQ(service->select_space("client-a", "profile-b", "profile-a").status, 404);
  }


  TEST_F(MultiseatLaunchService, GameLaunchRequiresTheSelectedSpacesCatalog) {
    ASSERT_TRUE(service->shutdown(2s));
    state->library = [](std::string_view id) { return spaces::library_t{true,
      {{id == "profile-a" ? "3527290" : "870780", "Installed Game"}}}; };
    std::vector<profile_summary_t> catalog{{"profile-a", "Alex", {"client-a"}, true, false, {}, true},
      {"profile-b", "Sam", {"client-b"}, true, false, {"client-a"}, true}};
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state, catalog), 2s);
    EXPECT_FALSE(service->library_for_client("stranger", "profile-a"));
    EXPECT_FALSE(service->library_for_client("client-a", "missing"));
    ASSERT_TRUE(service->library_for_client("client-a", "profile-b"));
    EXPECT_EQ(service->prepare(launch(), "profile-a", "870780").status, 409);
    EXPECT_EQ(service->prepare(launch(), "profile-b", "870780").status, 409);
    EXPECT_EQ(state->begins.load(), 0U);
    auto accepted = launch();
    EXPECT_EQ(service->prepare(accepted, "profile-a", "3527290").status, 200);
    EXPECT_EQ(accepted->worker_profile_key, "profile-a");
    EXPECT_EQ(accepted->worker_library_target, "3527290");
    accepted->cancel();
  }

  TEST_F(MultiseatLaunchService, SteamLauncherRemainsAvailableWhenCatalogReadFails) {
    ASSERT_TRUE(service->shutdown(2s));
    state->library = [](std::string_view) { return spaces::library_t{}; };
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state,
      std::vector<profile_summary_t>{{"profile-a", "Alex", {"client-a"}, true, false, {}, true}}), 2s);
    EXPECT_EQ(service->prepare(launch(), "profile-a", "3527290").status, 409);
    auto steam = launch();
    EXPECT_EQ(service->prepare(steam, "profile-a", "big-picture-v1").status, 200);
    EXPECT_EQ(steam->worker_library_target, "big-picture-v1");
    steam->cancel();
  }

  TEST_F(MultiseatLaunchService, DesktopRequiresAnExplicitGrantAndCannotSwitchDuringLaunch) {
    EXPECT_EQ(service->select_space("client-a", "desktop", "12345678-1234-4234-8234-123456789abc").status, 404);
    state->desktops = {"client-a"};
    EXPECT_EQ(service->select_space("client-a", "desktop", "12345678-1234-4234-8234-123456789abc").status, 200);
    EXPECT_FALSE(service->routes_client("client-a"));
    EXPECT_TRUE(service->routes_client("client-b"));
    auto host = launch();
    ASSERT_TRUE(service->track_host_launch(host));
    EXPECT_FALSE(service->client_spaces("client-a").can_switch);
    EXPECT_EQ(service->select_space("client-a", "12345678-1234-4234-8234-123456789abc", "desktop").status, 409);
    host->cancel();
    EXPECT_EQ(service->select_space("client-a", "12345678-1234-4234-8234-123456789abc", "desktop").status, 200);
    EXPECT_FALSE(service->track_host_launch(launch()));
    EXPECT_TRUE(service->routes_client("client-a"));
  }

  TEST_F(MultiseatProfileHttp, SpaceArtworkRefreshPreservesCachedKindsAndRetriesOnlyFailures) {
    uninstall_profile_launch_service(service); ASSERT_TRUE(service->shutdown(2s));
    state->library = [](std::string_view) { return spaces::library_t{true, {{"870780", "Control"}}}; };
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state,
      std::vector<profile_summary_t>{{"profile-a", "Alex", {"client-a"}, true, false, {}, true}}), 2s);
    ASSERT_TRUE(install_profile_launch_service(service));
    char pattern[] = "/tmp/polaris-space-artwork-XXXXXX";
    const auto created = ::mkdtemp(pattern); ASSERT_NE(created, nullptr);
    const std::filesystem::path root(created);
    const auto cleanup = util::fail_guard([&] { std::filesystem::remove_all(root); });
    using namespace game_artwork;
    std::vector<kind_e> calls;
    bool fail_hero = true;
    providers::transport_t transport = [&](const providers::request_t &request, std::uintmax_t limit)
        -> std::optional<providers::transport_response_t> {
      if (request.operation == providers::operation_e::list) return std::nullopt;
      EXPECT_EQ(limit, maximum_asset_bytes);
      calls.push_back(*request.kind);
      if (fail_hero && request.kind == kind_e::hero) return std::nullopt;
      return providers::transport_response_t{200, {0xff, 0xd8, 0xff, 0xe0, 1}, request.url};
    };
    for (const auto identity : {"space.profile-b.870780", "space.profile-a.620", "space.profile-a.big-picture-v1",
        "space.profile-a.870780/../private", "space.profile-a.4294967296"}) {
      EXPECT_EQ(nvhttp::profile_artwork_resolve_request(client, identity, root, transport).status, 404);
    }
    EXPECT_EQ(nvhttp::profile_artwork_resolve_request(nullptr, "space.profile-a.870780", root, transport).status, 404);
    EXPECT_TRUE(calls.empty());
    const auto first = nvhttp::profile_artwork_resolve_request(client, "space.profile-a.870780", root, transport);
    ASSERT_EQ(first.status, 200);
    EXPECT_EQ(first.body["resolution"]["status"], "partial_failure");
    EXPECT_EQ(first.body["resolution"]["remaining_kinds"], nlohmann::json::array({"hero"}));
    EXPECT_EQ(first.body["assets"]["poster"]["url"], "/polaris/v1/games/space.profile-a.870780/space-artwork/poster");
    EXPECT_EQ(first.body.dump().find(root.string()), std::string::npos);
    EXPECT_EQ(first.body.dump().find("53504143"), std::string::npos);
    calls.clear(); fail_hero = false;
    const auto second = nvhttp::profile_artwork_resolve_request(client, "space.profile-a.870780", root, transport);
    EXPECT_EQ(second.body["resolution"]["status"], "updated");
    EXPECT_EQ(calls, std::vector<kind_e>{kind_e::hero});
    EXPECT_NE(first.body["revision"], second.body["revision"]);
    calls.clear();
    const auto third = nvhttp::profile_artwork_resolve_request(client, "space.profile-a.870780", root, transport);
    EXPECT_EQ(third.body["resolution"]["status"], "healthy");
    EXPECT_EQ(third.body["revision"], second.body["revision"]);
    EXPECT_TRUE(calls.empty());
    const auto poster = cache_asset_path(root / "spaces-library-artwork", "53504143-4553-4000-8000-000000870780",
      kind_e::poster, source_e::steam, ".jpg");
    ASSERT_TRUE(poster);
    { std::ofstream output(*poster); output << "invalid image"; }
    const auto repaired = nvhttp::profile_artwork_resolve_request(client, "space.profile-a.870780", root, transport);
    EXPECT_EQ(repaired.body["resolution"]["status"], "updated");
    EXPECT_EQ(calls, std::vector<kind_e>{kind_e::poster});
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, SpaceArtworkRefreshRechecksPermissionAfterDownload) {
    uninstall_profile_launch_service(service); ASSERT_TRUE(service->shutdown(2s));
    state->library = [](std::string_view) { return spaces::library_t{true, {{"870780", "Control"}}}; };
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state,
      std::vector<profile_summary_t>{{"profile-a", "Alex", {"client-a"}, true, false, {}, true}}), 2s);
    ASSERT_TRUE(install_profile_launch_service(service));
    char pattern[] = "/tmp/polaris-space-artwork-revoke-XXXXXX";
    const auto created = ::mkdtemp(pattern); ASSERT_NE(created, nullptr);
    const std::filesystem::path root(created);
    const auto cleanup = util::fail_guard([&] { std::filesystem::remove_all(root); });
    bool revoked = false;
    const auto result = nvhttp::profile_artwork_resolve_request(client, "space.profile-a.870780", root,
      [&](const game_artwork::providers::request_t &request, std::uintmax_t)
          -> std::optional<game_artwork::providers::transport_response_t> {
        if (request.operation == game_artwork::providers::operation_e::list) return std::nullopt;
        if (!revoked) { EXPECT_TRUE(nvhttp::unpair_client(client->uuid)); revoked = true; }
        return game_artwork::providers::transport_response_t{200, {0xff, 0xd8, 0xff, 0xe0, 1}, request.url};
      });
    EXPECT_TRUE(revoked);
    EXPECT_EQ(result.status, 404);
    EXPECT_FALSE(result.body.contains("assets"));
    EXPECT_EQ(state->begins.load(), 0U);
  }

  TEST_F(MultiseatProfileHttp, SpaceLibrariesAndArtworkRequireCurrentPerSpaceAccess) {
    uninstall_profile_launch_service(service); ASSERT_TRUE(service->shutdown(2s));
    state->library = [](std::string_view id) { return spaces::library_t{true,
      {{id == "profile-a" ? "870780" : "3527290", "Installed Game"}}}; };
    service = std::make_shared<profile_launch_service_t>(std::make_unique<controller_t>(state,
      std::vector<profile_summary_t>{{"profile-a", "Alex", {"client-a"}, true, false, {}, true},
                                    {"profile-b", "Sam", {"client-b"}, true, false, {}, true}}), 2s);
    ASSERT_TRUE(install_profile_launch_service(service));
    const auto result = nvhttp::profile_library_request(client, "profile-a");
    ASSERT_EQ(result.status, 200); ASSERT_EQ(result.body.at("games").size(), 2U);
    EXPECT_EQ(result.body["games"][0]["name"], "Steam Big Picture");
    EXPECT_EQ(result.body["games"][1]["id"], "space.profile-a.870780");
    EXPECT_EQ(nvhttp::profile_library_request(client, "profile-b").status, 404);
    EXPECT_EQ(nvhttp::profile_artwork_target(client, "space.profile-a.870780"), "870780");
    EXPECT_FALSE(nvhttp::profile_artwork_target(client, "space.profile-b.3527290"));
    EXPECT_FALSE(nvhttp::profile_artwork_target(client, "space.profile-a.3527290"));
    EXPECT_FALSE(nvhttp::profile_artwork_target(nullptr, "space.profile-a.870780"));
    const auto resolved = nvhttp::resolve_profile_request(client, {{"game", "space.profile-a.870780"},
      {"width", "1920"}, {"height", "1080"}, {"fps", "120"}, {"client_max_fps", "120"}});
    ASSERT_TRUE(resolved); ASSERT_EQ(resolved->status, 200);
    EXPECT_EQ(resolved->body["worker_profile"]["target"], "870780");
    EXPECT_EQ(resolved->body["worker_profile"]["game_identity"], "space.profile-a.870780");
    auto start = args(); start.emplace("workerProfile", "profile-a"); start.emplace("workerTarget", "870780");
    start.emplace("resolvedProfile", "1"); start.emplace("expectedTopology", "gamescope_stream");
    start.emplace("resolvedHdr", "0"); start.emplace("bitrateKbps", "8000");
    start.erase("mode"); start.emplace("mode", "1920x1080x120");
    const auto started = nvhttp::launch_profile_request(client, start, false, [](const auto &) { return true; });
    ASSERT_TRUE(started); ASSERT_EQ(started->status, 200); ASSERT_TRUE(started->launch);
    EXPECT_EQ(started->launch->worker_library_target, "870780");
    started->launch->cancel();
    const auto began = state->begins.load();
    start.erase("workerTarget"); start.emplace("workerTarget", "3527290");
    EXPECT_EQ(nvhttp::launch_profile_request(client, start, false, [](const auto &) { return true; })->status, 409);
    EXPECT_EQ(state->begins.load(), began);
    auto replacement = std::make_shared<crypto::named_cert_t>();
    replacement->uuid = client->uuid; replacement->name = client->name; replacement->cert = client->cert;
    ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(replacement, crypto::PERM::_default));
    EXPECT_EQ(nvhttp::profile_library_request(client, "profile-a").status, 403);
    EXPECT_FALSE(nvhttp::profile_artwork_target(client, "space.profile-a.870780"));
    EXPECT_EQ(state->begins.load(), began);
  }

}  // namespace
