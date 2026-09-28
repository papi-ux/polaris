/**
 * @file tests/unit/test_pyrowave_availability.cpp
 * @brief Whether PyroWave is offered, and the refusals a launch it cannot serve gets instead.
 *
 * The regression behind these: a KDE host in HDR with capture = kms offered PyroWave, the
 * client negotiated it and built a decoder, and the stream carried no bytes, because the codec found
 * out at the first frame that it could not read the sixteen bit float scanout.
 */
#include <gtest/gtest.h>

#include <drm_fourcc.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "src/platform/linux/kms_connector_selection.h"
#include "src/pyrowave_availability.h"

namespace pa = pyrowave_availability;

namespace {
  /// Text a Moonlight client shows as it is must not send anyone to a Nova screen, and none of it
  /// may carry a dash used as punctuation.
  void expect_client_neutral(std::string_view text) {
    EXPECT_EQ(text.find("Play Setup"), std::string_view::npos) << text;
    EXPECT_EQ(text.find("Nova"), std::string_view::npos) << text;
    EXPECT_EQ(text.find("matching build"), std::string_view::npos) << text;
    EXPECT_EQ(text.find("—"), std::string_view::npos) << text;
    EXPECT_EQ(text.find("–"), std::string_view::npos) << text;
    EXPECT_EQ(text.find(" - "), std::string_view::npos) << text;
  }
}  // namespace

TEST(PyroWaveAvailability, ReasonIdsAreTheClientContract) {
  // Nova words its own screens from these ids, so they may never drift.
  EXPECT_EQ(pa::reason_id(pa::reason_e::not_built), "not_built");
  EXPECT_EQ(pa::reason_id(pa::reason_e::no_vulkan_device), "no_vulkan_device");
  EXPECT_EQ(pa::reason_id(pa::reason_e::fp16_capture), "fp16_capture");
  EXPECT_EQ(pa::reason_id(pa::reason_e::capture_route_unsupported), "capture_route_unsupported");
}

TEST(PyroWaveAvailability, FloatFourccsAreTheFourDrmNames) {
  for (const auto fourcc : {DRM_FORMAT_XRGB16161616F, DRM_FORMAT_ARGB16161616F,
                            DRM_FORMAT_XBGR16161616F, DRM_FORMAT_ABGR16161616F}) {
    EXPECT_TRUE(pa::is_fp16_fourcc(fourcc)) << fourcc;
  }
  // 1211384385 is the fourcc the Deck acceptance run logged for KWin's HDR scanout.
  EXPECT_TRUE(pa::is_fp16_fourcc(1211384385u));
  for (const auto fourcc : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR2101010,
                            DRM_FORMAT_XRGB2101010, DRM_FORMAT_NV12, 0u}) {
    EXPECT_FALSE(pa::is_fp16_fourcc(fourcc)) << fourcc;
  }
}

TEST(PyroWaveAvailability, OnlyAKmsScanoutCanBeUnreadable) {
  const pa::scanout_t fp16 {false, true};
  const pa::scanout_t other {false, false};
  const pa::scanout_t fine {true, false};

  // Every other backend negotiates or converts what it hands over.
  for (const auto *backend : {"wlr", "portal", "x11", "nvfbc"}) {
    EXPECT_EQ(pa::classify_route(backend, backend, fp16), pa::route_e::readable) << backend;
  }
  EXPECT_EQ(pa::classify_route("kms", "kms", fp16), pa::route_e::fp16_scanout);
  EXPECT_EQ(pa::classify_route("kms", "kms", other), pa::route_e::unreadable_scanout);
  EXPECT_EQ(pa::classify_route("kms", "kms", fine), pa::route_e::readable);
  // A scanout nobody could read is not evidence of anything.
  EXPECT_EQ(pa::classify_route("kms", "kms", std::nullopt), pa::route_e::unknown);
}

