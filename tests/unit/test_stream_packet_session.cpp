/**
 * @file tests/unit/test_stream_packet_session.cpp
 * @brief Packet destinations follow real host session stop and destruction.
 */
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <enet/enet.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
  std::shared_ptr<stream::session_t> make_session() {
    stream::config_t config {};
    rtsp_stream::launch_session_t launch {};
    launch.id = 1;
    launch.gcm_key.resize(16);
    launch.iv.resize(16);
    launch.unique_id = "packet-owner-client";
    return stream::session::alloc(config, launch);
  }
}

TEST(StreamPacketSessionTests, PublishedControlSessionAlreadyHasItsFullPingDeadline) {
  auto session = make_session();
  const auto before = std::chrono::steady_clock::now();
  const auto deadline = stream::session::register_control_session_for_tests(*session);
  const auto after = std::chrono::steady_clock::now();

  // A freshly published session must not look expired before its first ping.
  EXPECT_GE(deadline, before + config::stream.ping_timeout);
  EXPECT_LE(deadline, after + config::stream.ping_timeout);
}

TEST(StreamPacketSessionTests, StopRejectsQueuedAudioAndVideoBeforeSessionDestruction) {
  for (const auto graceful : {false, true}) {
    auto session = make_session();
    const auto destination = stream::session::packet_destination_for_tests(*session);
    video::packet_raw_generic video {{1, 2, 3}, 1, true};
    video.channel_data = destination;
    audio::packet_t audio {destination, audio::buffer_t {3}};
    auto active = video.channel_data.acquire();
    EXPECT_EQ(active.get(), session.get());
    EXPECT_EQ(audio.first.acquire().get(), session.get());

    // Exercise the actual capacity-32 mail queues: video loses only frame 0,
    // while audio keeps its existing clear-on-full behavior.
    auto mailbox = std::make_shared<safe::mail_raw_t>();
    auto video_packets = mailbox->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mailbox->queue<audio::packet_t>(mail::audio_packets);
    for (int index = 0; index < 33; ++index) {
      auto packet = std::make_unique<video::packet_raw_generic>(std::vector<std::uint8_t> {1, 2, 3}, index, true);
      packet->channel_data = destination;
      video_packets->raise(std::move(packet));
      audio::packet_t sound {destination, audio::buffer_t {1}};
      sound.second[0] = static_cast<std::uint8_t>(index);
      audio_packets->raise(std::move(sound));
    }

    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    if (graceful) {
      stream::session::graceful_stop(*session);
    } else {
      stream::session::stop(*session);
    }
    EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPING);
    EXPECT_FALSE(video.channel_data.acquire());
    EXPECT_FALSE(audio.first.acquire());
    // stop cannot block waiting for this already admitted broadcaster.
    EXPECT_EQ(stream::session::uuid(*static_cast<stream::session_t *>(active.get())),
              "packet-owner-client");
    active.reset();
    session.reset();
    EXPECT_FALSE(video.channel_data.acquire());
    EXPECT_FALSE(audio.first.acquire());
    for (int index = 1; index <= 32; ++index) {
      auto packet = video_packets->pop(0ms);
      ASSERT_TRUE(packet);
      EXPECT_EQ(packet->frame_index(), index);
      EXPECT_FALSE(packet->channel_data.acquire());
    }
    EXPECT_FALSE(video_packets->peek());
    auto sound = audio_packets->pop(0ms);
    ASSERT_TRUE(sound);
    EXPECT_EQ(sound->second[0], 32);
    EXPECT_FALSE(sound->first.acquire());
    EXPECT_FALSE(audio_packets->peek());
  }
}

TEST(StreamPacketSessionTests, AbortedAllocationDrainsSendsAndRejectsRetainedPackets) {
  auto session = make_session();
  const auto queued = stream::session::packet_destination_for_tests(*session);
  auto active = queued.acquire();
  std::promise<void> entered;
  auto destruction = std::async(std::launch::async,
    [retiring = std::move(session), &entered]() mutable {
      entered.set_value();
      retiring.reset();
    });
  entered.get_future().wait();
  EXPECT_EQ(destruction.wait_for(20ms), std::future_status::timeout);
  active.reset();
  EXPECT_EQ(destruction.wait_for(1s), std::future_status::ready);
  destruction.get();
  EXPECT_FALSE(queued.acquire());

  auto reconnected = make_session();
  auto current = stream::session::packet_destination_for_tests(*reconnected);
  EXPECT_FALSE(queued.acquire());
  EXPECT_EQ(current.acquire().get(), reconnected.get());
}

/**
 * A stream its encoder can never serve ends with the frame conversion code, and every other stop
 * keeps the answer it had.
 *
 * The encoder used to end such a stream by raising the shutdown event alone, and the control thread
 * answered that stop with a bare disconnect. A Moonlight client reads a bare disconnect as a dropped
 * connection: Nova reconnected up to four times, each new session met the same refusal, and then it
 * showed a generic error. The frame conversion code is one Nova already shows as a fatal video
 * encoding error and never reconnects into.
 */
