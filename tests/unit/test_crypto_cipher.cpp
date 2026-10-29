/**
 * @file tests/unit/test_crypto_cipher.cpp
 * @brief Validate AES-GCM decryption of [tag][cipher text] buffers, including ones too short to carry the tag.
 */

#include <src/crypto.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
  crypto::aes_t test_key() {
    return crypto::aes_t(crypto::cipher::key_size, 0x42);
  }

  // A 12-byte IV, as the RTSP and V2 control streams build.
  crypto::aes_t test_iv() {
    return crypto::aes_t {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 'C', 'C'};
  }

  std::vector<std::uint8_t> encrypt(std::string_view plaintext) {
    crypto::cipher::gcm_t cipher {test_key(), false};
    auto iv = test_iv();
    std::vector<std::uint8_t> tagged(crypto::cipher::tag_size + crypto::cipher::round_to_pkcs7_padded(plaintext.size()));
    const auto cipher_bytes = cipher.encrypt(plaintext, tagged.data(), &iv);
    EXPECT_GE(cipher_bytes, 0);
    tagged.resize(crypto::cipher::tag_size + cipher_bytes);
    return tagged;
  }

  std::string_view view(const std::vector<std::uint8_t> &bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }
}  // namespace

TEST(GcmDecrypt, RefusesInputShorterThanItsTag) {
  for (const auto bytes : {std::size_t {0}, std::size_t {1}, std::size_t {15}}) {
    // Sized exactly, so a read past the input would trip ASan.
    const std::vector<std::uint8_t> tagged_cipher(bytes, 0xA5);
    crypto::cipher::gcm_t cipher {test_key(), false};
    auto iv = test_iv();
    std::vector<std::uint8_t> plaintext;
    int result = 0;
    EXPECT_NO_THROW(result = cipher.decrypt(view(tagged_cipher), plaintext, &iv)) << "with " << bytes << " bytes";
    EXPECT_EQ(result, -1) << "with " << bytes << " bytes";
  }
}

TEST(GcmDecrypt, TagOnlyInputIsVerifiedAsAnEmptyMessage) {
  const auto tagged_cipher = encrypt("");
  ASSERT_EQ(tagged_cipher.size(), crypto::cipher::tag_size);

  {
    crypto::cipher::gcm_t cipher {test_key(), false};
    auto iv = test_iv();
    std::vector<std::uint8_t> plaintext {0xFF};
    EXPECT_EQ(cipher.decrypt(view(tagged_cipher), plaintext, &iv), 0);
    EXPECT_TRUE(plaintext.empty());
  }

  auto forged = tagged_cipher;
  forged[0] ^= 0x01;
  crypto::cipher::gcm_t cipher {test_key(), false};
  auto iv = test_iv();
  std::vector<std::uint8_t> plaintext;
  int result = 0;
  EXPECT_NO_THROW(result = cipher.decrypt(view(forged), plaintext, &iv));
  EXPECT_EQ(result, -1);
}

TEST(GcmDecrypt, RoundTripRecoversThePlaintext) {
  const std::string message = "input packet payload, longer than one block";
  const auto tagged_cipher = encrypt(message);
  ASSERT_EQ(tagged_cipher.size(), crypto::cipher::tag_size + message.size());

  crypto::cipher::gcm_t cipher {test_key(), false};
  auto iv = test_iv();
  std::vector<std::uint8_t> plaintext;
  ASSERT_EQ(cipher.decrypt(view(tagged_cipher), plaintext, &iv), 0);
  EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()), message);

  auto tampered = tagged_cipher;
  tampered.back() ^= 0x01;
  EXPECT_EQ(cipher.decrypt(view(tampered), plaintext, &iv), -1);
}
