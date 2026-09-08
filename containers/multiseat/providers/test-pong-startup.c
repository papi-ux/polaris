#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Link wrappers exercise the real main startup without a display or device. */
int __wrap_open(const char *path, int flags, ...) {
  (void)flags;
  if (strcmp(path,"/dev/input/polaris-gamepad-0")) _exit(90);
  const char *scenario=getenv("POLARIS_PONG_TEST");
  if (!scenario) _exit(91);
  errno = !strcmp(scenario,"absent") ? ENOENT : !strcmp(scenario,"denied") ? EACCES : ENODEV;
  return -1;
}
Display *__wrap_XOpenDisplay(const char *name) {
  (void)name;
  /* Only optional absence may proceed to create the display/game. */
  _exit(!strcmp(getenv("POLARIS_PONG_TEST"),"absent") ? 0 : 92);
}
