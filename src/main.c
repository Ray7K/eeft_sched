#include "lib/barrier.h"
#include "lib/log.h"

#include "processor.h"
#include "sys_config.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

LogLevel current_log_level = LOG_LEVEL_DEBUG;

static volatile sig_atomic_t shutdown_requested = 0;

barrier *proc_barrier = NULL;

static void sigint_handler(int signum) {
  (void)signum;
  shutdown_requested = 1;
  kill(0, SIGTERM);
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
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork failed");
      return 1;
    }
    if (pid == 0) {
      processor_init(proc_id);
      processor_run();
      exit(0);
    }
  }

  while (!shutdown_requested) {
    pid_t pid = wait(NULL);
    if (pid < 0) {
      if (errno == EINTR) {
        if (shutdown_requested) {
          break;
        }
      } else {
        break;
      }
    }
  }

  barrier_destroy(proc_barrier);
  shmdt(proc_barrier);
  shmctl(shmid, IPC_RMID, NULL);

  return 0;
}
