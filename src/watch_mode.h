/**
 * @file src/watch_mode.h
 * @brief The stream a watcher would join, said in fields a client can read before it asks.
 *
 * A watcher is handed the owner's encoded stream as it is, so its size, rate,
 * depth and codec are the owner's whatever the watcher asks for, and a watch
 * request for any other mode is refused. The only place the mode was ever
 * said was inside the sentence of that refusal. A client had to ask wrongly
 * first and then read prose to learn what to ask for, and two devices almost
 * never ask for the same thing: a handheld at 1920x1080@60 could not watch a
 * Deck at 1280x800@90.
 *
 * serverinfo also could not tell a game that someone is streaming from a game
 * that was left open with nobody attached. Both read as busy, and the second
 * has nothing to watch, so every other device offered "Watch Stream" and got
 * a 409 for pressing it.
 *
 * The mode is put on serverinfo for paired clients, so a client asks for the
 * right mode the first time, and on the 412 itself beside the sentence, for
 * the case where the owner's mode changed between the two requests. The
 * sentence stays as it was, because released clients read it.
 *
 * serverinfo gets elements, the way it says everything else about the running
 * game. The refusal gets root attributes, the way error_code and error_action
 * ride on a refusal: a client judges a response at its root tag and throws
 * there when the status is not 200, before it has read a single element.
 */
#pragma once

#include <boost/property_tree/ptree.hpp>
#include <string>

namespace watch_mode {
  struct mode_t {
    int width = 0;
    int height = 0;
    int fps_x1000 = 0;  ///< frames per second times a thousand, the way a session keeps it: 60000, or 59940
    int bit_depth = 8;  ///< 10 for an HDR stream
    std::string codec;  ///< "h264", "hevc" or "av1"
  };

  /// Whether a watch request for this size, rate and depth is a request for the stream as it is.
  inline bool asked_for(const mode_t &mode, int width, int height, int fps_x1000, bool hdr) {
    return width == mode.width && height == mode.height && fps_x1000 == mode.fps_x1000 &&
           hdr == (mode.bit_depth > 8);
  }

  /**
   * @brief Whether the display mode saved for a device replaces the mode it asked for.
   * @details The saved mode is for streams the device starts. A watcher joins a stream that is
   *          already running, at the mode its owner chose, so its request is left as it came. On
   *          a host with a saved 1920x1080x120 for a handheld, that handheld asked to watch a
   *          1920x1080@60 stream, had its request rewritten to 120, and was refused for asking for
   *          the wrong mode. A profile the host resolved for the client was never rewritten either.
   */
  inline bool device_display_mode_applies(bool device_has_display_mode, bool resolved_profile_from_client, bool watch_only) {
    return device_has_display_mode && !resolved_profile_from_client && !watch_only;
  }

  /**
   * @brief Put the mode on a response as flat elements: <prefix>width, height, fpsx1000, bitdepth, codec.
   * @details Flat and numeric on purpose. Clients read these responses one named element at a
   *          time, and a rate such as 59.94 does not survive being written as a whole number.
   */
  inline void put_elements(boost::property_tree::ptree &tree, const std::string &prefix, const mode_t &mode) {
    tree.put(prefix + "width", mode.width);
    tree.put(prefix + "height", mode.height);
    tree.put(prefix + "fpsx1000", mode.fps_x1000);
    tree.put(prefix + "bitdepth", mode.bit_depth);
    tree.put(prefix + "codec", mode.codec);
  }

  /// Put the mode on a refusal as root attributes: watch_width, watch_height, watch_fps_x1000, watch_bit_depth, watch_codec.
  inline void put_attributes(boost::property_tree::ptree &tree, const mode_t &mode) {
    tree.put("root.<xmlattr>.watch_width", mode.width);
    tree.put("root.<xmlattr>.watch_height", mode.height);
    tree.put("root.<xmlattr>.watch_fps_x1000", mode.fps_x1000);
    tree.put("root.<xmlattr>.watch_bit_depth", mode.bit_depth);
    tree.put("root.<xmlattr>.watch_codec", mode.codec);
  }
}  // namespace watch_mode
