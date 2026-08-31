#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int separator = -1;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      separator = i;
      break;
    }
  }
  if (separator < 0 || separator + 1 >= argc) {
    return 2;
  }

  pid_t child = fork();
  if (child < 0) {
    return 3;
  }
  if (child == 0) {
    execvp(argv[separator + 1], &argv[separator + 1]);
    _exit(127);
  }

  const char *pid_file = getenv("POLARIS_FAKE_CHILD_PID_FILE");
  const char *started_file = getenv("POLARIS_STEAM_STARTED_FILE");
  if (pid_file == NULL || started_file == NULL) {
    return 4;
  }
  FILE *out = fopen(pid_file, "w");
  if (out == NULL) {
    return 5;
  }
  fprintf(out, "%ld\n", (long) child);
  if (fclose(out) != 0) {
    return 6;
  }

  for (int i = 0; i < 500; ++i) {
    if (access(started_file, F_OK) == 0) {
      return 0;
    }
    usleep(10000);
  }
  kill(child, SIGKILL);
  waitpid(child, NULL, 0);
  return 7;
}
