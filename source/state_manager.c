



#include "aquachemd.h"
#include "utils.h"
#include "acd_timer.h"
#include "net_services.h"
#include "sensor_stats.h"

// COME BACK AND FIGURE OUT WHY I NEED TO INCLUDE THIS HERE, IT'S CAUSING A CIRCULAR DEPENDENCY ISSUE
//#include "state_manager.h"
void calculate_tank_volume_after_dose(acd_key_t *key, float dose_ml);
// COME BACK AND FIGURE OUT WHY I NEED TO INCLUDE THIS HERE, IT'S CAUSING A CIRCULAR DEPENDENCY ISSUE




bool set_cond_state(struct aquachemdata *acdata, acd_key_t *cond, acd_state_t state);
bool set_key_state(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state);

void check_master(struct aquachemdata *acdata);

void set_pump_off_due_to_tank_empty(struct aquachemdata *acdata, acd_key_t *key);


/*
* Codify the table from adc_types.h that describes which state transitions are allowed for each key category.
* This is used to enforce the rules in state_change_request() and state_change_request_extended().
*/



typedef enum { 
  KC_MASTER, 
  KC_PUMP_GLOBAL,
  KC_PUMP_LOCAL, 
  KC_OUTPUT_ALLOW, 
  KC_OUTPUT_GLOBAL, 
  KC_OUTPUT_LOCAL, 
  KC_CONDITION, 
  KC_SENSOR_ALLOW, 
  KC_SENSOR_GLOBAL, 
  KC_SENSOR_LOCAL, 
  KC_COUNT 
} key_category_t;

#define SCOPE_A   (1 << ACD_ACTION_ALLOW)
#define SCOPE_L   (1 << ACD_ACTION_LIMIT)
#define SCOPE_B   (1 << ACD_ACTION_BLOCK)
#define SCOPE_ALL (SCOPE_A | SCOPE_L | SCOPE_B)

// Directly mirrors the STATE PERMISSIVENESS MATRIX comment -- when that
// table changes, this array changes, and nothing else needs touching.
static const uint8_t transition_permission[KC_COUNT][5] = {
  //                     OFF        ON               ENABLED          DISABLED         DELAY
  [KC_MASTER]        = { SCOPE_ALL, SCOPE_ALL,       SCOPE_L|SCOPE_B, 0,               0 },
  [KC_CONDITION]     = { SCOPE_ALL, SCOPE_ALL,       0,               0,               SCOPE_ALL },

  [KC_PUMP_GLOBAL]   = { SCOPE_ALL, SCOPE_A,         SCOPE_A,         SCOPE_L|SCOPE_B, 0 },  // Allow dosing while GLOBAL
  [KC_PUMP_LOCAL]    = { SCOPE_ALL, SCOPE_A|SCOPE_L, SCOPE_A|SCOPE_L, SCOPE_B,         0 },  // Allow dosing while LOCAL & GLOBAL

  [KC_OUTPUT_ALLOW]  = { SCOPE_ALL, SCOPE_ALL,       0,               0,               0 },  // on/off only, ever -- ignores all interlocks
  [KC_OUTPUT_GLOBAL] = { SCOPE_ALL, SCOPE_A,         0,               SCOPE_L|SCOPE_B, 0 },  // respects Limit and Block
  [KC_OUTPUT_LOCAL]  = { SCOPE_ALL, SCOPE_A|SCOPE_L, 0,               SCOPE_B,         0 },  // only respects a hard interlock

  //[KC_SENSOR_ALLOW] =  { SCOPE_ALL, SCOPE_ALL,       0,               0,               0 },
  //[KC_SENSOR_GLOBAL] = { SCOPE_ALL, SCOPE_A|SCOPE_L, 0,               0,               0 },
  //[KC_SENSOR_LOCAL]  = { SCOPE_ALL, SCOPE_ALL,       SCOPE_ALL,       SCOPE_B,         0 },

  [KC_SENSOR_ALLOW]  = { SCOPE_ALL, SCOPE_ALL,       0,                 0,              0 },  // ignores scope entirely
  [KC_SENSOR_LOCAL]  = { SCOPE_ALL, SCOPE_ALL,       0,                 0,              0 },  // always reads, unaffected by anything
  [KC_SENSOR_GLOBAL] = { SCOPE_ALL, SCOPE_A|SCOPE_L, 0,                 SCOPE_B,        0 },  // stops only at the hard interlock

  //[KC_SENSOR_ALLOW]  = { SCOPE_ALL, SCOPE_ALL,       0,               0,               0 }, // ignores everything
  //[KC_SENSOR_GLOBAL] = { SCOPE_ALL, SCOPE_A,         0,               SCOPE_L|SCOPE_B, 0 }, // respects everything, matches pump/output "Global"
  //[KC_SENSOR_LOCAL]  = { SCOPE_ALL, SCOPE_A|SCOPE_L, 0,               0,               0 }, // respects hard interlock only, matches pump/output "Local"
  
};

static key_category_t classify_key(acd_key_t *key) {
  if (key->type == ACD_TYPE_MASTER)      return KC_MASTER;
  if (IS_CONDITION(key->type))           return KC_CONDITION;
  if (IS_PUMP(key->type))                return (key->scope == ACD_SCOPE_GLOBAL) ? KC_PUMP_GLOBAL : KC_PUMP_LOCAL;
  if (key->type == ACD_TYPE_GPIO_OUTPUT) return (key->scope == ACD_SCOPE_ALLOW) ? KC_OUTPUT_ALLOW : (key->scope == ACD_SCOPE_GLOBAL) ? KC_OUTPUT_GLOBAL : KC_OUTPUT_LOCAL;
  if (IS_INPUT(key->type))               return (key->scope == ACD_SCOPE_ALLOW) ? KC_SENSOR_ALLOW : (key->scope == ACD_SCOPE_GLOBAL) ? KC_SENSOR_GLOBAL : KC_SENSOR_LOCAL;
  return KC_COUNT; // VIR_TANK only ????? What about Virtual keys??????
}