TEST(StreamControlTerminationTests, AStreamItsEncoderCannotServeEndsWithTheFrameConversionCode) {
  {
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    stream::session::stop(*session);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*session), std::nullopt)
      << "a stop nobody asked to be graceful now sends a termination code";
  }
  {
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    stream::session::graceful_stop(*session);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*session), std::optional<std::uint32_t> {0x80030023u})
      << "a graceful stop no longer says the host closed the stream";
  }
  {
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    const auto session_mail = stream::session::mail_for_tests(*session);
    video::end_stream_encoder_cannot_serve_for_tests(session_mail);
    EXPECT_TRUE(session_mail->event<bool>(mail::shutdown)->peek()) << "the stream the encoder cannot serve goes on";
    // What the video thread's fail guard does once capture returns. It is not a graceful stop.
    stream::session::stop(*session);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*session), std::optional<std::uint32_t> {0x800e9403u})
      << "the stream ends with a bare disconnect, so the client reconnects into the same refusal";
  }
}

#ifdef POLARIS_BUILD_PYROWAVE
namespace {
  /// The host's log while a test runs, to count the lines it wrote.
  class HostLogLines {
  public:
    HostLogLines():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~HostLogLines() {
      boost::log::core::get()->remove_sink(sink_);
    }

    HostLogLines(const HostLogLines &) = delete;
    HostLogLines &operator=(const HostLogLines &) = delete;

    /// How many captured lines hold needle.
    [[nodiscard]] int count(std::string_view needle) const {
      std::istringstream input {stream_->str()};
      int found = 0;
      for (std::string line; std::getline(input, line);) {
        if (line.find(needle) != std::string::npos) {
          ++found;
        }
      }
      return found;
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
  };

  /**
   * A 1920x1080 display that is not in HDR, standing in for the one capture published. The loop asks
   * whether it is in HDR once for each encode session it builds on it, as it works out the stream's
   * colourimetry, so that question counts the sessions.
   */
  class SdrDisplay final: public platf::display_t {
  public:
    explicit SdrDisplay(std::function<void(int)> on_session):
        on_session {std::move(on_session)} {
      width = env_width = 1920;
      height = env_height = 1080;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &, bool *) override {
      ++captures;
      if (on_capture) return on_capture(push, failure);
      return failure.reason == platf::capture_failure_e::none ? platf::capture_e::ok : platf::capture_e::reinit;
    }

    platf::capture_failure_t capture_failure() const override { return failure; }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return {};
    }

    int dummy_img(platf::img_t *) override {
      return -1;
    }

    bool is_hdr() override {
      on_session(++sessions);
      return false;
    }

    int sessions = 0;
    int captures = 0;
    platf::capture_failure_t failure;
    std::function<void(int)> on_session;
    std::function<platf::capture_e(const push_captured_image_cb_t &, platf::capture_failure_t &)> on_capture;
  };

  /// A 60 fps PyroWave stream as a client negotiates it, in HDR or not.
  video::config_t pyrowave_stream(bool hdr) {
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 60;
    config.bitrate = 20000;
    config.videoFormat = video::VIDEO_FORMAT_PYROWAVE;
    config.dynamicRange = hdr ? 1 : 0;
    return config;
  }

  constexpr std::string_view hdr_refusal = "negotiated HDR and the captured display is not in HDR";
  constexpr std::string_view producer_refusal = "current producer snapshot did not advertise a supported packed 10-bit RGB format for this attempt";

  video::config_t gamescope_hdr_stream() {
    auto config = pyrowave_stream(true);
    config.capture_generation.generation_id = 71;
    config.capture_generation.stream_mode = "gamescope_stream";
    config.capture_generation.capture_backend = "portal";
    config.capture_generation.exact_display_name = "test-gamescope-source";
    config.capture_generation.requested_output_name = config.capture_generation.exact_display_name;
    return config;
  }

