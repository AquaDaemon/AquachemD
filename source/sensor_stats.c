
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <float.h>   // FLT_MAX

#include "sensor_stats.h"
#include "acd_types.h"
#include "aquachemd.h"
#include "utils.h"

/*
#ifdef DUMMY_SENSORS
  static void dummy_pulse_advance(acd_key_t *key, uint64_t now_ns);
#endif
*/

void _update_sensor_average(sensor_stats_t *stats, float new_value/*, float tau_seconds*/);

void update_sensor_average(acd_key_t *key) {
  if (isMASKSET(key->flags, CALC_AVERAGE)) {
    _update_sensor_average(&key->stats, key->value);
  }
  /*
  if (isMASKSET(key->flags, AVG_DAILY)) {
    _update_sensor_average(&key->stats, key->value, FILTER_TAU_DAILY);
  } else if (isMASKSET(key->flags, AVG_DAILY)) {
    _update_sensor_average(&key->stats, key->value, FILTER_TAU_WEEKLY);
  }
    */
}

/**
 * A single, unified function to update any time-based rolling average.
 *
 * @param stats       Pointer to the struct tracking this specific average
 * @param new_value   The raw sensor reading (pH, ORP, etc.)
 */

 /*
void _update_sensor_average(sensor_stats_t *stats, float new_value) {
    time_t now = time(NULL);

    // Lock ONLY this specific sensor's tracking box
    pthread_mutex_lock(&stats->lock);
    
    if (stats->last_sample_time == 0) {
        stats->average = new_value;
        stats->last_sample_time = now;
        stats->max = new_value;
        stats->min = new_value;
        pthread_mutex_unlock(&stats->lock);
        return;
    }

    double delta_t = difftime(now, stats->last_sample_time);
    stats->last_sample_time = now;

    if (delta_t <= 0) {
        pthread_mutex_unlock(&stats->lock);
        return;
    }

    float alpha = 1.0f - expf(-((float)delta_t / stats->tau_seconds));
    stats->average = (alpha * new_value) + ((1.0f - alpha) * stats->average);

    if (new_value > stats->max) { stats->max = new_value; }
    else if (new_value < stats->min || stats->min <= 0.0) { stats->min = new_value; }
    

    pthread_mutex_unlock(&stats->lock);
}


void _reset_sensor_average(sensor_stats_t *stats) {
    pthread_mutex_lock(&stats->lock);
    stats->average = 0.0f;
    stats->last_sample_time = 0;
    stats->max = 0.0f;
    stats->min = 0.0f;
    pthread_mutex_unlock(&stats->lock);
}
*/

void _update_sensor_average(sensor_stats_t *stats, float new_value)
{
  pthread_mutex_lock(&stats->lock);

  stats->sample_count++;

  if (stats->sample_count == 1) {
    stats->average = new_value;
    stats->max     = new_value;
    stats->min     = new_value;
  } else {
    stats->average += (new_value - stats->average) / (float)stats->sample_count;
    if (new_value > stats->max) stats->max = new_value;
    if (new_value < stats->min) stats->min = new_value;
  }

  stats->last_sample_time = time(NULL);   // kept for display/"last updated" only, not used in the calc

  pthread_mutex_unlock(&stats->lock);
}

void _reset_sensor_average(sensor_stats_t *stats)
{
  pthread_mutex_lock(&stats->lock);
  stats->average       = 0.0f;
  stats->sample_count  = 0;
  stats->last_sample_time = 0;
  stats->max = 0.0f;
  stats->min = 0.0f;
  pthread_mutex_unlock(&stats->lock);
}

bool reset_sensor_average(acd_key_t *key) {
  if (isMASKSET(key->flags, CALC_AVERAGE)) {
    _reset_sensor_average(&key->stats);
    return true;
  }
  return false; 
}

bool reset_sensors_average_by_hours(struct aquachemdata *acdata, float hours)
{
  bool found = false;
  float tau_seconds = hours * 3600.0f;

  for (acd_key_t *key = acdata->keys->next; key != NULL; key = key->next) {
    if (isMASKSET(key->flags, CALC_AVERAGE) &&
         key->stats.tau_seconds == tau_seconds) {
      _reset_sensor_average(&key->stats);
      found = true;
      LOG(LOG_NOTICE, "Reset %s stats", key->label);
    }
  }

  if (!found) {
    LOG(LOG_ERR, "Did not find any sensor stats set to '%.0f hours' to reset", hours);
    return false; 
  }

  return true;
}

