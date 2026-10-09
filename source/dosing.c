
#include "dosing.h"

#include <string.h>


/*
 * ============================================================================
 * Dosing calculation
 * ============================================================================
 *
 * This is the first actual dosing decision point.
 *
 * Something outside this function triggers the dosing cycle, for example:
 *
 *     scheduler
 *         |
 *         v
 *     dosing_calculate_request()
 *
 * The function examines:
 *
 *     dose_config_t
 *     sensor_snapshot_t
 *
 * and produces:
 *
 *     dose_request_t
 *
 * Nothing has been pumped at this point.
 * ============================================================================
 */

dose_request_t dosing_calculate_request( const dose_config_t *config,const sensor_snapshot_t *sensor)
{
    dose_request_t request = {0};

    /*
     * TODO:
     *
     * 1. Validate config
     * 2. Validate sensor
     * 3. Check sensor quality
     * 4. Determine distance from target
     * 5. Find matching dose table entry
     * 6. Calculate requested runtime
     * 7. Calculate estimated volume
     * 8. Apply calculation-level limits
     * 9. Populate request
     */

    (void)config;
    (void)sensor;

    request.reason = DOSE_REASON_NONE;
    request.sensor_value = 0.0;
    request.target_value = 0.0;
    request.requested_runtime_sec = 0;
    request.estimated_volume_ml = 0.0;
    request.unclamped_runtime_sec = 0;

    return request;
}


/*
 * ============================================================================
 * Safety evaluation
 * ============================================================================
 *
 * This is the second major decision point.
 *
 * We already have a dose request.
 *
 * Now we ask:
 *
 *     "Are we actually allowed to execute it?"
 *
 * ============================================================================
 */

dose_permission_t dosing_check_safety(const dose_config_t *config, const dose_request_t *request)
{
    dose_permission_t permission = {0};

    /*
     * TODO:
     *
     * Check things such as:
     *
     *     - doser enabled
     *     - request is non-zero
     *     - sensor still valid
     *     - filter pump running
     *     - flow confirmed
     *     - tank level OK
     *     - global interlock
     *     - local interlock
     *     - maximum single dose
     *     - cumulative period limit
     *     - minimum dose interval
     *     - mixing delay
     *
     * The first failed condition should produce a BLOCK decision and
     * an appropriate dose_reason_t.
     */

    (void)config;
    (void)request;

    permission.decision = DOSE_DECISION_BLOCK;
    permission.reason = DOSE_REASON_NONE;

    return permission;
}


/*
 * ============================================================================
 * Dose execution
 * ============================================================================
 *
 * This function represents the transition from:
 *
 *     "We want to dose"
 *
 * to:
 *
 *     "We actually operated the pump."
 *
 * ============================================================================
 */

dose_result_t dosing_execute(const dose_request_t *request, const dose_permission_t *permission)
{
    dose_result_t result = {0};

    /*
     * TODO:
     *
     * If permission != ALLOW:
     *
     *     - do not start pump
     *     - record blocked result
     *
     * If permission == ALLOW:
     *
     *     1. Record start time
     *     2. Start pump
     *     3. Run requested timer
     *     4. Monitor emergency/interlock conditions
     *     5. Stop pump
     *     6. Record actual runtime
     *     7. Calculate actual volume
     *     8. Update dosing statistics/history
     *     9. Publish/log result
     */

    (void)request;
    (void)permission;

    return result;
}


/*
 * ============================================================================
 * Reason strings
 * ============================================================================
 */

const char *dosing_reason_string(dose_reason_t reason)
{
    switch (reason) {

        case DOSE_REASON_NONE:
            return "none";

        case DOSE_REASON_THRESHOLD:
            return "threshold";

        case DOSE_REASON_TARGET_REACHED:
            return "target_reached";

        case DOSE_REASON_SENSOR_NOT_READY:
            return "sensor_not_ready";

        case DOSE_REASON_SENSOR_INVALID:
            return "sensor_invalid";

        case DOSE_REASON_INTERLOCK:
            return "interlock";

        case DOSE_REASON_FLOW_NOT_CONFIRMED:
            return "flow_not_confirmed";

        case DOSE_REASON_FILTER_PUMP_OFF:
            return "filter_pump_off";

        case DOSE_REASON_TANK_LOW:
            return "tank_low";

        case DOSE_REASON_MAX_SINGLE_DOSE:
            return "max_single_dose";

        case DOSE_REASON_MAX_PERIOD_VOLUME:
            return "max_period_volume";

        case DOSE_REASON_MIN_DOSE_INTERVAL:
            return "min_dose_interval";

        case DOSE_REASON_MIXING_DELAY:
            return "mixing_delay";

        case DOSE_REASON_DISABLED:
            return "disabled";

        case DOSE_REASON_INVALID_CONFIG:
            return "invalid_config";

        case DOSE_REASON_ZERO_FLOW_RATE:
            return "zero_flow_rate";

        case DOSE_REASON_MANUAL:
            return "manual";

        default:
            return "unknown";
    }
}


/*
 * ============================================================================
 * Decision strings
 * ============================================================================
 */

const char *dosing_decision_string(dose_decision_t decision)
{
    switch (decision) {

        case DOSE_DECISION_NONE:
            return "none";

        case DOSE_DECISION_ALLOW:
            return "allow";

        case DOSE_DECISION_BLOCK:
            return "block";

        case DOSE_DECISION_ERROR:
            return "error";

        default:
            return "unknown";
    }
}