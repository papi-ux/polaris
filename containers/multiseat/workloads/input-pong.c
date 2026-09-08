/* An offline game for isolated stream acceptance. No network, profile mutation,
 * controller injection, or writable input descriptors. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <gst/gst.h>
#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int signal_number) { (void)signal_number; stopping = 1; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int dimension(const char *name) {
  const char *text = getenv(name); char *end = NULL;
  if (!text || !*text) return 0;
  long value = strtol(text, &end, 10);
  return *end || value < 64 || value > 16384 ? 0 : (int)value;
}

int main(int argc, char **argv) {
  if (argc != 1) return 1;
  const int width = dimension("POLARIS_DISPLAY_WIDTH"), height = dimension("POLARIS_DISPLAY_HEIGHT");
  if (!width || !height || !getenv("PULSE_SINK") || !getenv("PULSE_SERVER")) return 1;
  struct sigaction action = {0}; action.sa_handler = stop;
  sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
  int gamepad = open("/dev/input/polaris-gamepad-0", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (gamepad < 0 && errno != ENOENT) return 1;
  Display *display = XOpenDisplay(NULL);
  if (!display) return 1;
  const int screen = DefaultScreen(display);
  Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0, 0, 0x101827);
  XStoreName(display, window, "Polaris Input Pong");
  XSelectInput(display, window, KeyPressMask | KeyReleaseMask | PointerMotionMask | StructureNotifyMask);
  Atom close_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &close_window, 1);
  GC gc = XCreateGC(display, window, 0, NULL);
  XMapRaised(display, window); XFlush(display);
  GError *error = NULL;
  if (!gst_init_check(&argc, &argv, &error)) { if (error) g_error_free(error); XCloseDisplay(display); return 1; }
  GstElement *audio = gst_parse_launch("audiotestsrc name=tone is-live=true wave=sine freq=440 volume=0.015 ! audioconvert ! audioresample ! pulsesink name=output sync=true", &error);
  if (!audio || error) { if (error) g_error_free(error); if (audio) gst_object_unref(audio); XCloseDisplay(display); return 1; }
  GstElement *output = gst_bin_get_by_name(GST_BIN(audio), "output");
  g_object_set(output, "device", getenv("PULSE_SINK"), "server", getenv("PULSE_SERVER"), NULL);
  gst_object_unref(output);
  if (gst_element_set_state(audio, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { gst_object_unref(audio); XCloseDisplay(display); return 1; }
  GstBus *bus = gst_element_get_bus(audio);
  double paddle = .5, opponent = .5, ball_x = .5, ball_y = .5, vx = .36, vy = .23, last = now();
  int up = 0, down = 0, axis = 0, score = 0, missed = 0, failed = 0;
  while (!stopping) {
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (message) { gst_message_unref(message); failed = 1; break; }
    for (int count = 0; count < 256 && XPending(display); ++count) {
      XEvent event; XNextEvent(display, &event);
      if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == close_window) stopping = 1;
      if (event.type == KeyPress || event.type == KeyRelease) {
        KeySym key = XLookupKeysym(&event.xkey, 0); int pressed = event.type == KeyPress;
        if (key == XK_Escape && pressed) stopping = 1;
        if (key == XK_Up || key == XK_w) up = pressed;
        if (key == XK_Down || key == XK_s) down = pressed;
      }
      if (event.type == MotionNotify) paddle = (double)event.xmotion.y / height;
    }
    if (gamepad >= 0) {
      struct input_event events[64]; ssize_t bytes = read(gamepad, events, sizeof(events));
      if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EINTR) || (bytes > 0 && bytes % sizeof(events[0]))) { failed = 1; break; }
      for (size_t i = 0; bytes > 0 && i < (size_t)bytes / sizeof(events[0]); ++i) {
        if (events[i].type == EV_ABS && events[i].code == ABS_Y) axis = events[i].value;
        if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) { failed = 1; stopping = 1; }
      }
    }
    double current = now(), dt = fmin(current - last, .05); last = current;
    paddle += ((down - up) * .8 + axis / 32768.0 * .8) * dt;
    paddle = fmax(.1, fmin(.9, paddle));
    opponent += fmax(-.40*dt, fmin(.40*dt, ball_y - opponent));
    opponent = fmax(.1, fmin(.9, opponent));
    ball_x += vx * dt; ball_y += vy * dt;
    if (ball_y < .02) { ball_y = .02; vy = fabs(vy); }
    if (ball_y > .98) { ball_y = .98; vy = -fabs(vy); }
    if (ball_x < .055 && vx < 0 && fabs(ball_y - paddle) < .11) { vx = fabs(vx) * 1.025; ++score; }
    if (ball_x > .945 && vx > 0 && fabs(ball_y - opponent) < .11) vx = -fabs(vx);
    if (ball_x < 0 || ball_x > 1) { if (ball_x < 0) ++missed; ball_x = ball_y = .5; vx = -.36; vy = .23; }
    XSetForeground(display, gc, 0x101827); XFillRectangle(display, window, gc, 0, 0, width, height);
    XSetForeground(display, gc, 0x223348);
    for (int y = 0; y < height; y += 30) XFillRectangle(display, window, gc, width/2, y, 2, 15);
    XSetForeground(display, gc, 0x36dbc3); XFillRectangle(display, window, gc, width*.035, height*(paddle-.1), width*.012+1, height*.2);
    XSetForeground(display, gc, 0xef8c6b); XFillRectangle(display, window, gc, width*.95, height*(opponent-.1), width*.012+1, height*.2);
    XSetForeground(display, gc, 0xf1f4f8); XFillArc(display, window, gc, width*ball_x-6, height*ball_y-6, 12, 12, 0, 360*64);
    char text[128]; int length = snprintf(text, sizeof(text), "Returns: %d    Misses: %d    Move: arrows / W S / mouse / gamepad    Esc: quit", score, missed);
    if (length > 0 && length < (int)sizeof(text)) XDrawString(display, window, gc, 20, 25, text, length);
    XFlush(display);
    struct pollfd wait = {ConnectionNumber(display), POLLIN, 0}; poll(&wait, 1, 8);
  }
  if (gamepad >= 0) close(gamepad);
  gst_element_set_state(audio, GST_STATE_NULL); gst_object_unref(bus); gst_object_unref(audio);
  XFreeGC(display, gc); XDestroyWindow(display, window); XCloseDisplay(display);
  return failed;
}
