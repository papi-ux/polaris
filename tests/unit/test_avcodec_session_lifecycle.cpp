/** @file tests/unit/test_avcodec_session_lifecycle.cpp
 * Real session submission and destruction with a bounded codec-call fixture.
 */
#include "src/video.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <utility>
#include <vector>

namespace {
  struct codec_calls_t {
    AVCodecContext *context = nullptr;
    std::vector<int> submissions;
    std::size_t submitted = 0;
    int flush_status = 0;
    int flushes = 0;
    int receives = 0;
    int drain_packets = 2;
    std::vector<std::string> retirement;
  };
  thread_local codec_calls_t *calls = nullptr;

  struct frame_device_t: platf::avcodec_encode_device_t {
    codec_calls_t &observed;
    explicit frame_device_t(codec_calls_t &observed): observed(observed) {
      frame = av_frame_alloc();
    }
    ~frame_device_t() override {
      observed.retirement.push_back("converter");
      av_frame_free(&frame);
    }
  };

  class AvcodecSessionLifecycle: public testing::Test {
  protected:
    codec_calls_t observed;
    void SetUp() override {
      ASSERT_EQ(calls, nullptr);
      calls = &observed;
    }
    void TearDown() override {
      calls = nullptr;
    }
    void exercise(std::vector<int> submissions = {}) {
      observed.submissions = std::move(submissions);
      // Finding/allocating a codec does not open it or touch a GPU. Prefer the
      // reported codec so the Vulkan-specific diagnostic branch is covered.
      const auto *codec = avcodec_find_encoder_by_name("h264_vulkan");
      video::avcodec_ctx_t context {avcodec_alloc_context3(codec)};
      ASSERT_NE(context, nullptr);
      observed.context = context.get();
      auto device = std::make_unique<frame_device_t>(observed);
      ASSERT_NE(device->frame, nullptr);
      const auto results = video::encode_and_destroy_avcodec_session_for_tests(
        std::move(context), std::move(device), observed.submissions.size()
      );
      ASSERT_EQ(results.size(), observed.submissions.size());
      for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i], observed.submissions[i] < 0 ? -1 : 0);
      }
      EXPECT_EQ(observed.retirement, (std::vector<std::string> {"codec", "converter"}));
      EXPECT_EQ(observed.submitted, observed.submissions.size());
    }
  };
}

extern "C" {
  int __real_avcodec_send_frame(AVCodecContext *, const AVFrame *);
  int __real_avcodec_receive_packet(AVCodecContext *, AVPacket *);
  void __real_avcodec_free_context(AVCodecContext **);

  int __wrap_avcodec_send_frame(AVCodecContext *context, const AVFrame *frame) {
    if (!calls || calls->context != context) return __real_avcodec_send_frame(context, frame);
    if (!frame) {
      ++calls->flushes;
      return calls->flush_status;
    }
    if (calls->submitted >= calls->submissions.size()) {
      ADD_FAILURE() << "Unexpected frame submission";
      return AVERROR(EINVAL);
    }
    return calls->submissions[calls->submitted++];
  }

  int __wrap_avcodec_receive_packet(AVCodecContext *context, AVPacket *packet) {
    if (!calls || calls->context != context) return __real_avcodec_receive_packet(context, packet);
    ++calls->receives;
    if (!calls->flushes) return AVERROR(EAGAIN);
    return calls->drain_packets-- > 0 ? 0 : AVERROR_EOF;
  }

  void __wrap_avcodec_free_context(AVCodecContext **context) {
    if (calls && context && *context == calls->context) {
      calls->retirement.push_back("codec");
    }
    __real_avcodec_free_context(context);
  }
}

TEST_F(AvcodecSessionLifecycle, UnusedCapabilityProbeNeverFlushesOrReceives) {
  exercise();
  EXPECT_EQ(observed.flushes, 0);
  EXPECT_EQ(observed.receives, 0);
}

TEST_F(AvcodecSessionLifecycle, RejectedFirstFramesNeverWarmTheCodec) {
  exercise({AVERROR(EAGAIN), AVERROR(EINVAL)});
  EXPECT_EQ(observed.flushes, 0);
  EXPECT_EQ(observed.receives, 0);
}

TEST_F(AvcodecSessionLifecycle, AcceptedFrameStillDrainsAfterALaterRejection) {
  exercise({0, AVERROR(EAGAIN)});
  EXPECT_EQ(observed.flushes, 1);
  EXPECT_EQ(observed.receives, 4); // One encode receive, two packets, then EOF.
}

TEST_F(AvcodecSessionLifecycle, FailedFlushStillClosesCodecBeforeConverter) {
  observed.flush_status = AVERROR(EIO);
  exercise({0});
  EXPECT_EQ(observed.flushes, 1);
  EXPECT_EQ(observed.receives, 1); // No drain read after the failed flush.
}
