
#ifndef EZO_H_
#define EZO_H_

#include <stdbool.h>


// I2C bus
#define I2C_BUS         "/dev/i2c-1"

// EZO device I2C addresses (default from Atlas Scientific)
#define EZO_DO_ADDR     0x61  // Dissolved Oxygen (DO)
#define EZO_ORP_ADDR    0x62  // Oxidation-Reduction Potential (ORP)
#define EZO_PH_ADDR     0x63  // pH Circuit
#define EZO_EC_ADDR     0x64  // Conductivity (EC)

#define EZO_RTD_ADDR    0x66  // Temperature (RTD)
#define EZO_PMP_ADDR    0x67  // Dosing Pump (EZO-PMP)
#define EZO_FLO_ADDR    0x68  // Flow Meter (EZO-FLOW)

#define EZO_CO2_ADDR    0x69  // Carbon Dioxide (CO2)
#define EZO_O2_ADDR     0x6A  // Oxygen (O2)
#define EZO_PRS_ADDR    0x6B  // Pressure (EZO-PRS)
#define EZO_HUM_ADDR    0x70  // Humidity (EZO-HUM)

// EZO response status codes
#define EZO_SUCCESS     1
#define EZO_PENDING     254
#define EZO_SYNTAX_ERR  2
#define EZO_NO_DATA     255
#define EZO_ERROR       -1    // Hardware, communication, or bus-level failure

// Command wait times in milliseconds
#define EZO_WAIT_READ          900
#define EZO_WAIT_CALIBRATE    1600
#define EZO_WAIT_GENERAL       300
#define EZO_WAIT_RTD           600
#define EZO_WAIT_EC            600
#define EZO_WAIT_EC_TEMP       900
#define EZO_WAIT_PUMP          100

// ─── Default EC values ──────────────────────────────────────────────────────

#define EC_DEFAULT_K 1.0f
#define EC_DEFAULT_TDS_FACTOR 0.5f

// ─── Calibration points ──────────────────────────────────────────────────────

#define PH_REF_LOW  4.00f
#define PH_REF_MID  7.00f
#define PH_REF_HIGH 10.00f

// ─── Enums ────────────────────────────────────────────────────────────────────

// Calibration point options for pH
typedef enum {
  PH_CAL_LOW  = 0,   // typically pH 4.00
  PH_CAL_MID  = 1,   // typically pH 7.00
  PH_CAL_HIGH = 2    // typically pH 10.00
} ph_cal_point_t;

// Temperature scale for RTD sensor
typedef enum {
  RTD_SCALE_CELSIUS    = 0,
  RTD_SCALE_FAHRENHEIT = 1,
  RTD_SCALE_KELVIN     = 2
} rtd_scale_t;

// Dosing pump direction
typedef enum {
  PUMP_FORWARD = 0,
  PUMP_REVERSE = 1
} pump_dir_t;

// ─── Structs ──────────────────────────────────────────────────────────────────

// Generic EZO sensor instance
typedef struct {
  unsigned char address;
  uint8_t flags;  // device subtype (e.g. EZO-EC has multiple subtypes)

  // Unions for any other sensor-specific parameters that may be needed in the future. 
  union { 
    float ec_sensor_k;
  };
  union {
    float ec_sensor_tds_factor;
  };
} ezo_sensor_t;


// Calibration status
typedef struct {
  int points;        // number of calibration points confirmed by device
} ezo_cal_status_t;

// pH reading result
typedef struct {
  float value;       // pH value e.g. 7.32
  int   status;      // EZO status code
} ph_reading_t;

// ORP reading result
typedef struct {
  float value;       // ORP in millivolts e.g. 350.5
  int   status;      // EZO status code
} orp_reading_t;

// RTD temperature reading result
typedef struct {
  float      value;   // temperature in the configured scale e.g. 25.00
  rtd_scale_t scale;  // scale the device is currently set to
  int        status;  // EZO status code
} rtd_reading_t;

// PRS Pressure reading result
typedef struct {
  float value;       // Pressure in psi e.g. 15.2
  int   status;      // EZO status code
} prs_reading_t;


// Dosing pump status
typedef struct {
  float requested_volume_ml;  // volume requested for the current/last dose (ml)
  int   is_pumping;           // 1 if pump is currently running, 0 if idle
  int   status;               // EZO status code
} pump_dose_status_t;

// EZO-EC conductivity reading result
typedef unsigned int ec_output_mask_t;

typedef struct {
  float conductivity;       // µS/cm
  float tds;                // ppm
  float salinity;           // PSU
  float specific_gravity;   // SG

  ec_output_mask_t valid;   // Which values are valid
  int status;               // EZO status code
} ec_reading_t;

// EZO-EC output parameters
typedef enum {
  EC_OUTPUT_CONDUCTIVITY     = 1 << 0,
  EC_OUTPUT_TDS              = 1 << 1,
  EC_OUTPUT_SALINITY         = 1 << 2,
  EC_OUTPUT_SPECIFIC_GRAVITY = 1 << 3
} ec_output_t;

