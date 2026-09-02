/**
 * @file tests/unit/test_settings_projection_contract.cpp
 * @brief Test the GET /api/settings/metadata projection contract.
 */

#include <src/config.h>
#include <src/confighttp.h>
#include <src/confighttp_validation.h>
#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/stream_stats.h>

#include <gtest/gtest.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace {
  constexpr auto k_paired_uuid = "11111111-2222-3333-4444-555555555555";

  // Same fixture certificate the pairing tests authorize clients with;
  // add_authorized_client refuses a client whose certificate does not parse.
  const std::string k_public_cert = R"(-----BEGIN CERTIFICATE-----
MIIC6zCCAdOgAwIBAgIBATANBgkqhkiG9w0BAQsFADA5MQswCQYDVQQGEwJJVDEW
MBQGA1UECgwNR2FtZXNPbldoYWxlczESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIy
MDQwOTA5MTYwNVoXDTQyMDQwNDA5MTYwNVowOTELMAkGA1UEBhMCSVQxFjAUBgNV
BAoMDUdhbWVzT25XaGFsZXMxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBAMt482VY3ToUuUy6NbMhfxQgI7tJZ8fkNeVp
9WOnHCL9YKR07oXGLGpE0a7vXAy8lcVsOU1Hx+pfbGj56rXsne4Uqf6p2OY/cvfx
uSrGGgn+cKteR4bIJND4Nq6DrdlhIl5bYyZ/4sBHn+L99Zh9elKVtx/lclA8Ra8Q
2kupa7405TnR0lcgRVilRdHHb7HhlvCQfu1Umb3gv4I5TKIkpA/JaBTZoWzIkbAc
V9499JSl9gepsdlX8guljn1UlqKsHAT31vH+YG8wjtqEGYlNIO4N98lw8OEUXmRl
rRSRA+s++FdxBpJG2Lu/RWicRCPylNKcZiv2S1YqT3bDEPKf1LcCAwEAATANBgkq
hkiG9w0BAQsFAAOCAQEAqPBqzvDjl89pZMll3Ge8RS7HeDuzgocrhOcT2jnk4ag7
/TROZuISjDp6+SnL3gPEt7E2OcFAczTg3l/wbT5PFb6vM96saLm4EP0zmLfK1FnM
JDRahKutP9rx6RO5OHqsUB+b4jA4W0L9UnXUoLKbjig501AUix0p52FBxu+HJ90r
HlLs3Vo6nj4Z/PZXrzaz8dtQ/KJMpd/g/9xlo6BKAnRk5SI8KLhO4hW6zG0QA56j
X4wnh1bwdiidqpcgyuKossLOPxbS786WmsesaAWPnpoY6M8aija+ALwNNuWWmyMg
9SVDV76xJzM36Uq7Kg3QJYTlY04WmPIdJHkCtXWf9g==
-----END CERTIFICATE-----)";

  class SettingsProjectionContract: public ::testing::Test {
  protected:
    void SetUp() override {
      previous_fresh_state = config::sunshine.flags.test(config::flag::FRESH_STATE);
      config::sunshine.flags.set(config::flag::FRESH_STATE);
      nvhttp::reset_pairing_state_for_tests();
    }

    void TearDown() override {
      nvhttp::reset_pairing_state_for_tests();
      config::sunshine.flags.set(config::flag::FRESH_STATE, previous_fresh_state);
    }

    static crypto::p_named_cert_t make_paired_client() {
      auto named_cert = std::make_shared<crypto::named_cert_t>();
      named_cert->cert = k_public_cert;
      named_cert->name = "Projection Test Device";
      named_cert->uuid = k_paired_uuid;
      named_cert->display_mode = "1920x1080x120";
      named_cert->target_bitrate_kbps = 25000;
      return named_cert;
    }

    bool previous_fresh_state = false;
  };
}  // namespace

