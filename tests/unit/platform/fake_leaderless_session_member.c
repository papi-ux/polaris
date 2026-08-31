#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static int write_pid(const char *path, pid_t pid) {
  FILE *out = fopen(path, "w");
  int result = 0;
  if (out == NULL) {
    return -1;
  }
  if (fprintf(out, "%ld\n", (long) pid) < 0) {
    result = -1;
  }
  if (fclose(out) != 0) {
    result = -1;
  }
  return result;
}

int main(void) {
  const char *group_file = getenv("POLARIS_STEAM_LEADERLESS_GROUP_FILE");
  const char *member_file = getenv("POLARIS_STEAM_LEADERLESS_MEMBER_FILE");
  if (group_file == NULL || member_file == NULL) {
    return 2;
  }
  if (setpgid(0, 0) < 0) {
    return 3;
  }

  pid_t group_leader = getpid();
  pid_t member = fork();
  if (member < 0) {
    return 4;
  }
  if (member == 0) {
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    for (;;) {
      pause();
    }
  }

  if (write_pid(group_file, group_leader) != 0 || write_pid(member_file, member) != 0) {
    kill(member, SIGKILL);
    return 5;
  }
  return 0;
}
