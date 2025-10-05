#include "sched.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define TOTAL_CORES (NUM_PROC * NUM_CORES_PER_PROC)

static CoreState core_states[TOTAL_CORES];

static uint32_t generate_acet(Job *job) {
  uint8_t criticality_chance = rand() % 100;
  CriticalityLevel criticality_level = job->parent_task->criticality_level;

  if (criticality_chance < 1) {
    criticality_level = ASIL_D;
  }
  if (criticality_chance < 5) {
    criticality_level = ASIL_C;
  } else if (criticality_chance < 15) {
    criticality_level = ASIL_B;
  } else if (criticality_chance < 30) {
    criticality_level = ASIL_A;
  } else {
    criticality_level = QM;
  }

  uint8_t percentage = rand() % 100;
  uint32_t acet =
      percentage / 100.00 * job->parent_task->wcet[criticality_level];
  if (acet == 0) {
    acet = 1;
  }
  return acet;
}

static Task *find_task_by_id(uint32_t task_id) {
  for (uint32_t i = 0; i < SYSTEM_TASKS_SIZE; i++) {
    if (system_tasks[i].id == task_id) {
      return (Task *)&system_tasks[i];
    }
  }
  return NULL;
}

static uint32_t find_slack(uint32_t global_core_id, Job *candidate_job) {
  CoreState *core_state = &core_states[global_core_id];
  uint32_t current_time = system_time;
  uint32_t deadline = candidate_job->virtual_deadline;

  if (deadline <= current_time) {
    return 0;
  }
  uint32_t max_slack = candidate_job->absolute_deadline - system_time;

  uint32_t demand = 0;

  // Demand from the currently running job
  if (core_state->running_job != NULL) {
    if (core_state->running_job->virtual_deadline <= deadline) {
      demand += core_state->running_job->wcet -
                core_state->running_job->executed_time;
    }
  }

  // Demand from jobs already in the ready queue
  Job *job, *next_job;
  list_for_each_entry_safe(job, next_job, &core_state->ready_queue, link) {
    if (job->virtual_deadline <= deadline) {
      demand += job->wcet - job->executed_time;
    }
  }
  list_for_each_entry_safe(job, next_job, &core_state->replica_queue, link) {
    if (job->virtual_deadline <= deadline) {
      demand += job->wcet - job->executed_time;
    }
    max_slack -= remaining_wcet;
  }

  // Demand from future job arrivals of high-criticality tasks
  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const Task *task = find_task_by_id(instance->task_id);

      if (task->criticality_level >= processor_state.system_criticality_level) {
        uint32_t wcet = task->wcet[processor_state.system_criticality_level];
        uint32_t period = task->period;

        uint32_t arrival_time = (current_time / period + 1) * period;
        if (current_time % period == 0) {
          arrival_time = current_time;
        }
        while (1) {
          uint32_t future_job_deadline =
              arrival_time +
              instance
                  ->tuned_deadlines[processor_state.system_criticality_level];

          if (future_job_deadline > deadline) {
            break;
          }
          demand += wcet;
          arrival_time += period;
        }
      }
    }
  }
  return max_slack;
}

void scheduler_init() {
  printf("Initializing Scheduler...\n");

  uint32_t interval_length = deadline - current_time;
  if (demand >= interval_length) {
    return 0;
  }

  return interval_length - demand;
}