static bool is_transition_permitted(struct aquachemdata *acdata, acd_key_t *key, acd_state_t requested) {
  if (IS_INPUT(key->type)) {
    LOG(LOG_WARNING, "%s: state changes on sensors/inputs are internal-only, rejecting external request", key->label);
    return false;
  }
  key_category_t cat = classify_key(key);
  if (cat == KC_COUNT || requested > ACD_LED_DELAY) return false;
  return (transition_permission[cat][requested] & (1 << acdata->keys->scope)) != 0;
}

// Not routed through state_change_request() -- no request/reject semantics here,
// just "what should this sensor's read-state be right now given its scope and
// the current master scope." Shares the same table data as the pump/output
// permission checks, so the two can't silently drift apart from each other.
/*
bool should_sensor_read(struct aquachemdata *acdata, acd_key_t *key) {
  key_category_t cat = (key->scope == ACD_SCOPE_GLOBAL) ? KC_SENSOR_GLOBAL : KC_SENSOR_LOCAL;
  return (transition_permission[cat][ACD_LED_ON] & (1 << get_master(acdata)->scope)) != 0;
}*/
bool should_sensor_read(struct aquachemdata *acdata, acd_key_t *key) {
  key_category_t cat = classify_key(key);
  if (cat == KC_COUNT) return false;
  return (transition_permission[cat][ACD_LED_ON] & (1 << get_master(acdata)->scope)) != 0;
}

// Given a category and the master's current scope, returns the single correct
// resting state (ENABLED or DISABLED) a device should sit in when it's not
// actively ON. Never returns ON/OFF -- those are earned by running, or set
// explicitly elsewhere (user request, tank-empty lockout).
/*
static acd_state_t resting_state_for_scope(key_category_t cat, acd_scope_t scope) {
  uint8_t mask = 1 << scope;
  if (transition_permission[cat][ACD_LED_ENABLED] & mask) return ACD_LED_ENABLED;
  return ACD_LED_DISABLED;
}
*/

// Pump: three real resting states depending on scope.
static acd_state_t pump_resting_state(key_category_t cat, acd_scope_t scope) {
  uint8_t mask = 1 << scope;
  return (transition_permission[cat][ACD_LED_ENABLED] & mask) ? ACD_LED_ENABLED : ACD_LED_DISABLED;
}

// Plain switch: no ENABLED state exists at all -- resting state is just
// whichever of OFF/DISABLED matches current permission to run.
static acd_state_t output_resting_state(key_category_t cat, acd_scope_t scope) {
  uint8_t mask = 1 << scope;
  return (transition_permission[cat][ACD_LED_ON] & mask) ? ACD_LED_OFF : ACD_LED_DISABLED;
}


/*
static key_category_t classify_key(acd_key_t *key) {
  switch (key->type) {
    case ACD_TYPE_MASTER:     return KC_MASTER;
    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:    return KC_PUMP;
    case ACD_TYPE_GPIO_OUTPUT: return (key->scope == ACD_SCOPE_GLOBAL) ? KC_OUTPUT_GLOBAL : KC_OUTPUT_LOCAL;
    case ACD_TYPE_MQTT_COND:
    case ACD_TYPE_GPIO_COND:  return KC_CONDITION;
    default:                  return KC_COUNT; // sentinel: unsupported type
  }
}

static bool is_transition_permitted(struct aquachemdata *acdata, acd_key_t *key, acd_state_t requested) {
  key_category_t cat = classify_key(key);
  if (cat == KC_COUNT || requested > ACD_LED_DELAY) return false;
  return (transition_permission[cat][requested] & (1 << get_master(acdata)->scope)) != 0;
}
*/









void devices_emergency_stop()
{
  LOG(LOG_INFO,"Checking any pumps to to turned off\n");
  // Turn off any pumps
  for (acd_key_t *curr = _acdconfig_.keys; curr != NULL; curr = curr->next) {
    if (curr->type == ACD_TYPE_GPIO_PMP) {
      if (relay_is_on(&curr->data.gpio)) {
        LOG(LOG_NOTICE,"Pump %s is on, turning off\n", curr->label);
        if (relay_off(&curr->data.gpio) == GPIO_ERROR) {
          LOG(LOG_ERR,"GPIO Failed, manually turn %s off\n", curr->label);
        }
      }
    }
  }
}

acd_scope_t check_master_action(struct aquachemdata *acdata) {
  if (get_master(acdata)->scope == ACD_ACTION_BLOCK || get_master(acdata)->state == ACD_LED_OFF){
    return ACD_ACTION_BLOCK;
  } else if (get_master(acdata)->scope == ACD_ACTION_LIMIT){
    return ACD_ACTION_LIMIT;
  }
  return ACD_ACTION_ALLOW;
}

void sensor_read_error(struct aquachemdata *acdata, acd_key_t *key) {
  if (key->err_cnt++ >= SENSOR_FAULT_THRESHOLD) {
    LOG(LOG_ERR, "Sensor %s too many read errors, removing from rotation", key->label);
    //LOG(LOG_WARNING, "Add code to remove key in sensor_error(), will need to also modify main function for loop since key->next will be null on return", key->label);
    setMASK(key->flags,  ACD_FLAG_FAULTED);
    //SET_IF_CHANGED(key->state, ACD_LED_DISABLED, acdata->is_dirty);
    set_key_state(acdata, key, ACD_LED_DISABLED);
    key->err_cnt = 0;
  } else {
    //SET_IF_CHANGED(key->state, ACD_LED_OFF, acdata->is_dirty);
    set_key_state(acdata, key, ACD_LED_OFF);
  }
}


