/**
 * @file tests/unit/test_recording_authorization.cpp
 * @brief Test the stored, host-admin-only recording approval of a paired device in src/nvhttp.cpp:
 * it is separate from the permission mask, off by default, never granted by pairing (presets,
 * re-pairing the same certificate) or held by a guest, and it persists, reloads and rolls back like
 * the rest of the device record. Nothing in the product reads it yet; these cases pin storage only.
 */

#include "../tests_common.h"
#include "../certificate_test_utils.h"

#include <src/config.h>
#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>
#include <src/utility.h>
#include <src/uuid.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using crypto::PERM;
  using nvhttp::client_mutation_result_t;

  /**
   * Each case starts with no paired device and its own pairing-state file, with fresh-state mode
   * off so that pairing writes the file exactly as a running host does. TearDown clears the write
   * fault, the in-memory state, and the file, and restores the global configuration.
   */
  class RecordingAuthorizationTests: public ::testing::Test {
  protected:
    void SetUp() override {
      previous_fresh_state = config::sunshine.flags.test(config::flag::FRESH_STATE);
      previous_state_path = config::nvhttp.file_state;
      state_path = fs::temp_directory_path() /
                   ("polaris-recording-authorization-" + uuid_util::uuid_t::generate().string() + ".json");
      config::nvhttp.file_state = state_path.string();
      config::sunshine.flags.reset(config::flag::FRESH_STATE);
      nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::none);
      nvhttp::reset_pairing_state_for_tests();
      std::string host_uuid = "12345678-1234-1234-1234-1234567890ab";
      http::uuid = uuid_util::uuid_t::parse(host_uuid);
      http::unique_id = http::uuid.string();
    }

    void TearDown() override {
      nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::none);
      nvhttp::reset_pairing_state_for_tests();
      config::sunshine.flags.set(config::flag::FRESH_STATE, previous_fresh_state);
      config::nvhttp.file_state = previous_state_path;
      std::error_code ec;
      fs::remove(state_path, ec);
      fs::remove(state_path.string() + ".lock", ec);
    }

    /// A new device with its own certificate, as the pairing handshake hands it to the commit.
    static crypto::p_named_cert_t device(const std::string &name, bool temporary = false) {
      auto client = std::make_shared<crypto::named_cert_t>();
      client->cert = crypto::gen_creds(name, 2048).x509;
      client->name = name;
      client->uuid = uuid_util::uuid_t::generate().string();
      client->temporary_authorization = temporary;
      return client;
    }

    /// The device as the admin client list reports it, or null when it is not listed.
    static nlohmann::json listed(const std::string &uuid) {
      for (const auto &client : nvhttp::get_all_clients()) {
        if (client.at("uuid") == uuid) {
          return client;
        }
      }
      return nullptr;
    }

    /// The device as the durable pairing-state file holds it, or null when it is not there.
    nlohmann::json persisted(const std::string &uuid) const {
      std::ifstream input(state_path);
      if (!input.is_open()) {
        return nullptr;
      }
      nlohmann::json state;
      input >> state;
      for (const auto &client : state["root"]["named_devices"]) {
        if (client.at("uuid") == uuid) {
          return client;
        }
      }
      return nullptr;
    }

    /// The admin device update, with every other field kept as listed.
    static client_mutation_result_t update(
      const std::string &uuid,
      std::optional<bool> recording,
      std::optional<bool> temporary = std::nullopt,
      std::optional<PERM> perm = std::nullopt
    ) {
      const auto current = listed(uuid);
      const auto name = current.is_null() ? std::string {} : current.at("name").get<std::string>();
      const auto current_perm =
        current.is_null() ? PERM::_no : static_cast<PERM>(current.at("perm").get<std::uint32_t>());
      return nvhttp::update_device_info_result(
        uuid, name, "", 0, {}, {}, perm.value_or(current_perm), true, true, false, temporary, std::nullopt, recording
      );
    }

    bool previous_fresh_state = false;
    std::string previous_state_path;
    fs::path state_path;
  };
}  // namespace

TEST_F(RecordingAuthorizationTests, EveryPairingPresetPairsWithRecordingOff) {
  for (const auto preset : {PERM::_gamepad_only, PERM::_default, PERM::_game_control, PERM::_all}) {
    auto client = device("preset-" + std::to_string(static_cast<std::uint32_t>(preset)));
    client->recording_authorized = true;  // Whatever the handshake carried, pairing does not grant
    ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, preset));
    const auto listed_client = listed(client->uuid);
    ASSERT_FALSE(listed_client.is_null());
    EXPECT_EQ(listed_client.at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(preset));
    EXPECT_FALSE(listed_client.at("recording_authorized").get<bool>());
    EXPECT_FALSE(persisted(client->uuid).at("recording_authorized").get<bool>());
  }
}

TEST_F(RecordingAuthorizationTests, ImplicitPairingKeepsGameControlWithoutRecording) {
  auto client = device("implicit");
  client->recording_authorized = true;
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, std::nullopt));
  const auto listed_client = listed(client->uuid);
  EXPECT_EQ(listed_client.at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(PERM::_game_control));
  EXPECT_FALSE(listed_client.at("recording_authorized").get<bool>());
}

