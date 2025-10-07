#ifndef POWER_MANAGEMENT_H
#define POWER_MANAGEMENT_H

#include <stdint.h>
#include <sys_config.h>

typedef struct {
  uint32_t frequency_mhz;
  uint32_t voltage_mv;
  float scaling_factor;
} DVFSLevel;

#define NUM_DVFS_LEVELS 5
extern const DVFSLevel dvfs_levels[NUM_DVFS_LEVELS];

void power_management_init(void);
float power_get_current_scaling_factor(uint16_t global_core_id);
uint8_t calc_required_dvfs_level(uint16_t global_core_id);
void power_set_dvfs_level(uint16_t global_core_id, uint8_t level_idx);
uint8_t power_get_current_dvfs_level(uint16_t global_core_id);

#endif