float get_sensor_value(struct aquachemdata *acdata, acd_type_t type)
{
// ACD_TYPE_EZO_ORP
// ACD_TYPE_EZO_PH
  for (acd_key_t *key = acdata->keys->next; key != NULL; key = key->next) {
    if( key->type == type) {
      return key->value;
    }
  }
  return 0;
}

uint32_t get_ph_dose_time(float current_ph) {
  if (_acdconfig_.ph_step_count <= 0) {
    return _acdconfig_.ph_default_dose_time;
  }
  // Iterate through the ranges defined in config
  for (int i = 0; i < _acdconfig_.ph_step_count; i++) {
    if (current_ph >= _acdconfig_.ph_steps[i].threshold) {
      return _acdconfig_.ph_steps[i].seconds;
    }
  }

  //return 0; // Default if under minimum
  // If we reach here, the pH is lower than the lowest threshold.
  // Return the seconds from the very last entry in the list (the minimum dose).
  return _acdconfig_.ph_steps[_acdconfig_.ph_step_count - 1].seconds;
}


uint32_t get_orp_dose_time(float current_orp) { // Changed name to current_orp for clarity
  if (_acdconfig_.orp_step_count <= 0) {
    return _acdconfig_.orp_default_dose_time;
  }

  // Iterate through ranges (Sorted Low to High: e.g., 600, 650, 700)
  for (int i = 0; i < _acdconfig_.orp_step_count; i++) {
    // If current ORP is BELOW or AT the threshold, we need that dose
    if (current_orp <= _acdconfig_.orp_steps[i].threshold) {
      return _acdconfig_.orp_steps[i].seconds;
    }
  }

  // If we are ABOVE the highest threshold (pool is clean), return the last/lowest dose
  return _acdconfig_.orp_steps[_acdconfig_.orp_step_count - 1].seconds;
}

uint32_t caculate_dose_time(struct aquachemdata *acdata, acd_key_t *key) {
  // Find latest ph and orp values.
  uint32_t runtime;

  if (isMASKSET(key->flags, PH_PUMP)) {
    float current_ph = get_sensor_value(acdata, ACD_TYPE_EZO_PH);
    runtime = get_ph_dose_time(current_ph);
    return runtime;
  } else if (isMASKSET(key->flags, ORP_PUMP)) {
    float current_orp = get_sensor_value(acdata, ACD_TYPE_EZO_ORP);
    runtime = get_orp_dose_time(current_orp);
    return runtime;
  } else if (isMASKSET(key->flags, H2O_PUMP)) {
    return key->default_duration;
  } else if (key->default_duration > 0) {
    return key->default_duration;
  }

  return 0;
}

uint32_t get_pump_runtime(struct aquachemdata *acdata, acd_key_t *key) {
  if (!key) return 0;

  uint32_t runtime = caculate_dose_time(acdata, key);

  if (isMASKSET(key->flags, PH_PUMP)) {
    float current_ph = get_sensor_value(acdata, ACD_TYPE_EZO_PH);
    LOG(LOG_NOTICE, "Dosing time calculated for %s at %u(s) (pH %.2f)", key->label, runtime, current_ph);
  } else if (isMASKSET(key->flags, ORP_PUMP)) {
    float current_orp = get_sensor_value(acdata, ACD_TYPE_EZO_ORP);
    LOG(LOG_NOTICE, "Dosing time calculated for %s at %u(s) (ORP %.1f)", key->label, runtime, current_orp);
  } else if (isMASKSET(key->flags, H2O_PUMP)) {
    LOG(LOG_NOTICE, "Dosing time calculated for %s at %u(s)", key->label, runtime);
  } else {
    LOG(LOG_ERR, "Unable to get pump runtime for %s", key->label ? key->label : "NULL");
  }

  return runtime;
}

// This is a request from MQTT/WebSocket/Web, NOT for a system change

void log_pump_null_event(acd_key_t *key, int value) {
  //LOG(LOG_WARNING, "Need to queue this pump request, not implimented yet\n"); 
  LOG_PUMP_EVENT(key, 0, 0, 0);
  post_dosing_event(key, 0, 0);
}

void turn_gpio_switch_on(struct aquachemdata *acdata, acd_key_t *key, uint32_t duration_sec) {

  if (key->type != ACD_TYPE_GPIO_OUTPUT) {
    LOG(LOG_ERR, "Add Code in turn_gpio_switch_on.c - turn_gpio_switch_on(), %s is not a GPIO_SWITCH", key->label);
    return;
  }

  if (isMASKSET(key->flags, TIMER_ACTIVE) && key->state == ACD_LED_ON) {
    LOG(LOG_INFO, "Resetting timer for %s to %d\n",key->label, duration_sec);
    start_timer(acdata, key, 0, duration_sec);
    return;
  }

  LOG(LOG_INFO, "Turning on %s\n", key->label);

  relay_on(&key->data.gpio);
  key->ison = relay_is_on(&key->data.gpio);

  if (key->ison) {
    set_key_state(acdata, key, ACD_LED_ON);
  } else {
    LOG(LOG_ERR, "Failed to turn on %s\n", key->label);
  }

  if (duration_sec > 0) {
    start_timer(acdata, key, 0, duration_sec);
  }
}

void turn_gpio_switch_off(struct aquachemdata *acdata, acd_key_t *key, acd_state_t desired_state) {

  if (key->type != ACD_TYPE_GPIO_OUTPUT) {
    LOG(LOG_ERR, "Add Code in turn_gpio_switch_on.c - turn_gpio_switch_on(), %s is not a GPIO_SWITCH", key->label);
    return;
  }

  LOG(LOG_INFO, "Turning off %s\n", key->label);

  relay_off(&key->data.gpio);
  key->ison = relay_is_on(&key->data.gpio);
  
  //ASSIGN_IF_CHANGED(key->state, ACD_LED_ENABLED, acdata->is_dirty, key->is_dirty);
  set_key_state(acdata, key, desired_state);
  
  clear_timer(acdata, key);
  key->value = 0;
}

