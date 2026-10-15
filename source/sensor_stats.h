#ifndef SENSOR_STATS_H_
#define SENSOR_STATS_H_

#include "acd_types.h"
#include "aquachemd.h"

// Time constants defined in seconds
//#define FILTER_TAU_DAILY   86400.0f   // 1 Day (24 hours)
//#define FILTER_TAU_WEEKLY  604800.0f  // 7 Days

void update_sensor_average(acd_key_t *key);
//void reset_sensor_average(sensor_stats_t *stats); 
//void reset_metrics(struct aquachemdata *acddata, uint8_t period);

bool reset_sensors_average_by_duration(struct aquachemdata *acddata, const char *duration);
bool reset_sensors_average_by_hours(struct aquachemdata *acdata, float hours);
bool reset_sensor_average(acd_key_t *key);
float parse_duration_to_seconds(const char *str);
bool duration_seconds_to_string(float seconds, char *dest, size_t dest_len);

bool tank_is_empty(acd_key_t *key);
void calculate_tank_volumes(acd_key_t *key);
void calculate_tank_volume_after_dose(acd_key_t *key, float dose_ml);
void set_tank_volume(acd_key_t *key, acd_uom_t uom, float value);

void calculate_dose_running_total(acd_key_t *key, float dose_ml);
void reset_dose_running_total(acd_key_t *key);

void set_pump_default_duration(acd_key_t *key, uint32_t default_duration);

// Flow rate / Pulse rate for an ACD_TYPE_GPIO_FLOW key, in key->uom, from the pulses counted
// since the previous call. Returns a negative value if there's no usable reading
// (first call, bad config, or clock failure).
float calculate_pulse_value(acd_key_t *key);

#define IS_RUNNING_DOSE_ENABLED(k) \
    ((k) && \
     ((k)->dose_stats.running_total_max_ml > 0.0f) && \
     (isMASKSET((k)->flags, PH_PUMP) || \
      isMASKSET((k)->flags, ORP_PUMP) || \
      isMASKSET((k)->flags, H2O_PUMP)))

#define IS_TANK_EMPTY_LOCKOUT_ENABLED(k) \
    ((k) && \
     ((k)->type == ACD_TYPE_VIR_TANK) && \
     ((k)->data.tank.total_volume > 0.0f) && \
     ((k)->data.tank.min_volume > 0.0f) && \
     isMASKSET((k)->flags, ACD_FLAG_VIRTUAL))

#define IS_TANK_VOLUME_ENABLED(k) \
    ((k) && \
     ((k)->type == ACD_TYPE_VIR_TANK) && \
     ((k)->data.tank.total_volume > 0.0f) && \
     isMASKSET((k)->flags, ACD_FLAG_VIRTUAL))

#endif