void scheduler_tick(uint16_t global_core_id) {
  if (global_core_id >= TOTAL_CORES) {
    return;
  }

  CoreState *core_state = &core_states[global_core_id];
  printf("Ready Queue: ");
  print_queue(&core_state->ready_queue);
  printf("Replica Queue: ");
  print_queue(&core_state->replica_queue);
  printf("\n");

  printf("Utilization: %.2f %%\n",
         core_state->busy_time * 100.0 / (system_time + 1));
  printf("Current Criticality Level: %d\n",
         processor_state.system_criticality_level);

  if (core_state->running_job != NULL) {
    core_state->busy_time++;
    core_state->running_job->executed_time++;

    printf(
        "Current Job on core %d: Job %d (Remaining WCET: %d, Remaining "
        "ACET: %d, Deadline: %d, Executed Time: %d)\n",
        global_core_id % NUM_CORES_PER_PROC,
        core_state->running_job->parent_task->id,
        core_state->running_job->wcet - core_state->running_job->executed_time,
        core_state->running_job->acet - core_state->running_job->executed_time,
        core_state->running_job->absolute_deadline,
        core_state->running_job->executed_time);
    // Incase job isn't able to meet deadline
    if (core_state->running_job->state != JOB_STATE_COMPLETED &&
        system_time > core_state->running_job->absolute_deadline) {
      printf("Job %d missed its deadline at time %d\n",
             core_state->running_job->parent_task->id, system_time);
      exit(1);
    }

    if (core_state->running_job->acet <=
        core_state->running_job->executed_time) {
      handle_job_completion(global_core_id);
    } else if (core_state->running_job->wcet <=
               core_state->running_job->executed_time) {
      CriticalityLevel new_criticality_level =
          processor_state.system_criticality_level;
      for (int level = processor_state.system_criticality_level + 1;
           level < MAX_CRITICALITY_LEVELS; level++) {
        if (core_state->running_job->executed_time <
            core_state->running_job->parent_task->wcet[level]) {
          new_criticality_level = (CriticalityLevel)level;
          break;
        }
      }
      handle_mode_change(global_core_id, new_criticality_level);
    }
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      Task *task = find_task_by_id(instance->task_id);

      if (system_time % task->period != 0) {
        continue;
      }
      Job *new_job = create_job(task, global_core_id);
      if (new_job == NULL) {
        continue;
      }

      // update job parameters
      new_job->arrival_time = system_time;
      new_job->absolute_deadline =
          system_time +
          new_job->parent_task
              ->deadline[processor_state.system_criticality_level];
      new_job->virtual_deadline = new_job->absolute_deadline;
      new_job->wcet =
          new_job->parent_task->wcet[processor_state.system_criticality_level];

      new_job->acet = generate_acet(new_job);
      new_job->executed_time = 0;

      new_job->is_replica = (instance->task_type == Replica);
      new_job->state = JOB_STATE_READY;

      printf("Job %d arrived on core %d with deadline %d\n with ACET %d and "
             "WCET %d\n",
             new_job->parent_task->id, global_core_id % NUM_CORES_PER_PROC,
             new_job->absolute_deadline, new_job->acet, new_job->wcet);

      // in case of a job arrival where job's criticality is less than system
      // criticality
      if (new_job->parent_task->criticality_level <
          processor_state.system_criticality_level) {
        // check for slack
        uint32_t slack = find_slack(global_core_id, new_job);
        if (slack > new_job->wcet) {
          printf("Sufficient slack (%d) found for Job %d. Adding to "
                 "ready/replica queue.\n",
                 slack, new_job->parent_task->id);
          if (new_job->is_replica)
            add_to_queue_sorted(&core_state->replica_queue, new_job);
          else {
            add_to_queue_sorted(&core_state->ready_queue, new_job);
          }
        } else {
          add_to_queue_sorted(&processor_state.discard_queue, new_job);
        }
      } else {
        if (new_job->is_replica)
          add_to_queue_sorted(&core_state->replica_queue, new_job);
        else {
          add_to_queue_sorted(&core_state->ready_queue, new_job);
        }
      }
    }
  }

  // FIX: don't preempt running job if it isn't necessary
  if (core_state->running_job != NULL) {
    Job *running_job = core_state->running_job;
    core_state->running_job = NULL;
    core_state->is_idle = true;
    running_job->state = JOB_STATE_READY;

    printf("Preempting Job %d on core %d at time %d\n",
           running_job->parent_task->id, global_core_id % NUM_CORES_PER_PROC,
           system_time);

    if (running_job->is_replica) {
      add_to_queue_sorted(&core_state->replica_queue, running_job);
    } else {
      add_to_queue_sorted(&core_state->ready_queue, running_job);
    }
  }

  if (!list_empty(&core_state->ready_queue) &&
      !list_empty(&core_state->replica_queue)) {
    Job *ready_job = peek_next_job(&core_state->ready_queue);
    Job *replica_job = peek_next_job(&core_state->replica_queue);

    uint32_t slack_ready = find_slack(global_core_id, ready_job);
    uint32_t slack_replica = find_slack(global_core_id, replica_job);

    if (slack_ready <= slack_replica) {
      Job *next_job = pop_next_job(&core_state->ready_queue);
      next_job->state = JOB_STATE_RUNNING;
      core_state->running_job = next_job;
      core_state->is_idle = false;
    } else {
      Job *next_job = pop_next_job(&core_state->replica_queue);
      next_job->state = JOB_STATE_RUNNING;
      core_state->running_job = next_job;
      core_state->is_idle = false;
    }
  } else if (list_empty(&core_state->ready_queue) == false) {
    Job *next_job = pop_next_job(&core_state->ready_queue);
    next_job->state = JOB_STATE_RUNNING;
    core_state->running_job = next_job;
    core_state->is_idle = false;
  } else if (list_empty(&core_state->replica_queue) == false) {
    Job *next_job = pop_next_job(&core_state->replica_queue);
    next_job->state = JOB_STATE_RUNNING;
    core_state->running_job = next_job;
    core_state->is_idle = false;
  } else {
    core_state->is_idle = true;
  }

  if (core_state->is_idle) {
    printf("Core %d is idle at time %d\n", global_core_id % NUM_CORES_PER_PROC,
           system_time);
    while (peek_next_job(&processor_state.discard_queue) != NULL) {
      uint32_t slack = find_slack(
          global_core_id, peek_next_job(&processor_state.discard_queue));
      if (slack >
          peek_next_job(&processor_state.discard_queue)->wcet -
              peek_next_job(&processor_state.discard_queue)->executed_time) {
        Job *job_to_readd = pop_next_job(&processor_state.discard_queue);
        if (job_to_readd->is_replica) {
          add_to_queue_sorted(&core_state->replica_queue, job_to_readd);
        } else {
          add_to_queue_sorted(&core_state->ready_queue, job_to_readd);
        }
      } else {
        break;
      }
    }
  }
}