bool reset_sensors_average_by_duration(struct aquachemdata *acdata, const char *duration)
{
  LOG(LOG_DEBUG, "Attempting to reset sensor averages for duration '%s'", duration);
  bool found = false;
  float tau_seconds = parse_duration_to_seconds(duration);

  if (tau_seconds <= 0) {
    return false;
  }
  for (acd_key_t *key = acdata->keys->next; key != NULL; key = key->next) {
    if (isMASKSET(key->flags, CALC_AVERAGE) &&
         key->stats.tau_seconds == tau_seconds) {
      _reset_sensor_average(&key->stats);
      found = true;
    }
  }

  if (!found) {
    LOG(LOG_ERR, "Did not find any sensor stats set to '%s' to reset",duration);
    return false; 
  }

  return true;
}

/*
  period = AVG_DAILY or AVG_WEEKLY
*/
/*
void reset_metrics(struct aquachemdata *acdata, uint8_t period)
{
  for (acd_key_t *key = acdata->keys->next; key != NULL; key = key->next) {
    if (isMASKSET(key->flags, period)) {
      _reset_sensor_average(&key->stats);
    }
  }
}
*/

/**
 * Converts a string like "1d", "24h", "1w" into seconds as a float.
 * Returns 0.0f if the format is invalid.
 */
float parse_duration_to_seconds(const char *str) {
    if (str == NULL) return 0.0f;

    char *endptr;
    float value = strtof(str, &endptr);

    // If no number was found, return 0
    if (endptr == str) return 0.0f;

    // Get the character suffix and make it lowercase
    char unit = tolower(*endptr);

    switch (unit) {
        case 'w': // Week: 7 days
            return value * 604800.0f; 
        case 'd': // Day: 24 hours
            return value * 86400.0f;
        case 'h': // Hour: 60 minutes
            return value * 3600.0f;
        default:
            // Log an error if the suffix is unknown
            //fprintf(stderr, "Invalid duration unit: %c\n", unit);
            return 0.0f;
    }
}


/**
 * Converts a float of seconds into a human-readable duration string ("1w", "24h", "1.5d", etc.).
 * Returns true on success, false if the buffer is too small or invalid.
 */
bool duration_seconds_to_string(float seconds, char *dest, size_t dest_len) {
    if (dest == NULL || dest_len == 0) return false;

    // Handle 0 or negative values gracefully
    if (seconds <= 0.0f) {
        snprintf(dest, dest_len, "none");
        return true;
    }

    float value = 0.0f;
    char unit = '\0';

    // 1. Determine the best unit based on what it divides cleanly into
    if (fmodf(seconds, 604800.0f) == 0.0f) { // Clean weeks
        value = seconds / 604800.0f;
        unit = 'w';
    } else if (fmodf(seconds, 86400.0f) == 0.0f) { // Clean days
        value = seconds / 86400.0f;
        unit = 'd';
    } else if (seconds >= 3600.0f) { // Default fallback to hours if it's large enough
        value = seconds / 3600.0f;
        unit = 'h';
    } else {
        // Optional fallback: If it's less than an hour, you can output in hours as a decimal 
        // or add minutes ('m') / seconds ('s') here if needed.
        value = seconds / 3600.0f;
        unit = 'h';
    }

    // 2. Format the string cleanly. 
    // If it's a whole number, don't print trailing decimals (e.g., "1d" instead of "1.00d")
    int printed;
    if (fmodf(value, 1.0f) == 0.0f) {
        printed = snprintf(dest, dest_len, "%.0f%c", value, unit);
    } else {
        printed = snprintf(dest, dest_len, "%.1f%c", value, unit); // Use 1 decimal point for fractions like 1.5d
    }

    // Check if the output was truncated due to a tiny buffer
    if (printed < 0 || (size_t)printed >= dest_len) {
        return false;
    }

    return true;
}





// Returns true if the tank tracked by `key` (the ACD_TYPE_VIR_TANK child of a doser)
// has run dry. Call this right after calculate_tank_volume_after_dose() to decide
// whether the doser needs to be forced off rather than re-armed.
bool tank_is_empty(acd_key_t *key) {
  if (!key || !IS_TANK_VOLUME_ENABLED(key)) {
    return false; // no tank configured for this doser -- nothing to check
  }
  LOG(LOG_INFO, "Tank %s is %sempty!",key->label, (key->data.tank.remaining_volume > key->data.tank.min_volume)?"not ":"" );
  return key->data.tank.remaining_volume <= key->data.tank.min_volume;
}


