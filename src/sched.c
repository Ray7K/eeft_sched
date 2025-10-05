#include "sched.h"
#include "list.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"
#include <stddef.h>
#include <stdint.h>
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
  if (criticality_level < processor_state.system_criticality_level) {
    criticality_level = processor_state.system_criticality_level;
  }
  uint32_t acet = (uint32_t)(percentage / 100.0 *
                             job->parent_task->wcet[criticality_level]);
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

  uint32_t interval_length = deadline - current_time;
  if (demand >= interval_length) {
    return 0;
  }

  return interval_length - demand;
}

static void handle_job_completion(uint16_t global_core_id) {
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

static void handle_mode_change(uint16_t global_core_id,
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
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job->relative_tuned_deadlines[processor_state
                                                  .system_criticality_level];
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
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job->relative_tuned_deadlines[processor_state
                                                  .system_criticality_level];
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
    if (slack >= discarded_job->wcet - discarded_job->executed_time) {
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

static void handle_job_arrivals(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

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
      for (uint8_t level = 0; level < MAX_CRITICALITY_LEVELS; level++) {
        new_job->relative_tuned_deadlines[level] =
            instance->tuned_deadlines[level];
      }
      new_job->actual_deadline = system_time + new_job->parent_task->deadline;
      new_job->virtual_deadline =
          system_time +
          instance->tuned_deadlines[processor_state.system_criticality_level];
      new_job->wcet =
          new_job->parent_task->wcet[processor_state.system_criticality_level];

      new_job->acet = generate_acet(new_job);
      new_job->executed_time = 0;

      new_job->is_replica = (instance->task_type == Replica);
      new_job->state = JOB_STATE_READY;

      printf("Job %d arrived on core %d with deadline (actual: %d, virtual: "
             "%d)\n with ACET %d and "
             "WCET %d\n",
             new_job->parent_task->id, global_core_id % NUM_CORES_PER_PROC,
             new_job->actual_deadline, new_job->virtual_deadline, new_job->acet,
             new_job->wcet);

      // in case of a job arrival where job's criticality is less than system
      // criticality
      if (new_job->parent_task->criticality_level <
          processor_state.system_criticality_level) {
        // check for slack
        uint32_t slack = find_slack(global_core_id, new_job);
        if (slack >= new_job->wcet) {
          printf("Sufficient slack (%d) found for Job %d. Adding to "
                 "ready/replica queue.\n",
                 slack, new_job->parent_task->id);
          if (new_job->is_replica) {
            add_to_queue_sorted(&core_state->replica_queue, new_job);
            // release_job(new_job, global_core_id);
          } else {
            add_to_queue_sorted(&core_state->ready_queue, new_job);
            // release_job(new_job, global_core_id);
          }
        } else {
          add_to_queue_sorted(&processor_state.discard_queue, new_job);
          // release_job(new_job, global_core_id);
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
}

static void handle_running_job(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

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
        core_state->running_job->actual_deadline,
        core_state->running_job->executed_time);
    // Incase job isn't able to meet deadline
    if (core_state->running_job->state != JOB_STATE_COMPLETED &&
        system_time > core_state->running_job->actual_deadline) {
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
}

static Job *select_next_job(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  bool from_ready_queue = false;

  Job *next_job_candidate;

  if (!list_empty(&core_state->ready_queue) &&
      !list_empty(&core_state->replica_queue)) {
    Job *ready_job = peek_next_job(&core_state->ready_queue);
    Job *replica_job = peek_next_job(&core_state->replica_queue);

    if (ready_job->virtual_deadline <= replica_job->virtual_deadline) {
      next_job_candidate = ready_job;
      from_ready_queue = true;
    } else {
      next_job_candidate = replica_job;
    }
  } else if (list_empty(&core_state->ready_queue) == false) {
    next_job_candidate = peek_next_job(&core_state->ready_queue);
    from_ready_queue = true;
  } else if (list_empty(&core_state->replica_queue) == false) {
    next_job_candidate = peek_next_job(&core_state->replica_queue);
  }

  if (core_state->running_job == NULL ||
      core_state->running_job->virtual_deadline >
          next_job_candidate->virtual_deadline) {
    if (from_ready_queue) {
      return pop_next_job(&core_state->ready_queue);
    } else {
      return pop_next_job(&core_state->replica_queue);
    }
  }

  return NULL;
}

static void dispatch_job(uint16_t global_core_id, Job *job_to_dispatch) {
  CoreState *core_state = &core_states[global_core_id];

  if (core_state->running_job != NULL) {
    Job *current_job = core_state->running_job;
    current_job->state = JOB_STATE_READY;
    if (current_job->is_replica) {
      add_to_queue_sorted(&core_state->replica_queue, current_job);
    } else {
      add_to_queue_sorted(&core_state->ready_queue, current_job);
    }
  }

  core_state->running_job = job_to_dispatch;
  job_to_dispatch->state = JOB_STATE_RUNNING;
}

// BUG: Discard queue task accommodation fails to meet deadlines in rare cases
// under high utilization
static void reclaim_discarded_jobs(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

  while (peek_next_job(&processor_state.discard_queue)) {
    Job *cur_job = peek_next_job(&processor_state.discard_queue);
    cur_job->virtual_deadline = cur_job->actual_deadline;
    cur_job->wcet =
        cur_job->parent_task->wcet[processor_state.system_criticality_level];

    uint32_t slack = find_slack(global_core_id, cur_job);

    if (slack >= cur_job->wcet - cur_job->executed_time) {
      printf("Accommodating job from discard queue\n");
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

void scheduler_init() {
  printf("Initializing Scheduler...\n");

  task_management_init();

  for (int i = 0; i < TOTAL_CORES; i++) {
    core_states[i].proc_id = i / NUM_CORES_PER_PROC;
    core_states[i].core_id = i % NUM_CORES_PER_PROC;
    core_states[i].running_job = NULL;
    core_states[i].is_idle = true;
    core_states[i].busy_time = 0;

    INIT_LIST_HEAD(&core_states[i].ready_queue);
    INIT_LIST_HEAD(&core_states[i].replica_queue);
    INIT_LIST_HEAD(&core_states[i].discard_list);
  }

  printf("Scheduler Initialized for %d cores.\n", TOTAL_CORES);
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

  handle_running_job(global_core_id);

  handle_job_arrivals(global_core_id);

  Job *next_job = select_next_job(global_core_id);

  if (next_job != NULL) {
    dispatch_job(global_core_id, next_job);
  }

  reclaim_discarded_jobs(global_core_id);
}
