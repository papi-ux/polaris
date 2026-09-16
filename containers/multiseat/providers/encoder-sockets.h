/* Retain exact capture/audio socket inodes below private owned directories. */
#pragma once
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool encoder_private_directory(int descriptor) {
  struct stat status;
  return descriptor >= 0 && !fstat(descriptor, &status) && S_ISDIR(status.st_mode) &&
    status.st_uid == geteuid() && (status.st_mode & 07777) == 0700;
}
static int pin_encoder_socket(const char *runtime_path, const char *name, bool pulse) {
  if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return -1;
  int directory = open(runtime_path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!encoder_private_directory(directory)) {
    if (directory >= 0) close(directory);
    return -1;
  }
  if (pulse) {
    int nested = openat(directory, "pulse", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    directory = nested;
    if (!encoder_private_directory(directory)) {
      if (directory >= 0) close(directory);
      return -1;
    }
  }
  int descriptor = openat(directory, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  close(directory);
  if (descriptor < 0) return -1;
  struct stat status;
  /* Pulse creates a 0777 socket. Its two mode-0700 owned parents enforce
   * isolation; relaxing either parent is still refused before opening it. */
  if (fstat(descriptor, &status) || !S_ISSOCK(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 07000) ||
      (!pulse && (status.st_mode & 07777) != 0600 && (status.st_mode & 07777) != 0700)) {
    close(descriptor);
    return -1;
  }
  return descriptor;
}
