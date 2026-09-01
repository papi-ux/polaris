#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for_file(const char *path) {
  for (int i = 0; i < 500; ++i) {
    if (access(path, F_OK) == 0) {
      return 0;
    }
    usleep(10000);
  }
  return -1;
}

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

  /* Production launches Gamescope as the leader of a private session and
   * process group. Model that boundary so the child can safely fence the
   * complete group when this fake compositor exits. */
  if (setsid() < 0) {
    return 8;
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
  const char *exit_gate_file = getenv("POLARIS_FAKE_PARENT_EXIT_GATE_FILE");
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

  if (wait_for_file(started_file) == 0
      && (exit_gate_file == NULL || wait_for_file(exit_gate_file) == 0)) {
    return 0;
  }
  kill(child, SIGKILL);
  waitpid(child, NULL, 0);
  return 7;
}