// EZO SubType sensors used ezo_sensor_t.flags 
#define EC_CONDUCTIVITY     EC_OUTPUT_CONDUCTIVITY
#define EC_TDS              EC_OUTPUT_TDS
#define EC_SALINITY         EC_OUTPUT_SALINITY
#define EC_SPECIFIC_GRAVITY EC_OUTPUT_SPECIFIC_GRAVITY



// Address of an EZO device, or -1 for a NULL sensor.  -1 (like anything that fails
// i2c_addr_valid()) is rejected by ezo_open(), so callers need not NULL-check first.
static inline int ezo_sensor_addr(const ezo_sensor_t *sensor)
{
  return sensor ? (int)sensor->address : -1;
}


// ─── Bus utilities ────────────────────────────────────────────────────────────
int  ezo_bus_available();
//void ezo_i2cdetect();

const char *ezo_query_device_type(int addr);
const char *ezo_name_from_addr(int addr);

// ─── Generic EZO helpers (use when adding new sensor types) ──────────────────
int ezo_get_info(ezo_sensor_t *sensor, char *info, int len);
int ezo_get_status(ezo_sensor_t *sensor, char *status, int len);
int ezo_clear_calibration(ezo_sensor_t *sensor);
int ezo_get_cal_status(ezo_sensor_t *sensor, ezo_cal_status_t *cal);
int ezo_sleep(ezo_sensor_t *sensor);

// ─── pH sensor ────────────────────────────────────────────────────────────────
ph_reading_t ph_get_reading(ezo_sensor_t *ph);
ph_reading_t ph_get_reading_compensated(ezo_sensor_t *ph, float temp_c);
ph_reading_t ph_get_reading_filtered(ezo_sensor_t *ph);
int ph_calibrate(ezo_sensor_t *ph, ph_cal_point_t point, float ph_value);
int ph_calibrate_by_value(ezo_sensor_t *ph, float calibrationValue);
int ph_calibrate_low(ezo_sensor_t *ph);
int ph_calibrate_mid(ezo_sensor_t *ph);
int ph_calibrate_high(ezo_sensor_t *ph);
int ph_get_cal_status(ezo_sensor_t *ph, ezo_cal_status_t *cal);
int ph_clear_calibration(ezo_sensor_t *ph);
int ph_get_info(ezo_sensor_t *ph, char *info, int len);
int ph_get_status(ezo_sensor_t *ph, char *status, int len);
int ph_sleep(ezo_sensor_t *ph);

// ─── ORP sensor ───────────────────────────────────────────────────────────────
orp_reading_t orp_get_reading(ezo_sensor_t *orp);
int orp_calibrate(ezo_sensor_t *orp, float mv_value);
int orp_get_cal_status(ezo_sensor_t *orp, ezo_cal_status_t *cal);
int orp_clear_calibration(ezo_sensor_t *orp);
int orp_get_info(ezo_sensor_t *orp, char *info, int len);
int orp_get_status(ezo_sensor_t *orp, char *status, int len);
int orp_sleep(ezo_sensor_t *orp);

// ─── RTD temperature sensor ───────────────────────────────────────────────────
rtd_reading_t rtd_get_reading(ezo_sensor_t *rtd);
int rtd_set_scale(ezo_sensor_t *rtd, rtd_scale_t scale);
int rtd_calibrate(ezo_sensor_t *rtd, float known_temp);
int rtd_get_cal_status(ezo_sensor_t *rtd, ezo_cal_status_t *cal);
int rtd_clear_calibration(ezo_sensor_t *rtd);
int rtd_get_info(ezo_sensor_t *rtd, char *info, int len);
int rtd_get_status(ezo_sensor_t *rtd, char *status, int len);
int rtd_sleep(ezo_sensor_t *rtd);

// ─── Pressure sensor (EZO-PRS)  ──────────────────────────────
prs_reading_t prs_get_reading(ezo_sensor_t *prs);
int prs_calibrate(ezo_sensor_t *prs, float psi_value);
int prs_calibrate_zero(ezo_sensor_t *prs);
int prs_get_cal_status(ezo_sensor_t *prs, ezo_cal_status_t *cal);
int prs_clear_calibration(ezo_sensor_t *prs);
int prs_get_info(ezo_sensor_t *prs, char *info, int len);
int prs_get_status(ezo_sensor_t *prs, char *status, int len);
int prs_sleep(ezo_sensor_t *prs);

// ─── Conductivity sensor (EZO-EC) ─────────────────────────────────────────────

