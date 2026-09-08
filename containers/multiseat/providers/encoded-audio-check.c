/* Bounded worker-local Opus acceptance. The fixed game produces a quiet 440 Hz
 * tone in its allocated sink. This does not establish client audio delivery. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUDIO_RATE 48000
#define AUDIO_PACKET_SAMPLES 240
#define AUDIO_MAX_PACKET 1400
static volatile sig_atomic_t stopping;
static void stop(int signal_number) { (void)signal_number; stopping = 1; }

struct audio_stats { GMutex lock; unsigned packets, maximum; gboolean failed; };
static gboolean audio_bus_error(GstBus *bus) {
  GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  if (!message) return FALSE;
  GError *error = NULL; gst_message_parse_error(message, &error, NULL);
  fprintf(stderr, "audio pipeline failed: %.200s\n", error ? error->message : "unknown");
  if (error) g_error_free(error);
  gst_message_unref(message); return TRUE;
}
static GstPadProbeReturn encoded_packet(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  (void)pad;
  struct audio_stats *stats = opaque;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  GstMapInfo mapped;
  gboolean valid = buffer && gst_buffer_get_size(buffer) >= 2 && gst_buffer_get_size(buffer) <= AUDIO_MAX_PACKET;
  unsigned size = 0;
  if (valid) {
    valid = gst_buffer_map(buffer, &mapped, GST_MAP_READ);
    if (valid) {
      size = mapped.size;
      /* RFC 6716 TOC: one CELT-only frame, with 5 ms duration. The decoder
       * below validates the full packet, including either stereo coding mode. */
      if (mapped.size < 2 || mapped.size > AUDIO_MAX_PACKET) valid = FALSE;
      else {
        const unsigned config = mapped.data[0] >> 3;
        valid = (mapped.data[0] & 3) == 0 && config >= 16 && (config & 3) == 1;
      }
      gst_buffer_unmap(buffer, &mapped);
    }
  }
  g_mutex_lock(&stats->lock);
  if (!valid || stats->packets >= 400) stats->failed = TRUE;
  else { ++stats->packets; if (size > stats->maximum) stats->maximum = size; }
  const gboolean failed = stats->failed;
  g_mutex_unlock(&stats->lock);
  return failed ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static gboolean valid_sink(const char *name) {
  const size_t length = strlen(name);
  if (!length || length > 128 || !g_ascii_isalnum(name[0])) return FALSE;
  for (const char *p = name; *p; ++p)
    if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return FALSE;
  return TRUE;
}

