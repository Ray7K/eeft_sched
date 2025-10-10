#include "power_management.h"
#include "list.h"
#include "log.h"
#include "platform.h"
#include "sched.h"
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const DVFSLevel dvfs_levels[NUM_DVFS_LEVELS] = {{2000, 1000, 1.0},
                                                {1800, 900, 0.9},
                                                {1500, 750, 0.75},
                                                {1200, 600, 0.6},
                                                {1000, 500, 0.5}};

void power_management_init() {
  LOG(LOG_LEVEL_INFO, "Power Management Initialized.");
}

float power_get_current_scaling_factor(uint16_t global_core_id) {
  return dvfs_levels[core_states[global_core_id].current_dvfs_level]
      .scaling_factor;
}

uint8_t calc_required_dvfs_level(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  if (core_state->is_idle || core_state->running_job == NULL) {
    return NUM_DVFS_LEVELS - 1;
  }

  float remaining_wcet =
      core_state->running_job->wcet - core_state->running_job->executed_time;

  float running_job_slack =
      (core_state->running_job->actual_deadline - processor_state.system_time) -
      remaining_wcet;

  float min_slack = running_job_slack;

  Job *cur, *next;
  list_for_each_entry_safe(cur, next, &core_state->ready_queue, link) {
    float slack = find_slack(global_core_id, cur->virtual_deadline, 1.0);
    min_slack = (min_slack < slack) ? min_slack : slack;
  }
  list_for_each_entry_safe(cur, next, &core_state->replica_queue, link) {
    float slack = find_slack(global_core_id, cur->virtual_deadline, 1.0);
    min_slack = (min_slack < slack) ? min_slack : slack;
  }

  if (min_slack < 0) {
    return 0;
  }

  int8_t dvfs_level = 0; // Default to highest speed
  for (int8_t i = 0; i < NUM_DVFS_LEVELS; i++) {
    float scaling_factor = dvfs_levels[i].scaling_factor;
    float extra_time_needed =
        (remaining_wcet / scaling_factor) - remaining_wcet;

    if (extra_time_needed > min_slack) {
      break;
    }
    dvfs_level = i;
  }

  return dvfs_level;
}

void power_set_dvfs_level(uint16_t global_core_id, uint8_t level_idx) {
  if (level_idx < NUM_DVFS_LEVELS) {
    core_states[global_core_id].current_dvfs_level = level_idx;
    LOG(LOG_LEVEL_DEBUG,
        "Core %u DVFS level set to %u (Freq: %uMHz, Scale: %.2f)",
        global_core_id, level_idx, dvfs_levels[level_idx].frequency_mhz,
        dvfs_levels[level_idx].scaling_factor);
  }
}

uint8_t power_get_current_dvfs_level(uint16_t global_core_id) {
  return core_states[global_core_id].current_dvfs_level;
}
