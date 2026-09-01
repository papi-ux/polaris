#include <errno.h>
#include <signal.h>
#include <libgen.h>
#include <sys/prctl.h>
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

static int find_separator(int argc, char **argv) {
  int separator = -1;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      separator = i;
      break;
    }
  }
  return separator;
}

static int run_fake_reaper(int argc, char **argv) {
  int separator = find_separator(argc, argv);
  if (separator < 0 || separator + 1 >= argc) {
    return 2;
  }

  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() == 1) {
    return 9;
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
  if (pid_file == NULL) {
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return 4;
  }
  FILE *out = fopen(pid_file, "w");
  if (out == NULL) {
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return 5;
  }
  fprintf(out, "%ld\n", (long) child);
  if (fclose(out) != 0) {
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return 6;
  }

  int status = 0;
  for (;;) {
    if (waitpid(child, &status, 0) >= 0) {
      break;
    }
    if (errno != EINTR) {
      return 10;
    }
  }
  return 0;
}

static int run_fake_gamescope(int argc, char **argv) {
  int separator = find_separator(argc, argv);
  if (separator < 0 || separator + 1 >= argc) {
    return 2;
  }

  /* Production launches Gamescope as the leader of a private session and
   * process group. Gamescope then launches gamescopereaper, and the reaper
   * launches the primary command. Model the full upstream ancestry so the
   * wrapper proves the same chain exercised on a real host. */
  if (setsid() < 0) {
    return 8;
  }

  pid_t reaper = fork();
  if (reaper < 0) {
    return 3;
  }
  if (reaper == 0) {
    size_t command_count = (size_t) (argc - separator - 1);
    char **reaper_argv = calloc(command_count + 3, sizeof(char *));
    if (reaper_argv == NULL) {
      _exit(126);
    }
    reaper_argv[0] = (char *) "gamescopereaper";
    reaper_argv[1] = (char *) "--";
    for (size_t i = 0; i < command_count; ++i) {
      reaper_argv[i + 2] = argv[separator + 1 + (int) i];
    }
    execvp(reaper_argv[0], reaper_argv);
    _exit(127);
  }

  const char *started_file = getenv("POLARIS_STEAM_STARTED_FILE");
  const char *exit_gate_file = getenv("POLARIS_FAKE_PARENT_EXIT_GATE_FILE");
  if (started_file == NULL) {
    return 4;
  }

  if (wait_for_file(started_file) == 0
      && (exit_gate_file == NULL || wait_for_file(exit_gate_file) == 0)) {
    return 0;
  }
  kill(reaper, SIGKILL);
  waitpid(reaper, NULL, 0);
  return 7;
}

int main(int argc, char **argv) {
  char *program = basename(argv[0]);
  if (strcmp(program, "gamescopereaper") == 0) {
    return run_fake_reaper(argc, argv);
  }
  return run_fake_gamescope(argc, argv);
}