TEST_F(SettingsProjectionContract, HostViewWithZeroClientsServesCompleteContract) {
  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload(std::string {}, error);

  EXPECT_TRUE(error.empty()) << error;
  ASSERT_FALSE(payload.empty());
  EXPECT_TRUE(payload.at("status").get<bool>());
  EXPECT_EQ(payload.at("version").get<int>(), 1);
  EXPECT_EQ(payload.at("view").get<std::string>(), "host");
  EXPECT_FALSE(payload.contains("client"));

  const auto &fields = payload.at("fields");
  ASSERT_TRUE(fields.is_object());
  ASSERT_FALSE(fields.empty());
  for (const auto &[name, field] : fields.items()) {
    for (const auto *key : {"direction", "scope", "status", "live", "requires_relaunch"}) {
      EXPECT_TRUE(field.contains(key)) << name << " is missing " << key;
    }
  }
  // The host view has no client identity, so client_to_host reports read
  // pending instead of borrowing state from an arbitrary paired device.
  EXPECT_EQ(fields.at("client_runtime").at("status").get<std::string>(), "pending");
  EXPECT_EQ(fields.at("applied_stream_settings").at("status").get<std::string>(), "pending");

  // The sync block is the GameStream sync_status with fields hoisted out.
  const auto &sync = payload.at("sync");
  ASSERT_TRUE(sync.is_object());
  EXPECT_FALSE(sync.contains("fields"));
  EXPECT_EQ(sync.at("endpoint").get<std::string>(), "/polaris/v1/client-settings");

  const auto &stream_display = payload.at("stream_display");
  for (const auto *key : {"configured", "effective", "configured_label", "effective_label", "relaunch_required"}) {
    EXPECT_TRUE(stream_display.contains(key)) << "stream_display is missing " << key;
  }

  const auto &write_paths = payload.at("write_paths");
  EXPECT_EQ(write_paths.at("web_ui").at("endpoint").get<std::string>(), "/api/config");
  EXPECT_EQ(write_paths.at("web_ui").at("auth").get<std::string>(), "web_session");
  EXPECT_EQ(write_paths.at("gamestream").at("endpoint").get<std::string>(), "/polaris/v1/client-settings");
  EXPECT_EQ(write_paths.at("gamestream").at("auth").get<std::string>(), "paired_client_cert");
  EXPECT_FALSE(write_paths.at("coordinated").get<bool>());
  EXPECT_FALSE(write_paths.at("note").get<std::string>().empty());

  EXPECT_TRUE(payload.at("modes").is_array());
  EXPECT_TRUE(payload.at("tuning").is_object());
  EXPECT_FALSE(payload.at("tuning").at("ai_auto_quality_enabled").get<bool>());
  EXPECT_TRUE(payload.at("auto_quality").is_object());
  EXPECT_EQ(payload.at("auto_quality").at("state").get<std::string>(), "off");

  ASSERT_TRUE(payload.at("clients").is_array());
  ASSERT_TRUE(payload.at("provenance").is_array());
  EXPECT_TRUE(payload.at("clients").empty());
}

TEST_F(SettingsProjectionContract, PairedClientViewSurfacesOverridesInDesired) {
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(make_paired_client(), crypto::PERM::_all));

  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload(k_paired_uuid, error);

  EXPECT_TRUE(error.empty()) << error;
  ASSERT_FALSE(payload.empty());
  EXPECT_EQ(payload.at("view").get<std::string>(), "paired_client");
  ASSERT_TRUE(payload.contains("client"));
  EXPECT_EQ(payload.at("client").at("uuid").get<std::string>(), k_paired_uuid);
  EXPECT_EQ(payload.at("client").at("name").get<std::string>(), "Projection Test Device");
  EXPECT_EQ(payload.at("client").at("display_mode").get<std::string>(), "1920x1080x120");
  EXPECT_EQ(payload.at("client").at("target_bitrate_kbps").get<int>(), 25000);

  const auto &fields = payload.at("fields");
  EXPECT_EQ(fields.at("display_mode").at("desired").get<std::string>(), "1920x1080x120");
  EXPECT_EQ(fields.at("target_bitrate_kbps").at("desired").get<int>(), 25000);
  EXPECT_TRUE(fields.at("target_bitrate_kbps").at("paired_override_active").get<bool>());

  // The per-client view answers for one device; the roster is host-view only.
  EXPECT_FALSE(payload.contains("clients"));
}