  /**
   * The HDR owner at the front of the actual shared capture thread, and an SDR stream of the same
   * generation behind it. @p end_front ends the owner's attempt at @p site: its first open ("open"),
   * its reopen after a reinit ("reset"), or a capture that already admitted the SDR stream
   * ("capture"). Returns the owner's opens, and whether the SDR stream was then served: a display
   * reopened with its own config captured while its images ran, its stream was not shut down and
   * the owner's images had stopped.
   */
  std::pair<int, bool> serve_sdr_after_hdr_owner_ends(std::string_view site, stream::session_t &owner, stream::session_t &sdr,
    const std::function<void(const video::config_t &, platf::capture_failure_t &)> &end_front,
    const std::function<void()> &before_shutdown = {}) {
    auto sdr_config = gamescope_hdr_stream();
    sdr_config.dynamicRange = 0;
    const std::vector<video::capture_subscriber_for_tests_t> subscribers {
      {std::make_shared<video::img_event_t::element_type>(), stream::session::mail_for_tests(owner),
       stream::session::packet_destination_for_tests(owner), gamescope_hdr_stream()},
      {std::make_shared<video::img_event_t::element_type>(), stream::session::mail_for_tests(sdr),
       stream::session::packet_destination_for_tests(sdr), sdr_config},
    };
    const auto &front = subscribers.front();
    const auto &survivor = subscribers.back();
    int owner_opens = 0;
    bool served = false;
    video::capture_thread_for_tests(subscribers, [&](platf::mem_type_e, const std::string &, const video::config_t &requested,
                                                     platf::capture_failure_t &failure) -> std::shared_ptr<platf::display_t> {
      auto display = std::make_shared<SdrDisplay>([](int) {});
      if (requested.dynamicRange == 0) {
        display->on_capture = [&](const platf::display_t::push_captured_image_cb_t &, platf::capture_failure_t &) {
          served = survivor.images->running() && !survivor.mail->event<bool>(mail::shutdown)->peek() && !front.images->running();
          return platf::capture_e::ok;
        };
        return display;
      }
      if (++owner_opens == 1 && site == "reset") {
        display->on_capture = [](const platf::display_t::push_captured_image_cb_t &, platf::capture_failure_t &) {
          return platf::capture_e::reinit;
        };
        return display;
      }
      if (site == "capture") {
        display->on_capture = [&end_front, requested](const platf::display_t::push_captured_image_cb_t &push,
                                                      platf::capture_failure_t &captured) {
          push({}, false);  // The capture thread admits the queued SDR stream here.
          end_front(requested, captured);
          return platf::capture_e::reinit;
        };
        return display;
      }
      end_front(requested, failure);
      return {};
    }, before_shutdown);
    return {owner_opens, served};
  }
}  // namespace

/**
 * An HDR PyroWave stream from a display that is not in HDR is refused once and ends with the frame
 * conversion code, through the loop the host runs for a parallel encode session.
 *
 * The client agreed its colourimetry before the stream began, and the display is not in HDR, so the
 * next attempt refuses the same way. The loop builds a session again whenever one returns, so a
 * refusal that only returned was built again and logged again for as long as the stream lasted, and
 * a stream that ended with a bare disconnect was reconnected into the same refusal. The code the
 * stream ends with is the one Moonlight clients read as a fatal video encoding error, which Nova does
 * not retry and on Android explains by suggesting HDR be turned off or the resolution changed.
 */