void calculate_tank_volumes(acd_key_t *key)
{
  if ( READ_LAST_TANK_LEVEL_EVENT(key) ) {
    LOG(LOG_NOTICE, "Restored %s tank level from journal: %.1f%% (%.1f %s remaining)",
        key->label, key->data.tank.percent_full, key->data.tank.remaining_volume, uom_to_display_str(key->data.tank.uom));
  } else {
  // Blindly start at 50% for the moment.
    key->data.tank.percent_full  = 50.0f;
    key->data.tank.remaining_volume  = key->data.tank.total_volume * (key->data.tank.percent_full / 100.0f);
    key->value     = key->data.tank.percent_full;
    key->is_dirty  = true;
    LOG(LOG_NOTICE, "No tank level found for %s from journal, initializing at : %.1f%% (%.1f %s remaining)",
        key->label, key->data.tank.percent_full, key->data.tank.remaining_volume, uom_to_display_str(key->data.tank.uom));
  }

  /* NO GOOD HERE, need to move it.
  if (tank_is_empty(key->child)) {
    LOG(LOG_WARNING, "%s: dose tank is empty (%.1f / %.1f %s), forcing doser OFF -- switch back to Enabled once refilled\n", 
    key->label, key->data.tank.remaining_volume, key->data.tank.total_volume, uom_to_str(key->data.tank.uom));
    ASSIGN_IF_CHANGED(key->state, ACD_LED_OFF, acdata->is_dirty, key->is_dirty);
  }
  */
}


void set_tank_volume(acd_key_t *key, acd_uom_t uom, float value)
{
    if (!key || !IS_TANK_VOLUME_ENABLED(key)) {
        LOG(LOG_ERR, "set_tank_volume: invalid key or zero total volume for %s", key ? key->label : "NULL");
        return;
    }

    float percent = 0.0f;

    if (uom == UOM_PERCENT) {
      percent = value;
    } else {
      percent = (value / key->data.tank.total_volume) * 100.0f;
    }

    // Clamp percentage bounds
    if (percent < 0.0f)   percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;

    // Apply updates to node state
    key->data.tank.percent_full = percent;
    key->data.tank.remaining_volume    = key->data.tank.total_volume * (percent / 100.0f);
    key->value                  = percent;
    key->is_dirty               = true;

    LOG_TANK_LEVEL_SET_EVENT(key);

    LOG(LOG_INFO, "%s: tank volume set to %.1f%% (%.1f %s remaining)", 
        key->label, percent, key->data.tank.remaining_volume, uom_to_str(key->data.tank.uom));
}


// Apply a dose (in ml) taken from a chemical tank -- call this whenever a
// dosing pump actually runs against a tank you're tracking by volume
// rather than a physical level sensor, so the tank's level stays in sync.
void calculate_tank_volume_after_dose(acd_key_t *key, float dose_ml)
{
  if (!key || !IS_TANK_VOLUME_ENABLED(key)) {
    //LOG(LOG_ERR, "calculate_tank_volume_after_dose: key is NULL");
    // Tank hasn't been setup for this pump
    return;
  }
  /*
  if (key->data.tank.total_volume <= 0.0f) {
    LOG(LOG_ERR, "%s: total_volume is not set (%.1f) — cannot calculate tank level",
        key->label, key->data.tank.total_volume);
    return;
  }
  */
  if (dose_ml <= 0.0f) {
    return;   // nothing dosed (or bad input) -- leave the tank level untouched
  }

  float dose_in_uom = 0.0f;

  if (key->data.tank.uom == UOM_GALLONS)
    dose_in_uom = dose_ml / ML_PER_GALLON;
  else 
    dose_in_uom = dose_ml / ML_PER_LITER;

  float new_remaining = key->data.tank.remaining_volume - dose_in_uom;

  // Clamp so a dose that (per our accounting) would drain more than what's
  // left doesn't go negative -- e.g. a refill that was never logged, or
  // rounding drift accumulated over many small doses.
  if (new_remaining < 0.0f) {
    LOG(LOG_WARNING, "%s: dose of %.2f ml exceeds tracked remaining volume (%.2f %s) — clamping to 0",
        key->label, dose_ml, key->data.tank.remaining_volume, uom_to_str(key->data.tank.uom));
    new_remaining = 0.0f;
  }

  key->data.tank.remaining_volume = new_remaining;

  float percent = (new_remaining / key->data.tank.total_volume) * 100.0f;

  // remaining is already floor-clamped above; guard the top too in case
  // total_volume is ever understated relative to an actual refill.
  if (percent > 100.0f) percent = 100.0f;
  if (percent < 0.0f)   percent = 0.0f;

  key->data.tank.percent_full = percent;   // no rounding needed now that this is a float field
  key->value    = percent;
  key->is_dirty = true;

  LOG_TANK_LEVEL(key);

  LOG(LOG_INFO, "%s: dosed %.2f ml, remaining %.2f / %.1f %s (%.1f%% full)",
      key->label, dose_ml, key->data.tank.remaining_volume,
      key->data.tank.total_volume, uom_to_str(key->data.tank.uom), key->data.tank.percent_full);
}

