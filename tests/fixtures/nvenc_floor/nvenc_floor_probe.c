// Opens h264_nvenc against whichever prepared FFmpeg this is linked to, and says what
// happened. Used to measure the minimum NVIDIA driver a prepared archive demands, by running
// it against a stub libnvidia-encode that reports a chosen NVENC API version.
#include <stdio.h>
#include <libavcodec/avcodec.h>

int main(void) {
  av_log_set_level(AV_LOG_ERROR);

  const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
  if (!codec) {
    printf("RESULT no-encoder\n");
    return 2;
  }

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  ctx->width = 640;
  ctx->height = 480;
  ctx->time_base = (AVRational) {1, 60};
  ctx->framerate = (AVRational) {60, 1};
  ctx->pix_fmt = AV_PIX_FMT_NV12;

  int rc = avcodec_open2(ctx, codec, NULL);
  printf("RESULT %s rc=%d\n", rc == 0 ? "opened" : "failed", rc);
  avcodec_free_context(&ctx);
  return rc == 0 ? 0 : 1;
}
