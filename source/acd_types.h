#ifndef ACD_TYPES_H_
#define ACD_TYPES_H_  

#include <time.h>
#include <stdatomic.h>

#include "ezo.h"
#include "1wire.h"
#include "gpio.h"
#include "sysfs.h"
#include "i2c.h"
#include "uom.h"


#define ACD_MASTER_ID "AquachemD"

typedef enum {
  ACD_STARTING,
  ACD_KEEPRUNNING,
  ACD_RELOAD,
  ACD_CLEANUP,
  ACD_FINISHED,
  ACD_FAILED
} acd_runstate_t;

typedef struct {
  pthread_t parent_id;
  pthread_t id;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  //acd_runstate_t state;
  atomic_int state; 
} acd_thread_t;

/* Use as below
static acd_thread_t _my_worker = {
    .parent_id = 0,
    .id = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER, 
    .cond = PTHREAD_COND_INITIALIZER, 
    .state = ACD_STARTING
};
and
 if (atomic_load_explicit(&_my_worker.state, memory_order_relaxed) != ACD_KEEPRUNNING) {
 ..........
 }
*/

// NOT USED YET
typedef enum {
  ACD_MAIN,
  ACD_WEBSOCKET,
  ACD_HTTP,
  ACD_MQTT
} acd_source_t;

typedef enum {
    ACD_TYPE_NONE = 0,
    
    // Special
    ACD_TYPE_MASTER,

    // Conditions
    ACD_TYPE_MQTT_COND,
    ACD_TYPE_GPIO_COND,
    #define ACD_COND_FIRST ACD_TYPE_MQTT_COND
    #define ACD_COND_LAST  ACD_TYPE_GPIO_COND

    // Inputs (Sensors)
    ACD_TYPE_EZO_PH,
    ACD_TYPE_EZO_ORP,
    ACD_TYPE_EZO_TEMP,
    ACD_TYPE_EZO_PRS,
    ACD_TYPE_EZO_EC,
    ACD_TYPE_I2C_PRS,
    ACD_TYPE_I2C_TEMP,
    ACD_TYPE_MQTT_TEMP,
    ACD_TYPE_D1W_TEMP,
    ACD_TYPE_SYSFS_VALUE,
    ACD_TYPE_MQTT_VALUE,
    ACD_TYPE_GPIO_INPUT,
    #define ACD_IN_FIRST ACD_TYPE_EZO_PH
    #define ACD_IN_LAST  ACD_TYPE_GPIO_INPUT


    // Outputs (Actuators)
    ACD_TYPE_GPIO_PMP,
    ACD_TYPE_EZO_PMP,
    ACD_TYPE_GPIO_OUTPUT, 

    #define ACD_PMP_FIRST ACD_TYPE_GPIO_PMP
    #define ACD_PMP_LAST  ACD_TYPE_EZO_PMP

    #define ACD_OUT_FIRST ACD_TYPE_GPIO_PMP
    #define ACD_OUT_LAST  ACD_TYPE_GPIO_OUTPUT

    // Self managed
    ACD_TYPE_VIR_TANK,
} acd_type_t;

/* Grouping Macros */

// Returns true for MQTT and GPIO conditions, but false for MASTER
#define IS_CONDITION(t) ((t) >= ACD_COND_FIRST && (t) <= ACD_COND_LAST)

// Returns true for any input sensor (pH, Temp, etc.)
#define IS_INPUT(t)     ((t) >= ACD_IN_FIRST   && (t) <= ACD_IN_LAST)

// Returns true for any output/actuator (Pumps, etc.)
#define IS_OUTPUT(t)    ((t) >= ACD_OUT_FIRST  && (t) <= ACD_OUT_LAST)

// REturns true for any pump (GPIO or EZO)
#define IS_PUMP(t)      ((t) >= ACD_PMP_FIRST && (t) <= ACD_PMP_LAST)

// Don't have a MQTT sensor code, so place it here.
typedef struct {
  char *topic;
  char *target_value; 
  //char *current_value; 
} mqtt_sensor_t;
/*
typedef struct {
  unsigned char address;
} ezo_sensor_t;
*/
typedef struct {
  float total_volume;
  float remaining_volume;
  float percent_full;
  float min_volume;   // Consider the tank empty once remaining_volume drops to this level.
  acd_uom_t uom;
} tank_sensor_t;


