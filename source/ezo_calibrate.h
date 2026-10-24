

#ifndef EZO_CALIBRATE_H_
#define EZO_CALIBRATE_H_

#include "ezo.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef enum {
    EZO_PH,
    EZO_ORP,
    EZO_PRS,
    EZO_RTD,
    EZO_EC,
    EZO_PMP,
} ezo_device_type_t;

typedef enum {
    EZO_ACTION_READING,
    EZO_ACTION_CALIBRATE,
    EZO_ACTION_STATUS,
    EZO_ACTION_CLEAR,
    EZO_ACTION_EXPORT,
    EZO_ACTION_IMPORT,
    EZO_ACTION_DOSE,
} ezo_action_t;

typedef enum {
    EZO_OPT_NONE,
    EZO_OPT_LOW,
    EZO_OPT_MID,
    EZO_OPT_HIGH,
    EZO_OPT_DRY
} ezo_option_t;

typedef struct {
    ezo_device_type_t device;
    ezo_action_t action;
    ezo_option_t option; // Now an enum!
    float float_value;
    char *filename;      // points to argv string for export/import
} ezo_calibrate_t;


bool ezo_calibrate_parse_args(int argc, char *argv[], ezo_calibrate_t *parsed);
float ezo_calibrate_action(ezo_calibrate_t *cal);

// Save / restore an EZO circuit's calibration as a small ASCII file.
//
//   # AquachemD EZO calibration export
//   format=1
//   device=EC            <- from the circuit's "i" command; load refuses a different type
//   firmware=2.10
//   chunks=10
//   bytes=120
//   crc32=1A2B3C4D       <- over the strings, detects a damaged or hand-edited file
//   1:<string>           <- the circuit's export strings, verbatim (spaces are kept)
//   2:<string>
//   ...
//
// Return values: EZO_SUCCESS, or anything ezo_download_calibration() /
// ezo_upload_calibration() return, plus:
#define EZO_CAL_FILE_BAD        (-10)   // missing, corrupt or unsupported file (nothing was sent)
#define EZO_CAL_DEVICE_MISMATCH (-11)   // file is for a different type of circuit (nothing was sent)

// Reads the calibration out of the circuit and writes it to `filename`.
// The file is written to "<filename>.tmp" and renamed, so a failure never leaves a
// half-written file behind or damages an existing one.
int ezo_save_calibration_file(ezo_sensor_t *sensor, const char *filename);

// Checks the file completely (format, counts, CRC, circuit type), and only then
// uploads it to the circuit.  A firmware difference is logged, not refused.
int ezo_load_calibration_file(ezo_sensor_t *sensor, const char *filename);

#endif // EZO_CALIBRATE_H_