void turn_pump_on(struct aquachemdata *acdata, acd_key_t *key, uint32_t duration_sec) {
  
  int runtime = duration_sec<=0?get_pump_runtime(acdata,key):duration_sec;

  if (runtime <= 0) {
    LOG(LOG_NOTICE, "Pump %s runtime is %d, not turning on", key->label, runtime);
    if (_acdconfig_.log_zerorun_pump_events) {
      LOG_PUMP_EVENT(key, 0, 0, 0);
      post_dosing_event(key, 0, 0);
    }
    return;
  }

  // User-configured safety cap: total ml dosed this period (e.g. per day) must stat under running_total_max_ml. This only warns and skips THIS dose -- it does not
  // touch key->state, so the doser stays fully enabled and will dose normally again as soon as running_total_ml resets for the next period. A runaway sensor reading
  if (key->dose_stats.running_total_max_ml > 0.0f &&
      key->dose_stats.running_total_ml >= key->dose_stats.running_total_max_ml) {
    LOG(LOG_WARNING, "%s: dosing limit reached for this period (%.1f / %.1f ml) -- skipping dose until reset",
        key->label, key->dose_stats.running_total_ml, key->dose_stats.running_total_max_ml);
    return;
  }

  // Sanity check
  if (isMASKSET(key->flags, PH_PUMP) && runtime > _acdconfig_.ph_max_dose_time) {
    LOG(LOG_WARNING, "Pump %s request runtime %d is greater than maximum, reset to %d", key->label, runtime, _acdconfig_.ph_max_dose_time);
    runtime = _acdconfig_.ph_max_dose_time;
  } else if (isMASKSET(key->flags, ORP_PUMP) && runtime > _acdconfig_.orp_max_dose_time) {
    LOG(LOG_WARNING, "Pump %s request runtime %d is greater than maximum, reset to %d", key->label, runtime, _acdconfig_.orp_max_dose_time);
    runtime = _acdconfig_.orp_max_dose_time;
  } else if (isMASKSET(key->flags, H2O_PUMP) && runtime > _acdconfig_.h2o_max_dose_time) {
    LOG(LOG_WARNING, "Pump %s request runtime %d is greater than maximum, reset to %d", key->label, runtime, _acdconfig_.h2o_max_dose_time);
    runtime = _acdconfig_.h2o_max_dose_time;
  }

  if (isMASKSET(key->flags, TIMER_ACTIVE) && key->state == ACD_LED_ON) {
    LOG(LOG_INFO, "Resetting timer for %s to %d\n",key->label, runtime);
    start_timer(acdata, key, 0, runtime);
    return;
  }

  // NSF, We turn doser off after dose, Do we want to allow manual overide?
  // If yes don;t use below code
  /*
  if ( tank_is_empty(key->child)) {
    LOG(LOG_ERR, "%s: is below min volume (%.1f / %.1f %s) -- skipping dose until filled",
        key->label, key->child->data.tank.remaining_volume, key->child->data.tank.total_volume, uom_to_str(key->child->data.tank.uom));
    return;
  }*/

  if (isMASKSET(key->flags, PH_PUMP)) {
    key->value = get_sensor_value(acdata, ACD_TYPE_EZO_PH);
  } else if (isMASKSET(key->flags, ORP_PUMP)) {
    key->value = get_sensor_value(acdata, ACD_TYPE_EZO_ORP);
  }

  LOG(LOG_INFO, "Turning on %s\n", key->label);
    
  if (key->type == ACD_TYPE_GPIO_PMP) {
    relay_on(&key->data.gpio);
    key->ison = pump_is_on(&key->data.gpio);
  } else if (key->type == ACD_TYPE_EZO_PMP) { // EZO pump needs to be set from different thread
    // Going to pump at a known ML rate and time, then stop early.
    // Since EZO is only in minutes, we need to change from ml/s and also round up to next minute.
    if (pump_dose_rate(&key->data.ezo, key->dose_stats.flow_rate * 60, (runtime + 59) / 60) != EZO_SUCCESS) {
      LOG(LOG_ERR, "EZO Pump '%s' failed to turn on");
      return;
    }
  } else {
    LOG(LOG_ERR, "Add Code in state_manage.c - turn_pump_on()");
  }

  ASSIGN_IF_CHANGED(key->state , ACD_LED_ON, acdata->is_dirty, key->is_dirty);
  
  start_timer(acdata, key, 0, runtime);
}

void turn_pump_off(struct aquachemdata *acdata, acd_key_t *key, acd_state_t desired_state) {
  time_t start = get_timer_started_at(key);
  time_t now = time(NULL);
  float dose_ml = 0;

  LOG(LOG_INFO, "Turning off %s\n", key->label);

  if (key->type == ACD_TYPE_GPIO_PMP) {
    relay_off(&key->data.gpio);
    key->ison = pump_is_on(&key->data.gpio);
  } else if (key->type == ACD_TYPE_EZO_PMP) { // EZO pump needs to be set from different thread
    pump_dose_status_t pump_reading = pump_get_dose_status(&key->data.ezo);
    if (pump_reading.status != EZO_SUCCESS) {
      LOG(LOG_ERR, "Reading pump %s state - turn_pump_off()\n",key->label);
    }
    // Need to destinguish off from scheduler vs off from some force.
    if (true) { // If pump is force off (ie not from scheduler)
        if (pump_reading.is_pumping) { pump_stop(&key->data.ezo); }
        dose_ml = pump_get_dispensed_volume(&key->data.ezo);  
    } else {
      // Wait for off
    }
  } else {
    LOG(LOG_ERR, "Add Code in state_manage.c - turn_pump_off()");
  }

  // Calculate actual runtime and log the event
  if (start > 0) {
    uint32_t actual_runtime = (uint32_t)(now - start);
    if (key->type != ACD_TYPE_EZO_PMP){ dose_ml = actual_runtime * key->dose_stats.flow_rate;}
    LOG_PUMP_EVENT(key, actual_runtime, key->value, dose_ml);
    post_dosing_event(key, actual_runtime, dose_ml);
    calculate_tank_volume_after_dose(key->child, dose_ml);
    calculate_dose_running_total(key, dose_ml);
  }

  ASSIGN_IF_CHANGED(key->state, desired_state, acdata->is_dirty, key->is_dirty);
  
  clear_timer(acdata, key);
  key->value = 0;

  if (tank_is_empty(key->child)) {
    //LOG(LOG_WARNING, "%s: dose tank is empty (%.1f / %.1f %s), forcing doser OFF -- switch back to Enabled once refilled\n", 
    //    key->label, key->child->data.tank.remaining_volume, key->child->data.tank.total_volume, uom_to_str(key->child->data.tank.uom));
    //ASSIGN_IF_CHANGED(key->state, ACD_LED_OFF, acdata->is_dirty, key->is_dirty);
    set_pump_off_due_to_tank_empty(acdata, key);
  }
}


