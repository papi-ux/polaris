#define _GNU_SOURCE
#include <gst/gst.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Exercise the real poll loop with three idle pipes. No physical input is
 * opened. The single initial frame callback is the simulated last good frame. */
static gint64 first_frame;
static bool recovered, retired;
static int writers[3];
static GstPad *test_pad; static GstPadProbeCallback test_callback; static gpointer test_data;
__attribute__((constructor)) static void idle_input(void) {
  int readers[3];
  for (int i=0;i<3;++i) {
    int pair[2]; if (pipe2(pair,O_NONBLOCK|O_CLOEXEC)) _exit(90);
    readers[i]=fcntl(pair[0],F_DUPFD_CLOEXEC,20);
    writers[i]=fcntl(pair[1],F_DUPFD_CLOEXEC,20);
    if (readers[i]<0 || writers[i]<0) _exit(90);
    close(pair[0]); close(pair[1]); /* High-numbered writer stays open and idle. */
  }
  for (int i=0;i<3;++i) { if (dup2(readers[i],4+i)<0) _exit(90); close(readers[i]); }
}
int __real_fstat(int,struct stat *);
int __wrap_fstat(int fd,struct stat *status) {
  int result=__real_fstat(fd,status);
  if (!result && fd>=4 && fd<=6) { status->st_mode=S_IFCHR|0400; status->st_rdev=makedev(13,fd); }
  return result;
}
gulong __wrap_gst_pad_add_probe(GstPad *pad,GstPadProbeType mask,GstPadProbeCallback callback,gpointer data,GDestroyNotify destroy) {
  (void)mask; (void)destroy;
  test_pad=pad; test_callback=callback; test_data=data;
  return 1;
}
GstStateChangeReturn __real_gst_element_set_state(GstElement *,GstState);
GstStateChangeReturn __wrap_gst_element_set_state(GstElement *element,GstState state) {
  GstStateChangeReturn result=__real_gst_element_set_state(element,state);
  if (state==GST_STATE_PLAYING && test_callback) {
    first_frame=g_get_monotonic_time(); test_callback(test_pad,NULL,test_data);
  }
  return result;
}
void __wrap_gst_pad_remove_probe(GstPad *pad,gulong id) { (void)pad; (void)id; }
/* Deliver a second frame after a real two-second gap through the production
 * poll loop. A short activity deadline must fail before this recovery. */
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
  int result=__real_poll(fds,count,timeout);
  const char *scenario=getenv("POLARIS_CAPTURE_TEST");
  gint64 elapsed=g_get_monotonic_time()-first_frame;
  if (scenario && !strcmp(scenario,"recover") && elapsed>=2000000 && !recovered) {
    test_callback(test_pad,NULL,test_data); recovered=true;
  }
  if (recovered && elapsed>=2300000) raise(SIGTERM);
  if (scenario && !strcmp(scenario,"retire") && elapsed>=2000000 && !retired) {
    close(writers[0]); retired=true;
  }
  return result;
}
__attribute__((destructor)) static void bounded_result(void) {
  gint64 elapsed=g_get_monotonic_time()-first_frame;
  const char *scenario=getenv("POLARIS_CAPTURE_TEST");
  if (!first_frame) _Exit(91);
  if (scenario && !strcmp(scenario,"recover")) {
    if (!recovered || elapsed<2300000 || elapsed>4000000) _Exit(92);
  } else if (scenario && !strcmp(scenario,"retire")) {
    if (!retired || elapsed<2000000 || elapsed>3500000) _Exit(93);
  } else if (elapsed<4900000 || elapsed>7000000) _Exit(94);
}
