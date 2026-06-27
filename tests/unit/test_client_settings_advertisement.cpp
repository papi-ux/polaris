/**
 * @file tests/unit/test_client_settings_advertisement.cpp
 * @brief Tests for the /api/config client-settings v1 endpoint advertisement contract.
 */

#include <src/confighttp.h>

#include <gtest/gtest.h>

TEST(ClientSettingsAdvertisementTests, BuildsGamestreamHttpsUrlFromConfigHostHeader) {
  const auto advertised = confighttp::client_settings_endpoint_url("10.0.0.232:47990", 47984);

  EXPECT_EQ(advertised, "https://10.0.0.232:47984/polaris/v1/client-settings");
}

TEST(ClientSettingsAdvertisementTests, PreservesBracketedIpv6HostsWhenReplacingPort) {
  const auto advertised = confighttp::client_settings_endpoint_url("[::1]:47990", 47984);

  EXPECT_EQ(advertised, "https://[::1]:47984/polaris/v1/client-settings");
}