TEST_F(RecordingAuthorizationTests, AdminGrantIsListedPersistedAndReloaded) {
  auto client = device("granted");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_game_control));
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  EXPECT_TRUE(listed(client->uuid).at("recording_authorized").get<bool>());
  EXPECT_TRUE(persisted(client->uuid).at("recording_authorized").get<bool>());

  nvhttp::reset_pairing_state_for_tests();
  ASSERT_TRUE(listed(client->uuid).is_null());
  nvhttp::load_pairing_state_for_tests();
  const auto reloaded = listed(client->uuid);
  ASSERT_FALSE(reloaded.is_null());
  EXPECT_TRUE(reloaded.at("recording_authorized").get<bool>());
  EXPECT_EQ(reloaded.at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(PERM::_game_control));
}

TEST_F(RecordingAuthorizationTests, AnOmittedFieldKeepsTheStoredChoiceEitherWay) {
  auto client = device("omitted");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_default));
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  ASSERT_EQ(update(client->uuid, std::nullopt), client_mutation_result_t::success);
  EXPECT_TRUE(listed(client->uuid).at("recording_authorized").get<bool>());

  ASSERT_EQ(update(client->uuid, false), client_mutation_result_t::success);
  EXPECT_FALSE(persisted(client->uuid).at("recording_authorized").get<bool>());
  ASSERT_EQ(update(client->uuid, std::nullopt), client_mutation_result_t::success);
  EXPECT_FALSE(listed(client->uuid).at("recording_authorized").get<bool>());
}

TEST_F(RecordingAuthorizationTests, PermissionChangesAndRecordingChangesNeverTouchEachOther) {
  auto client = device("independent");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_default));
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  for (const auto perm : {PERM::_all, PERM::_no, PERM::_gamepad_only}) {
    ASSERT_EQ(update(client->uuid, std::nullopt, std::nullopt, perm), client_mutation_result_t::success);
    const auto listed_client = listed(client->uuid);
    EXPECT_EQ(listed_client.at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(perm));
    EXPECT_TRUE(listed_client.at("recording_authorized").get<bool>());
  }
  ASSERT_EQ(update(client->uuid, false), client_mutation_result_t::success);
  EXPECT_EQ(
    listed(client->uuid).at("perm").get<std::uint32_t>(),
    static_cast<std::uint32_t>(PERM::_gamepad_only)
  );
}

TEST_F(RecordingAuthorizationTests, AStoredApprovalGrantsNoLaunchAuthority) {
  auto client = device("no-launch");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_default));
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  const auto current = nvhttp::resolve_authorized_client(client);
  ASSERT_TRUE(current);
  ASSERT_TRUE(current->recording_authorized);
  int publications = 0;
  EXPECT_EQ(nvhttp::publish_authorized_launch(current, PERM::launch, [&]() {
              ++publications;
              return true;
            }),
            403);
  EXPECT_EQ(publications, 0);
  EXPECT_FALSE(static_cast<std::uint32_t>(current->perm & PERM::server_cmd));
}

TEST_F(RecordingAuthorizationTests, AGrantAskedForAGuestIsStoredOff) {
  auto guest = device("guest", true);
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(guest, PERM::_all));
  ASSERT_EQ(update(guest->uuid, true), client_mutation_result_t::success);
  const auto listed_guest = listed(guest->uuid);
  EXPECT_TRUE(listed_guest.at("temporary_authorization").get<bool>());
  EXPECT_FALSE(listed_guest.at("recording_authorized").get<bool>());
  EXPECT_TRUE(persisted(guest->uuid).is_null());  // A guest never reaches the durable file
}

TEST_F(RecordingAuthorizationTests, GrantingWhileMakingTheDeviceAGuestIsStoredOff) {
  auto client = device("same-request");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_game_control));
  ASSERT_EQ(update(client->uuid, true, true), client_mutation_result_t::success);
  const auto listed_client = listed(client->uuid);
  EXPECT_TRUE(listed_client.at("temporary_authorization").get<bool>());
  EXPECT_FALSE(listed_client.at("recording_authorized").get<bool>());
  EXPECT_TRUE(persisted(client->uuid).is_null());
}

TEST_F(RecordingAuthorizationTests, MakingAGuestPermanentAgainDoesNotResurrectAnEarlierGrant) {
  auto client = device("round-trip");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_game_control));
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  ASSERT_EQ(update(client->uuid, std::nullopt, true), client_mutation_result_t::success);
  EXPECT_FALSE(listed(client->uuid).at("recording_authorized").get<bool>());

  ASSERT_EQ(update(client->uuid, std::nullopt, false), client_mutation_result_t::success);
  const auto listed_client = listed(client->uuid);
  EXPECT_FALSE(listed_client.at("temporary_authorization").get<bool>());
  EXPECT_FALSE(listed_client.at("recording_authorized").get<bool>());
  EXPECT_FALSE(persisted(client->uuid).at("recording_authorized").get<bool>());
}