void check_gpio_output_state(struct aquachemdata *acdata, acd_key_t *key) {
  int current = relay_is_on(&key->data.gpio);

  if (current >= 0 && current != key->ison) {
    LOG(LOG_WARNING, "%s %s changed externally\n", key->type == ACD_TYPE_GPIO_PMP ? "Pump" : "GPIO Output", key->label);
    key->ison = current;

    acd_state_t off_target = IS_PUMP(key->type)
      ? pump_resting_state(classify_key(key), acdata->keys->scope)
      : output_resting_state(classify_key(key), acdata->keys->scope);

    set_key_state(acdata, key, key->ison ? ACD_LED_ON : off_target);
    LOG(LOG_NOTICE, "Output %s, GPIO %d is now %s/%s/%s", key->label, key->data.gpio.pin, (relay_is_on(&key->data.gpio) ? "ON" : "OFF"), acd_state_to_str(key->state), key->ison ? "ON" : "OFF");
  }
}

bool _state_change_request(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state, uint32_t value);

bool state_change_request(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state)
{
  return _state_change_request(acdata, key, state, 0);
}

bool state_change_request_extended(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state, uint32_t value)
{
  return _state_change_request(acdata, key, state, value);
}

bool _state_change_request(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state, uint32_t value)
{
  LOG(LOG_INFO, "Request to set %s to %s (current state: %s) with value %d (master scope %s, master state %s)", key->label, acd_state_to_str(state), acd_state_to_str(key->state), value, acd_scope_to_str(get_master(acdata)->scope), acd_state_to_str(get_master(acdata)->state));

  // If MQTT requested a change, we need to send back the new state regardless of if it changed ot not, so simply force that.
  // We need to pass a value here to know who requested the change, MQTT / WebSocket / API
  // Below is a hack for the moment
  acdata->is_dirty = true;
  key->is_dirty = true;

  if (!is_transition_permitted(acdata, key, state) || ( key->type != ACD_TYPE_MASTER && get_master(acdata)->state == ACD_LED_OFF)) {
    LOG(LOG_WARNING, "%s is %s, can't turn %s (master scope %s)", key->label, acd_state_to_str(key->state), acd_state_to_str(state), acd_scope_to_str(get_master(acdata)->scope));
    // Need to log any pump requests that are blocked by master scope, so we can see record the 0 value.
    if (_acdconfig_.log_zerorun_pump_events && state == ACD_LED_ON && IS_PUMP(key->type)) {
      log_pump_null_event(key, value);
    }
    return false;
  }

  switch(key->type) {
    case ACD_TYPE_MASTER:
      // Need to validate if this is even ndded any more.
      if (key->state == ACD_LED_DISABLED && state == ACD_LED_ON) { // if disabled, can't turn on
        LOG(LOG_WARNING, "%s is %s, can't turn %s", key->label, acd_state_to_str(key->state), acd_state_to_str(state));
        LOG(LOG_ERR, "Shouldn't have got here, validate is_transition_permitted() is working correctly\n");
        return false;
      }
      ASSIGN_IF_CHANGED(key->state , state, acdata->is_dirty, key->is_dirty);
      check_master(acdata); // Set things to disabled
      if (state == ACD_LED_ON) { 
        aquachemd_force_sensor_poll();
      } else if (state == ACD_LED_OFF && _acdconfig_.master_off_as_interlock) {
        //get_master(acdata)->scope = ACD_ACTION_BLOCK; 
      } 
      break;

    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:
      if (state == ACD_LED_ON) {
        turn_pump_on(acdata, key, value<=0?0:value);
      } else if ((state == ACD_LED_OFF || state == ACD_LED_ENABLED || state == ACD_LED_DISABLED) &&  key->state == ACD_LED_ON) {
        turn_pump_off(acdata, key, state==ACD_LED_DISABLED?ACD_LED_DISABLED:ACD_LED_ENABLED);
      // key->state can't be ON here -- branch above already claims every ON case
      // for state ∈ {OFF, ENABLED, DISABLED}, a superset of what we check below.
      } else if (state == ACD_LED_OFF || state == ACD_LED_DISABLED) {
        ASSIGN_IF_CHANGED(key->state, state, acdata->is_dirty, key->is_dirty);
      } else if (state == ACD_LED_ENABLED && key->state != ACD_LED_ON) {
        ASSIGN_IF_CHANGED(key->state, state, acdata->is_dirty, key->is_dirty);
      }
      break;

    case ACD_TYPE_MQTT_COND:
    case ACD_TYPE_GPIO_COND:
      // The only time we "request" a state change on a condition is if it's from DELAY to ON.
      if (state == ACD_LED_DELAY || isMASKSET(key->flags, DELAY_ACTIVE)) {
        // Make sure the condition is still met.
        set_cond_state(acdata, key, key->met?ACD_LED_ON:ACD_LED_OFF);
      }
      break;

    case ACD_TYPE_GPIO_OUTPUT:
      // No ENABLED for GPIO_OUTPUT.  Below check shouldn't get here
      if (state == ACD_LED_ENABLED) {
        LOG(LOG_WARNING,"GPIO Output %s request set to enabled, not supported using off\n",key->label);
        state = ACD_LED_OFF;
      }
      if (state == ACD_LED_ON) {
        turn_gpio_switch_on(acdata, key, value<=0?0:value);
      } else if ((state == ACD_LED_OFF || state == ACD_LED_DISABLED)  &&  key->state == ACD_LED_ON) {
        turn_gpio_switch_off(acdata, key, state);
      } else if (state == ACD_LED_OFF || state == ACD_LED_DISABLED) {
        ASSIGN_IF_CHANGED(key->state, state, acdata->is_dirty, key->is_dirty);
      }
      break;

    default:
      //SET_IF_CHANGED(key->state , state, acdata->is_dirty);
      LOG(LOG_WARNING, "%s is %s, setting to %s not supported", key->label, acd_state_to_str(key->state), acd_state_to_str(state));
      return false;
      break;
  }

  LOG(LOG_DEBUG, "%s set to %s", key->label, acd_state_to_str(key->state));

  return true;
}

