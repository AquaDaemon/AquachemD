#ifndef DOSING_H
#define DOSING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ============================================================================
 * Dosing domain model
 * ============================================================================
 *
 * High-level flow:
 *
 *   Trigger / schedule
 *          |
 *          v
 *   Gather current state
 *          |
 *          +-------------------+
 *          |                   |
 *          v                   v
 *   dose_config_t       sensor_snapshot_t
 *          |                   |
 *          +---------+---------+
 *                    |
 *                    v
 *              dosing_engine
 *                    |
 *                    v
 *              dose_request_t
 *                    |
 *                    v
 *             safety checks (interlocks)
 *                    |
 *             +------+------+
 *             |             |
 *           BLOCK          ALLOW
 *             |             |
 *             v             v
 *          log it       start pump
 *                           |
 *                           v
 *                       stop pump
 *                           |
 *                           v
 *                    dose_result_t
 *
 * Important:
 *
 *   dose_request_t = what the dosing calculation WANTS to happen
 *   dose_permission_t = whether safety allows it to happen
 *   dose_result_t = what ACTUALLY happened
 *
 * ============================================================================
 */


/*
 * ============================================================================
 * Sensor types
 * ============================================================================
 *
 * These types should probably live in a sensor-related module rather than
 * dosing.h. They are shown here conceptually so the dosing interface can be
 * understood holistically.
 *
 * TODO: Move/use the existing project sensor definitions.
 *
 *   typedef enum {
 *       SENSOR_QUALITY_UNKNOWN = 0,
 *       SENSOR_QUALITY_GOOD,
 *       SENSOR_QUALITY_STALE,
 *       SENSOR_QUALITY_WARMING_UP,
 *       SENSOR_QUALITY_OUT_OF_RANGE,
 *       SENSOR_QUALITY_COMMUNICATION_ERROR,
 *       SENSOR_QUALITY_INTERLOCKED
 *   } sensor_quality_t;
 *
 *   typedef struct {
 *       double value;
 *       sensor_quality_t quality;
 *       uint64_t timestamp;
 *   } sensor_value_t;
 *
 *   typedef struct {
 *       sensor_value_t ph;
 *       sensor_value_t orp;
 *       sensor_value_t temperature;
 *       uint64_t timestamp;
 *   } sensor_snapshot_t;
 *
 * ============================================================================
 */


/*
 * ============================================================================
 * Dosing sensor
 * ============================================================================
 */

typedef enum {
    DOSE_SENSOR_NONE = 0,
    DOSE_SENSOR_PH,
    DOSE_SENSOR_ORP
} dose_sensor_t;


/*
 * ============================================================================
 * Dosing direction
 * ============================================================================
 *
 * This is deliberately separate from the sensor.
 *
 * Example:
 *
 *   pH doser might reduce pH
 *   ORP doser might increase ORP
 *
 * The sensor tells us WHAT we are measuring.
 * The direction tells us HOW the doser changes that measurement.
 * ============================================================================
 */

typedef enum {
    DOSE_DIRECTION_NONE = 0,
    DOSE_DIRECTION_UP,
    DOSE_DIRECTION_DOWN
} dose_direction_t;


/*
 * ============================================================================
 * Dose reason
 * ============================================================================
 *
 * This describes WHY a dose decision was made or prevented.
 *
 * Keep this relatively stable because these values will eventually become
 * part of logs, MQTT messages, statistics, etc.
 * ============================================================================
 */

typedef enum {
    DOSE_REASON_NONE = 0,

    /* Normal dosing calculation */
    DOSE_REASON_THRESHOLD,
    DOSE_REASON_TARGET_REACHED,

    /* Sensor/state problems */
    DOSE_REASON_SENSOR_NOT_READY,
    DOSE_REASON_SENSOR_INVALID,

    /* Safety / interlocks */
    DOSE_REASON_INTERLOCK,
    DOSE_REASON_FLOW_NOT_CONFIRMED,
    DOSE_REASON_FILTER_PUMP_OFF,
    DOSE_REASON_TANK_LOW,

    /* Dosing limits */
    DOSE_REASON_MAX_SINGLE_DOSE,
    DOSE_REASON_MAX_PERIOD_VOLUME,
    DOSE_REASON_MIN_DOSE_INTERVAL,
    DOSE_REASON_MIXING_DELAY,

    /* Configuration / system */
    DOSE_REASON_DISABLED,
    DOSE_REASON_INVALID_CONFIG,
    DOSE_REASON_ZERO_FLOW_RATE,

    /* Operator initiated */
    DOSE_REASON_MANUAL

} dose_reason_t;


/*
 * ============================================================================
 * Dose decision
 * ============================================================================
 */

typedef enum {
    DOSE_DECISION_NONE = 0,
    DOSE_DECISION_ALLOW,
    DOSE_DECISION_BLOCK,
    DOSE_DECISION_ERROR
} dose_decision_t;


/*
 * ============================================================================
 * Dose table
 * ============================================================================
 *
 * This represents the existing concept of a threshold -> dose relationship.
 *
 * Example:
 *
 *   threshold       runtime
 *   -----------------------
 *   7.60            10 sec
 *   7.70            20 sec
 *   7.80            30 sec
 *
 * The actual interpretation of threshold depends on the dosing direction.
 * ============================================================================
 */

