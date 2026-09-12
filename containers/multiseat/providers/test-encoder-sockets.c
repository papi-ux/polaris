#define _GNU_SOURCE
#include "encoder-sockets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>

static int make_socket(const char *path, mode_t mode) {
  int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(descriptor >= 0);
  struct sockaddr_un address = {.sun_family=AF_UNIX};
  assert(strlen(path) < sizeof(address.sun_path));
  strcpy(address.sun_path, path);
  assert(bind(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(chmod(path, mode) == 0);
  return descriptor;
}
int main(void) {
  char root[] = "/tmp/encoder-sockets-XXXXXX";
  assert(mkdtemp(root));
  char pulse[128], native[128], capture[128], retained[128], alias[128];
  assert(snprintf(pulse, sizeof(pulse), "%s/pulse", root) > 0);
  assert(snprintf(native, sizeof(native), "%s/pulse/native", root) > 0);
  assert(snprintf(capture, sizeof(capture), "%s/frames", root) > 0);
  assert(snprintf(retained, sizeof(retained), "%s/retained", root) > 0);
  assert(snprintf(alias, sizeof(alias), "%s/alias", root) > 0);
  assert(mkdir(pulse, 0700) == 0);
  int audio = make_socket(native, 0777), video = make_socket(capture, 0600);
  int pinned = pin_encoder_socket(root, "native", true);
  assert(pinned >= 0); close(pinned);
  pinned = pin_encoder_socket(root, "frames", false);
  assert(pinned >= 0);
  struct stat original, replacement;
  assert(fstat(pinned, &original) == 0);
  assert(rename(capture, retained) == 0);
  int other = make_socket(capture, 0600);
  assert(stat(capture, &replacement) == 0);
  assert(original.st_ino != replacement.st_ino || original.st_dev != replacement.st_dev);
  assert(fstat(pinned, &replacement) == 0 && original.st_ino == replacement.st_ino);
  close(pinned);
  assert(chmod(capture, 0777) == 0);
  assert(pin_encoder_socket(root, "frames", false) < 0);
  assert(chmod(pulse, 0755) == 0);
  assert(pin_encoder_socket(root, "native", true) < 0);
  assert(chmod(pulse, 0700) == 0 && chmod(root, 0755) == 0);
  assert(pin_encoder_socket(root, "native", true) < 0);
  assert(chmod(root, 0700) == 0);
  assert(symlink(native, alias) == 0);
  assert(pin_encoder_socket(root, "alias", false) < 0);
  assert(pin_encoder_socket(root, "../pulse/native", true) < 0);
  assert(pin_encoder_socket(root, ".", false) < 0);
  assert(unlink(alias) == 0);
  int file = open(alias, O_CREAT | O_EXCL | O_WRONLY, 0600);
  assert(file >= 0); close(file);
  assert(pin_encoder_socket(root, "alias", false) < 0);
  assert(unlink(alias) == 0);
  assert(rename(pulse, alias) == 0 && symlink(alias, pulse) == 0);
  assert(pin_encoder_socket(root, "native", true) < 0);
  assert(unlink(pulse) == 0 && rename(alias, pulse) == 0);
  assert(symlink(root, alias) == 0);
  assert(pin_encoder_socket(alias, "native", true) < 0);
  assert(unlink(alias) == 0);
  close(audio); close(video); close(other);
  assert(unlink(native) == 0 && unlink(capture) == 0 && unlink(retained) == 0);
  assert(rmdir(pulse) == 0 && rmdir(root) == 0);
  puts("private Pulse socket, capture permissions, symlinks and retained socket identity passed");
  return 0;
}