#define USE_MASTER_OFF_INTERLOCK

void check_master(struct aquachemdata *acdata) {
  acd_key_t *failed_condition = NULL;
  //acd_action_t action = ACD_ACTION_ALLOW;

#ifndef USE_MASTER_OFF_INTERLOCK
  // Turn everything to disabled if master if off
  if ( get_master(acdata)->state == ACD_LED_OFF ) {
    for (acd_key_t *curr = acdata->keys->next; curr != NULL; curr = curr->next) {
      if (!IS_OUTPUT(curr->type) && !IS_INPUT(curr->type)) continue; // Conditions, Tank, Master itself -- nothing to do here

      if (curr->state != ACD_LED_OFF) {
        if (IS_OUTPUT(curr->type) && curr->state == ACD_LED_ON) {
          IS_PUMP(curr->type) ? turn_pump_off(acdata, curr, ACD_LED_DISABLED) : turn_gpio_switch_off(acdata, curr, ACD_LED_DISABLED);
          LOG(LOG_INFO, "State Manager - %s no longer permitted, turning off (-> %s)", curr->label, acd_state_to_str(ACD_LED_DISABLED));
        }
        set_key_state(acdata, curr, ACD_LED_DISABLED);
      } else if (IS_OUTPUT(curr->type) && !IS_PUMP(curr->type) && curr->state == ACD_LED_OFF) {
        // Outputs need to go from OFF to DISABLED, Pumps OFF mean something different since it supports ENABLED and need to be MANUALLY turned on.
        set_key_state(acdata, curr, ACD_LED_DISABLED);
      }
    }
    return;
  }

#else
  bool master_manually_off = (get_master(acdata)->state == ACD_LED_OFF);

  if (master_manually_off && !_acdconfig_.master_off_as_interlock) {
    // Mode A: same as above, unchanged
    for (acd_key_t *curr = acdata->keys->next; curr != NULL; curr = curr->next) {
      if (!IS_OUTPUT(curr->type) && !IS_INPUT(curr->type)) continue;
      if (curr->state != ACD_LED_OFF) {
        if (IS_OUTPUT(curr->type) && curr->state == ACD_LED_ON) {
          IS_PUMP(curr->type) ? turn_pump_off(acdata, curr, ACD_LED_DISABLED) : turn_gpio_switch_off(acdata, curr, ACD_LED_DISABLED);
          LOG(LOG_INFO, "State Manager - %s no longer permitted, turning off (-> %s)", curr->label, acd_state_to_str(ACD_LED_DISABLED));
        }
        set_key_state(acdata, curr, ACD_LED_DISABLED);
      } else if (IS_OUTPUT(curr->type) && !IS_PUMP(curr->type) && curr->state == ACD_LED_OFF) {
        set_key_state(acdata, curr, ACD_LED_DISABLED);
      }
    }
    return;
  }
  // Mode B (or master genuinely ON): fall through to the shared tail below.
  // Deliberately NOT setting scope/state here anymore -- the shared block
  // computes both, now taking master_manually_off into account directly.
#endif

  // Check for any conditions that are not met.
  get_master(acdata)->scope = ACD_ACTION_ALLOW; // Reset to good, below will set to bad.
  for (acd_key_t *curr = get_master(acdata)->next; curr != NULL; curr = curr->next) {
    if (IS_CONDITION(curr->type)) {LOG(LOG_INFO, "State Manager - Condition %s, scope %s, %s",curr->label, acd_scope_to_str(curr->scope), curr->met?"Safe":"Not Safe");}
    //if (IS_CONDITION(curr->type) && curr->met == false) {
    if (IS_CONDITION(curr->type) && (curr->met == false || curr->state == ACD_LED_DELAY)) {
      if (curr->scope == ACD_ACTION_BLOCK) {
        failed_condition = curr;
        get_master(acdata)->scope = ACD_ACTION_BLOCK;
      } else if (curr->scope == ACD_ACTION_LIMIT && get_master(acdata)->scope != ACD_ACTION_BLOCK) {
        // Don't set to limit if already block
        get_master(acdata)->scope = ACD_ACTION_LIMIT; 
      }
      LOG(LOG_INFO,"Condition %s not met",curr->label);
    }
  }
  
#ifdef USE_MASTER_OFF_INTERLOCK
  if (master_manually_off) {
    // Manual off is at least as severe as any condition -- never let a
    // condition-derived ALLOW/LIMIT downgrade it back below BLOCK.
    get_master(acdata)->scope = ACD_ACTION_BLOCK;
  }
#endif

  LOG(LOG_INFO, "State Manager - Master actions = %s",acd_scope_to_str(get_master(acdata)->scope) );

  // Condition-scanning loop that derives acdata->keys->scope: unchanged --
  // this computes the input the table needs, the table can't replace it.

  //set_key_state(acdata, acdata->keys, failed_condition == NULL ? ACD_LED_ON : ACD_LED_ENABLED);
  
  acd_state_t master_state;
#ifdef USE_MASTER_OFF_INTERLOCK
  if (master_manually_off) {
    master_state = ACD_LED_OFF;
  } else if (failed_condition != NULL) {
    master_state = ACD_LED_ENABLED;
  } else {
    master_state = ACD_LED_ON;
  }
#else
  master_state = (failed_condition == NULL) ? ACD_LED_ON : ACD_LED_ENABLED;
#endif

  set_key_state(acdata, acdata->keys, master_state);

  for (acd_key_t *curr = acdata->keys->next; curr != NULL; curr = curr->next) {
    key_category_t cat = classify_key(curr);
    LOG(LOG_DEBUG, "Validating %s classify_key=%d\n",curr->label,cat); 
    if (cat == KC_COUNT) continue;

    bool on_permitted = transition_permission[cat][ACD_LED_ON] & (1 << acdata->keys->scope);

    LOG(LOG_DEBUG, "Validating status of %s, scope %s, state %s against Master scope %s. is permitted = %s\n",
        curr->label, acd_scope_to_str(curr->scope), acd_state_to_str(curr->state),
        acd_action_to_str(get_master(acdata)->scope), on_permitted ? "Yes" : "No");

    if (IS_OUTPUT(curr->type))
    {
      //if (cat == KC_OUTPUT_ALLOW)
      //  continue; // ignores all interlocks -- nothing to compute
      if (cat == KC_OUTPUT_ALLOW) {
        if (curr->state == ACD_LED_DISABLED) set_key_state(acdata, curr, ACD_LED_OFF);
        continue;
      }

      bool is_pump = IS_PUMP(curr->type);

      if (is_pump && curr->state == ACD_LED_OFF) continue; // sticky-off, manual/tank-only recovery

      acd_state_t target = is_pump ? pump_resting_state(cat, acdata->keys->scope)
                                   : output_resting_state(cat, acdata->keys->scope);

      if (curr->state == ACD_LED_ON && !on_permitted) {
        is_pump ? turn_pump_off(acdata, curr, target) : turn_gpio_switch_off(acdata, curr, target);
        LOG(LOG_INFO, "State Manager - %s no longer permitted, turning off (-> %s)", curr->label, acd_state_to_str(target));
      } else if (curr->state != ACD_LED_ON && curr->state != target) {
        set_key_state(acdata, curr, target);
      }
    }
    else if (IS_INPUT(curr->type))
    {
      if (on_permitted && curr->state != ACD_LED_ON)
        set_key_state(acdata, curr, ACD_LED_ENABLED);
      else if (!on_permitted)
        set_key_state(acdata, curr, ACD_LED_DISABLED);
    }
  }
  
}

