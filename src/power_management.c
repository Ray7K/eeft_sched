#include "power_management.h"
#include "lib/list.h"
#include "lib/log.h"
#include "processor.h"
#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"
#include "sys_config.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const DVFSLevel dvfs_levels[NUM_DVFS_LEVELS] = {
    {2000, 1000, 1.0f}, {1800, 900, 0.9f}, {1500, 750, 0.75f},
    {1200, 600, 0.6f},  {1000, 500, 0.5f}, {800, 300, 0.3f}};

void power_management_init(void) {
  LOG(LOG_LEVEL_INFO, "Power Management Initialized.");
}

float power_get_current_scaling_factor(uint16_t core_id) {
  return dvfs_levels[core_states[core_id].current_dvfs_level].scaling_factor;
}

uint8_t calc_required_dvfs_level(uint16_t core_id) {
  CoreState *core_state = &core_states[core_id];
  if (core_state->is_idle || core_state->running_job == NULL) {
    return NUM_DVFS_LEVELS - 1;
  }

  float min_slack = FLT_MAX;
  for (uint8_t crit_lvl = core_state->local_criticality_level;
       crit_lvl < MAX_CRITICALITY_LEVELS; crit_lvl++) {

    float remaining_wcet =
        (float)core_state->running_job->parent_task->wcet[crit_lvl] -
        core_state->running_job->executed_time;
    uint32_t virtual_deadline =
        core_state->running_job->arrival_time +
        core_state->running_job->relative_tuned_deadlines[crit_lvl];

    float running_job_slack =
        (float)(virtual_deadline - processor_state.system_time) -
        remaining_wcet;

    min_slack = running_job_slack < min_slack ? running_job_slack : min_slack;

    Job *cur;

    uint32_t tstart = processor_state.system_time;
    list_for_each_entry(cur, &core_state->ready_queue, link) {
      uint32_t tend =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];

      float slack = find_slack(core_id, crit_lvl, tstart, tend, 1.0f);
      if (slack < min_slack)
        min_slack = slack;
    }
    list_for_each_entry(cur, &core_state->replica_queue, link) {
      uint32_t tend =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];

      float slack = find_slack(core_id, crit_lvl, tstart, tend, 1.0f);
      if (slack < min_slack)
        min_slack = slack;
    }
    list_for_each_entry(cur, &core_state->pending_jobs_queue, link) {
      uint32_t tend =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      float slack = find_slack(core_id, crit_lvl, tstart, tend, 1.0f);
      if (slack < min_slack)
        min_slack = slack;
    }
    bid_entry *be;
    list_for_each_entry(be, &core_state->bid_history_queue, link) {
      cur = be->bidded_job;
      uint32_t tend =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      float slack = find_slack(core_id, crit_lvl, tstart, tend, 1.0f);
      if (slack < min_slack)
        min_slack = slack;
    }

    if (min_slack < 0) {
      return 0;
    }
  }

  float worst_remaining_wcet = (float)core_state->running_job->parent_task
                                   ->wcet[MAX_CRITICALITY_LEVELS - 1] -
                               core_state->running_job->executed_time;

  int8_t dvfs_level = 0; // Default to highest speed
  for (int8_t i = 0; i < NUM_DVFS_LEVELS; i++) {
    float scaling_factor = dvfs_levels[i].scaling_factor;
    float extra_time_needed =
        (worst_remaining_wcet / scaling_factor) - worst_remaining_wcet;

    if (extra_time_needed > min_slack) {
      break;
    }
    dvfs_level = i;
  }

  return dvfs_level;
}

void power_set_dvfs_level(uint16_t core_id, uint8_t level_idx) {
  if (level_idx < NUM_DVFS_LEVELS) {
    core_states[core_id].current_dvfs_level = level_idx;
    LOG(LOG_LEVEL_DEBUG, "DVFS level set to %u (Freq: %uMHz, Scale: %.2f)",
        level_idx, dvfs_levels[level_idx].frequency_mhz,
        dvfs_levels[level_idx].scaling_factor);
  }
}

uint8_t power_get_current_dvfs_level(uint16_t core_id) {
  return core_states[core_id].current_dvfs_level;
}

void power_management_set_dpm_interval(uint16_t core_id,
                                       uint32_t next_arrival_time) {
  CoreState *core_state = &core_states[core_id];
  uint32_t now = processor_state.system_time;

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