#define MAX_DOSE_TABLE_ENTRIES 32

typedef struct {
    double threshold;
    double dose_seconds;
} dose_table_entry_t;

typedef struct {
    size_t count;
    dose_table_entry_t entries[MAX_DOSE_TABLE_ENTRIES];
} dose_table_t;


/*
 * ============================================================================
 * Doser configuration
 * ============================================================================
 *
 * This is configuration only.
 *
 * It should NOT contain runtime state such as:
 *
 *   - pump currently running
 *   - last dose time
 *   - accumulated period volume
 *   - current interlock state
 *
 * Those belong elsewhere.
 * ============================================================================
 */

typedef struct {

    /* What sensor drives this doser? */
    dose_sensor_t sensor;

    /* Does this doser increase or decrease the measured value? */
    dose_direction_t direction;

    /* Target sensor value */
    double target;

    /* Optional deadband around target */
    double deadband;

    /* Threshold -> runtime dosing table */
    dose_table_t table;

    /* Pump flow rate */
    double flow_rate_ml_per_sec;

    /* Safety / dosing limits */
    uint32_t max_single_dose_sec;
    double max_period_volume_ml;
    uint32_t min_dose_interval_sec;

    /* Sensor / mixing delays */
    uint32_t sensor_stabilization_sec;
    uint32_t mixing_delay_sec;

    /* Optional sensor averaging */
    bool use_average;
    uint32_t average_period_sec;

    /* Is this doser enabled? */
    bool enabled;

    /* Existing condition/interlock configuration */
    // condition/interlock references go here

} dose_config_t;



/*
 * ============================================================================
 * Dose request
 * ============================================================================
 *
 * Produced by the dosing engine.
 *
 * It answers:
 *
 *   "Based on the configuration and sensor data, what dose do we want?"
 *
 * It does NOT mean that the pump has started.
 * It does NOT mean that safety has approved the dose.
 * ============================================================================
 */

typedef struct {

    /* Why did the dosing engine arrive at this request? */
    dose_reason_t reason;

    /* Sensor value that drove the calculation */
    double sensor_value;

    /* Configured target */
    double target_value;

    /* Requested pump runtime */
    uint32_t requested_runtime_sec;

    /* Expected volume based on configured flow rate */
    double estimated_volume_ml;

    /*
     * Runtime before safety/configuration clamps.
     *
     * Useful for logging:
     *
     *   calculated = 60 sec
     *   allowed    = 30 sec
     */
    uint32_t unclamped_runtime_sec;

} dose_request_t;


/*
 * ============================================================================
 * Dose permission
 * ============================================================================
 *
 * Produced after the dose request has been evaluated against safety and
 * operational constraints.
 *
 * This is deliberately separate from dose_request_t.
 * ============================================================================
 */

typedef struct {
    dose_decision_t decision;
    dose_reason_t reason;
} dose_permission_t;


/*
 * ============================================================================
 * Dose result
 * ============================================================================
 *
 * Represents what actually happened during execution.
 *
 * This is different from dose_request_t.
 *
 * Request:
 *     "Run for 30 seconds."
 *
 * Result:
 *     "Ran for 23 seconds before being stopped."
 * ============================================================================
 */

typedef struct {

    uint64_t start_time;
    uint32_t requested_runtime_sec;
    uint32_t actual_runtime_sec;
    double estimated_volume_ml;
    double actual_volume_ml;
    double sensor_value;
    double target_value;
    dose_decision_t decision;
    dose_reason_t reason;
} dose_result_t;


/*
 * ============================================================================
 * Dosing engine
 * ============================================================================
 *
 * Pure calculation stage.
 *
 * Ideally this function should not know about:
 *
 *   GPIO
 *   MQTT
 *   timers
 *   threads
 *   logging
 *   Raspberry Pi hardware
 *
 * It takes state/configuration as input and produces a request.
 * ============================================================================
 *
 * NOTE:
 *
 * sensor_snapshot_t is intentionally not defined here.
 * See the sensor-types comment near the top of this file.
 */

typedef struct sensor_snapshot sensor_snapshot_t;


/*
 * Calculate what dose should be requested based on the current sensor state
 * and doser configuration.
 */
dose_request_t dosing_calculate_request(const dose_config_t *config,const sensor_snapshot_t *sensor
);


/*
 * ============================================================================
 * Safety evaluation
 * ============================================================================
 *
 * Determines whether a calculated dose is actually permitted.
 *
 * This should contain the safety/permission logic, not the chemistry
 * calculation itself.
 * ============================================================================
 */

dose_permission_t dosing_check_safety(const dose_config_t *config, const dose_request_t *request
);


/*
 * ============================================================================
 * Dose execution
 * ============================================================================
 *
 * The actual implementation will eventually interface with the pump/output
 * layer.
 *
 * The dosing module should ideally orchestrate this rather than directly
 * knowing GPIO details.
 * ============================================================================
 */

dose_result_t dosing_execute( const dose_request_t *request,const dose_permission_t *permission
);


/*
 * ============================================================================
 * Utility / logging helpers
 * ============================================================================
 */

const char *dosing_reason_string(dose_reason_t reason);

const char *dosing_decision_string(dose_decision_t decision);

#endif /* DOSING_H */