// These are hard coded in MQTT and UI, don't re-order.
typedef enum {
    ACD_LED_UNKNOWN = -1,
    ACD_LED_OFF = 0,     // Maps to false
    ACD_LED_ON = 1,      // Maps to true
    ACD_LED_ENABLED,
    ACD_LED_DISABLED, // Sensor caused button to be disabled.
    ACD_LED_DELAY // Condition has a delay before turning on.
} acd_state_t;



// ============================================================================
//                    SCOPE, SEVERITY, AND STATE PERMISSIONS
// ============================================================================
//
// TWO VOCABULARIES, ONE ENUM
// --------------------------
// acd_scope_t values are used for two different things. They share an
// underlying enum (and the ACD_ACTION_* aliases below), but they answer
// different questions, and conflating them is the single easiest way to
// misread this system:
//
//   DEVICE SCOPE     - configured per device via "<device>_interlock_scope".
//                      Answers: "how much does THIS device care about
//                      interlocks?"  Values: None / Local / Global.
//
//   MASTER SEVERITY  - computed, lives on the master key (acdata->keys->scope).
//                      Answers: "how bad is the system's current state?"
//                      Values: Allow / Limit / Block.
//
// Always say "Allow/Limit/Block" for severity and "None/Local/Global" for a
// device's own scope. Both print through different helpers for this reason:
// acd_action_to_str() for severity, acd_scope_to_str() for device scope.
//
//
// HOW MASTER SEVERITY IS DERIVED
// ------------------------------
// Severity is the worst (highest) scope among all currently-unmet conditions:
//   No condition failed                 -> ALLOW
//   A Local-scope condition failed      -> LIMIT   (soft limit)
//   A Global-scope condition failed     -> BLOCK   (hard interlock)
// A single failed Global condition overrides any number of failed Local ones.
// Master OFF is handled separately -- see MASTER OFF below.
//
//
// ============================================================================
//                    CONDITIONS -- WHAT SETS THE INTERLOCK
// ============================================================================
//
// A condition is an input whose only job is to answer one yes/no question:
// "is it currently safe to run?" Conditions are the sole source of master
// severity. Nothing else raises or lowers it. Two types exist:
//
//   ACD_TYPE_MQTT_COND  - watches an external MQTT topic.
//       mqtt_condition_label            display name
//       mqtt_condition_topic            topic to subscribe to
//       mqtt_condition_value            payload that means "safe"
//       mqtt_condition_met_delay        seconds to wait before trusting it
//       mqtt_condition_interlock_scope  none / local / global
//     met = (received payload matches mqtt_condition_value).
//     Typical use: AqualinkD publishing filter-pump state.
//
//   ACD_TYPE_GPIO_COND  - watches a physical pin.
//       gpio_condition_label            display name
//       gpio_condition_pin              pin to read
//       gpio_condition_pin_mode         Active High / Active Low
//       gpio_condition_required_state   logical level that means "safe"
//       gpio_condition_met_delay        seconds to wait before trusting it
//       gpio_condition_interlock_scope  none / local / global
//     met = (gpio_read() == required). pin_mode handles electrical polarity
//     first, so required_state is always expressed in logical terms.
//     Typical use: flow switch, flow-cell level switch.
//
// Both resolve to the same thing: key->met, a plain bool.
//
//
// HOW met GETS UPDATED
// --------------------
//   GPIO  - primarily by the libgpiod event callback in gpio_monitor.c, with
//           a second comparison each poll cycle in aquachemd.c as a backstop.
//           Both compare the live reading against key->met, so they detect
//           the transition in BOTH directions (met->unmet and unmet->met).
//   MQTT  - by action_mqtt_condition_message() whenever a message lands on
//           the configured topic. No polling: an MQTT condition only updates
//           when the publisher sends something, so a publisher that goes
//           silent leaves the last known value in place indefinitely.
//
//
// THE MET-DELAY
// -------------
// When a condition becomes met, it does NOT immediately count as safe. It
// enters ACD_LED_DELAY (flag DELAY_ACTIVE) and starts a timer for
// <type>_condition_met_delay seconds. Only when that timer expires does
// set_cond_state() re-check key->met and promote it to ACD_LED_ON.
//
// While a condition sits in DELAY it is treated as NOT met for the purpose of
// computing severity. This is deliberate: it is what keeps probes from being
// trusted on water that has been sitting stagnant in the flow cell while the
// pump was off. Going unmet is immediate -- the delay only ever applies to
// becoming safe again, never to becoming unsafe.
//
//
// DERIVING MASTER SEVERITY
// ------------------------
// Every pass through check_master(), severity is recomputed from scratch:
//
//   1. master->scope = ALLOW
//   2. For each condition where (met == false || state == ACD_LED_DELAY):
//        - condition scope Global -> master->scope = BLOCK  (and records it
//                                     as failed_condition)
//        - condition scope Local  -> master->scope = LIMIT, unless BLOCK has
//                                     already been set by another condition
//   3. Worst wins. One failed Global condition outranks any number of failed
//      Local ones, regardless of order in the list.
//
// Master's own STATE is set separately, and only from failed_condition:
//   failed_condition == NULL -> master state ON
//   failed_condition != NULL -> master state ENABLED
// Because failed_condition is only ever assigned by a Global-scope failure, a
// Local-only failure leaves master state at ON while severity is LIMIT. Read
// severity (master->scope), not master state, to know how degraded the system
// currently is.
//
//
// CONDITION SCOPE "NONE"
// ----------------------
// The severity loop only tests for Global and Local. A condition configured
// with scope None therefore never affects severity at all -- it is evaluated,
// logged and published, but it gates nothing. That makes it usable as a
// monitor-only indicator, but it also means a mistyped scope silently produces
// a condition that looks configured and does nothing. Note that
// ACD_SCOPE_UNKNOWN currently aliases to ACD_SCOPE_ALLOW, so a scope value the
// parser does not recognise lands here.
//
//
// CONDITIONS ARE NEVER GATED THEMSELVES
// -------------------------------------
// KC_CONDITION permits OFF / ON / DELAY at every severity, and never permits
// ENABLED or DISABLED. Conditions must keep evaluating no matter how degraded
// the system is -- they are the only thing that can clear a BLOCK. The
// master-OFF sweep skips them for the same reason.
// ============================================================================
//
//
//
//
//
// WHAT EACH DEVICE SCOPE MEANS
// ----------------------------
//   None   - ignores interlock conditions entirely. Never disabled by
//            severity. (Still obeys Master OFF -- see below.)
//   Local  - only respects a hard interlock. Keeps running through LIMIT,
//            stops at BLOCK.
//   Global - respects every interlock. Stops at LIMIT and at BLOCK.
//
// Note the asymmetry between outputs and sensors:
//   - For an OUTPUT, "stops" means it is prevented from running.
//   - For a SENSOR, "stops" means it is no longer polled/trusted, because its
//     reading would be meaningless (e.g. probes sitting in stagnant water).
//
//
// PERMISSION MATRIX
// -----------------
// This mirrors transition_permission[] in state_manager.c exactly. That array
// is the single source of truth at runtime; this table exists so the intent is
// readable. If one changes, change both.
//
//   [A]   = permitted only when severity is ALLOW
//   [A/L] = permitted when severity is ALLOW or LIMIT
//   [L/B] = permitted when severity is LIMIT or BLOCK
//   [ALL] = permitted at every severity
//   [ - ] = never a valid state for this category
//
// +----------------------+-------+-------+---------+----------+-------+
// | CATEGORY             | OFF   | ON    | ENABLED | DISABLED | DELAY |
// +----------------------+-------+-------+---------+----------+-------+
// | MASTER               | ALL   | ALL   |   L/B   |    -     |   -   |
// | PUMP   (Global)      | ALL   |   A   |    A    |   L/B    |   -   |
// | PUMP   (Local)       | ALL   |  A/L  |   A/L   |    B     |   -   |
// | OUTPUT (None)        | ALL   |  ALL  |    -    |    -     |   -   |
// | OUTPUT (Global)      | ALL   |   A   |    -    |   L/B    |   -   |
// | OUTPUT (Local)       | ALL   |  A/L  |    -    |    B     |   -   |
// | CONDITION            | ALL   |  ALL  |    -    |    -     |  ALL  |
// | SENSOR (None)        | ALL   |  ALL  |    -    |    -     |   -   |
// | SENSOR (Global)      | ALL   |  A/L  |    -    |    -     |   -   |
// | SENSOR (Local)       | ALL   |  ALL  |   ALL   |    B     |   -   |
// +----------------------+-------+-------+---------+----------+-------+
//
// Note there is no PUMP (None) row: a doser must never be exempt from safety
// interlocks. classify_key() deliberately maps a pump with scope None to the
// Local category rather than granting it an exemption.
//
//
// WHAT THE STATES MEAN
// --------------------
//   ON       - physically running / actively being polled.
//   OFF      - not running. For a PUMP this is sticky: set by the user, or by
//              the tank-empty lockout, and NEVER cleared automatically. It
//              requires an explicit re-enable (or a tank refill). For an
//              OUTPUT, OFF carries no such weight -- it is simply "not on",
//              and the system moves it to/from DISABLED freely as severity
//              changes.
//   ENABLED  - armed and waiting: not running now, but automation may start it.
//              Only meaningful for pumps (and sensors, meaning "polling").
//              Plain outputs have no ENABLED state -- nothing automates them.
//   DISABLED - blocked by severity. Clears itself automatically once severity
//              improves. Contrast with a pump's OFF, which does not.
//   DELAY    - conditions only: the condition is satisfied but still inside
//              its configured met-delay window, so it is not yet trusted.
//
//
// MASTER OFF
// ----------
// Master OFF is a manual, whole-system shutdown and is handled before any
// severity logic runs. Its exact reach is configurable:
//
//   Mode A (default) - every output and sensor is forced to DISABLED,
//                      regardless of its own scope. "The master switch means
//                      everything, no exceptions." Scope-None devices are
//                      included, and are restored to OFF when master returns.
//
//   Mode B           - Master OFF is treated as an interlock at BLOCK
//                      severity, so the normal table above applies. Local
//                      sensors keep reading; scope-None devices are untouched.
//
//
// WHO APPLIES THIS TABLE
// ----------------------
// Two consumers, deliberately different:
//
//   _state_change_request()  - external requests (API / MQTT / timers). Asks
//                              is_transition_permitted(); may reject. Sensors
//                              and inputs are rejected outright here: their
//                              state is derived from physical reality, never
//                              requested.
//
//   check_master()           - internal reconciliation, run every cycle. Never
//                              rejects anything; it computes the state each
//                              device should now be in and applies it via
//                              set_key_state().
//
// Keys with no permission row at all -- VIR_TANK, NONE -- are returned as
// KC_COUNT by classify_key() and must be skipped by both consumers before any
// array indexing. transition_permission[KC_COUNT] is out of bounds.
// ============================================================================