TEST(StreamControlTerminationTests, AnHdrStreamFromADisplayNotInHdrIsRefusedOnceAndEndsWithTheFrameConversionCode) {
  HostLogLines log;
  auto session = make_session();
  stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
  const auto session_mail = stream::session::mail_for_tests(*session);
  // A loop that builds the refused session again never returns on its own, so the third session
  // stops the stream the way a client disconnecting would, and the test fails instead of hanging.
  const auto display = std::make_shared<SdrDisplay>([&session](int built) {
    if (built == 3) {
      stream::session::stop(*session);
    }
  });

  video::encode_published_display_for_tests(session_mail, pyrowave_stream(true), display,
                                             stream::session::packet_destination_for_tests(*session));

  EXPECT_EQ(display->sessions, 1) << "the loop built the refused session again";
  EXPECT_EQ(log.count(hdr_refusal), 1) << "the refusal was logged again for every session built";
  EXPECT_TRUE(session_mail->event<bool>(mail::frame_conversion_failed)->peek());
  EXPECT_TRUE(session_mail->event<bool>(mail::shutdown)->peek());
  // What the video thread's fail guard does once capture returns. It is not a graceful stop.
  stream::session::stop(*session);
  EXPECT_EQ(stream::session::control_termination_code_for_tests(*session), std::optional<std::uint32_t> {0x800e9403u})
    << "the stream ends with a code the client reconnects into, or with none";

  // The same terminal agreement also applies before any display exists and when that display
  // reopens capture. Both go through the actual reset/capture consumers, not an encoder-only loop.
  for (const bool reopen_capture : {false, true}) {
    auto opening = make_session();
    stream::session::set_state_for_tests(*opening, stream::session::state_e::RUNNING);
    const auto opening_mail = stream::session::mail_for_tests(*opening);
    const auto destination = stream::session::packet_destination_for_tests(*opening);
    const auto config = gamescope_hdr_stream();
    auto opened = std::make_shared<SdrDisplay>([](int) {});
    int attempts = 0, ordered_stops = 0;
    const auto before = log.count(producer_refusal);
    const auto open = [&](platf::mem_type_e, const std::string &, const video::config_t &requested,
                          platf::capture_failure_t &failure) -> std::shared_ptr<platf::display_t> {
      ++attempts;
      const platf::capture_failure_t unsupported {platf::capture_failure_e::unsupported_hdr_producer_format,
                                                 requested.capture_generation, destination.capture_owner_tag()};
      if (reopen_capture) { opened->failure = unsupported; return opened; }
      failure = unsupported;
      return {};
    };
    const auto before_shutdown = [&] {
      ++ordered_stops;
      EXPECT_TRUE(opening_mail->event<bool>(mail::frame_conversion_failed)->peek());
      EXPECT_FALSE(opening_mail->event<bool>(mail::shutdown)->peek());
    };
    video::reset_capture_display_for_tests(opening_mail, config, destination, open, reopen_capture, before_shutdown);
    video::reset_capture_display_for_tests(opening_mail, config, destination, open);
    EXPECT_EQ(attempts, 1) << "the outer display-open path retried a terminal or already stopped owner";
    EXPECT_EQ(opened->captures, reopen_capture ? 1 : 0);
    EXPECT_EQ(ordered_stops, 1);
    EXPECT_EQ(log.count(producer_refusal), before + 1);
    EXPECT_TRUE(opening_mail->event<bool>(mail::shutdown)->peek());
    stream::session::stop(*opening);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*opening), std::optional<std::uint32_t> {0x800e9403u});
  }

  // A refusal ends only its owner. The shared capture thread keeps the SDR stream of the same
  // generation behind it and reopens for that stream, whether the refusal came at the first open,
  // a reopen or a capture.
  for (const std::string_view site : {"open", "reset", "capture"}) {
    auto owner = make_session(), sdr = make_session();
    stream::session::set_state_for_tests(*owner, stream::session::state_e::RUNNING);
    stream::session::set_state_for_tests(*sdr, stream::session::state_e::RUNNING);
    const auto owner_mail = stream::session::mail_for_tests(*owner);
    const auto sdr_mail = stream::session::mail_for_tests(*sdr);
    const auto owner_tag = stream::session::packet_destination_for_tests(*owner).capture_owner_tag();
    int ordered_stops = 0;
    const auto before = log.count(producer_refusal);
    const auto [owner_opens, served] = serve_sdr_after_hdr_owner_ends(site, *owner, *sdr,
      [&](const video::config_t &requested, platf::capture_failure_t &failure) {
        failure = {platf::capture_failure_e::unsupported_hdr_producer_format, requested.capture_generation, owner_tag};
      }, [&] {
        ++ordered_stops;
        EXPECT_TRUE(owner_mail->event<bool>(mail::frame_conversion_failed)->peek());
        EXPECT_FALSE(owner_mail->event<bool>(mail::shutdown)->peek());
      });
    EXPECT_EQ(owner_opens, site == "reset" ? 2 : 1) << site;
    EXPECT_TRUE(served) << site << ": the stream sharing the capture was ended or never reopened";
    EXPECT_EQ(ordered_stops, 1) << site;
    EXPECT_EQ(log.count(producer_refusal), before + 1) << site;
    EXPECT_TRUE(owner_mail->event<bool>(mail::shutdown)->peek()) << site;
    EXPECT_FALSE(sdr_mail->event<bool>(mail::frame_conversion_failed)->peek()) << site;
    EXPECT_FALSE(sdr_mail->event<bool>(mail::shutdown)->peek()) << site;
    stream::session::stop(*owner);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*owner), std::optional<std::uint32_t> {0x800e9403u});
  }

  // The synchronized capture worker retires its front owner with the operation its reset and capture
  // sites call, and goes on to reopen for the session it kept. With nobody left it ends, as it does
  // when its last session leaves.
  const auto retired = std::make_shared<safe::mail_raw_t>();
  const auto kept = std::make_shared<safe::mail_raw_t>();
  // Mail keeps its events only weakly. A stream session holds its own shutdown event, so hold
  // both here as the sessions would, or the retired context takes the raised one with it.
  const auto retired_shutdown = retired->event<bool>(mail::shutdown);
  const auto kept_shutdown = kept->event<bool>(mail::shutdown);
  safe::signal_t retired_join, kept_join;
  EXPECT_TRUE(video::retire_front_sync_session_for_tests({{retired, &retired_join}, {kept, &kept_join}}));
  EXPECT_TRUE(retired_shutdown->peek());
  EXPECT_TRUE(retired_join.peek());
  EXPECT_FALSE(kept_shutdown->peek());
  EXPECT_FALSE(kept_join.peek());
  EXPECT_FALSE(video::retire_front_sync_session_for_tests({{kept, &kept_join}}));
  EXPECT_TRUE(kept_join.peek());
}

/**
 * A session that could not be built for a reason that may not hold next time is still built again,
 * and the stop that ends its stream keeps the answer it had.
 *
 * Ending the stream is for a refusal that cannot change. A PyroWave session the codec could not
 * make says nothing about the next attempt, so the loop builds it again, and whoever stops the
 * stream decides how it ends: a plain stop sends nothing and a graceful one says the host closed it.
 */