void set_pump_default_duration(acd_key_t *key, uint32_t default_duration)
{
    if (!key) return;

    uint32_t max_duration = 0;

    if (isMASKSET(key->flags, PH_PUMP)) {
        max_duration = _acdconfig_.ph_max_dose_time;
    } else if (isMASKSET(key->flags, ORP_PUMP)) {
        max_duration = _acdconfig_.orp_max_dose_time;
    } else if (isMASKSET(key->flags, H2O_PUMP)) {
        max_duration = _acdconfig_.h2o_max_dose_time;
    } else if (key->type == ACD_TYPE_GPIO_OUTPUT) {
        max_duration = _acdconfig_.switch_max_runtime;
    } else {
        LOG(LOG_ERR, "set_pump_default_duration: unknown pump flags for %s", key->label ? key->label : "NULL");
        return;
    }

    if (default_duration > max_duration) {
        key->default_duration = max_duration;
        LOG(LOG_WARNING, "Pump %s duration can't be set to %u, using max (%u)\n", 
            key->label, default_duration, max_duration);
    } else {
        key->default_duration = default_duration;
    }
    key->is_dirty = true;
}

void calculate_dose_running_total(acd_key_t *key, float dose_ml)
{
  if (!key || !IS_RUNNING_DOSE_ENABLED(key)) {
    return;
  }

  if (dose_ml > 0.0f && isfinite(dose_ml)) {
    if (key->dose_stats.running_total_ml > FLT_MAX - dose_ml) {
        // Would overflow - clamp instead of wrapping/going to inf
        key->dose_stats.running_total_ml = FLT_MAX;
        LOG(LOG_WARNING, "Running total overflow clamp for pump %s, please configure a reset\n", key->label);
    } else {
        key->dose_stats.running_total_ml += dose_ml;
    }
  }
  LOG(LOG_INFO, "Running total for %s is %f",key->label, key->dose_stats.running_total_ml);

  key->is_dirty = true;
}

void reset_dose_running_total(acd_key_t *key)
{
  if (!key || !IS_RUNNING_DOSE_ENABLED(key)) {
    LOG(LOG_INFO, "Can't reset running total for %s, Running total is not enabled\n",key->label);
    return;
  }
  
  LOG(LOG_INFO, "Reset Running total for %s\n",key->label);

  key->dose_stats.running_total_ml = 0.0f;
  key->is_dirty = true;
}

#define PULSE_MIN_WINDOW_NS 500000000ULL   // 0.5 s: shorter than this, one pulse swamps the reading