typedef enum {
    ACD_SCOPE_UNKNOWN = -1,
    ACD_SCOPE_ALLOW  = 0, // Device: ignores interlocks. Master: no condition failed.
    ACD_SCOPE_LOCAL  = 1, // Device: respects hard interlocks only. Master: soft limit.
    ACD_SCOPE_GLOBAL = 2  // Device: respects all interlocks. Master: hard interlock.
} acd_scope_t;

//#define ACD_SCOPE_UNKNOWN ACD_SCOPE_ALLOW //default to ALLOW if unknown, so that we don't block dosing due to a config error.

// Inverse the names for conditions to make code easier to read.
#define ACD_ACTION_ALLOW  ACD_SCOPE_ALLOW
#define ACD_ACTION_LIMIT  ACD_SCOPE_LOCAL
#define ACD_ACTION_BLOCK  ACD_SCOPE_GLOBAL


typedef struct {
    float average;
    time_t last_sample_time;
    float min;
    float max;
    uint32_t sample_count;   // replaces tau_seconds' role in the calc
    float tau_seconds;       // Probably need to remove in future
    pthread_mutex_t lock;
} sensor_stats_t;


// For future replace "float flow_rate;" with this struct in acd_key_t
typedef struct {
  float flow_rate;            // Pump flow rate
  float running_total_ml;     // Running total for period (day?)
  float running_total_max_ml; // Maximum allowed for period (day?)
} dose_stats_t;

