#include "power_management.h"
#include "processor.h"
#include "sys_config.h"

#include "lib/list.h"
#include "lib/log.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_util.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const dvfs_level dvfs_levels[NUM_DVFS_LEVELS] = {
    {2000, 1000, 1.00f}, // 2.0 GHz @ 1.0 V
    {1800, 950, 0.90f},  // 1.8 GHz @ 0.95 V
    {1500, 900, 0.75f},  // 1.5 GHz @ 0.90 V
    {1200, 850, 0.60f},  // 1.2 GHz @ 0.85 V
    {1000, 800, 0.50f},  // 1.0 GHz @ 0.80 V
    {800, 760, 0.40f}    // 0.8 GHz @ 0.76 V
};

void power_management_init(void) {
  LOG(LOG_LEVEL_INFO, "Power Management Initialized.");
}

float power_get_current_scaling_factor(uint8_t core_id) {
  return dvfs_levels[core_states[core_id].current_dvfs_level].scaling_factor;
}

uint8_t calc_required_dvfs_level(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  if (core_state->is_idle || core_state->running_job == NULL)
    return NUM_DVFS_LEVELS - 1; // lowest power state

  uint32_t now = proc_state.system_time;
  float min_slack = FLT_MAX;

  for (uint8_t crit_lvl = core_state->local_criticality_level;
       crit_lvl < MAX_CRITICALITY_LEVELS; crit_lvl++) {
    float slack = find_slack(core_id, crit_lvl, now, 1.0f, NULL);
    if (slack < min_slack) {
      min_slack = slack;
    }
  }

  if (min_slack <= 0.0f) {
    return 0;
  }

  float remaining_hi = (float)core_state->running_job->parent_task
                           ->wcet[MAX_CRITICALITY_LEVELS - 1] -
                       core_state->running_job->executed_time;

  int8_t best_level = 0;

  for (int8_t i = 0; i < NUM_DVFS_LEVELS; i++) {
    float scale = dvfs_levels[i].scaling_factor;
    float added_time = (remaining_hi / scale) - remaining_hi;

    if (added_time <= min_slack) {
      best_level = i;
    } else {
      break;
    }
  }

  return best_level;
}

void power_set_dvfs_level(uint8_t core_id, uint8_t level_idx) {
  if (level_idx < NUM_DVFS_LEVELS) {
    core_states[core_id].current_dvfs_level = level_idx;
    LOG(LOG_LEVEL_DEBUG, "DVFS level set to %u (Freq: %uMHz, Scale: %.2f)",
        level_idx, dvfs_levels[level_idx].frequency_mhz,
        dvfs_levels[level_idx].scaling_factor);
  }
}

uint8_t power_get_current_dvfs_level(uint8_t core_id) {
  return core_states[core_id].current_dvfs_level;
}

void power_management_set_dpm_interval(uint8_t core_id,
                                       uint32_t next_arrival_time) {
  core_state *core_state = &core_states[core_id];
  uint32_t now = proc_state.system_time;

  if (!core_state->is_idle)
    return;

  if (core_state->dpm_control_block.in_low_power_state)
    return;

  if (next_arrival_time == UINT32_MAX || next_arrival_time <= now) {
    LOG(LOG_LEVEL_INFO,
        "No upcoming task arrivals. Entering indefinite low power state...");
    core_state->dpm_control_block.in_low_power_state = true;
    core_state->dpm_control_block.dpm_start_time = now;
    core_state->dpm_control_block.dpm_end_time = UINT32_MAX;
    return;
  }

  float slack = (float)(next_arrival_time - now);

  if (slack >= DPM_IDLE_THRESHOLD_TICKS + DPM_ENTRY_LATENCY_TICKS +
                   DPM_EXIT_LATENCY_TICKS) {
    core_state->dpm_control_block.in_low_power_state = true;
    core_state->dpm_control_block.dpm_start_time = now;
    core_state->dpm_control_block.dpm_end_time = now + (uint32_t)floorf(slack);
    LOG(LOG_LEVEL_INFO,
        "Found Slack %.2f. Entering DPM for interval %u–%u ticks...", slack,
        core_state->dpm_control_block.dpm_start_time,
        core_state->dpm_control_block.dpm_end_time);
  }
}

bool power_management_try_procrastination(uint8_t core_id) {

  core_state *core_state = &core_states[core_id];

  uint32_t min_arrival_time = find_next_effective_arrival_time(core_id);

  if (min_arrival_time == UINT32_MAX) {
    LOG(LOG_LEVEL_INFO,
        "No upcoming task arrivals. Procrastination not needed.");
    return false;
  }

  float time_until_next_arrival =
      (float)(min_arrival_time - proc_state.system_time);

  if (time_until_next_arrival < DPM_ENTRY_LATENCY_TICKS +
                                    DPM_IDLE_THRESHOLD_TICKS +
                                    DPM_EXIT_LATENCY_TICKS) {

    LOG(LOG_LEVEL_INFO,
        "Next arrival too soon (%u). Procrastination not beneficial.",
        min_arrival_time);
    return false;
  }

  float min_slack = FLT_MAX;

  dvfs_level min_dvfs_level = dvfs_levels[NUM_DVFS_LEVELS - 1];

  for (uint8_t level = core_state->local_criticality_level;
       level < MAX_CRITICALITY_LEVELS; level++) {
    float slack = find_slack(core_id, level, proc_state.system_time,
                             min_dvfs_level.scaling_factor, NULL);

    if (slack < min_slack) {
      min_slack = slack;
    }
  }

  if (min_slack < DPM_ENTRY_LATENCY_TICKS + DPM_IDLE_THRESHOLD_TICKS +
                      DPM_EXIT_LATENCY_TICKS) {
    LOG(LOG_LEVEL_INFO,
        "Not enough slack (%.2f). Procrastination not beneficial.", min_slack);

    return false;
  }

  float deferrable_time = fminf(min_slack, time_until_next_arrival);

  if (core_state->running_job == NULL) {
    LOG(LOG_LEVEL_INFO, "Core is already idle, no need to procrastinate");
    return false;
  }

  LOG(LOG_LEVEL_INFO, "Preempting Job %d",
      core_state->running_job->parent_task->id);
  core_state->running_job->state = JOB_STATE_READY;

  LOCK_RQ(core_id);
  if (core_state->running_job->is_replica) {
    add_to_queue_sorted(&core_state->replica_queue, core_state->running_job);
  } else {
    add_to_queue_sorted(&core_state->ready_queue, core_state->running_job);
  }
  core_state->running_job = NULL;
  core_state->is_idle = true;
  UNLOCK_RQ(core_id);

  power_management_set_dpm_interval(
      core_id, proc_state.system_time + (uint32_t)floorf(deferrable_time));

  LOG(LOG_LEVEL_INFO, "Procrastinating for %.2f ticks", deferrable_time);

  return true;
}
