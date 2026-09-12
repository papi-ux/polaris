/**
 * Private physical acceptance consumer. Decodes the actual authenticated worker
 * packets; it never creates a substitute capture source or a network listener.
 */
#pragma once

#include "src/platform/linux/multiseat_worker_media_pump.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <opus/opus.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace multiseat::physical {
  class media_observer_t {
  public:
    explicit media_observer_t(worker_ipc::controller_connection_t connection):
        thread_([this, connection](std::stop_token stop) {
      try {
        decode(connection, stop);
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
    }

    std::atomic_uint64_t video {0}, audio {0}, idrs {0}, motion {0}, audible {0};
    std::atomic_bool request_idr {false}, done {false};
    // Read these only after stop() has joined the consumer.
    media::pump_report_t report;
    std::string error;
    std::uint64_t submitted_video = 0;

  private:
    void decode(const worker_ipc::controller_connection_t &connection, std::stop_token stop) {
      using namespace std::chrono_literals;
      const auto *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
      if (!codec) throw std::runtime_error {"physical H.264 decoder unavailable"};
      auto delete_context = [](AVCodecContext *context) { avcodec_free_context(&context); };
      std::unique_ptr<AVCodecContext, decltype(delete_context)> context {avcodec_alloc_context3(codec)};
      if (!context) throw std::runtime_error {"physical decoder allocation failed"};
      context->thread_count = 1;
      context->err_recognition = AV_EF_EXPLODE;
      if (avcodec_open2(context.get(), codec, nullptr) < 0)
        throw std::runtime_error {"physical decoder initialization failed"};
      auto delete_frame = [](AVFrame *frame) { av_frame_free(&frame); };
      auto delete_packet = [](AVPacket *packet) { av_packet_free(&packet); };
      std::unique_ptr<AVFrame, decltype(delete_frame)> frame {av_frame_alloc()};
      std::unique_ptr<AVPacket, decltype(delete_packet)> packet {av_packet_alloc()};
      int opus_error = 0;
      std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> opus {
        opus_decoder_create(48000, 2, &opus_error), opus_decoder_destroy};
      if (!frame || !packet || !opus || opus_error != OPUS_OK)
        throw std::runtime_error {"physical media allocation failed"};
      std::uint64_t previous_hash = 0;
      const auto receive = [&] {
        for (;;) {
          const auto result = avcodec_receive_frame(context.get(), frame.get());
          if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
          if (result < 0 || frame->decode_error_flags || frame->width != 1920 ||
              frame->height != 1080 || frame->format != AV_PIX_FMT_YUV420P)
            throw std::runtime_error {"worker H.264 did not decode to clean 1080p SDR frames"};
          // Sample decoded luma to prove the private game keeps changing. This
          // does not measure visual quality or client presentation latency.
          std::uint64_t hash = 14695981039346656037ULL;
          for (int y = 0; y < frame->height; y += 8)
            for (int x = 0; x < frame->width; x += 8)
              hash = (hash ^ frame->data[0][y * frame->linesize[0] + x]) * 1099511628211ULL;
          if (video.load() && hash != previous_hash) ++motion;
          previous_hash = hash;
          av_frame_unref(frame.get());
          ++video;
        }
      };
      const auto deadline = std::chrono::steady_clock::now() + 180s;
      report = media::run(connection, {.width=1920, .height=1080, .fps=60, .video_format=0, .audio_channels=2},
        {
          .video = [&](std::vector<std::uint8_t> &&bytes, std::int64_t, bool idr) {
            if (bytes.empty() || bytes.size() > 4 * 1024 * 1024 ||
                av_new_packet(packet.get(), static_cast<int>(bytes.size())) < 0)
              throw std::runtime_error {"worker video packet exceeds decoder bound"};
            std::memcpy(packet->data, bytes.data(), bytes.size());
            if (idr) packet->flags |= AV_PKT_FLAG_KEY;
            const auto result = avcodec_send_packet(context.get(), packet.get());
            av_packet_unref(packet.get());
            if (result < 0) throw std::runtime_error {"worker H.264 packet refused by decoder"};
            ++submitted_video;
            receive();
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
      if (avcodec_send_packet(context.get(), nullptr) < 0)
        throw std::runtime_error {"worker H.264 decoder could not flush"};
      receive();
      if (submitted_video != video.load())
        throw std::runtime_error {"worker H.264 decoded frame count differs from packets"};
    }
    // Construct this last so every value accessed by the consumer already exists.
    std::jthread thread_;
  };
}