bool set_key_state(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state)
{
  //  Master         off / on / enabled ----> Means a condition is not met.
  //  sensors        on / disabled / enabled (when master turns them on only, then go to on after first reading.
  //. sensors        off when a single poll failed, disabled when taken out of poll cycle.
  //  doser (sensor) on / off / disabled / enabled

  bool goodState = true;

  switch(key->type){
    case ACD_TYPE_MASTER:
      if (state != ACD_LED_OFF && state != ACD_LED_ON && state != ACD_LED_ENABLED) {goodState = false;}
      break;
    case ACD_TYPE_MQTT_COND:
    case ACD_TYPE_GPIO_COND:
      goodState = set_cond_state(acdata,  key, state);
      break;
    case ACD_TYPE_EZO_PH:
    case ACD_TYPE_EZO_ORP:
    case ACD_TYPE_EZO_TEMP:
    case ACD_TYPE_EZO_PRS:
    case ACD_TYPE_EZO_EC:
    case ACD_TYPE_MQTT_TEMP:
    case ACD_TYPE_D1W_TEMP:
    case ACD_TYPE_SYSFS_VALUE:
    case ACD_TYPE_MQTT_VALUE:
    case ACD_TYPE_I2C_PRS:
    case ACD_TYPE_I2C_TEMP: // At present only virtual I2C temp is supported.
    /*
      if (state != ACD_LED_ON && state != ACD_LED_DISABLED) {
        goodState = false;
      }*/
     // Above is accurate for a request, below is accurate for master to change.
      if (state != ACD_LED_ON && state != ACD_LED_DISABLED && state != ACD_LED_ENABLED && state != ACD_LED_OFF) {
        goodState = false;
      }
      if (goodState && get_master(acdata)->state != ACD_LED_ON && state == ACD_LED_ON && key->scope == ACD_SCOPE_GLOBAL) {
        // If mater was turned off while sampeling we can get the change request here.
        goodState = false;
      }
      break;
    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:
      if (state != ACD_LED_OFF && state != ACD_LED_ON && state != ACD_LED_ENABLED && state != ACD_LED_DISABLED) {
        goodState = false;
      }
      if (state == ACD_LED_ENABLED && get_master(acdata)->state != ACD_LED_ON) {
        state = ACD_LED_DISABLED;
      }
      break;

    case ACD_TYPE_GPIO_OUTPUT:
      // ENABLED was never a real state for a plain switch -- ON/OFF/DISABLED only.
      if (state != ACD_LED_OFF && state != ACD_LED_ON && state != ACD_LED_DISABLED) {
        goodState = false;
      }
      break;

    case ACD_TYPE_GPIO_INPUT:
      // Same shape as every other sensor type -- ON/ENABLED/DISABLED (no manual OFF).
      if (state != ACD_LED_ON && state != ACD_LED_ENABLED && state != ACD_LED_DISABLED && state != ACD_LED_OFF) {
        goodState = false;
      }
      break;
/*
    case ACD_TYPE_GPIO_OUTPUT:
    case ACD_TYPE_GPIO_INPUT:
    // Do need to validate scope here.
      if (state != ACD_LED_OFF && state != ACD_LED_ON && state != ACD_LED_ENABLED && state != ACD_LED_DISABLED) {
        goodState = false;
      }
      break;
  */  
    case ACD_TYPE_VIR_TANK: // Virtual device can;t change
    case ACD_TYPE_NONE:
      goodState = false;
      break;
  }

  if (!goodState) {
    // If we set to delay, we return false, but don;t print the error.
    if (isMASKSET(key->flags, DELAY_ACTIVE) || key->state == ACD_LED_DELAY) {
      return false;
    }
    LOG(LOG_WARNING, "Device %s can't be set to %s", key->label, acd_state_to_str(state));
    return false;
  }

  // If being asked to turn on, clear any flags.
  if ( (state == ACD_LED_ON || state == ACD_LED_ENABLED) && isMASKSET(key->flags, ACD_FLAG_FAULTED)) {
    removeMASK(key->flags, ACD_FLAG_FAULTED);
  }

  //return SET_IF_CHANGED(key->state , state, acdata->is_dirty);

  if (ASSIGN_IF_CHANGED(key->state , state, acdata->is_dirty, key->is_dirty)) {
    LOG(LOG_INFO, "State Manager - Set %s to %s (scope %s)",key->label, acd_state_to_str(key->state), acd_scope_to_str(key->scope));
    return true;
  } else {
    LOG(LOG_DEBUG, "State Manager - Request to set same state, ignored %s to %s (scope %s)",key->label, acd_state_to_str(key->state), acd_scope_to_str(key->scope));
    //LOG(LOG_INFO, "State Manager - No change for %s, remains at %s (scope %s)",curr->label, acd_state_to_str(curr->state), acd_scope_to_str(curr->scope));
    return false;
  }
}