void handle_job_completion(uint16_t global_core_id) {
  if (global_core_id >= TOTAL_CORES) {
    return;
  }

  CoreState *core_state = &core_states[global_core_id];
  Job *completed_job = core_state->running_job;
  printf("Job %d completed on core %d at time %d\n",
         completed_job->parent_task->id, global_core_id % NUM_CORES_PER_PROC,
         system_time);

  if (completed_job == NULL) {
    printf("No running job to complete on core %d\n",
           global_core_id % NUM_CORES_PER_PROC);
    return;
  }

  completed_job->state = JOB_STATE_COMPLETED;

  release_job(completed_job, global_core_id);
  core_state->running_job = NULL;
  core_state->is_idle = true;
}

// TODO: Implement message passing logic for MPMC in case of mode switch

void handle_mode_change(uint16_t global_core_id,
                        CriticalityLevel new_criticality_level) {
  CoreState *core_state = &core_states[global_core_id];

  processor_state.system_criticality_level = new_criticality_level;
  printf("Mode Change to %d on core %d at time %d\n",
         processor_state.system_criticality_level,
         global_core_id % NUM_CORES_PER_PROC, system_time);

  if (core_state->running_job != NULL) {
    Job *running_job = core_state->running_job;
    core_state->running_job = NULL;
    running_job->state = JOB_STATE_READY;
    core_state->is_idle = true;

    if (running_job->is_replica) {
      add_to_queue_sorted(&core_state->replica_queue, running_job);
    } else {
      add_to_queue_sorted(&core_state->ready_queue, running_job);
    }
  }

  LIST_HEAD(new_ready_queue);
  LIST_HEAD(new_replica_queue);

  while (peek_next_job(&core_state->ready_queue) != NULL) {
    Job *current_job = pop_next_job(&core_state->ready_queue);
    current_job->absolute_deadline =
        current_job->arrival_time +
        current_job->parent_task
            ->deadline[processor_state.system_criticality_level];
    current_job->virtual_deadline = current_job->absolute_deadline;
    current_job->wcet = current_job->parent_task
                            ->wcet[processor_state.system_criticality_level];

    if (current_job->parent_task->criticality_level <
        processor_state.system_criticality_level) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_ready_queue, current_job);
    }
  }

  while (peek_next_job(&core_state->replica_queue) != NULL) {
    Job *current_job = pop_next_job(&core_state->replica_queue);
    current_job->absolute_deadline =
        current_job->arrival_time +
        current_job->parent_task
            ->deadline[processor_state.system_criticality_level];
    current_job->virtual_deadline = current_job->absolute_deadline;
    current_job->wcet = current_job->parent_task
                            ->wcet[processor_state.system_criticality_level];

    if (current_job->parent_task->criticality_level <
        processor_state.system_criticality_level) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_replica_queue, current_job);
    }
  }

  list_splice_init(&new_ready_queue, &core_state->ready_queue);
  list_splice_init(&new_replica_queue, &core_state->replica_queue);

  while (peek_next_job(&core_state->discard_list) != NULL) {
    Job *discarded_job = pop_next_job(&core_state->discard_list);
    uint32_t slack = find_slack(global_core_id, discarded_job);
    if (slack > discarded_job->wcet - discarded_job->executed_time) {
      if (discarded_job->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, discarded_job);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, discarded_job);
      }
    } else {
      add_to_queue_sorted(&processor_state.discard_queue, discarded_job);
    }
  }
}