TEST(PyroWaveAvailability, ARequestOnlyPyroWaveCannotUseIsUnsupported) {
  // capture = nvfbc: NVENC's CUDA memory gets NvFBC, and PyroWave's memory gets nothing.
  EXPECT_EQ(pa::classify_route("none", "nvfbc", std::nullopt), pa::route_e::unsupported_backend);
  // A request nothing can use is refused elsewhere with its own reason, so it is not PyroWave's.
  EXPECT_EQ(pa::classify_route("none", "none", std::nullopt), pa::route_e::unknown);
  EXPECT_EQ(pa::classify_route("none", "", std::nullopt), pa::route_e::unknown);
  // Before the host has evaluated its capture sources there is nothing to judge.
  EXPECT_EQ(pa::classify_route("", "kms", pa::scanout_t {false, true}), pa::route_e::unknown);
}

TEST(PyroWaveAvailability, ABuildOrDeviceWithoutPyroWaveSaysWhich) {
  pa::offer_facts_t facts;
  auto unavailable = pa::unavailable(facts);
  ASSERT_TRUE(unavailable.has_value());
  EXPECT_EQ(unavailable->reason, pa::reason_e::not_built);
  EXPECT_FALSE(unavailable->message.empty());

  facts.built = true;
  unavailable = pa::unavailable(facts);
  ASSERT_TRUE(unavailable.has_value());
  EXPECT_EQ(unavailable->reason, pa::reason_e::no_vulkan_device);
  // Only not_built may send a player looking for another build.
  EXPECT_EQ(unavailable->message.find("Install"), std::string::npos) << unavailable->message;
  expect_client_neutral(unavailable->message);
}

TEST(PyroWaveAvailability, AnHdrDesktopHidesPyroWaveOnlyWhenNoLaunchCouldStreamIt) {
  pa::offer_facts_t facts;
  facts.built = true;
  facts.device = true;
  facts.host_route = pa::route_e::fp16_scanout;

  auto unavailable = pa::unavailable(facts);
  ASSERT_TRUE(unavailable.has_value());
  EXPECT_EQ(unavailable->reason, pa::reason_e::fp16_capture);
  EXPECT_NE(unavailable->message.find("HDR"), std::string::npos) << unavailable->message;
  expect_client_neutral(unavailable->message);

  // A Private Stream launch is captured from its own compositor, so it streams whatever the desktop
  // scans out, and hiding the codec would take it away from that launch too.
  facts.private_mode_available = true;
  EXPECT_FALSE(pa::unavailable(facts).has_value());
}

TEST(PyroWaveAvailability, OtherUnreadableRoutesAreCaptureRouteUnsupported) {
  pa::offer_facts_t facts;
  facts.built = true;
  facts.device = true;

  for (const auto route : {pa::route_e::unreadable_scanout, pa::route_e::unsupported_backend}) {
    facts.host_route = route;
    const auto unavailable = pa::unavailable(facts);
    ASSERT_TRUE(unavailable.has_value());
    EXPECT_EQ(unavailable->reason, pa::reason_e::capture_route_unsupported);
    expect_client_neutral(unavailable->message);
  }

  // Refusing on a guess would hide a working codec; the launch refusal still stands behind it.
  for (const auto route : {pa::route_e::readable, pa::route_e::unknown}) {
    facts.host_route = route;
    EXPECT_FALSE(pa::unavailable(facts).has_value());
  }
}

TEST(PyroWaveAvailability, ALaunchOnAFloatScanoutIsRefusedWithAReasonAnyClientCanShow) {
  const auto refusal = pa::launch_refusal(pa::route_e::fp16_scanout, "kms");
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 503);
  EXPECT_EQ(refusal->code, "pyrowave_capture_unreadable");
  EXPECT_NE(refusal->message.find("HDR"), std::string::npos) << refusal->message;
  EXPECT_NE(refusal->action.find("Turn HDR off"), std::string::npos) << refusal->action;
  expect_client_neutral(refusal->message);
  expect_client_neutral(refusal->action);

  const auto other = pa::launch_refusal(pa::route_e::unreadable_scanout, "kms");
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->code, "pyrowave_capture_unreadable");
  EXPECT_EQ(other->message.find("HDR"), std::string::npos) << other->message;
  expect_client_neutral(other->message);
  expect_client_neutral(other->action);
}