TEST(StreamControlTerminationTests, ASessionThatCouldNotBeBuiltThisTimeIsBuiltAgainAndItsStopKeepsItsAnswer) {
  for (const bool graceful : {false, true}) {
    SCOPED_TRACE(graceful ? "a graceful stop" : "a plain stop");
    HostLogLines log;
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    const auto session_mail = stream::session::mail_for_tests(*session);
    const auto display = std::make_shared<SdrDisplay>([&session, graceful](int built) {
      if (built == 3) {
        graceful ? stream::session::graceful_stop(*session) : stream::session::stop(*session);
      }
    });
    // PyroWave makes no session of no size, on any host, and nothing about that refusal is final.
    auto config = pyrowave_stream(false);
    config.width = 0;
    config.height = 0;

    video::encode_published_display_for_tests(session_mail, config, display,
                                               stream::session::packet_destination_for_tests(*session));

    EXPECT_EQ(display->sessions, 3) << "the loop stopped building a session whose failure may not repeat";
    EXPECT_EQ(log.count(hdr_refusal), 0);
    EXPECT_FALSE(session_mail->event<bool>(mail::frame_conversion_failed)->peek())
      << "a failure that may not repeat ended the stream as one that cannot change";
    stream::session::stop(*session);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*session),
              graceful ? std::optional<std::uint32_t> {0x80030023u} : std::nullopt);

    auto opening = make_session();
    stream::session::set_state_for_tests(*opening, stream::session::state_e::RUNNING);
    const auto opening_mail = stream::session::mail_for_tests(*opening);
    int attempts = 0;
    video::reset_capture_display_for_tests(opening_mail, gamescope_hdr_stream(),
      stream::session::packet_destination_for_tests(*opening),
      [&](platf::mem_type_e, const std::string &, const video::config_t &, platf::capture_failure_t &) {
        ++attempts; return std::shared_ptr<platf::display_t> {};
      });
    EXPECT_EQ(attempts, 3) << "a generic display-open failure became permanent";
    // Browser capture passes no packet destination. It must still open and capture normally.
    const auto unowned = std::make_shared<SdrDisplay>([](int) {});
    video::reset_capture_display_for_tests(opening_mail, gamescope_hdr_stream(), {},
      [&](platf::mem_type_e, const std::string &, const video::config_t &, platf::capture_failure_t &) {
        return unowned;
      }, true);
    EXPECT_EQ(unowned->captures, 1);
    EXPECT_FALSE(opening_mail->event<bool>(mail::frame_conversion_failed)->peek());
    EXPECT_FALSE(opening_mail->event<bool>(mail::shutdown)->peek());
    graceful ? stream::session::graceful_stop(*opening) : stream::session::stop(*opening);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*opening),
              graceful ? std::optional<std::uint32_t> {0x80030023u} : std::nullopt);
  }
}
TEST(StreamControlTerminationTests, RepeatedEncoderInitializationFailureHasABoundedBudgetAndEndsAsVideoFailure) {
  auto session = make_session();
  stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
  const auto session_mail = stream::session::mail_for_tests(*session);
  // The old loop needs this independent stop to return; it must not supply the
  // new failure reason or budget. Both paths exercise the real encode loop.
  const auto display = std::make_shared<SdrDisplay>([&session](int built) {
    if (built == 6) stream::session::stop(*session);
  });
  auto config = pyrowave_stream(false);
  config.width = config.height = 0;
  const auto before = std::chrono::steady_clock::now();
  video::encode_published_display_for_tests(session_mail, config, display,
                                           stream::session::packet_destination_for_tests(*session));
  EXPECT_EQ(display->sessions, 5);
  EXPECT_GE(std::chrono::steady_clock::now() - before, 1500ms)
    << "initialization failures still rebuild in a tight loop";
  EXPECT_TRUE(session_mail->event<bool>(mail::frame_conversion_failed)->peek());
  EXPECT_TRUE(session_mail->event<bool>(mail::shutdown)->peek());
  stream::session::stop(*session);
  EXPECT_EQ(stream::session::control_termination_code_for_tests(*session),
            std::optional<std::uint32_t> {0x800e9403u});
}

