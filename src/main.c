#include "barrier.h"
#include "log.h"
#include "platform.h"
#include "sys_config.h"
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

LogLevel current_log_level = LOG_LEVEL_DEBUG;

barrier_t *proc_barrier = NULL;

void sigint_handler(int signum) {}

int main() {

  signal(SIGINT, sigint_handler);

  int shmid;

  shmid = shmget(IPC_PRIVATE, sizeof(barrier_t), IPC_CREAT | 0666);
  if (shmid < 0) {
    perror("shmget failed");
    return 1;
  }

  proc_barrier = (barrier_t *)shmat(shmid, NULL, 0);
  if (proc_barrier == (barrier_t *)-1) {
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
      platform_init(proc_id);
      platform_run();
    }
  }

  for (uint8_t proc_id = 0; proc_id < NUM_PROC; proc_id++) {
    wait(NULL);
  }

  barrier_destroy(proc_barrier);
  shmdt(proc_barrier);
  shmctl(shmid, IPC_RMID, NULL);

  return 0;
}