bool set_cond_state(struct aquachemdata *acdata, acd_key_t *cond, acd_state_t state)
{
  //  conditions     on / off

  // If condition set to on, but it has a delay, then start the timer.
  if (state == ACD_LED_ON && cond->delay_on > 0) {
    if (cond->state == ACD_LED_OFF) {
      LOG(LOG_NOTICE, "Condition '%s' met, delaying activation %d seconds!",cond->label, cond->delay_on);
      ASSIGN_IF_CHANGED(cond->state , ACD_LED_DELAY, acdata->is_dirty, cond->is_dirty);
      start_delay(acdata, cond, 0, cond->delay_on);
      return false;
    } else if (cond->state == ACD_LED_DELAY || isMASKSET(cond->flags, DELAY_ACTIVE)) {
      if ( get_timer_left_sec(cond) > 0 ) {
        LOG(LOG_WARNING, "State Manager - Request to set '%s' on, but still in delay, ignored!",cond->label);
        return false;
      }
      // Good to fall through to set new ON state.
    }
  } else if (state == ACD_LED_OFF && cond->state == ACD_LED_DELAY) {
    // cancel the delay and fall through to turning off.
    clear_timer(acdata, cond);
  }

  if (state == ACD_LED_ON || state == ACD_LED_OFF ) {
    if (ASSIGN_IF_CHANGED(cond->state , state, acdata->is_dirty, cond->is_dirty)) {
      LOG(LOG_INFO, "State Manager - Condition %s changed to %s",cond->label, acd_state_to_str(cond->state));
    }
    //if ( state == ACD_LED_OFF && get_master(acdata)->state == ACD_LED_ON) {
    //  master_state_change(acdata, ACD_LED_ENABLED);
    //}
  } else {
     //LOG(LOG_ERR, "Condition %s can't be set to %s", cond->label, acd_state_to_str(state));
     return false;
  }

  check_master(acdata);
  if (state == ACD_LED_ON) { // NSF in future see if we can pass and use acd_source_t here and ignore if from ACD_MAIN
    aquachemd_force_sensor_poll();
  }
  return true;
}

void set_pump_off_due_to_tank_empty(struct aquachemdata *acdata, acd_key_t *key)
{
  if (key->state == ACD_LED_ON) {
    turn_pump_off(acdata, key, ACD_LED_OFF);
  }
  
  LOG(LOG_WARNING, "%s: dose tank is empty (%.2f / %.2f %s), forcing doser OFF -- switch back to Enabled once refilled\n", 
        key->label, key->child->data.tank.remaining_volume, key->child->data.tank.total_volume, uom_to_str(key->child->data.tank.uom));
  set_key_state(acdata, key, ACD_LED_OFF);
  key->state = ACD_LED_OFF;
  setMASK(key->flags, SET_OFF_DUE_TO_TANK_VOLUME);
}

// Turn the appropiate pump on or off based on the volume of the tank.
void validate_pumps_against_tanks(struct aquachemdata *acdata)
{
  for (acd_key_t *key = acdata->keys->next; key != NULL; key = key->next) {
    if (IS_OUTPUT(key->type) && key->child != NULL && IS_TANK_EMPTY_LOCKOUT_ENABLED(key->child)) {
      if (tank_is_empty(key->child)) {
        if (key->state == ACD_LED_ENABLED || key->state == ACD_LED_ON) {
          set_pump_off_due_to_tank_empty(acdata, key);
        }
      } else if(isMASKSET(key->flags, SET_OFF_DUE_TO_TANK_VOLUME)) {
        LOG(LOG_WARNING, "%s: dose tank is refilled (%.1f / %.1f %s), switching doser back to Enabled\n", 
            key->label, key->child->data.tank.remaining_volume, key->child->data.tank.total_volume, uom_to_str(key->child->data.tank.uom));
        removeMASK(key->flags, SET_OFF_DUE_TO_TANK_VOLUME);
        set_key_state(acdata, key, ACD_LED_ENABLED);
      }
    }
  }
}