TEST(StreamControlTerminationTests, CancellationDuringEncoderInitializationBackoffKeepsItsOwnTerminationReason) {
  for (const bool graceful : {false, true}) {
    SCOPED_TRACE(graceful ? "graceful stop" : "plain stop");
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    const auto session_mail = stream::session::mail_for_tests(*session);
    std::future<void> cancellation;
    const auto display = std::make_shared<SdrDisplay>([&](int built) {
      if (built == 1) {
        cancellation = std::async(std::launch::async, [&] {
          std::this_thread::sleep_for(20ms);
          graceful ? stream::session::graceful_stop(*session) : stream::session::stop(*session);
        });
      }
      if (built == 6) stream::session::stop(*session);
    });
    auto config = pyrowave_stream(false);
    config.width = config.height = 0;
    video::encode_published_display_for_tests(session_mail, config, display,
                                             stream::session::packet_destination_for_tests(*session));
    ASSERT_TRUE(cancellation.valid());
    cancellation.get();
    EXPECT_EQ(display->sessions, 1) << "shutdown during backoff started another encoder";
    EXPECT_FALSE(session_mail->event<bool>(mail::frame_conversion_failed)->peek());
    stream::session::stop(*session);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*session),
              graceful ? std::optional<std::uint32_t> {0x80030023u} : std::nullopt);

    auto opening = make_session();
    stream::session::set_state_for_tests(*opening, stream::session::state_e::RUNNING);
    const auto opening_mail = stream::session::mail_for_tests(*opening);
    const auto destination = stream::session::packet_destination_for_tests(*opening);
    int attempts = 0;
    video::reset_capture_display_for_tests(opening_mail, gamescope_hdr_stream(), destination,
      [&](platf::mem_type_e, const std::string &, const video::config_t &requested, platf::capture_failure_t &failure) {
        ++attempts;
        failure = {platf::capture_failure_e::unsupported_hdr_producer_format, requested.capture_generation, destination.capture_owner_tag()};
        graceful ? stream::session::graceful_stop(*opening) : stream::session::stop(*opening);
        return std::shared_ptr<platf::display_t> {};
      });
    EXPECT_EQ(attempts, 1);
    EXPECT_FALSE(opening_mail->event<bool>(mail::frame_conversion_failed)->peek());
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*opening),
              graceful ? std::optional<std::uint32_t> {0x80030023u} : std::nullopt);
  }
  for (const bool replaced_generation : {false, true}) {
    auto current = make_session(), foreign = make_session();
    stream::session::set_state_for_tests(*current, stream::session::state_e::RUNNING);
    const auto current_mail = stream::session::mail_for_tests(*current);
    const auto foreign_mail = stream::session::mail_for_tests(*foreign);
    const auto destination = stream::session::packet_destination_for_tests(*current);
    video::reset_capture_display_for_tests(current_mail, gamescope_hdr_stream(), destination,
      [&](platf::mem_type_e, const std::string &, const video::config_t &requested, platf::capture_failure_t &failure) {
        failure = {platf::capture_failure_e::unsupported_hdr_producer_format, requested.capture_generation,
          replaced_generation ? destination.capture_owner_tag() : stream::session::packet_destination_for_tests(*foreign).capture_owner_tag()};
        if (replaced_generation) --failure.generation.generation_id;
        return std::shared_ptr<platf::display_t> {};
      });
    EXPECT_FALSE(current_mail->event<bool>(mail::frame_conversion_failed)->peek());
    EXPECT_FALSE(current_mail->event<bool>(mail::shutdown)->peek());
    EXPECT_FALSE(foreign_mail->event<bool>(mail::frame_conversion_failed)->peek());
    stream::session::stop(*current);
    EXPECT_EQ(stream::session::control_termination_code_for_tests(*current), std::nullopt);
  }
  // A cancelled owner at the front of a shared capture leaves it to the SDR stream behind it, at the
  // first open, a reopen or a capture, and keeps its own termination reason.
  for (const auto graceful : {false, true}) {
    for (const std::string_view site : {"open", "reset", "capture"}) {
      auto owner = make_session(), sdr = make_session();
      stream::session::set_state_for_tests(*owner, stream::session::state_e::RUNNING);
      stream::session::set_state_for_tests(*sdr, stream::session::state_e::RUNNING);
      const auto owner_mail = stream::session::mail_for_tests(*owner);
      const auto sdr_mail = stream::session::mail_for_tests(*sdr);
      const auto owner_tag = stream::session::packet_destination_for_tests(*owner).capture_owner_tag();
      const auto [owner_opens, served] = serve_sdr_after_hdr_owner_ends(site, *owner, *sdr,
        [&](const video::config_t &requested, platf::capture_failure_t &failure) {
          failure = {platf::capture_failure_e::unsupported_hdr_producer_format, requested.capture_generation, owner_tag};
          graceful ? stream::session::graceful_stop(*owner) : stream::session::stop(*owner);
        });
      EXPECT_EQ(owner_opens, site == "reset" ? 2 : 1) << site;
      EXPECT_TRUE(served) << site << ": the stream sharing the capture was ended or never reopened";
      EXPECT_FALSE(owner_mail->event<bool>(mail::frame_conversion_failed)->peek()) << site;
      EXPECT_FALSE(sdr_mail->event<bool>(mail::frame_conversion_failed)->peek()) << site;
      EXPECT_FALSE(sdr_mail->event<bool>(mail::shutdown)->peek()) << site;
      EXPECT_EQ(stream::session::control_termination_code_for_tests(*owner),
                graceful ? std::optional<std::uint32_t> {0x80030023u} : std::nullopt) << site;
    }
  }
}
#endif

TEST(StreamPacketSessionTests, StopRequestCannotAcknowledgeControlThreadCompletion) {
  for (const auto graceful : {false, true}) {
    auto session = make_session();
    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    EXPECT_FALSE(stream::session::control_ended_for_tests(*session));
    if (graceful) {
      stream::session::graceful_stop(*session);
    } else {
      stream::session::stop(*session);
    }
    EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPING);
    // The control thread can still hold this session while decrypting a
    // packet. Only that thread may acknowledge its final use.
    EXPECT_FALSE(stream::session::control_ended_for_tests(*session));
  }
}

namespace {
  // packetTypes[IDX_TERMINATION], which moonlight-common-c reads as the host ending the stream.
  constexpr std::uint16_t termination_type = 0x0109;
  // NVST_DISCONN_SERVER_TERMINATED_CLOSED, which moonlight-common-c reports as
  // ML_ERROR_GRACEFUL_TERMINATION once the stream has shown a frame.
  constexpr std::uint32_t graceful_termination = 0x80030023u;