TEST_F(SettingsProjectionContract, HostViewListsPairedClientsInSummary) {
  ASSERT_TRUE(nvhttp::add_authorized_client_for_tests(make_paired_client(), crypto::PERM::_all));

  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload(std::string {}, error);

  EXPECT_TRUE(error.empty()) << error;
  const auto &clients = payload.at("clients");
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients.at(0).at("uuid").get<std::string>(), k_paired_uuid);
  EXPECT_EQ(clients.at(0).at("name").get<std::string>(), "Projection Test Device");
  EXPECT_TRUE(clients.at(0).contains("connected"));
  EXPECT_EQ(clients.at(0).at("display_mode").get<std::string>(), "1920x1080x120");
  EXPECT_EQ(clients.at(0).at("target_bitrate_kbps").get<int>(), 25000);
}

TEST_F(SettingsProjectionContract, UnknownClientUuidIsRejected) {
  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload("not-a-paired-uuid", error);

  EXPECT_EQ(error, "unknown client");
  EXPECT_TRUE(payload.empty());
  EXPECT_TRUE(nvhttp::client_settings_projection("not-a-paired-uuid").empty());
}

TEST_F(SettingsProjectionContract, FieldMapCoversExactlyTheWritableConfigKeys) {
  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload(std::string {}, error);
  ASSERT_TRUE(error.empty()) << error;

  std::set<std::string> expected;
  for (const auto &key : payload.at("live_fields")) {
    expected.insert(key.get<std::string>());
  }
  for (const auto &key : payload.at("restart_fields")) {
    expected.insert(key.get<std::string>());
  }
  expected.insert("disconnect_resume_timeout_seconds");

  std::set<std::string> actual;
  for (const auto &[key, mapped] : payload.at("field_map").items()) {
    actual.insert(key);
    EXPECT_TRUE(mapped.is_string()) << key;
  }
  EXPECT_EQ(actual, expected);
}

TEST_F(SettingsProjectionContract, StatsChannelAugmentationAddsTuningAndAutoQualityAdditively) {
  nlohmann::json synthetic {
    {"streaming", true},
    {"fps", 60.0},
    {"bitrate_kbps", 20000},
    {"custom_consumer_key", "keep-me"}
  };
  const auto original = synthetic;

  stream_stats::stats_t stats {};
  stats.adaptive_target_bitrate_kbps = 12345;

  const auto augmented = confighttp::augment_stream_stats_json(synthetic, stats);

  // Every pre-existing key survives with its original value.
  for (const auto &[key, value] : original.items()) {
    ASSERT_TRUE(augmented.contains(key)) << key;
    EXPECT_EQ(augmented.at(key), value) << key;
  }
  // Exactly the two additive keys appear, nothing else.
  EXPECT_EQ(augmented.size(), original.size() + 2);
  ASSERT_TRUE(augmented.contains("tuning"));
  ASSERT_TRUE(augmented.contains("auto_quality"));

  const auto &tuning = augmented.at("tuning");
  ASSERT_TRUE(tuning.is_object());
  EXPECT_EQ(tuning.size(), 14u);
  EXPECT_EQ(tuning.at("adaptive_target_bitrate_kbps").get<int>(), 12345);
  EXPECT_FALSE(tuning.at("ai_auto_quality_enabled").get<bool>());

  const auto &auto_quality = augmented.at("auto_quality");
  ASSERT_TRUE(auto_quality.is_object());
  EXPECT_EQ(auto_quality.at("state").get<std::string>(), "off");
  EXPECT_TRUE(auto_quality.contains("components"));
}

TEST_F(SettingsProjectionContract, ResponseOnlyKeysMirrorTheValidationList) {
  std::string error;
  const auto payload = confighttp::build_settings_metadata_payload(std::string {}, error);
  ASSERT_TRUE(error.empty()) << error;

  const auto &response_only_keys = payload.at("response_only_keys");
  ASSERT_TRUE(response_only_keys.is_array());
  ASSERT_FALSE(response_only_keys.empty());
  EXPECT_EQ(response_only_keys.size(), confighttp::validation::response_only_config_keys().size());

  bool saw_stream_path_id = false;
  for (const auto &key : response_only_keys) {
    if (key.get<std::string>() == "stream_path_id") {
      saw_stream_path_id = true;
    }
  }
  EXPECT_TRUE(saw_stream_path_id);
}