TEST(PyroWaveAvailability, AnUnsupportedCaptureSettingIsNamedInTheRefusal) {
  const auto refusal = pa::launch_refusal(pa::route_e::unsupported_backend, "nvfbc");
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "pyrowave_capture_unreadable");
  EXPECT_NE(refusal->message.find("nvfbc"), std::string::npos) << refusal->message;
  EXPECT_NE(refusal->action.find("Autodetect"), std::string::npos) << refusal->action;
  expect_client_neutral(refusal->message);

  const auto automatic = pa::launch_refusal(pa::route_e::unsupported_backend, "auto");
  ASSERT_TRUE(automatic.has_value());
  EXPECT_NE(automatic->message.find("Autodetect"), std::string::npos) << automatic->message;
}

TEST(PyroWaveAvailability, AReadableOrUnknownRouteIsNeverRefused) {
  EXPECT_FALSE(pa::launch_refusal(pa::route_e::readable, "kms").has_value());
  EXPECT_FALSE(pa::launch_refusal(pa::route_e::unknown, "kms").has_value());
}

TEST(PyroWaveAvailability, OneCaptureServesStreamsOnOneSideOfPyroWaveOnly) {
  // The capture thread opens its display for the first stream's memory type.
  EXPECT_TRUE(pa::shares_capture(true, {}));
  EXPECT_TRUE(pa::shares_capture(false, {}));
  EXPECT_TRUE(pa::shares_capture(true, {true, true}));
  EXPECT_TRUE(pa::shares_capture(false, {false}));
  EXPECT_FALSE(pa::shares_capture(true, {false}));
  EXPECT_FALSE(pa::shares_capture(false, {true}));
  EXPECT_FALSE(pa::shares_capture(false, {false, true}));

  for (const bool incoming : {true, false}) {
    const auto refusal = pa::capture_in_use_refusal(incoming);
    EXPECT_EQ(refusal.status, 503);
    EXPECT_EQ(refusal.code, "capture_in_use_by_other_codec");
    EXPECT_FALSE(refusal.action.empty());
    expect_client_neutral(refusal.message);
    expect_client_neutral(refusal.action);
  }
}

TEST(KmsCaptureIndex, ResolvesANameTheWayCaptureDoes) {
  namespace ks = platf::kms_selection;
  const std::vector<std::string> names {"kms:pci-0000:01:00.0/DP-2", "kms:pci-0000:01:00.0/HDMI-A-1"};

  // No configured output is the first display, and a number is a legacy position.
  EXPECT_EQ(ks::capture_index(names, ""), 0u);
  EXPECT_EQ(ks::capture_index(names, "1"), 1u);
  EXPECT_FALSE(ks::capture_index(names, "2").has_value());
  // A full name, the name without its prefix, or the connector alone.
  EXPECT_EQ(ks::capture_index(names, "kms:pci-0000:01:00.0/HDMI-A-1"), 1u);
  EXPECT_EQ(ks::capture_index(names, "pci-0000:01:00.0/DP-2"), 0u);
  EXPECT_EQ(ks::capture_index(names, "HDMI-A-1"), 1u);
  EXPECT_FALSE(ks::capture_index(names, "DP-3").has_value());
  EXPECT_FALSE(ks::capture_index({}, "").has_value());

  // A connector alias is refused when an unnamed entry could be the same connector elsewhere.
  const std::vector<std::string> partly_named {"kms:pci-0000:01:00.0/DP-2", "1"};
  EXPECT_FALSE(ks::capture_index(partly_named, "DP-2").has_value());
  EXPECT_EQ(ks::capture_index(partly_named, "kms:pci-0000:01:00.0/DP-2"), 0u);
}
