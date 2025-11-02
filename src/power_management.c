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

void power_management_init(void) {
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

  Job *cur;
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    float slack = find_slack(global_core_id, cur->virtual_deadline, 1.0);
    min_slack = (min_slack < slack) ? min_slack : slack;
  }
  list_for_each_entry(cur, &core_state->replica_queue, link) {
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
    LOG(LOG_LEVEL_DEBUG, "DVFS level set to %u (Freq: %uMHz, Scale: %.2f)",
        level_idx, dvfs_levels[level_idx].frequency_mhz,
        dvfs_levels[level_idx].scaling_factor);
  }
}

uint8_t power_get_current_dvfs_level(uint16_t global_core_id) {
  return core_states[global_core_id].current_dvfs_level;
}

void power_management_set_dpm_interval(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  if (core_state->is_idle) {
    if (core_state->dpm_control_block.in_low_power_state) {
      return;
    }
    const Task *next_arrival_task = find_next_arrival_task(global_core_id);
    if (next_arrival_task == NULL) {
      LOG(LOG_LEVEL_INFO,
          "No upcoming task arrivals. Entering indefinite low power state...");
      core_state->dpm_control_block.in_low_power_state = true;
      core_state->dpm_control_block.dpm_start_time =
          processor_state.system_time;
      core_state->dpm_control_block.dpm_end_time = UINT32_MAX;
      return;
    }
    uint64_t next_arrival_time =
        ((processor_state.system_time / next_arrival_task->period) + 1) *
        next_arrival_task->period;

    const float slack = next_arrival_time - processor_state.system_time;

    if (slack >= DPM_IDLE_THRESHOLD_TICKS + DPM_ENTRY_LATENCY_TICKS +
                     DPM_EXIT_LATENCY_TICKS) {
      core_state->dpm_control_block.in_low_power_state = true;
      core_state->dpm_control_block.dpm_start_time =
          processor_state.system_time;
      core_state->dpm_control_block.dpm_end_time =
          processor_state.system_time + slack;
      LOG(LOG_LEVEL_INFO,
          "Found Slack %.2f. Entering DPM for the interval %d to %d ticks...",
          slack, core_state->dpm_control_block.dpm_start_time,
          core_state->dpm_control_block.dpm_end_time);
    }
  } else {
    core_state->dpm_control_block.in_low_power_state = false;
  }
}