typedef struct acd_key_t {
    acd_type_t type;
    acd_state_t state;
    acd_scope_t scope;
    //char *remote_instance;  // Future // NULL for local devices; the remote instance's ID/label if mirrored
    
    volatile bool is_dirty;
    char *label;
    char *ID;
    uint8_t index; //values from 0 to 255
    uint16_t flags; // Any bitmasks (like timer active, pump type, global interlock)
    uint8_t err_cnt;
    
    float value; // sensor uses for current value, pump uses for value of ph/orp when turned on.
    acd_uom_t uom;

    union {
      //float flow_rate;      // Pumps     = ml per second rate for pumps
      dose_stats_t dose_stats;
      uint32_t delay_on;    // Condition = used for a delay before setting to on.
      sensor_stats_t stats; // Sensor    = used for statics.
      float requested_dose; // EZO Pump  = used for for requested dose volume in ml.
    };

    union {
      bool met;    // For conditions, met or not.
      bool ison;   // For output pump, or gpio inputs
    };

    union {
      uint32_t default_duration;  // Pumps use for default duration, mainly for homekit.
    };

    union {
        ezo_sensor_t   ezo;
        gpio_handle_t  gpio;
        w1_sensor_t    w1;
        mqtt_sensor_t  mqtt;
        sysfs_sensor_t sysfs;  
        i2c_sensor_t   i2c;
        tank_sensor_t  tank;
    } data;

    struct acd_key_t *child;  // Virtual key if sensor has multiple outputs (like PTE7300 with pressure and temperature), or pump with tankvolume as child 
    struct acd_key_t *next;
} acd_key_t;