TEST_F(RecordingAuthorizationTests, RepairingTheSameCertificateClearsTheGrant) {
  const auto credentials = crypto::gen_creds("repaired", 2048);
  auto original = std::make_shared<crypto::named_cert_t>();
  original->cert = credentials.x509;
  original->name = "repaired-original";
  original->uuid = uuid_util::uuid_t::generate().string();
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(original, PERM::_all));
  ASSERT_EQ(update(original->uuid, true), client_mutation_result_t::success);

  for (const auto &preset : std::vector<std::optional<PERM>> {std::nullopt, PERM::_all}) {
    auto replacement = std::make_shared<crypto::named_cert_t>();
    replacement->cert = test_utils::certificates::to_crlf_pem(credentials.x509);  // Same identity
    replacement->name = "repaired-replacement";
    replacement->uuid = uuid_util::uuid_t::generate().string();
    replacement->recording_authorized = true;
    ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(replacement, preset));
    const auto clients = nvhttp::get_all_clients();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients[0].at("uuid"), replacement->uuid);
    EXPECT_EQ(clients[0].at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(PERM::_all));
    EXPECT_FALSE(clients[0].at("recording_authorized").get<bool>());
    EXPECT_FALSE(persisted(replacement->uuid).at("recording_authorized").get<bool>());
    ASSERT_EQ(update(replacement->uuid, true), client_mutation_result_t::success);  // Grant again
  }
}

TEST_F(RecordingAuthorizationTests, UnpairingThenPairingAgainStartsOff) {
  const auto credentials = crypto::gen_creds("unpaired", 2048);
  auto original = std::make_shared<crypto::named_cert_t>();
  original->cert = credentials.x509;
  original->name = "unpaired";
  original->uuid = uuid_util::uuid_t::generate().string();
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(original, PERM::_game_control));
  ASSERT_EQ(update(original->uuid, true), client_mutation_result_t::success);
  ASSERT_EQ(nvhttp::unpair_client_result(original->uuid), client_mutation_result_t::success);
  EXPECT_TRUE(listed(original->uuid).is_null());

  auto again = std::make_shared<crypto::named_cert_t>();
  again->cert = credentials.x509;
  again->name = "unpaired-again";
  again->uuid = uuid_util::uuid_t::generate().string();
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(again, PERM::_game_control));
  EXPECT_FALSE(listed(again->uuid).at("recording_authorized").get<bool>());
}

TEST_F(RecordingAuthorizationTests, AFailedWriteRollsBackAGrantAndARevocation) {
  auto client = device("rollback");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_game_control));
  auto reset_fault = util::fail_guard([]() {
    nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::none);
  });

  nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::rename);
  EXPECT_EQ(update(client->uuid, true), client_mutation_result_t::persistence_failed);
  EXPECT_FALSE(listed(client->uuid).at("recording_authorized").get<bool>());
  EXPECT_FALSE(persisted(client->uuid).at("recording_authorized").get<bool>());

  nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::none);
  ASSERT_EQ(update(client->uuid, true), client_mutation_result_t::success);
  nvhttp::set_pairing_state_write_fault_for_tests(nvhttp::pairing_state_write_fault_t::rename);
  EXPECT_EQ(update(client->uuid, false), client_mutation_result_t::persistence_failed);
  EXPECT_TRUE(listed(client->uuid).at("recording_authorized").get<bool>());
  EXPECT_TRUE(persisted(client->uuid).at("recording_authorized").get<bool>());
}

TEST_F(RecordingAuthorizationTests, RecordsWrittenBeforeTheSettingLoadOff) {
  const auto named = crypto::gen_creds("legacy-named", 2048);
  const auto legacy = crypto::gen_creds("legacy-device", 2048);
  nlohmann::json state;
  state["root"]["uniqueid"] = http::unique_id;
  state["root"]["named_devices"] = nlohmann::json::array({{
    {"name", "legacy-named"},
    {"uuid", uuid_util::uuid_t::generate().string()},
    {"cert", named.x509},
    {"perm", static_cast<std::uint32_t>(PERM::_all)},
  }});
  state["root"]["devices"] = nlohmann::json::array({{{"certs", nlohmann::json::array({legacy.x509})}}});
  {
    std::ofstream output(state_path);
    output << state.dump(2);
  }
  nvhttp::load_pairing_state_for_tests();
  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 2u);
  for (const auto &client : clients) {
    EXPECT_EQ(client.at("perm").get<std::uint32_t>(), static_cast<std::uint32_t>(PERM::_all));
    EXPECT_FALSE(client.at("recording_authorized").get<bool>()) << client.at("name");
  }
}

TEST_F(RecordingAuthorizationTests, AnUnknownDeviceIsNotFoundAndNothingIsAdded) {
  auto client = device("known");
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(client, PERM::_game_control));
  EXPECT_EQ(
    nvhttp::update_device_info_result(
      "missing-device", "missing", "", 0, {}, {}, PERM::_all, true, true, false, std::nullopt, std::nullopt, true
    ),
    client_mutation_result_t::not_found
  );
  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_FALSE(clients[0].at("recording_authorized").get<bool>());
}
