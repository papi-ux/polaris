/**
 * @file tests/unit/test_otp_claim.cpp
 * @brief The one-time pin is issued on the confighttp thread and redeemed on
 *        the nvhttp one, so matching it, reading what it authorizes, and
 *        clearing it all have to happen as a single locked step. These cover
 *        the observable half of that: a pin is redeemable exactly once, and a
 *        miss neither reveals anything nor leaves the pin usable.
 */

#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/utility.h>

#include <gtest/gtest.h>

namespace {

  constexpr auto salt = "0123456789abcdef0123456789abcdef";
  constexpr auto passphrase = "correct horse battery staple";

  std::string presented_hash_for(const std::string &pin) {
    return std::string {util::hex(crypto::hash(pin + salt + passphrase), true).to_string_view()};
  }

  class OtpClaim: public ::testing::Test {
  protected:
    void SetUp() override {
      nvhttp::reset_pairing_state_for_tests();
    }

    void TearDown() override {
      nvhttp::reset_pairing_state_for_tests();
    }
  };

}  // namespace

TEST_F(OtpClaim, NoOutstandingPinMatchesNothing) {
  const auto claim = nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for("1234"));
  EXPECT_FALSE(claim.matched);
  EXPECT_TRUE(claim.pin.empty());
}

TEST_F(OtpClaim, MatchingHashYieldsThePinAndWhatItAuthorizes) {
  const auto pin = nvhttp::request_otp(
    passphrase,
    "Living Room TV",
    crypto::PERM::_game_control,
    true
  );
  ASSERT_FALSE(pin.empty());

  const auto claim = nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for(pin));

  ASSERT_TRUE(claim.matched);
  EXPECT_EQ(claim.pin, pin);
  EXPECT_EQ(claim.device_name, "Living Room TV");
  ASSERT_TRUE(claim.pairing_perm.has_value());
  EXPECT_EQ(*claim.pairing_perm, crypto::PERM::_game_control);
  EXPECT_TRUE(claim.temporary_authorization);
}

TEST_F(OtpClaim, APinIsRedeemableOnlyOnce) {
  const auto pin = nvhttp::request_otp(passphrase, "Living Room TV", crypto::PERM::_game_control);
  ASSERT_FALSE(pin.empty());

  ASSERT_TRUE(nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for(pin)).matched);

  const auto replay = nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for(pin));
  EXPECT_FALSE(replay.matched);
  EXPECT_TRUE(replay.pin.empty());
}

TEST_F(OtpClaim, AWrongHashNeitherMatchesNorBurnsThePin) {
  const auto pin = nvhttp::request_otp(passphrase, "Living Room TV", crypto::PERM::_game_control);
  ASSERT_FALSE(pin.empty());

  // A mismatched salt is the same shape of failure as a guessed hash.
  EXPECT_FALSE(nvhttp::claim_one_time_pin_for_tests("ffffffffffffffffffffffffffffffff", presented_hash_for(pin)).matched);
  EXPECT_FALSE(nvhttp::claim_one_time_pin_for_tests(salt, "not-a-hash").matched);

  // The legitimate client can still redeem it afterwards.
  EXPECT_TRUE(nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for(pin)).matched);
}

TEST_F(OtpClaim, ARejectedPassphraseIssuesNoPin) {
  EXPECT_TRUE(nvhttp::request_otp("abc", "Living Room TV", crypto::PERM::_game_control).empty());
  EXPECT_FALSE(nvhttp::claim_one_time_pin_for_tests(salt, presented_hash_for("1234")).matched);
}