typedef struct runtime_range_t{
  float threshold;
  uint32_t seconds;
} runtime_range_t;



#define MAX_DOSING_RANGES 10

// For special_mask in acd_key (15 is the highest bit in uint16_t, so we can have 15 special_mask's)
#define TIMER_ACTIVE           (1 << 0)
#define DELAY_ACTIVE           (1 << 1)
#define PH_PUMP                (1 << 2) 
#define ORP_PUMP               (1 << 3)
#define H2O_PUMP               (1 << 4)

#define ACD_FLAG_FAULTED       (1 << 5)   
#define ACD_FLAG_ACTIVE        (1 << 6)   // not used at present

#define CALC_AVERAGE           (1 << 7)

#define CONDITION_NOTIFIED     (1 << 8)

#define ACD_FLAG_VIRTUAL       (1 << 9)
#define SET_OFF_DUE_TO_TANK_VOLUME (1 << 10)

// CAN'T ADD ANY MORE wuthout changeing uint8_t to uint16_t 
//#define CONDITION_SCOPE_GLOBAL (1 << 3) // For conditions Set if global interlock, clear if local restriction
//#define CONDITION_SCOPE_LOCAL  (1 << 4) // ONLY for master key, if on and this is set, then re can read sensors but not dose.

/*
#define XXX            (1 << 3) // For special_mask's
#define XXX            (1 << 4) // For special_mask's
#define XXX            (1 << 5) // For special_mask's
#define XXX            (1 << 6) // For special_mask's
#define XXX            (1 << 7) // For special_mask's
// Change acd_key_t->flags & config.c - _staging->flags before defining more,
*/

/*
#define isMASKSET(bitmask, mask) ((bitmask & mask) == mask)
#define setMASK(bitmask, mask)    (bitmask |= mask)
*/

#define UNKNOWN -9999 
#define MASTER_ID 1



/*
   Logic for dosing events / Pull information like.
   journalctl PUMP_ID=PMP_1 --since "7 days ago" -o json | jq -r '.RUNTIME_SEC' | awk '{sum+=$1} END {print sum}'

   Hash string "ACD-PMP-Event", you get valid 32-character ID
   The 128-bit Message ID 332918807d4b46949f50e93149872583
*/
#define SD_PUMP_EVENT_ID "332918807d4b46949f50e93149872583"
#define SD_MESSAGE_STARTUP_ID "5e982c7a12344567890abcdef1234567"
#define SD_MESSAGE_UPGRADE_ID "c3b9b418e24440939b4bfae6dfbc1122"
#define SD_TANK_LEVEL_SET_EVENT_ID "a7f82b1c4d3e4e899c125f6b7a8d90e1"

#endif // ACD_TYPES_H_