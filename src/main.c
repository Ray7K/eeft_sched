#include "processor.h"
#include "sys_config.h"

#include "lib/barrier.h"
#include "lib/log.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

log_level current_log_level = LOG_LEVEL_DEBUG;

static volatile sig_atomic_t shutdown_requested = 0;

static pid_t proc_pids[NUM_PROC] = {0};

barrier *proc_barrier = NULL;

static void sigint_handler(int signum) {
  (void)signum;
  shutdown_requested = 1;
}

static void sigterm_handler(int signum) { (void)signum; }

int main(int argc, char *argv[]) {
  srand((unsigned)time(NULL));

  (void)argc;
  (void)argv;

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigterm_handler);

  int shmid;

  shmid = shmget(IPC_PRIVATE, sizeof(barrier), IPC_CREAT | 0666);
  if (shmid < 0) {
    perror("shmget failed");
    return 1;
  }

  proc_barrier = (barrier *)shmat(shmid, NULL, 0);
  if (proc_barrier == (barrier *)-1) {
    perror("shmat failed");
    return 1;
  }

  if (barrier_init(proc_barrier, NUM_PROC, 1) != 0) {
    perror("barrier_init failed");
    return 1;
  }

  for (uint8_t proc_id = 0; proc_id < NUM_PROC; proc_id++) {
    proc_pids[proc_id] = fork();
    if (proc_pids[proc_id] < 0) {
      perror("fork failed");
      return 1;
    }
    if (proc_pids[proc_id] == 0) {
      processor_init(proc_id);
      processor_run();

      if (atomic_load(&core_fatal_shutdown_requested) == 1) {
        _exit(EXIT_FAILURE);
      } else {
        _exit(EXIT_SUCCESS);
      }
    }
  }

  int children_left = NUM_PROC;
  int fatal_error = 0;

  while (children_left > 0) {
    int status;
    pid_t pid = waitpid(-1, &status, 0);

    if (pid > 0) {
      children_left--;

      if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != EXIT_SUCCESS) {
          fatal_error = 1;
        }
      } else if (WIFSIGNALED(status)) {
        fatal_error = 1;
      }

      if (fatal_error) {
        shutdown_requested = 1;
        break;
      }

    } else if (pid < 0) {
      if (errno == EINTR && shutdown_requested)
        break;
      continue;
    }
  }

  if (shutdown_requested || fatal_error) {
    for (uint8_t i = 0; i < NUM_PROC; i++) {
      pid_t pid = proc_pids[i];
      if (pid > 0) {
        if (kill(pid, 0) == 0) {
          kill(pid, SIGUSR1);
        }
      }
    }

    while (1) {
      pid_t r = waitpid(-1, NULL, 0);
      if (r > 0)
        continue;
      if (r < 0) {
        if (errno == EINTR)
          continue;
        if (errno == ECHILD)
          break;
        perror("waitpid");
        break;
      }
    }
  }

  barrier_destroy(proc_barrier);
  shmdt(proc_barrier);
  shmctl(shmid, IPC_RMID, NULL);

  return shutdown_requested || fatal_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