float calculate_pulse_value(acd_key_t *key)
{
  if (!key || key->type != ACD_TYPE_GPIO_PULSE) return -1.0f;

  gpio_pulse_t *f = &key->data.gpio_pulse;

  if (f->k_factor <= 0.0f) {
    LOG(LOG_ERR, "%s: k_factor not set (%.3f), cannot calculate\n", key->label, f->k_factor);
    return -1.0f;
  }

  uint64_t now_ns = mono_now_ns();
  if (now_ns == 0) return -1.0f;

/*  Uncomment if you want dummy test for this */
#ifdef GPIO_DUMMY_SENSORS
  dummy_pulse_advance(key, now_ns);
#endif


  uint64_t count = __atomic_load_n(&f->pulses, __ATOMIC_RELAXED);

  if (f->last_ns == 0) {            // first call: baseline only
    GPDBG("pulse[%s]: baseline set, count=%llu", key->label, (unsigned long long)count);
    f->last_pulses = count;
    f->last_ns     = now_ns;
    return -1.0f;
  }

  // These can't happen if only this function and the monitor thread touch the
  // struct, so a hit means something else is writing it.
  if (now_ns < f->last_ns) {
    GPDBG("pulse[%s]: last_ns is %.3f s IN THE FUTURE (now=%llu last=%llu) - struct overwritten?",
         key->label, (double)(f->last_ns - now_ns) / 1e9,
         (unsigned long long)now_ns, (unsigned long long)f->last_ns);
    return -1.0f;
  }
  if (count < f->last_pulses) {
    GPDBG("pulse[%s]: counter WENT BACKWARDS (count=%llu last=%llu) - reset or clobbered",
         key->label, (unsigned long long)count, (unsigned long long)f->last_pulses);
    f->last_pulses = count;
    f->last_ns     = now_ns;
    return -1.0f;
  }

  uint64_t dt_ns = now_ns - f->last_ns;
  // A window this short holds 0 or 1 pulses, which reads as 0 or a huge spike.  An extra
  // poll can be forced right after a regular one (e.g. when a condition goes ON), so skip
  // it and DON'T advance the baseline: the next regular poll measures the whole period.
   if (dt_ns < PULSE_MIN_WINDOW_NS) {
    GPDBG("pulse[%s]: window %.3f s too short, returning previous value %.3f", key->label, (double)dt_ns / 1e9, f->last_value);
    return f->last_value;
  }

  uint64_t delta = count - f->last_pulses;
  float hz = (float)delta * 1e9f / (float)dt_ns;

#ifdef GPIO_PULSE_DEBUG
  uint64_t prev_pulses = f->last_pulses;
#endif

  f->last_pulses = count;
  f->last_ns     = now_ns;

  float value;
  switch (key->uom) {
    case UOM_RPM:             value = hz * 60.0f / f->k_factor; break;
    case UOM_GALLONS_PER_MIN: value = (hz / f->k_factor) * (ML_PER_LITER / ML_PER_GALLON); break;
    case UOM_LITERS_PER_MIN:  value = hz / f->k_factor; break;
    default:
      LOG(LOG_ERR, "%s: unit not supported for pulse input\n", key->label);
      return -1.0f;
  }

  GPDBG("pulse[%s] key=%p cnt@%p tid=%lu dt=%.3fs count=%llu prev=%llu delta=%llu hz=%.3f k=%.3f uom=%d -> %.3f%s",
       key->label, (void *)key, (void *)&f->pulses, (unsigned long)pthread_self(),
       (double)dt_ns / 1e9, (unsigned long long)count, (unsigned long long)prev_pulses,
       (unsigned long long)delta, hz, f->k_factor, key->uom, value,
       delta == 0 ? "   <-- ZERO PULSES IN WINDOW" : "");

  f->last_value = value;
  return value;
}


/*
#ifdef DUMMY_SENSORS
// Stand-in for the gpio_monitor thread. Adds the pulses a real sensor would have
// produced since the last poll, at a rate chosen to give a plausible reading in the
// key's own unit, so k_factor and uom handling are exercised too.
static void dummy_pulse_advance(acd_key_t *key, uint64_t now_ns)
{
  gpio_pulse_t *f = &key->data.gpio_pulse;
  if (f->last_ns == 0 || now_ns <= f->last_ns) return;       // baseline call: nothing to add

  float hz;
  switch (key->uom) {
    case UOM_RPM:             hz = (1200.0f + dummy_drift(50.0f)) * f->k_factor / 60.0f; break;
    case UOM_GALLONS_PER_MIN: hz = (3.0f + dummy_drift(0.5f)) * f->k_factor * (ML_PER_GALLON / ML_PER_LITER); break;
    default:                  hz = (12.0f + dummy_drift(2.0f)) * f->k_factor; break;   // L/min
  }

  // Optional: no flow for 1 minute in every 4, to exercise conditions / zero handling
  // if ((time(NULL) / 60) % 4 == 3) hz = 0.0f;

  uint64_t add = (uint64_t)((double)hz * (double)(now_ns - f->last_ns) / 1e9 + 0.5);
  __atomic_fetch_add(&f->pulses, add, __ATOMIC_RELAXED);
}
#endif // DUMMY_SENSORS
*/