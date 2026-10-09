/**
 * @file tests/unit/test_recorder_media_capabilities.cpp
 * @brief Record which codecs the linked FFmpeg registers, for the recorder's later media slices (#361).
 *
 * Registration only: a codec is looked up by name or id and its identity is read. Nothing is
 * opened, allocated, encoded or decoded, so a registered codec here is not proof that it works,
 * that it can open on this host, or that any recorded file is playable. The facts are the linked
 * runtime's, as this binary sees them; which archive provided it is recorded separately by CI.
 */
#include "../tests_common.h"

#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace {
  /// One registration looked up the way the recorder's later slices would look it up.
  struct lookup_t {
    const char *key;        ///< Property prefix, e.g. "encoderLibx264"
    bool encoder;           ///< An encoder looked up by name, or a decoder looked up by id
    const char *name;       ///< The encoder name requested (encoders only)
    AVCodecID expected_id;  ///< The codec id the registration must carry when present
  };

  constexpr lookup_t k_lookups[] = {
    {"encoderLibx264", true, "libx264", AV_CODEC_ID_H264},
    {"encoderLibx265", true, "libx265", AV_CODEC_ID_HEVC},
    {"encoderLibsvtav1", true, "libsvtav1", AV_CODEC_ID_AV1},
    {"decoderH264", false, nullptr, AV_CODEC_ID_H264},
    {"decoderHevc", false, nullptr, AV_CODEC_ID_HEVC},
    {"decoderAv1", false, nullptr, AV_CODEC_ID_AV1},
  };

  std::string version_text(unsigned version) {
    return std::to_string(AV_VERSION_MAJOR(version)) + "." + std::to_string(AV_VERSION_MINOR(version)) + "." +
           std::to_string(AV_VERSION_MICRO(version));
  }

  std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out += escaped;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out + "\"";
  }
}  // namespace

// Every value is recorded twice, identically: as a GTest XML property and in one console JSON line
// prefixed POLARIS_RECORDER_MEDIA_CAPABILITIES_JSON. An absent registration is recorded as present 0
// with an empty name, id 0 and an empty type. Only the internal consistency of what is found is asserted.
TEST(RecorderMediaCapabilitiesTests, LinkedFfmpegRegistrationInventory) {
  std::vector<std::pair<std::string, std::string>> properties;
  const auto add = [&properties](std::string key, std::string value) {
    properties.emplace_back(std::move(key), std::move(value));
  };

  const char *version_info = av_version_info();
  const char *configuration = avcodec_configuration();
  ASSERT_NE(version_info, nullptr);
  ASSERT_NE(configuration, nullptr);
  add("scope", "registration lookup only; no codec opened, encoded or decoded");
  add("ffmpegVersionInfo", version_info);
  add("avcodecHeaderVersion", version_text(LIBAVCODEC_VERSION_INT));
  add("avcodecRuntimeVersion", version_text(avcodec_version()));
  add("avutilHeaderVersion", version_text(LIBAVUTIL_VERSION_INT));
  add("avutilRuntimeVersion", version_text(avutil_version()));
  add("avcodecConfiguration", configuration);

  for (const auto &lookup : k_lookups) {
    const AVCodec *codec = lookup.encoder ? avcodec_find_encoder_by_name(lookup.name) : avcodec_find_decoder(lookup.expected_id);
    const std::string key = lookup.key;
    add(key + "Lookup", lookup.encoder ? std::string {"encoder name "} + lookup.name :
                                         std::string {"decoder id "} + avcodec_get_name(lookup.expected_id));
    add(key + "ExpectedId", std::to_string(static_cast<int>(lookup.expected_id)));
    add(key + "Present", codec ? "1" : "0");
    if (!codec) {
      add(key + "Name", "");
      add(key + "Id", "0");
      add(key + "Type", "");
      continue;
    }
    const char *type = av_get_media_type_string(codec->type);
    add(key + "Name", codec->name ? codec->name : "");
    add(key + "Id", std::to_string(static_cast<int>(codec->id)));
    add(key + "Type", type ? type : "");
    // A registration found must be the codec it was looked up as.
    EXPECT_EQ(codec->id, lookup.expected_id) << key;
    EXPECT_EQ(codec->type, AVMEDIA_TYPE_VIDEO) << key;
    EXPECT_NE(codec->name, nullptr) << key;
    if (lookup.encoder) {
      EXPECT_TRUE(av_codec_is_encoder(codec)) << key;
      EXPECT_STREQ(codec->name, lookup.name) << key;
    } else {
      EXPECT_TRUE(av_codec_is_decoder(codec)) << key;
    }
  }

  // Coherent properties: unique keys, binary presence, and empty identity exactly when absent.
  std::set<std::string> keys;
  for (const auto &[key, value] : properties) {
    EXPECT_TRUE(keys.insert(key).second) << "repeated property " << key;
  }
  for (const auto &lookup : k_lookups) {
    const auto value = [&properties](const std::string &key) {
      for (const auto &[k, v] : properties) {
        if (k == key) {
          return v;
        }
      }
      return std::string {"<missing>"};
    };
    const std::string key = lookup.key;
    const auto present = value(key + "Present");
    ASSERT_TRUE(present == "0" || present == "1") << key;
    if (present == "0") {
      EXPECT_EQ(value(key + "Name"), "") << key;
      EXPECT_EQ(value(key + "Id"), "0") << key;
      EXPECT_EQ(value(key + "Type"), "") << key;
    } else {
      EXPECT_NE(value(key + "Name"), "") << key;
      EXPECT_EQ(value(key + "Id"), value(key + "ExpectedId")) << key;
      EXPECT_EQ(value(key + "Type"), "video") << key;
    }
  }

  std::string receipt = "{";
  for (std::size_t i = 0; i < properties.size(); ++i) {
    ::testing::Test::RecordProperty(properties[i].first, properties[i].second);
    receipt += (i ? "," : "") + json_string(properties[i].first) + ":" + json_string(properties[i].second);
  }
  receipt += "}";
  std::cout << "POLARIS_RECORDER_MEDIA_CAPABILITIES_JSON " << receipt << std::endl;
}
