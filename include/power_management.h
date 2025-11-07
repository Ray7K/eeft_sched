#ifndef POWER_MANAGEMENT_H
#define POWER_MANAGEMENT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys_config.h>

typedef struct {
  uint32_t frequency_mhz;
  uint32_t voltage_mv;
  float scaling_factor;
} DVFSLevel;

typedef struct {
  uint32_t dpm_start_time;
  uint32_t dpm_end_time;
  bool in_low_power_state;
} DPMControlBlock;

#define NUM_DVFS_LEVELS 6

#define DPM_EXIT_LATENCY_TICKS 1
#define DPM_ENTRY_LATENCY_TICKS 1
#define DPM_IDLE_THRESHOLD_TICKS 2

extern const DVFSLevel dvfs_levels[NUM_DVFS_LEVELS];

void power_management_init(void);
float power_get_current_scaling_factor(uint16_t global_core_id);
uint8_t calc_required_dvfs_level(uint16_t global_core_id);
void power_set_dvfs_level(uint16_t global_core_id, uint8_t level_idx);
uint8_t power_get_current_dvfs_level(uint16_t global_core_id);
void power_management_set_dpm_interval(uint16_t core_id,
                                       uint32_t next_arrival_time);

#endif
