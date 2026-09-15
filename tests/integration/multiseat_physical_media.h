/**
 * Private physical acceptance consumer of authenticated worker packets.
 * Video is retained within a fixed bound and decoded with system OpenH264 at
 * stop; the prepared host FFmpeg library intentionally omits H.264 decoding.
 */
#pragma once

#include "src/platform/linux/multiseat_worker_media_pump.h"
#include "src/platform/linux/multiseat_container_host.h"
#include "src/utility.h"

#include <opus/opus.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace multiseat::physical {
  class media_observer_t {
  public:
    explicit media_observer_t(worker_ipc::controller_connection_t connection):
        thread_([this, connection](std::stop_token stop) {
      try {
        consume(connection, stop);
      } catch (const std::exception &failure) {
        error = failure.what();
        connection.close();
      }
      done = true;
    }) {}
    ~media_observer_t() { stop(); }
    void stop() {
      thread_.request_stop();
      if (thread_.joinable()) thread_.join();
      if (!decoded_) {
        decoded_ = true;
        if (error.empty() && video.load()) {
          try { decode_video(); }
          catch (const std::exception &failure) { error = failure.what(); }
        }
      }
    }

    // Video counts packets while running. decoded_video is final proof at stop.
    std::atomic_uint64_t video {0}, audio {0}, idrs {0}, audible {0};
    std::atomic_bool request_idr {false}, done {false};
    // Read these only after stop() has joined the consumer and decoder.
    media::pump_report_t report;
    std::string error;
    std::uint64_t decoded_video = 0, motion = 0;

  private:
    void decode_video() {
      char path[] = "/tmp/polaris-physical-video-XXXXXX";
      const int descriptor = mkostemp(path, O_CLOEXEC);
      if (descriptor < 0) throw std::runtime_error {"physical decoder file unavailable"};
      auto cleanup = util::fail_guard([&] { close(descriptor); unlink(path); });
      std::size_t offset = 0;
      while (offset < encoded_.size()) {
        const auto written = write(descriptor, encoded_.data() + offset, encoded_.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error {"physical decoder file write failed"};
        offset += static_cast<std::size_t>(written);
      }
      container::local_host_t host;
      const auto result = host.run({"/usr/bin/python3",
        std::string {POLARIS_SOURCE_DIR} + "/tests/integration/multiseat_decode_media.py",
        path, std::to_string(video.load())}, std::chrono::seconds {25}, 1024);
      if (result.exit_status != 0 || result.timed_out || result.output_truncated)
        throw std::runtime_error {"worker H.264 failed full OpenH264 decoding or frame count"};
      const auto decoded = nlohmann::json::parse(result.output);
      decoded_video = decoded.at("decoded_frames").get<std::uint64_t>();
      motion = decoded.at("changing_frames").get<std::uint64_t>();
      if (decoded_video != video.load())
        throw std::runtime_error {"worker H.264 decoded frame count differs from packets"};
      encoded_.clear();
    }
    void consume(const worker_ipc::controller_connection_t &connection, std::stop_token stop) {
      using namespace std::chrono_literals;
      int opus_error = 0;
      std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> opus {
        opus_decoder_create(48000, 2, &opus_error), opus_decoder_destroy};
      if (!opus || opus_error != OPUS_OK)
        throw std::runtime_error {"physical Opus decoder allocation failed"};
      const auto deadline = std::chrono::steady_clock::now() + 180s;
      report = media::run(connection, {.width=1920, .height=1080, .fps=60, .video_format=0, .audio_channels=2},
        {
          .video = [&](std::vector<std::uint8_t> &&bytes, std::int64_t, bool idr) {
            if (bytes.empty() || bytes.size() > 4 * 1024 * 1024 ||
                bytes.size() > 64 * 1024 * 1024 - encoded_.size())
              throw std::runtime_error {"physical encoded video exceeds receipt bound"};
            encoded_.insert(encoded_.end(), bytes.begin(), bytes.end());
            ++video;
            if (idr) ++idrs;
          },
          .audio = [&](std::vector<std::uint8_t> &&bytes) {
            std::array<opus_int16, 480> samples {};
            if (bytes.empty() || bytes.size() > 1400 ||
                opus_decode(opus.get(), bytes.data(), static_cast<opus_int32>(bytes.size()),
                  samples.data(), 240, 0) != 240)
              throw std::runtime_error {"worker Opus packet did not decode to 5 ms stereo"};
            if (std::any_of(samples.begin(), samples.end(),
                [](auto value) { return value > 8 || value < -8; })) ++audible;
            ++audio;
          },
        },
        {
          .stop_requested = [&] {
            if (std::chrono::steady_clock::now() >= deadline)
              throw std::runtime_error {"physical media lifetime expired"};
            return stop.stop_requested();
          },
          .take_idr_request = [&] { return request_idr.exchange(false); },
        });
    }
    bool decoded_ = false;
    std::vector<std::uint8_t> encoded_;
    // Construct this last so every value accessed by the consumer already exists.
    std::jthread thread_;
  };
}
