/**
 * @file tests/unit/test_watch_mode.cpp
 * @brief A watcher can learn the stream's mode before it asks, and from fields, not from prose.
 *
 * papi, 2026-09-21: "i would like for the Watch Stream to function properly in general, even if
 * the resolution isnt calibrated". A Retroid at 1920x1080@60 could not watch a Deck at
 * 1280x800@90, because the only place the host said the mode was inside the sentence of the 412
 * it refused the watcher with. The same day a game sat open on the host with nobody streaming it,
 * and every other device offered "Watch Stream" for it, because serverinfo said busy either way.
 */

#include <src/watch_mode.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

namespace pt = boost::property_tree;

namespace {
  std::string source(const char *relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
  }

  std::string between(const std::string &text, const std::string &begin, const std::string &end) {
    const auto from = text.find(begin);
    if (from == std::string::npos) {
      return {};
    }
    const auto to = text.find(end, from + begin.size());
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
  }

  const watch_mode::mode_t deck {1280, 800, 90000, 8, "hevc"};
}  // namespace

TEST(WatchMode, OnlyARequestForTheStreamAsItIsCounts) {
  EXPECT_TRUE(watch_mode::asked_for(deck, 1280, 800, 90000, false));
  EXPECT_FALSE(watch_mode::asked_for(deck, 1920, 1080, 90000, false)) << "another size";
  EXPECT_FALSE(watch_mode::asked_for(deck, 1280, 800, 60000, false)) << "another rate";
  EXPECT_FALSE(watch_mode::asked_for(deck, 1280, 800, 90000, true)) << "HDR asked of an SDR stream";

  const watch_mode::mode_t hdr {3840, 2160, 59940, 10, "av1"};
  EXPECT_TRUE(watch_mode::asked_for(hdr, 3840, 2160, 59940, true));
  EXPECT_FALSE(watch_mode::asked_for(hdr, 3840, 2160, 59940, false)) << "SDR asked of an HDR stream";
  EXPECT_FALSE(watch_mode::asked_for(hdr, 3840, 2160, 60000, true)) << "59.94 is not 60";
}

TEST(WatchMode, TheModeIsWrittenAsFlatElementsAClientReadsByName) {
  pt::ptree tree;
  watch_mode::put_elements(tree, "root.currentgamewatch", {3840, 2160, 59940, 10, "av1"});

  EXPECT_EQ(tree.get<int>("root.currentgamewatchwidth"), 3840);
  EXPECT_EQ(tree.get<int>("root.currentgamewatchheight"), 2160);
  EXPECT_EQ(tree.get<int>("root.currentgamewatchfpsx1000"), 59940) << "a fractional rate survives";
  EXPECT_EQ(tree.get<int>("root.currentgamewatchbitdepth"), 10);
  EXPECT_EQ(tree.get<std::string>("root.currentgamewatchcodec"), "av1");

  std::ostringstream xml;
  pt::write_xml(xml, tree);
  EXPECT_NE(xml.str().find("<currentgamewatchfpsx1000>59940</currentgamewatchfpsx1000>"), std::string::npos);
}

TEST(WatchMode, TheRefusalCarriesTheModeBesideTheSentenceReleasedClientsRead) {
  // Root attributes, like error_code: a client throws at the root tag of a refusal, before
  // it has read any element under it.
  pt::ptree tree;
  tree.put("root.<xmlattr>.status_code", 412);
  watch_mode::put_attributes(tree, deck);
  std::ostringstream xml;
  pt::write_xml(xml, tree);
  for (const auto *attribute : {"watch_width=\"1280\"", "watch_height=\"800\"", "watch_fps_x1000=\"90000\"", "watch_bit_depth=\"8\"", "watch_codec=\"hevc\""}) {
    EXPECT_NE(xml.str().find(attribute), std::string::npos) << attribute;
  }
  EXPECT_EQ(xml.str().find("<watch"), std::string::npos) << "nothing a client would have to read past the root for";

  const auto nvhttp = source("src/nvhttp.cpp");
  const auto pin = between(nvhttp, "pin_watch_session_to_active_profile(rtsp_stream::launch_session_t", "void put_watch_refusal(");
  EXPECT_NE(pin.find("\"Watch mode must match the active stream profile ({})\""), std::string::npos)
    << "released Nova reads the mode out of this sentence; it stays word for word";
  EXPECT_NE(pin.find("owner_mode,\n        };"), std::string::npos) << "and the 412 hands the mode over as fields";
  EXPECT_NE(pin.find("return watch_refusal_t {409,"), std::string::npos);
  EXPECT_NE(pin.find("std::nullopt};"), std::string::npos) << "a 409 has no mode to offer: there is no stream";

  const auto put = between(nvhttp, "void put_watch_refusal(", "@brief The title someone has told us");
  EXPECT_NE(put.find("watch_mode::put_attributes(tree, *refusal.mode);"), std::string::npos);

  const auto launch = between(nvhttp, "void launch(", "void resume(");
  const auto resume = between(nvhttp, "void resume(", "void cancel(");
  EXPECT_NE(launch.find("put_watch_refusal(tree, *watch_refusal);"), std::string::npos)
    << "a watch that arrives as a same-app launch is refused the same way";
  EXPECT_NE(resume.find("put_watch_refusal(tree, *watch_refusal);"), std::string::npos);
}

TEST(WatchMode, ServerinfoSaysWhetherThereIsAStreamAndWhoseGameItIs) {
  const auto fields = between(
    source("src/nvhttp.cpp"),
    "void append_current_game_session_fields(",
    "}  // namespace"
  );
  EXPECT_NE(fields.find("tree.put(\"root.currentgamewatchable\", owner_profile ? 1 : 0);"), std::string::npos)
    << "a game left open with nobody streaming it is busy and cannot be watched";
  EXPECT_NE(fields.find("const auto owner_profile = active_owner_watch_profile();"), std::string::npos)
    << "watchable is decided by the same question the watch request is judged by";
  EXPECT_NE(fields.find("watch_mode::put_elements(tree, \"root.currentgamewatch\", watch_mode_of(*owner_profile));"), std::string::npos);
  EXPECT_NE(fields.find("proc::proc.get_session_owner_device_name()"), std::string::npos)
    << "a card can say whose game is open instead of \"another device\"";
}