  /**
   * The first thing a Moonlight client's control stream reads as its stream ends: the host's
   * termination, or ENet's disconnect, which moonlight-common-c reports as "Control stream received
   * unexpected disconnect event" (error -1).
   */
  struct client_end_t {
    std::optional<std::uint32_t> termination;
    bool unexpected_disconnect = false;
  };

  /**
   * moonlight-common-c's ignoreDisconnectIntercept: it holds back a disconnect that arrives on its
   * own so pending receives are read first. It recognises only a disconnect of this exact size.
   */
  int ignore_disconnect_intercept(ENetHost *host, ENetEvent *event) {
    if (host->receivedDataLength == sizeof(ENetProtocolHeader) + sizeof(ENetProtocolDisconnect)) {
      const auto *disconnect = reinterpret_cast<const ENetProtocolDisconnect *>(host->receivedData + sizeof(ENetProtocolHeader));
      if ((disconnect->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT) {
        if (event) {
          event->type = ENET_EVENT_TYPE_NONE;
        }
        return 1;
      }
    }
    return 0;
  }

  /// A loopback client set up the way moonlight-common-c sets up its control stream.
  class moonlight_control_client_t {
  public:
    explicit moonlight_control_client_t(std::uint16_t port):
        host {enet_host_create(AF_INET, nullptr, 1, 0x30, 0, 0)} {
      if (!host) {
        return;
      }
      host->intercept = ignore_disconnect_intercept;
      ENetAddress address {};
      enet_address_set_host(&address, "127.0.0.1");
      enet_address_set_port(&address, port);
      peer = enet_host_connect(host, &address, 0x30, 0);
    }

    ~moonlight_control_client_t() {
      if (host) {
        enet_host_destroy(host);
      }
    }

    moonlight_control_client_t(const moonlight_control_client_t &) = delete;
    moonlight_control_client_t &operator=(const moonlight_control_client_t &) = delete;

    /// Connects, through the host's own control handling, to the session waiting for it.
    bool connect(stream::session::control_wire_for_tests_t &wire, const stream::session_t &session) {
      if (!peer) {
        return false;
      }
      bool connected = false;
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (std::chrono::steady_clock::now() < deadline && !(connected && stream::session::control_connected_for_tests(session))) {
        wire.service(1ms);
        ENetEvent event;
        if (enet_host_service(host, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
          connected = true;
        }
      }
      return connected && stream::session::control_connected_for_tests(session);
    }

    /// A reliable message on the control channel, as the client's periodic ping is.
    void send_reliable() {
      const std::uint8_t ping[] {0x00, 0x02, 0x04, 0x00, 0, 0, 0, 0};
      enet_peer_send(peer, 0, enet_packet_create(ping, sizeof(ping), ENET_PACKET_FLAG_RELIABLE));
      enet_host_flush(host);
    }

    /**
     * Reads the control stream as moonlight-common-c's receive thread does, until whatever ends it.
     *
     * The thread is woken by the host's first datagram and reads the socket a moment later, by
     * which time the host's next datagram has arrived too: on loopback, as on a LAN, they are sent
     * microseconds apart. The wait makes that the case every time.
     */
    client_end_t read_end(stream::session::control_wire_for_tests_t &wire) {
      std::this_thread::sleep_for(50ms);
      client_end_t end;
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (std::chrono::steady_clock::now() < deadline) {
        ENetEvent event;
        if (enet_host_service(host, &event, 0) > 0) {
          if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            end.unexpected_disconnect = true;
            return end;
          }
          if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            const auto *data = event.packet->data;
            const auto size = event.packet->dataLength;
            // The host's control header is a little-endian type and length; the code is big-endian.
            if (size >= 8 && (data[0] | data[1] << 8) == termination_type) {
              end.termination = std::uint32_t {data[4]} << 24 | std::uint32_t {data[5]} << 16 |
                                std::uint32_t {data[6]} << 8 | std::uint32_t {data[7]};
              enet_packet_destroy(event.packet);
              // What moonlight-common-c does next: it marks its peer gone at once and stops reading.
              enet_peer_disconnect_now(peer, 0);
              return end;
            }
            enet_packet_destroy(event.packet);
          }
        }
        wire.service(1ms);
      }
      return end;
    }

  private:
    ENetHost *host = nullptr;
    ENetPeer *peer = nullptr;
  };

  /// Services the host until its retiring peers have gone, or the deadline passes.
  bool wait_for_no_retiring_peers(stream::session::control_wire_for_tests_t &wire, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (wire.retiring_peers() > 0 && std::chrono::steady_clock::now() < deadline) {
      wire.service(1ms);
    }
    return wire.retiring_peers() == 0;
  }
}  // namespace

/**
 * A stream the host ends because its app exited reaches the client as a graceful termination.
 *
 * The control thread sent the termination and disconnected the peer in the same turn. Both
 * datagrams reached the RP6 in one receive batch, ENet's handling of the disconnect discarded the
 * termination the client had not read yet, and moonlight-common-c logged "Control stream received
 * unexpected disconnect event" (error -1) on every host-side End (#568). Nova took -1 for a dropped
 * connection, found the host still reporting a stream and relaunched the app the host had just ended.
 */
