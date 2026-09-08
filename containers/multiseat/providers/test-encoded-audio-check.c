#define main encoded_audio_main
#include "encoded-audio-check.c"
#undef main
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int scenario, poisoned, samples_seen;
static int packets_seen;
static int witness_fd = -1;
static void injected(void) {
  const char marker = 'I';
  if (write(witness_fd, &marker, 1) != 1) _exit(93);
}
static GstPadProbeReturn corrupt_payload(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  if (++packets_seen == 50) {
    /* Valid single-frame 5 ms TOC and accepted wire size, but the compressed
     * frame exceeds Opus's 1275-byte limit. Only the real decoder rejects it. */
    GstBuffer *replacement = gst_buffer_new_allocate(NULL, AUDIO_MAX_PACKET, NULL);
    gst_buffer_memset(replacement, 0, 0, AUDIO_MAX_PACKET);
    const guint8 toc = 17 << 3;
    gst_buffer_fill(replacement, 0, &toc, 1);
    GstBuffer *original = GST_PAD_PROBE_INFO_BUFFER(info);
    gst_buffer_copy_into(replacement, original, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    gst_buffer_unref(original);
    GST_PAD_PROBE_INFO_DATA(info) = replacement;
    injected();
  }
  return encoded_packet(pad, info, opaque);
}
gulong __real_gst_pad_add_probe(GstPad *, GstPadProbeType, GstPadProbeCallback, gpointer, GDestroyNotify);
gulong __wrap_gst_pad_add_probe(GstPad *pad, GstPadProbeType type, GstPadProbeCallback callback, gpointer data, GDestroyNotify destroy) {
  if (scenario == 4 && callback == encoded_packet) callback = corrupt_payload;
  return __real_gst_pad_add_probe(pad, type, callback, data, destroy);
}
GstStateChangeReturn __real_gst_element_set_state(GstElement *, GstState);
GstStateChangeReturn __wrap_gst_element_set_state(GstElement *element, GstState state) {
  if (state == GST_STATE_NULL && (scenario == 1 || scenario == 2)) {
    poisoned = 1;
    injected();
    return scenario == 1 ? GST_STATE_CHANGE_FAILURE : GST_STATE_CHANGE_ASYNC;
  }
  return __real_gst_element_set_state(element, state);
}
void __real_gst_object_unref(gpointer);
void __wrap_gst_object_unref(gpointer object) {
  if (poisoned) _exit(90);
  __real_gst_object_unref(object);
}
void __real_gst_pad_remove_probe(GstPad *, gulong);
void __wrap_gst_pad_remove_probe(GstPad *pad, gulong id) {
  if (poisoned) _exit(91);
  __real_gst_pad_remove_probe(pad, id);
}
void __real_g_mutex_clear(GMutex *);
void __wrap_g_mutex_clear(GMutex *lock) {
  if (poisoned) _exit(92);
  __real_g_mutex_clear(lock);
}
GstSample *__real_gst_app_sink_try_pull_sample(GstAppSink *, GstClockTime);
GstSample *__wrap_gst_app_sink_try_pull_sample(GstAppSink *sink, GstClockTime timeout) {
  GstSample *sample = __real_gst_app_sink_try_pull_sample(sink, timeout);
  if (sample && scenario == 3 && ++samples_seen == 50) {
    GstCaps *caps = gst_caps_copy(gst_sample_get_caps(sample));
    gst_caps_set_simple(caps, "channels", G_TYPE_INT, 1, NULL);
    GstSample *changed = gst_sample_new(gst_sample_get_buffer(sample), caps, gst_sample_get_segment(sample), NULL);
    gst_caps_unref(caps); gst_sample_unref(sample);
    injected(); return changed;
  }
  return sample;
}

static void packet_case(size_t size, guint8 toc, gboolean accepted) {
  struct audio_stats stats = {0}; g_mutex_init(&stats.lock);
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
  if (size) gst_buffer_fill(buffer, 0, &toc, 1);
  GstPadProbeInfo info = {.type = GST_PAD_PROBE_TYPE_BUFFER, .data = buffer};
  GstPadProbeReturn result = encoded_packet(NULL, &info, &stats);
  g_assert_cmpint(result, ==, accepted ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP);
  g_assert_cmpint(stats.failed, ==, !accepted);
  g_assert_cmpuint(stats.packets, ==, accepted ? 1 : 0);
  gst_buffer_unref(buffer); g_mutex_clear(&stats.lock);
}

static int listener(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  g_assert_cmpint(fd, >=, 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  g_assert_cmpuint(strlen(path), <, sizeof(address.sun_path));
  strcpy(address.sun_path, path);
  g_assert_cmpint(bind(fd, (struct sockaddr *)&address, sizeof(address)), ==, 0);
  g_assert_cmpint(listen(fd, 1), ==, 0);
  return fd;
}

static void retained_pulse_socket(void) {
  g_assert_cmpint(mkdir("/run/polaris", 0700), ==, 0);
  g_assert_cmpint(mkdir("/run/polaris/pulse", 0700), ==, 0);
  int original = listener("/run/polaris/pulse/native");
  g_assert_cmpint(chmod("/run/polaris/pulse/native", 0777), ==, 0);
  struct pulse_identity pinned = {.root = -1, .directory = -1, .socket = -1};
  g_assert_true(pin_pulse(&pinned));
  g_assert_true(verify_pulse(&pinned));
  g_assert_cmpint(rename("/run/polaris/pulse/native", "/run/polaris/pulse/retired"), ==, 0);
  int replacement = listener("/run/polaris/pulse/native");
  g_assert_false(verify_pulse(&pinned));
  int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(client, >=, 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof(address.sun_path), "/proc/self/fd/%d", pinned.socket);
  g_assert_cmpint(connect(client, (struct sockaddr *)&address, sizeof(address)), ==, 0);
  int connection = accept4(original, NULL, NULL, SOCK_CLOEXEC);
  g_assert_cmpint(connection, >=, 0);
  g_assert_cmpint(accept4(replacement, NULL, NULL, SOCK_CLOEXEC), ==, -1);
  g_assert_cmpint(errno, ==, EAGAIN);
  close(connection); close(client); close(replacement);
  g_assert_cmpint(unlink("/run/polaris/pulse/native"), ==, 0);
  g_assert_cmpint(rename("/run/polaris/pulse/retired", "/run/polaris/pulse/native"), ==, 0);
  g_assert_true(verify_pulse(&pinned));
  release_pulse(&pinned); close(original);
  g_assert_cmpint(chmod("/run/polaris/pulse", 0770), ==, 0);
  pinned = (struct pulse_identity){.root = -1, .directory = -1, .socket = -1};
  g_assert_false(pin_pulse(&pinned)); release_pulse(&pinned);
  g_assert_cmpint(chmod("/run/polaris/pulse", 0700), ==, 0);
  g_assert_cmpint(unlink("/run/polaris/pulse/native"), ==, 0);
  g_assert_cmpint(symlink("retired", "/run/polaris/pulse/native"), ==, 0);
  pinned = (struct pulse_identity){.root = -1, .directory = -1, .socket = -1};
  g_assert_false(pin_pulse(&pinned)); release_pulse(&pinned);
  g_assert_cmpint(unlink("/run/polaris/pulse/native"), ==, 0);
  g_assert_cmpint(rmdir("/run/polaris/pulse"), ==, 0);
  g_assert_cmpint(rmdir("/run/polaris"), ==, 0);
}

int main(void) {
  /* Fork before this parent initializes GStreamer or creates any GL/audio
   * threads. Each negative exercises the actual codec/main in isolation. */
  for (scenario = 1; scenario <= 4; ++scenario) {
    int witness[2]; g_assert_cmpint(pipe2(witness, O_CLOEXEC), ==, 0);
    pid_t child = fork(); g_assert_cmpint(child, >=, 0);
    if (!child) {
      close(witness[0]); witness_fd = witness[1];
      char *arguments[] = {"encoded-audio-check", "--self-test", NULL};
      _exit(encoded_audio_main(2, arguments));
    }
    close(witness[1]);
    int status;
    g_assert_cmpint(waitpid(child, &status, 0), ==, child);
    g_assert_true(WIFEXITED(status)); g_assert_cmpint(WEXITSTATUS(status), ==, 1);
    char marker = 0;
    g_assert_cmpint(read(witness[0], &marker, 1), ==, 1);
    g_assert_cmpint(marker, ==, 'I');
    g_assert_cmpint(read(witness[0], &marker, 1), ==, 0);
    close(witness[0]);
  }
  scenario = 0;
  gst_init(NULL, NULL);
  const guint8 five_ms = 17 << 3;
  packet_case(0, five_ms, FALSE); packet_case(1, five_ms, FALSE);
  packet_case(AUDIO_MAX_PACKET + 1, five_ms, FALSE);
  packet_case(2, five_ms | 3, FALSE); packet_case(2, 0, FALSE);
  packet_case(2, 16 << 3, FALSE); packet_case(2, 18 << 3, FALSE); packet_case(2, 19 << 3, FALSE);
  packet_case(2, five_ms, TRUE); packet_case(AUDIO_MAX_PACKET, five_ms | 4, TRUE);
  retained_pulse_socket();
  gst_deinit();
  puts("Opus payload/packet, decoded-format, quiescence and retained Pulse socket regressions passed");
  return 0;
}