struct pulse_identity { int root, directory, socket; struct stat root_stat, directory_stat, socket_stat; };
static gboolean same_node(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode && a->st_uid == b->st_uid;
}
static gboolean private_directory(int fd, struct stat *status) {
  return fd >= 0 && !fstat(fd, status) && S_ISDIR(status->st_mode) &&
    status->st_uid == geteuid() && (status->st_mode & 07777) == 0700;
}
static gboolean pin_pulse(struct pulse_identity *identity) {
  identity->root = open("/run/polaris", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!private_directory(identity->root, &identity->root_stat)) return FALSE;
  identity->directory = openat(identity->root, "pulse", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!private_directory(identity->directory, &identity->directory_stat)) return FALSE;
  identity->socket = openat(identity->directory, "native", O_PATH | O_NOFOLLOW | O_CLOEXEC);
  return identity->socket >= 0 && !fstat(identity->socket, &identity->socket_stat) &&
    S_ISSOCK(identity->socket_stat.st_mode) && identity->socket_stat.st_uid == geteuid() &&
    !(identity->socket_stat.st_mode & 07000);
}
static gboolean verify_pulse(const struct pulse_identity *identity) {
  struct stat current;
  return !lstat("/run/polaris", &current) && same_node(&current, &identity->root_stat) &&
    !fstatat(identity->root, "pulse", &current, AT_SYMLINK_NOFOLLOW) && same_node(&current, &identity->directory_stat) &&
    !fstatat(identity->directory, "native", &current, AT_SYMLINK_NOFOLLOW) && same_node(&current, &identity->socket_stat);
}
static void release_pulse(struct pulse_identity *identity) {
  if (identity->socket >= 0) close(identity->socket);
  if (identity->directory >= 0) close(identity->directory);
  if (identity->root >= 0) close(identity->root);
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  const gboolean synthetic = !strcmp(argv[1], "--self-test") || !strcmp(argv[1], "--self-test-silence") || !strcmp(argv[1], "--self-test-wrong-tone");
  if (!synthetic && !valid_sink(argv[1])) return 1;
  struct pulse_identity pulse = {.root = -1, .directory = -1, .socket = -1};
  if (!synthetic && !pin_pulse(&pulse)) { release_pulse(&pulse); return 1; }
  struct sigaction action = {0}; action.sa_handler = stop;
  sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
  g_setenv("GST_REGISTRY_FORK", "no", TRUE);
  gst_init(NULL, NULL);
  const char *head = synthetic ? "audiotestsrc name=source samplesperbuffer=240 volume=0.015 freq=440 ! " : "pulsesrc name=source ! ";
  char *description = g_strconcat(head,
    "audioconvert ! audioresample ! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 ! "
    "opusenc audio-type=restricted-lowdelay bitrate=96000 bitrate-type=cbr frame-size=5 max-payload-size=1400 ! "
    "identity name=encoded ! opusdec max-errors=0 ! audioconvert ! audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2 ! "
    "appsink name=decoded max-buffers=2 drop=false sync=false", NULL);
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error); g_free(description);
  if (!pipeline || error) {
    fprintf(stderr, "audio pipeline unavailable: %.200s\n", error ? error->message : "unknown");
    if (error) g_error_free(error);
    if (pipeline) gst_object_unref(pipeline);
    release_pulse(&pulse); return 1;
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  char monitor[138] = {0};
  if (synthetic) {
    if (!strcmp(argv[1], "--self-test-silence")) g_object_set(source, "volume", 0.0, NULL);
    if (!strcmp(argv[1], "--self-test-wrong-tone")) g_object_set(source, "freq", 880.0, NULL);
  } else {
    char server[64]; snprintf(server, sizeof(server), "unix:/proc/self/fd/%d", pulse.socket);
    snprintf(monitor, sizeof(monitor), "%s.monitor", argv[1]);
    g_object_set(source, "server", server, "device", monitor, NULL);
  }
  GstElement *encoded = gst_bin_get_by_name(GST_BIN(pipeline), "encoded");
  GstPad *pad = gst_element_get_static_pad(encoded, "src");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "decoded");
  GstBus *bus = gst_element_get_bus(pipeline);
  /* Keep errors posted at the final-sample/NULL boundary until inspection. */
  gst_pipeline_set_auto_flush_bus(GST_PIPELINE(pipeline), FALSE);
  struct audio_stats stats = {0}; g_mutex_init(&stats.lock);
  gulong probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, encoded_packet, &stats, NULL);
  unsigned samples = 0, crossings[2] = {0};
  double energy[2] = {0}; float previous[2] = {0};
  gboolean failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
  const gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!failed && !stopping && g_get_monotonic_time() < deadline && samples < AUDIO_RATE) {
    if (audio_bus_error(bus)) { failed = TRUE; break; }
    g_mutex_lock(&stats.lock); failed = stats.failed; g_mutex_unlock(&stats.lock);
    if (failed) break;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
    if (!sample) { if (gst_app_sink_is_eos(GST_APP_SINK(sink))) failed = TRUE; continue; }
    GstAudioInfo info;
    GstMapInfo mapped;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    gboolean valid = gst_audio_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      GST_AUDIO_INFO_FORMAT(&info) == GST_AUDIO_FORMAT_F32LE && GST_AUDIO_INFO_LAYOUT(&info) == GST_AUDIO_LAYOUT_INTERLEAVED &&
      GST_AUDIO_INFO_RATE(&info) == AUDIO_RATE && GST_AUDIO_INFO_CHANNELS(&info) == 2 &&
      gst_buffer_get_size(buffer) > 0 && gst_buffer_get_size(buffer) <= AUDIO_PACKET_SAMPLES * 8 && gst_buffer_get_size(buffer) % 8 == 0;
    if (valid && gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
      valid = mapped.size > 0 && mapped.size <= AUDIO_PACKET_SAMPLES * 8 && mapped.size % 8 == 0;
      for (size_t i = 0; i < mapped.size / 8 && valid; ++i) for (unsigned channel = 0; channel < 2; ++channel) {
        guint32 bits = GST_READ_UINT32_LE(mapped.data + i * 8 + channel * 4);
        float value; memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value) || fabsf(value) > 1.0f) { valid = FALSE; break; }
        energy[channel] += (double)value * value;
        if (previous[channel] < 0 && value >= 0) ++crossings[channel];
        previous[channel] = value;
      }
      if (valid) samples += mapped.size / 8;
      gst_buffer_unmap(buffer, &mapped);
    } else valid = FALSE;
    gst_sample_unref(sample);
    if (!valid) failed = TRUE;
  }
  if (!synthetic) {
    char *current = NULL; g_object_get(source, "current-device", &current, NULL);
    if (!current || strcmp(current, monitor) || !verify_pulse(&pulse)) failed = TRUE;
    g_free(current);
  }
  if (gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_SUCCESS) {
    fprintf(stderr, "audio pipeline quiescence unproven\n"); _exit(1);
  }
  if (audio_bus_error(bus)) failed = TRUE;
  gst_bus_set_flushing(bus, TRUE);
  unsigned rms[2] = {0}, frequency[2] = {0};
  for (unsigned channel = 0; channel < 2 && samples; ++channel) {
    rms[channel] = sqrt(energy[channel] / samples) * 1000000;
    frequency[channel] = (guint64)crossings[channel] * AUDIO_RATE / samples;
    if (rms[channel] < 2000 || rms[channel] > 30000 || frequency[channel] < 430 || frequency[channel] > 450) failed = TRUE;
  }
  if (stopping || samples < AUDIO_RATE || samples >= AUDIO_RATE + AUDIO_PACKET_SAMPLES || stats.failed ||
      stats.packets < (samples + AUDIO_PACKET_SAMPLES - 1) / AUDIO_PACKET_SAMPLES ||
      stats.packets > (samples + AUDIO_PACKET_SAMPLES - 1) / AUDIO_PACKET_SAMPLES + 3 || !stats.maximum) failed = TRUE;
  printf("{\"source\":\"%s\",\"codec\":\"opus\",\"rate\":48000,\"channels\":2,\"packet_ms\":5,\"encoded_packets\":%u,\"decoded_samples\":%u,\"max_packet_bytes\":%u,\"left_hz\":%u,\"right_hz\":%u,\"left_rms_million\":%u,\"right_rms_million\":%u,\"passed\":%s}\n",
    synthetic ? "synthetic" : "worker-pulse-monitor", stats.packets, samples, stats.maximum, frequency[0], frequency[1], rms[0], rms[1], failed ? "false" : "true");
  gst_pad_remove_probe(pad, probe); gst_object_unref(pad); gst_object_unref(encoded); gst_object_unref(source);
  gst_object_unref(sink); gst_object_unref(bus); gst_object_unref(pipeline); g_mutex_clear(&stats.lock);
  release_pulse(&pulse); gst_deinit(); return failed ? 1 : 0;
}