// Read conductivity and any other enabled EC output parameters
ec_reading_t ec_get_reading(ezo_sensor_t *ec);
// Set temperature compensation and return a compensated reading
ec_reading_t ec_get_reading_compensated(ezo_sensor_t *ec, float temp_c);
// Output parameter configuration
int ec_get_output_mask(ezo_sensor_t *ec, ec_output_mask_t *mask);
int ec_set_output(ezo_sensor_t *ec, ec_output_t output, bool enabled);
// Probe K value
int ec_set_k(ezo_sensor_t *ec, float k);
int ec_get_k(ezo_sensor_t *ec, float *k);
// TDS conversion factor
int ec_set_tds_factor(ezo_sensor_t *ec, float factor);
int ec_get_tds_factor(ezo_sensor_t *ec, float *factor);
// Calibration
int ec_calibrate_dry(ezo_sensor_t *ec);
int ec_calibrate(ezo_sensor_t *ec, float conductivity);
int ec_calibrate_low(ezo_sensor_t *ec, float conductivity);
int ec_calibrate_high(ezo_sensor_t *ec, float conductivity);
int ec_get_cal_status(ezo_sensor_t *ec,ezo_cal_status_t *cal);
int ec_clear_calibration(ezo_sensor_t *ec);
// Device information
int ec_get_info(ezo_sensor_t *ec, char *info, int len);
int ec_get_status(ezo_sensor_t *ec, char *status, int len);
int ec_sleep(ezo_sensor_t *ec);



// ─── Dosing pump (EZO-PMP) ────────────────────────────────────────────────────

/* ---- start a dose (the four device modes) ------------------------------ */
int pump_dose_volume(ezo_sensor_t *pump, float ml);                          /* D,<ml>          */
int pump_dose_volume_over_time(ezo_sensor_t *pump, float ml, int minutes);   /* D,<ml>,<min>    */
int pump_dose_rate(ezo_sensor_t *pump, float ml_per_min, int minutes);       /* DC,<rate>,<min> */
int pump_dose_rate_continuous(ezo_sensor_t *pump, float ml_per_min);         /* DC,<rate>,*     */
int pump_dose_max_continuous(ezo_sensor_t *pump);                            /* D,*             */

/* ---- control ----------------------------------------------------------- */
int pump_stop(ezo_sensor_t *pump);                                           /* X               */
int pump_pause(ezo_sensor_t *pump);                                          /* P (idempotent)  */
int pump_resume(ezo_sensor_t *pump);                                         /* P (idempotent)  */
int pump_is_paused(ezo_sensor_t *pump);                                      /* P,?  1/0, -1 err*/

/* ---- query ------------------------------------------------------------- */
pump_dose_status_t pump_get_dose_status(ezo_sensor_t *pump);                 /* D,?             */
float pump_get_dispensed_volume(ezo_sensor_t *pump);                         /* R               */
float pump_get_total_volume(ezo_sensor_t *pump);                             /* TV,?            */
float pump_get_absolute_total_volume(ezo_sensor_t *pump);                    /* ATV,? (no Clear)*/
int   pump_clear_total_volume(ezo_sensor_t *pump);                           /* Clear           */
float pump_get_max_flow_rate(ezo_sensor_t *pump);                            /* DC,?  ml/min    */
float pump_get_voltage(ezo_sensor_t *pump);                                  /* PV,?  volts     */

/* ---- calibration ------------------------------------------------------- */
int pump_get_calibration_status(ezo_sensor_t *pump);                         /* Cal,? 0..3, -1  */
int pump_set_calibration_volume(ezo_sensor_t *pump, float ml);               /* Cal,<ml>        */
int pump_clear_calibration(ezo_sensor_t *pump);                              /* Cal,clear       */

/* ---- device ------------------------------------------------------------ */
int pump_find(ezo_sensor_t *pump);                                           /* Find (LED)      */
int pump_set_i2c_address(ezo_sensor_t *pump, int new_addr);                  /* I2C,<n>         */
int pump_get_info(ezo_sensor_t *pump, char *info, int len);                  /* i               */
int pump_get_device_status(ezo_sensor_t *pump, char *status, int len);       /* Status          */
int pump_sleep(ezo_sensor_t *pump);                                          /* Sleep           */

#endif

/*
// Dose a fixed volume (ml) — pump runs until volume is dispensed then stops
int pump_dose(float ml);

// Dose at a continuous rate (ml/min) — runs until pump_stop() is called
int pump_dose_continuous(float ml_per_min);

// Dose a fixed volume at a specific rate (ml at ml/min)
int pump_dose_volume_at_rate(float ml, float ml_per_min);

// Stop pumping immediately
int pump_stop();

// Pause/resume pump (retains remaining dose target)
int pump_pause();
int pump_resume();

// Set pump direction
int pump_set_direction(pump_dir_t dir);

// Get current pump status and volume dispensed
pump_status_t pump_get_status();
float pump_get_dispensed_volume();
// Get total volume dispensed since last clear (ml)
float pump_get_total_volume();

// Clear the total volume counter
int pump_clear_total_volume();

int pump_get_info(char *info, int len);
int pump_sleep();
*/