TEST(StreamControlTerminationTests, AStreamTheHostEndsBecauseItsAppExitedReachesTheClientAsAGracefulTermination) {
  stream::session::control_wire_for_tests_t wire;
  ASSERT_NE(wire.port(), 0) << "the loopback control server could not bind";
  auto session = make_session();
  wire.publish(*session);
  moonlight_control_client_t client {wire.port()};
  ASSERT_TRUE(client.connect(wire, *session)) << "the client never claimed its session's control stream";
  stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);

  // The control thread's turn after the host's app has exited.
  ASSERT_TRUE(wire.turn(*session, false)) << "a stream whose app exited was not ended";
  EXPECT_TRUE(stream::session::control_ended_for_tests(*session)) << "the session's cleanup waits for its client";

  const auto end = client.read_end(wire);
  EXPECT_FALSE(end.unexpected_disconnect)
    << "the client read a disconnect before the termination, the -1 Nova reconnects and relaunches after";
  EXPECT_EQ(end.termination, std::optional<std::uint32_t> {graceful_termination})
    << "the client never read that the host ended the stream on purpose";
  EXPECT_TRUE(wait_for_no_retiring_peers(wire, 200ms))
    << "the host kept the connection after the client had read its termination and left";
}

/**
 * A stop that only disconnects, as when the client leaves or stops answering, still closes the
 * connection at once and says nothing more.
 */
TEST(StreamControlTerminationTests, AStopThatOnlyDisconnectsStillClosesTheConnectionAtOnce) {
  stream::session::control_wire_for_tests_t wire;
  ASSERT_NE(wire.port(), 0) << "the loopback control server could not bind";
  auto session = make_session();
  wire.publish(*session);
  moonlight_control_client_t client {wire.port()};
  ASSERT_TRUE(client.connect(wire, *session)) << "the client never claimed its session's control stream";
  stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);

  stream::session::stop(*session);
  ASSERT_TRUE(wire.turn(*session, true));
  EXPECT_EQ(wire.retiring_peers(), 0u) << "a stop with nothing to tell the client now keeps its connection";

  const auto end = client.read_end(wire);
  EXPECT_TRUE(end.unexpected_disconnect) << "the client was not disconnected";
  EXPECT_EQ(end.termination, std::nullopt) << "a stop nobody asked to be graceful now sends a termination";
}

/**
 * A client that never reads its termination is dropped at the linger deadline, so a host-side End
 * cannot wait on a client that stopped answering (#424 keeps every End bounded).
 */
TEST(StreamControlTerminationTests, AClientThatNeverReadsItsTerminationIsDroppedAtTheDeadline) {
  stream::session::control_wire_for_tests_t wire;
  ASSERT_NE(wire.port(), 0) << "the loopback control server could not bind";
  auto session = make_session();
  wire.publish(*session);
  moonlight_control_client_t client {wire.port()};
  ASSERT_TRUE(client.connect(wire, *session)) << "the client never claimed its session's control stream";
  stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);

  const auto ended = std::chrono::steady_clock::now();
  ASSERT_TRUE(wire.turn(*session, false));
  EXPECT_TRUE(stream::session::control_ended_for_tests(*session)) << "the session's cleanup waits for its client";
  EXPECT_EQ(wire.retiring_peers(), 1u) << "the host closed the connection before the client could read its termination";

  // The client never services its connection again, so it acknowledges nothing.
  EXPECT_TRUE(wait_for_no_retiring_peers(wire, 2s)) << "a client that never answers keeps its connection";
  const auto held = std::chrono::steady_clock::now() - ended;
  EXPECT_GE(held, 450ms) << "the client was dropped before its deadline";
  EXPECT_LT(held, 1500ms) << "the connection outlived its deadline";
}

/**
 * Traffic still arriving from a connection the host has told its stream ended cannot claim another
 * session from the same address, such as the one a reconnecting client is about to connect to.
 */
TEST(StreamControlTerminationTests, AnEndedConnectionCannotClaimTheNextSessionFromItsAddress) {
  stream::session::control_wire_for_tests_t wire;
  ASSERT_NE(wire.port(), 0) << "the loopback control server could not bind";
  auto ended = make_session();
  wire.publish(*ended);
  moonlight_control_client_t client {wire.port()};
  ASSERT_TRUE(client.connect(wire, *ended)) << "the client never claimed its session's control stream";
  stream::session::set_state_for_tests(*ended, stream::session::state_e::RUNNING);
  ASSERT_TRUE(wire.turn(*ended, false));

  auto next = make_session();
  wire.publish(*next);
  // A ping the client sent before it read its termination.
  client.send_reliable();
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (std::chrono::steady_clock::now() < deadline) {
    wire.service(1ms);
  }
  EXPECT_FALSE(stream::session::control_connected_for_tests(*next))
    << "the ended connection's traffic claimed the next session from its address";
}
