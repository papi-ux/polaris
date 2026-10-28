/**
 * @file tests/unit/test_numeric_decode.cpp
 * @brief Test the overflow boundary of the legacy decimal decoder used for query, RTSP and config values.
 */
#include "../tests_common.h"

#include <src/utility.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {
  std::optional<std::int64_t> checked(std::string_view text) {
    return util::checked_from_chars(text.data(), text.data() + text.size());
  }

  // The rikeyid value of the 139-byte input retained from the first query fuzz campaign (#542). Its
  // 0xF5 run overflowed at "-59 * 1000000000000000000" in the original decoder.
  std::string retained_crash_rikeyid() {
    std::string value {"1\"cor\x01"};
    value.push_back('\0');
    value += "ever";
    value.append(78, '\xF5');
    value += "=1";
    return value;
  }
}  // namespace

TEST(NumericDecodeTests, InRangeValuesKeepTheirDecodedResult) {
  EXPECT_EQ(util::from_view(""), 0);
  EXPECT_EQ(util::from_view("0"), 0);
  EXPECT_EQ(util::from_view("1"), 1);
  EXPECT_EQ(util::from_view("007"), 7);
  EXPECT_EQ(util::from_view("100"), 100);
  EXPECT_EQ(util::from_view("196610"), 196610);
  // Narrowing stays with the callers that store an int or uint32_t.
  EXPECT_EQ(util::from_view("4294967296"), 4294967296LL);
  EXPECT_EQ(util::from_view("1000000000000000000"), 1000000000000000000LL);

  EXPECT_EQ(checked(""), std::optional<std::int64_t> {0});
  EXPECT_EQ(checked("196610"), std::optional<std::int64_t> {196610});
}

TEST(NumericDecodeTests, SignAndNonDigitBytesKeepTheirEarlierArithmetic) {
  EXPECT_EQ(util::from_view("-1"), -1);
  EXPECT_EQ(util::from_view("-0"), 0);
  EXPECT_EQ(util::from_view("-"), 0);
  // Only a leading '-' is a sign; any other byte adds (byte - '0') at its place.
  EXPECT_EQ(util::from_view("+5"), -45);
  EXPECT_EQ(util::from_view("x"), 72);
  EXPECT_EQ(util::from_view("1x"), 82);

  EXPECT_EQ(checked("x"), std::optional<std::int64_t> {72});
}

TEST(NumericDecodeTests, Int64BoundariesFollowTheEarlierArithmetic) {
  EXPECT_EQ(util::from_view("9223372036854775807"), std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(util::from_view("-999999999999999999"), -999999999999999999LL);

  // Each of these overflowed std::int64_t in the original decoder, so none had a defined value.
  for (const std::string_view text : {"9223372036854775808", "-9223372036854775807", "-9223372036854775808"}) {
    EXPECT_FALSE(checked(text).has_value()) << text;
    EXPECT_EQ(util::from_view(text), 0) << text;
  }
}

TEST(NumericDecodeTests, TwentyZeroDigitsCannotBeRepresented) {
  // The place multiplier itself leaves std::int64_t after nineteen bytes, whatever the digits.
  const std::string_view twenty_zeros {"00000000000000000000"};
  EXPECT_FALSE(checked(twenty_zeros).has_value());
  EXPECT_EQ(util::from_view(twenty_zeros), 0);
}

TEST(NumericDecodeTests, OverflowingDigitsDecodeAsAnEmptyValue) {
  for (const std::string_view text : {"9999999999999999999", "99999999999999999999", "12345678901234567890123"}) {
    EXPECT_FALSE(checked(text).has_value()) << text;
    EXPECT_EQ(util::from_view(text), 0) << text;
  }

  // A non-digit at the nineteenth place overflows its product within nineteen bytes.
  const std::string high_byte_place = '\xF5' + std::string(18, '0');
  EXPECT_FALSE(checked(high_byte_place).has_value());
  EXPECT_EQ(util::from_view(high_byte_place), 0);
}

TEST(NumericDecodeTests, RetainedCrashRikeyIdDecodesAsAnEmptyValue) {
  const auto value = retained_crash_rikeyid();
  ASSERT_EQ(value.size(), 91U);

  EXPECT_FALSE(checked(value).has_value());
  EXPECT_EQ(util::from_view(value), 0);
  EXPECT_EQ(util::from_chars(value.data(), value.data() + value.size()), 0);
}
