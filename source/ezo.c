#define _GNU_SOURCE
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "ezo.h"
#include "utils.h"
#include "i2c_bus.h"
#include "acd_types.h"

// ─── Real implementation ──────────────────────────────────────────────────────
// All code in this block is compiled only when DUMMY_SENSORS is NOT defined.
// To build with fake sensors: make dummy
// To build normally:          make

#ifndef DUMMY_SENSORS

#include <linux/i2c-dev.h>

// ─── Core I2C functions (private) ────────────────────────────────────────────
//
// Everything that talks to a circuit goes through these layers, so locking, response
// timing and the "still busy" retry behave identically for every device type:
//
//   ezo_open / ezo_close   validate the address, take / release the per-address lock
//   ezo_read               wait, then one read of the circuit's response
//   ezo_read_retry         ezo_read, re-reading while the circuit answers 254 (busy)
//   ezo_xfer               send + wait + read_retry on an already-open fd.  Call it
//                          directly only when several exchanges must share ONE lock
//                          hold (see pump_set_paused)
//   ezo_command            open + xfer + close            (fixed command string)
//   ezo_commandf           same, printf-style command     (refuses a truncated command)
//   ezo_send_only          open + send + close            (Sleep, I2C,n: no reply)
//   ezo_read_value         "R" -> number
//   ezo_query_float        "?XX,<n>" style query -> number
//
// The only deliberate exception is the bus scan in ezo_i2cdetect(), which does raw
// reads to probe addresses.

// Every bus transaction in this file starts here.  The address is validated first,
// because i2c_open() treats a negative address as "no lock, no ioctl", and a NULL
// sensor reaches us as -1 (see ezo_sensor_addr()).  i2c_open() then takes the
// per-address lock, which i2c_close() (via ezo_close) releases.
static int ezo_open(int address)
{
  if (!i2c_addr_valid(address)) return EZO_ERROR;
  return i2c_open(I2C_BUS, address);
}

static int ezo_close(int fd)
{
  return i2c_close(fd);
}

static void ezo_delay_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

// What kind of circuit a command is going to.  The response time of the same command
// differs between circuits (an "R" is 600 ms on an EC or RTD, 900 ms on a pH, 100 ms
// on a pump), so the caller says what it is talking to and the timing lives in ONE
// place, ezo_get_wait_ms().
typedef enum {
  EZO_KIND_ANY,     // generic helpers that don't know the circuit (i, Status, Cal,clear)
  EZO_KIND_PH,
  EZO_KIND_ORP,
  EZO_KIND_RTD,
  EZO_KIND_PRS,
  EZO_KIND_EC,
  EZO_KIND_PMP
} ezo_kind_t;

// How long to wait before the first read.  This is the datasheet figure; a circuit
// that needs longer answers 254 and ezo_read_retry() keeps reading, so it only has
// to be good enough that the usual case needs no retry.
static int ezo_get_wait_ms(ezo_kind_t kind, const char *cmd)
{
  // The pump answers quickly and has no slow commands; anything longer is caught
  // by the retry (ESPHome waits 400 ms for every pump command).
  if (kind == EZO_KIND_PMP)
    return EZO_WAIT_PUMP;

  if (strcasecmp(cmd, "R") == 0)
  {
    if (kind == EZO_KIND_RTD) return EZO_WAIT_RTD;
    if (kind == EZO_KIND_EC)  return EZO_WAIT_EC;
    return EZO_WAIT_READ;
  }

  if (strncasecmp(cmd, "RT,", 3) == 0)          // EC: temperature compensated read
    return EZO_WAIT_EC_TEMP;

  if (strchr(cmd, '?') != NULL)                 // queries (Cal,? O,? K,? ...) are quick
    return EZO_WAIT_GENERAL;

  if (strncasecmp(cmd, "Cal", 3) == 0)
    return EZO_WAIT_CALIBRATE;

  return EZO_WAIT_GENERAL;
}

static int ezo_read(int fd, char *response, int len, int wait_ms)
{
  unsigned char buf[64] = {0};
  ezo_delay_ms(wait_ms);

  if (read(fd, buf, sizeof(buf) - 1) < 0)       // -1 keeps buf NUL terminated
    return EZO_ERROR;

  if (buf[0] != EZO_SUCCESS)
    return buf[0];

  strncpy(response, (char *)&buf[1], len - 1);
  response[len - 1] = '\0';
  return EZO_SUCCESS;
}

static int ezo_send_cmd(int fd, const char *cmd)
{
  return write(fd, cmd, strlen(cmd));
}

//#define EZO_RETRIES        8      // re-reads while the circuit answers 254 (busy)
//#define EZO_RETRY_WAIT_MS  100

static int ezo_read_retry(int fd, char *response, int len, int wait_ms)
{
  int status = ezo_read(fd, response, len, wait_ms);
  for (int i = 0; status == EZO_PENDING && i < EZO_RETRIES; i++)
    status = ezo_read(fd, response, len, EZO_RETRY_WAIT_MS);
  return status;
}

// One command/response exchange on an already-open fd.  result may be NULL when the
// reply isn't wanted (the circuit still has to be read so it is ready for the next
// command).  result[0] is cleared first so a failed exchange never leaves stale text.
static int ezo_xfer(int fd, ezo_kind_t kind, const char *cmd, char *result, int result_len)
{
  char scratch[64];
  if (result == NULL || result_len <= 0)
  {
    result = scratch;
    result_len = (int)sizeof(scratch);
  }
  result[0] = '\0';

  if (ezo_send_cmd(fd, cmd) < 0)
    return EZO_ERROR;

  return ezo_read_retry(fd, result, result_len, ezo_get_wait_ms(kind, cmd));
}

// One locked transaction: ezo_open (validate + lock) -> exchange -> ezo_close (unlock).
static int ezo_command(ezo_kind_t kind, const ezo_sensor_t *sensor,
                       const char *cmd, char *result, int result_len)
{
  int fd = ezo_open(ezo_sensor_addr(sensor));
  if (fd < 0) return EZO_ERROR;

  int status = ezo_xfer(fd, kind, cmd, result, result_len);
  ezo_close(fd);
  return status;
}

/* snprintf that reports truncation, so a clipped number is never sent. */
static int ezo_vbuild(char *buf, size_t n, const char *fmt, va_list ap)
{
  int len = vsnprintf(buf, n, fmt, ap);
  return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

static int ezo_build(char *buf, size_t n, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
static int ezo_build(char *buf, size_t n, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int rc = ezo_vbuild(buf, n, fmt, ap);
  va_end(ap);
  return rc;
}

// ezo_command() with a printf-style command.  Returns EZO_SYNTAX_ERR, without
// touching the bus, if the command would not fit.
static int ezo_commandf(ezo_kind_t kind, const ezo_sensor_t *sensor,
                        char *result, int result_len, const char *fmt, ...)
  __attribute__((format(printf, 5, 6)));
static int ezo_commandf(ezo_kind_t kind, const ezo_sensor_t *sensor,
                        char *result, int result_len, const char *fmt, ...)
{
  char cmd[32];
  va_list ap;

  va_start(ap, fmt);
  int rc = ezo_vbuild(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);
  if (rc < 0) return EZO_SYNTAX_ERR;

  return ezo_command(kind, sensor, cmd, result, result_len);
}

// For commands the circuit never answers (Sleep, and I2C,<n> which restarts it at a
// new address): send and release, don't wait for a reply.
static int ezo_send_only(const ezo_sensor_t *sensor, const char *cmd)
{
  int fd = ezo_open(ezo_sensor_addr(sensor));
  if (fd < 0) return EZO_ERROR;

  int rc = (ezo_send_cmd(fd, cmd) < 0) ? EZO_ERROR : EZO_SUCCESS;
  ezo_close(fd);
  return rc;
}

// Plain "R" returned as a number.  *value is only written on success.
static int ezo_read_value(ezo_kind_t kind, const ezo_sensor_t *sensor, float *value)
{
  char response[32];
  int status = ezo_command(kind, sensor, "R", response, sizeof(response));
  if (status == EZO_SUCCESS)
    *value = (float)atof(response);
  return status;
}

/* text after the n-th comma (n >= 1), or NULL */
static const char *ezo_field(const char *s, int n)
{
  for (; n > 0; n--) {
    s = strchr(s, ',');
    if (!s) return NULL;
    s++;
  }
  return s;
}

static int ezo_to_float(const char *s, float *out)
{
  if (!s) return -1;
  char *end = NULL;
  float v = strtof(s, &end);
  if (end == s) return -1;
  *out = v;
  return 0;
}

/* Query returning a single number.  skip_prefix=1 for "?XX,<n>" replies,
 * 0 for bare numbers ("R").  Returns -1.0f on any failure.                  */
static float ezo_query_float(ezo_kind_t kind, const ezo_sensor_t *sensor,
                             const char *cmd, int skip_prefix)
{
  char resp[32];
  float v = 0.0f;

  if (ezo_command(kind, sensor, cmd, resp, sizeof(resp)) != EZO_SUCCESS) return -1.0f;
  return (ezo_to_float(skip_prefix ? ezo_field(resp, 1) : resp, &v) == 0) ? v : -1.0f;
}



// ─── I2C detect ──────────────────────────────────────────────────────────────

typedef struct {
  int         addr;
  const char *name;
} ezo_addr_map_t;

static const ezo_addr_map_t ezo_known_devices[] = {
  { 0x61, "DO"    },
  { 0x62, "ORP"   },
  { 0x63, "pH"    },
  { 0x64, "EC"    },
  { 0x66, "RTD"   },
  { 0x67, "PUMP"  },
  { 0x68, "FLOW"  },
  { 0x69, "CO2"   },
  { 0x6A, "O2"    },
  { 0x6B, "PRS"   },
  { 0x70, "HUM"   },
  { 0,    NULL    }
};

const char *ezo_name_from_addr(int addr)
{
  for (int i = 0; ezo_known_devices[i].name != NULL; i++)
    if (ezo_known_devices[i].addr == addr)
      return ezo_known_devices[i].name;
  return NULL;
}

const char *ezo_query_device_type(int addr)
{
  static char type_buf[16];
  char resp[64];

  // Address-only probe, so build a throwaway sensor for the generic path.  This takes
  // the per-address lock and gets the same timing and 254 retry as every other command.
  if (addr < 0 || addr > 0xFF) return NULL;
  ezo_sensor_t probe = { .address = (unsigned char)addr };
  if (ezo_command(EZO_KIND_ANY, &probe, "i", resp, sizeof(resp)) != EZO_SUCCESS)
    return NULL;

  char *start = strchr(resp, ',');           // "?I,<type>,<version>"
  if (!start) return NULL;
  start++;
  char *end = strchr(start, ',');
  if (!end) end = start + strlen(start);

  int len = end - start;
  if (len <= 0 || len >= (int)sizeof(type_buf)) return NULL;
  strncpy(type_buf, start, len);
  type_buf[len] = '\0';
  return type_buf;
}

/*
This will probe EZO devices well, but potentially break generic i2c sensors forcing them to need a reset.
*/

void ezo_i2cdetect()
{
  // One fd walks every address, so it is opened with addr = -1 (no lock, no
  // I2C_SLAVE) and each probe takes that address's lock itself.  A device that is
  // mid-transaction on another thread is skipped until it is free.
  int fd = i2c_open(I2C_BUS, -1);
  if (fd < 0) { perror("open i2c"); return; }

  printf("\nScanning I2C bus %s...\n\n", I2C_BUS);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    printf("%02x: ", row * 16);
    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) { printf("   "); continue; }
      if (i2c_lock_addr(addr) != 0)   { printf("   "); continue; }

      unsigned char buf;
      int sel = ioctl(fd, I2C_SLAVE, addr);
      int rd  = (sel < 0) ? -1 : (int)read(fd, &buf, 1);
      i2c_unlock_addr(addr);

      if (sel < 0) { printf("   "); continue; }
      if (rd < 0)
        printf("-- ");
      else
      {
        printf("%02x ", addr);
        detected[count++] = addr;
      }
    }
    printf("\n");
  }
  i2c_close(fd);

  if (count == 0) { printf("\nNo devices found.\n"); return; }

  printf("\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known   = ezo_name_from_addr(addr);
    const char *queried = ezo_query_device_type(addr);

    if (queried)
      printf("  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    else if (known)
      printf("  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    else {/*
      const char *known = i2c_name_from_addr(addr);
      if (known)
        printf("  0x%02x  likely: %s (by default address, unconfirmed)\n", addr, known);
      else*/
        printf("  0x%02x  unknown device\n", addr);
    }
  }
  printf("\n");
}

// ─── Bus availability ─────────────────────────────────────────────────────────

int ezo_bus_available()
{
  int fd = i2c_open(I2C_BUS, -1);   // just proves the bus device opens
  if (fd < 0) return 0;
  i2c_close(fd);
  return 1;
}

// ─── Shared EZO helpers ───────────────────────────────────────────────────────
// Every helper takes the sensor struct.  A NULL sensor, or one with an invalid
// address, fails in ezo_open() with EZO_ERROR before anything touches the bus.

int ezo_get_info(ezo_sensor_t *sensor, char *info, int len)
{
  return ezo_command(EZO_KIND_ANY, sensor, "i", info, len);
}

int ezo_get_status(ezo_sensor_t *sensor, char *status, int len)
{
  return ezo_command(EZO_KIND_ANY, sensor, "Status", status, len);
}

int ezo_clear_calibration(ezo_sensor_t *sensor)
{
  return ezo_command(EZO_KIND_ANY, sensor, "Cal,clear", NULL, 0);
}

int ezo_get_cal_status(ezo_sensor_t *sensor, ezo_cal_status_t *cal)
{
  char response[32];
  if (cal == NULL) return EZO_ERROR;

  int status = ezo_command(EZO_KIND_ANY, sensor, "Cal,?", response, sizeof(response));
  if (status == EZO_SUCCESS)
  {
    int points = 0;
    if (sscanf(response, "?CAL,%d", &points) == 1)
      cal->points = points;
    else
      cal->points = EZO_ERROR;
  }
  return status;
}

int ezo_sleep(ezo_sensor_t *sensor)
{
  return ezo_send_only(sensor, "Sleep");
}

// ─── pH sensor ────────────────────────────────────────────────────────────────

ph_reading_t ph_get_reading(ezo_sensor_t *ph)
{
  ph_reading_t result = {0.0f, EZO_ERROR};
  result.status = ezo_read_value(EZO_KIND_PH, ph, &result.value);
  return result;
}

ph_reading_t ph_get_reading_compensated(ezo_sensor_t *ph, float temp_c)
{
  ph_reading_t result = {0.0f, EZO_ERROR};
  ezo_commandf(EZO_KIND_PH, ph, NULL, 0, "T,%.2f", temp_c);   // status not checked, as before
  result.status = ezo_read_value(EZO_KIND_PH, ph, &result.value);
  return result;
}

// Median of 3 readings — discards outliers from interference spikes
ph_reading_t ph_get_reading_filtered(ezo_sensor_t *ph)
{
  float readings[3];
  int valid = 0;
  for (int i = 0; i < 3; i++)
  {
    ph_reading_t r = ph_get_reading(ph);
    if (r.status == EZO_SUCCESS)
      readings[valid++] = r.value;
  }
  if (valid == 0) return (ph_reading_t){0.0f, EZO_ERROR};
  if (valid < 3)  return (ph_reading_t){readings[0], EZO_SUCCESS};
  if (readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  if (readings[1] > readings[2]) { float t = readings[1]; readings[1] = readings[2]; readings[2] = t; }
  if (readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  return (ph_reading_t){readings[1], EZO_SUCCESS};
}

int ph_calibrate(ezo_sensor_t *ph, ph_cal_point_t point, float ph_value)
{
  const char *point_str[] = {"low", "mid", "high"};
  if ((unsigned)point > PH_CAL_HIGH) return EZO_SYNTAX_ERR;
  return ezo_commandf(EZO_KIND_PH, ph, NULL, 0, "Cal,%s,%.2f", point_str[point], ph_value);
}

int ph_calibrate_mid(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_MID,  PH_REF_MID); }
int ph_calibrate_low(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_LOW,  PH_REF_LOW); }
int ph_calibrate_high(ezo_sensor_t *ph) { return ph_calibrate(ph, PH_CAL_HIGH, PH_REF_HIGH); }

int ph_get_cal_status(ezo_sensor_t *ph, ezo_cal_status_t *cal) { return ezo_get_cal_status(ph, cal); }
int ph_clear_calibration(ezo_sensor_t *ph)                     { return ezo_clear_calibration(ph); }
int ph_get_info(ezo_sensor_t *ph, char *info, int len)         { return ezo_get_info(ph, info, len); }
int ph_get_status(ezo_sensor_t *ph, char *status, int len)     { return ezo_get_status(ph, status, len); }
int ph_sleep(ezo_sensor_t *ph)                                 { return ezo_sleep(ph); }

int ph_calibrate_by_value(ezo_sensor_t *ph, float calibrationValue) {
    // Dynamically calculate the midpoints
    float low_mid_boundary  = (PH_REF_LOW + PH_REF_MID) / 2.0f;   // (4.00 + 7.00) / 2 = 5.50f
    float mid_high_boundary = (PH_REF_MID + PH_REF_HIGH) / 2.0f; // (7.00 + 10.00) / 2 = 8.50f

    //Anything below the low/mid midpoint
    if (calibrationValue < low_mid_boundary) {
        return ph_calibrate_low(ph);
    }
    // Anything between the low/mid midpoint and mid/high midpoint
    else if (calibrationValue >= low_mid_boundary && calibrationValue <= mid_high_boundary) {
        return ph_calibrate_mid(ph);
    }
    // Anything above the mid/high midpoint
    else {
        return ph_calibrate_high(ph);
    }
}

// ─── ORP sensor ───────────────────────────────────────────────────────────────

orp_reading_t orp_get_reading(ezo_sensor_t *orp)
{
  orp_reading_t result = {0.0f, EZO_ERROR};
  result.status = ezo_read_value(EZO_KIND_ORP, orp, &result.value);
  return result;
}

int orp_calibrate(ezo_sensor_t *orp, float mv_value)
{
  return ezo_commandf(EZO_KIND_ORP, orp, NULL, 0, "Cal,%.2f", mv_value);
}

int orp_get_cal_status(ezo_sensor_t *orp, ezo_cal_status_t *cal) { return ezo_get_cal_status(orp, cal); }
int orp_clear_calibration(ezo_sensor_t *orp)                     { return ezo_clear_calibration(orp); }
int orp_get_info(ezo_sensor_t *orp, char *info, int len)         { return ezo_get_info(orp, info, len); }
int orp_get_status(ezo_sensor_t *orp, char *status, int len)     { return ezo_get_status(orp, status, len); }
int orp_sleep(ezo_sensor_t *orp)                                 { return ezo_sleep(orp); }

// ─── RTD temperature sensor ───────────────────────────────────────────────────

rtd_reading_t rtd_get_reading(ezo_sensor_t *rtd)
{
  rtd_reading_t result = {0.0f, RTD_SCALE_CELSIUS, EZO_ERROR};
  result.status = ezo_read_value(EZO_KIND_RTD, rtd, &result.value);
  return result;
}

int rtd_set_scale(ezo_sensor_t *rtd, rtd_scale_t scale)
{
  const char *cmd;
  switch (scale)
  {
    case RTD_SCALE_FAHRENHEIT: cmd = "S,f"; break;
    case RTD_SCALE_KELVIN:     cmd = "S,k"; break;
    default:                   cmd = "S,c"; break;
  }
  return ezo_command(EZO_KIND_RTD, rtd, cmd, NULL, 0);
}

int rtd_calibrate(ezo_sensor_t *rtd, float known_temp)
{
  return ezo_commandf(EZO_KIND_RTD, rtd, NULL, 0, "Cal,%.2f", known_temp);
}

int rtd_get_cal_status(ezo_sensor_t *rtd, ezo_cal_status_t *cal) { return ezo_get_cal_status(rtd, cal); }
int rtd_clear_calibration(ezo_sensor_t *rtd)                     { return ezo_clear_calibration(rtd); }
int rtd_get_info(ezo_sensor_t *rtd, char *info, int len)         { return ezo_get_info(rtd, info, len); }
int rtd_get_status(ezo_sensor_t *rtd, char *status, int len)     { return ezo_get_status(rtd, status, len); }
int rtd_sleep(ezo_sensor_t *rtd)                                 { return ezo_sleep(rtd); }

// ─── Pressure sensor (EZO-PRS) ────────────────────────────────────────────────

prs_reading_t prs_get_reading(ezo_sensor_t *prs)
{
  prs_reading_t result = {0.0f, EZO_ERROR};
  result.status = ezo_read_value(EZO_KIND_PRS, prs, &result.value);
  return result;
}

int prs_calibrate(ezo_sensor_t *prs, float psi_value)
{
  return ezo_commandf(EZO_KIND_PRS, prs, NULL, 0, "Cal,%.2f", psi_value);
}

int prs_calibrate_zero(ezo_sensor_t *prs)
{
  return ezo_command(EZO_KIND_PRS, prs, "Cal,0", NULL, 0);
}

int prs_get_cal_status(ezo_sensor_t *prs, ezo_cal_status_t *cal) { return ezo_get_cal_status(prs, cal); }
int prs_clear_calibration(ezo_sensor_t *prs)                     { return ezo_clear_calibration(prs); }
int prs_get_info(ezo_sensor_t *prs, char *info, int len)         { return ezo_get_info(prs, info, len); }
int prs_get_status(ezo_sensor_t *prs, char *status, int len)     { return ezo_get_status(prs, status, len); }
int prs_sleep(ezo_sensor_t *prs)                                 { return ezo_sleep(prs); }


// ─── EZO-PMP dosing pump ─────────────────────────────────────────────────────

#ifndef PMP_RESTART_WAIT_MS
#define PMP_RESTART_WAIT_MS  1000     /* after I2C,<n> the device reboots          */
#endif

/* ---- start a dose ------------------------------------------------------ */

int pump_dose_volume(ezo_sensor_t *pump, float ml)
{
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;                 /* never send <= 0 or NaN */
  return ezo_commandf(EZO_KIND_PMP, pump, NULL, 0, "D,%.2f", ml);
}

/* "Dose over time": the device takes WHOLE minutes (ESPHome sends an int), so
 * this is not usable for second-scale doses.                                  */
int pump_dose_volume_over_time(ezo_sensor_t *pump, float ml, int minutes)
{
  if (!(ml > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  return ezo_commandf(EZO_KIND_PMP, pump, NULL, 0, "D,%.2f,%d", ml, minutes);
}

/* Constant flow rate for a fixed number of whole minutes. */
int pump_dose_rate(ezo_sensor_t *pump, float ml_per_min, int minutes)
{
  if (!(ml_per_min > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  return ezo_commandf(EZO_KIND_PMP, pump, NULL, 0, "DC,%.2f,%d", ml_per_min, minutes);
}

/* Constant flow rate until pump_stop().  The device needs the second field
 * ("DC,<rate>" alone is incomplete) and caps continuous runs at 20 days, so a
 * dead daemon leaves it running: prefer pump_dose_volume() for chemical dosing. */
int pump_dose_rate_continuous(ezo_sensor_t *pump, float ml_per_min)
{
  if (!(ml_per_min > 0.0f)) return EZO_SYNTAX_ERR;
  return ezo_commandf(EZO_KIND_PMP, pump, NULL, 0, "DC,%.2f,*", ml_per_min);
}

/* Continuous at the pump's maximum (calibrated) rate, until pump_stop(). */
int pump_dose_max_continuous(ezo_sensor_t *pump)
{
  return ezo_command(EZO_KIND_PMP, pump, "D,*", NULL, 0);
}

/* ---- control ----------------------------------------------------------- */

int pump_stop(ezo_sensor_t *pump)
{
  return ezo_command(EZO_KIND_PMP, pump, "X", NULL, 0);
}

/* P is a TOGGLE on the device (pause and resume are the same command), so a
 * blind P twice would resume.  Read P,? and toggle only when needed, all on one
 * fd (one lock hold).  If the state can't be read nothing is sent.
 * VERIFY: P while idle.                                                       */
static int pump_set_paused(ezo_sensor_t *pump, int want_paused)
{
  char resp[32];
  int fd = ezo_open(ezo_sensor_addr(pump));
  if (fd < 0) return EZO_ERROR;

  int rc = ezo_xfer(fd, EZO_KIND_PMP, "P,?", resp, sizeof(resp));
  if (rc == EZO_SUCCESS) {
    const char *f = ezo_field(resp, 1);                    /* "?P,<0|1>" */
    if (!f)
      rc = EZO_NO_DATA;
    else if ((atoi(f) == 1) != (want_paused != 0))
      rc = ezo_xfer(fd, EZO_KIND_PMP, "P", resp, sizeof(resp));
  }

  ezo_close(fd);
  return rc;
}

int pump_pause(ezo_sensor_t *pump)  { return pump_set_paused(pump, 1); }
int pump_resume(ezo_sensor_t *pump) { return pump_set_paused(pump, 0); }

/* 1 paused, 0 not paused, -1 error */
int pump_is_paused(ezo_sensor_t *pump)
{
  char resp[32];
  float v = 0.0f;

  if (ezo_command(EZO_KIND_PMP, pump, "P,?", resp, sizeof(resp)) != EZO_SUCCESS) return -1;
  if (ezo_to_float(ezo_field(resp, 1), &v) != 0) return -1;
  return ((int)v == 1) ? 1 : 0;
}

/* ---- query ------------------------------------------------------------- */

pump_dose_status_t pump_get_dose_status(ezo_sensor_t *pump)
{
  pump_dose_status_t result = { 0.0f, 0, EZO_ERROR };
  char resp[32];

  /* D,? is the dispense-state query.  ("Status" is the generic EZO restart-reason
   * command and says nothing about whether the pump is running.)               */
  result.status = ezo_command(EZO_KIND_PMP, pump, "D,?", resp, sizeof(resp));
  if (result.status != EZO_SUCCESS) return result;

  /* "?D,<volume>,<flag>": the flag is the LAST field and is what ESPHome keys on
   * (== 1 means dosing).  The volume is parsed leniently since it may not be
   * numeric while continuous (VERIFY: could be "*").                           */
  const char *first = ezo_field(resp, 1);
  const char *last  = strrchr(resp, ',');
  if (!first || last < first) {
    result.status = EZO_NO_DATA;
    return result;
  }

  float vol = 0.0f;
  result.requested_volume_ml = (ezo_to_float(first, &vol) == 0) ? vol : 0.0f;
  result.is_pumping = (atoi(last + 1) == 1);
  return result;
}

/* Volume dispensed by the current/last dose.
 * VERIFY: value after a completed dose, and after X.                          */
float pump_get_dispensed_volume(ezo_sensor_t *pump)        { return ezo_query_float(EZO_KIND_PMP, pump, "R", 0); }
float pump_get_total_volume(ezo_sensor_t *pump)            { return ezo_query_float(EZO_KIND_PMP, pump, "TV,?", 1); }
float pump_get_absolute_total_volume(ezo_sensor_t *pump)   { return ezo_query_float(EZO_KIND_PMP, pump, "ATV,?", 1); }
float pump_get_max_flow_rate(ezo_sensor_t *pump)           { return ezo_query_float(EZO_KIND_PMP, pump, "DC,?", 1); }
float pump_get_voltage(ezo_sensor_t *pump)                 { return ezo_query_float(EZO_KIND_PMP, pump, "PV,?", 1); }

int pump_clear_total_volume(ezo_sensor_t *pump)
{
  return ezo_command(EZO_KIND_PMP, pump, "Clear", NULL, 0);
}

/* ---- calibration ------------------------------------------------------- */

/* 0 uncalibrated, 1 fixed volume, 2 volume/time, 3 both; -1 on error. */
int pump_get_calibration_status(ezo_sensor_t *pump)
{
  float v = ezo_query_float(EZO_KIND_PMP, pump, "Cal,?", 1);
  return (v < 0.0f) ? -1 : (int)v;
}

/* VERIFY the datasheet procedure before calling (dispense, measure, then Cal,<ml>). */
int pump_set_calibration_volume(ezo_sensor_t *pump, float ml)
{
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  return ezo_commandf(EZO_KIND_PMP, pump, NULL, 0, "Cal,%.2f", ml);
}

int pump_clear_calibration(ezo_sensor_t *pump)
{
  return ezo_command(EZO_KIND_PMP, pump, "Cal,clear", NULL, 0);
}

/* ---- device ------------------------------------------------------------ */

/* Blinks the LED.  ESPHome treats it as a 60 s command; VERIFY whether any
 * following command cancels it.  We only wait for the ack.                    */
int pump_find(ezo_sensor_t *pump)
{
  return ezo_command(EZO_KIND_PMP, pump, "Find", NULL, 0);
}

/* One-time provisioning (two pumps both ship at 0x67).  The device restarts at
 * the new address and sends no usable reply, so success means the write was
 * accepted.  Refuses if something already answers at new_addr.  On success
 * pump->address is updated (not thread-safe against other users of the same
 * struct: do this at setup/provisioning time), then save it to the config.
 * VERIFY on the bench with ONE pump on the bus.                               */
int pump_set_i2c_address(ezo_sensor_t *pump, int new_addr)
{
  int addr = ezo_sensor_addr(pump);

  char cmd[16];

  if (!i2c_addr_valid(addr) || !i2c_addr_valid(new_addr)) return EZO_SYNTAX_ERR;
  if (new_addr == addr) return EZO_SUCCESS;
  if (i2c_probe_address(I2C_BUS, new_addr)) return EZO_ERROR;   /* occupied */
  if (ezo_build(cmd, sizeof(cmd), "I2C,%d", new_addr) < 0) return EZO_SYNTAX_ERR;

  int rc = ezo_send_only(pump, cmd);

  if (rc == EZO_SUCCESS) {
    pump->address = (unsigned char)new_addr;     /* keep the struct in step with the device */
    ezo_delay_ms(PMP_RESTART_WAIT_MS);
  }
  return rc;
}

/* The generic EZO helpers go through ezo_open() themselves, so they take the same
 * per-address lock.                                                           */
int pump_get_info(ezo_sensor_t *pump, char *info, int len)            { return ezo_get_info(pump, info, len); }
int pump_get_device_status(ezo_sensor_t *pump, char *status, int len) { return ezo_get_status(pump, status, len); }
int pump_sleep(ezo_sensor_t *pump)                                    { return ezo_sleep(pump); }


// ─── Conductivity sensor (EZO-EC) ─────────────────────────────────────────────

static int ec_parse_output_mask(const char *response,
                                ec_output_mask_t *mask)
{
  const char *p = response;

  *mask = 0;

  /*
   * Response is normally:
   *
   *     ?O,EC,TDS,S,SG
   *
   * The parameters are always reported in the order:
   *
   *     EC, TDS, S, SG
   */

  if (*p == '?')
    p++;

  if (strncasecmp(p, "O,", 2) == 0)
    p += 2;
  else
    return EZO_ERROR;

  char copy[64];
  snprintf(copy, sizeof(copy), "%s", p);

  char *saveptr = NULL;
  char *token = strtok_r(copy, ",", &saveptr);

  while (token != NULL)
  {
    if (strcasecmp(token, "EC") == 0)
      *mask |= EC_OUTPUT_CONDUCTIVITY;
    else if (strcasecmp(token, "TDS") == 0)
      *mask |= EC_OUTPUT_TDS;
    else if (strcasecmp(token, "S") == 0)
      *mask |= EC_OUTPUT_SALINITY;
    else if (strcasecmp(token, "SG") == 0)
      *mask |= EC_OUTPUT_SPECIFIC_GRAVITY;

    token = strtok_r(NULL, ",", &saveptr);
  }

  return EZO_SUCCESS;
}

int ec_get_output_mask(ezo_sensor_t *ec, ec_output_mask_t *mask)
{
  char response[64];

  if (mask == NULL)
    return EZO_ERROR;

  int status = ezo_command(EZO_KIND_EC, ec, "O,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  return ec_parse_output_mask(response, mask);
}

int ec_set_output(ezo_sensor_t *ec, ec_output_t output, bool enabled)
{
  const char *parameter;

  switch (output)
  {
    case EC_OUTPUT_CONDUCTIVITY:
      parameter = "EC";
      break;

    case EC_OUTPUT_TDS:
      parameter = "TDS";
      break;

    case EC_OUTPUT_SALINITY:
      parameter = "S";
      break;

    case EC_OUTPUT_SPECIFIC_GRAVITY:
      parameter = "SG";
      break;

    default:
      return EZO_ERROR;
  }

  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "O,%s,%d", parameter, enabled ? 1 : 0);
}

static int ec_parse_reading(const char *response, ec_output_mask_t mask, ec_reading_t *reading)
{
  char copy[64];
  char *saveptr = NULL;
  char *token;
  int field = 0;

  if (reading == NULL)
    return EZO_ERROR;

  memset(reading, 0, sizeof(*reading));

  snprintf(copy, sizeof(copy), "%s", response);

  token = strtok_r(copy, ",", &saveptr);

  while (token != NULL)
  {
    float value = strtof(token, NULL);

    switch (field)
    {
      case 0:
        if (mask & EC_OUTPUT_CONDUCTIVITY)
        {
          reading->conductivity = value;
          reading->valid |= EC_OUTPUT_CONDUCTIVITY;
        }
        break;

      case 1:
        if (mask & EC_OUTPUT_TDS)
        {
          reading->tds = value;
          reading->valid |= EC_OUTPUT_TDS;
        }
        break;

      case 2:
        if (mask & EC_OUTPUT_SALINITY)
        {
          reading->salinity = value;
          reading->valid |= EC_OUTPUT_SALINITY;
        }
        break;

      case 3:
        if (mask & EC_OUTPUT_SPECIFIC_GRAVITY)
        {
          reading->specific_gravity = value;
          reading->valid |= EC_OUTPUT_SPECIFIC_GRAVITY;
        }
        break;

      default:
        break;
    }

    field++;
    token = strtok_r(NULL, ",", &saveptr);
  }

  return reading->valid ? EZO_SUCCESS : EZO_NO_DATA;
}

// Shared by ec_get_reading() and ec_get_reading_compensated(): ask which outputs are
// enabled, read ("R", or "RT,<temp>" when compensated), then parse.
static ec_reading_t ec_read(ezo_sensor_t *ec, bool compensated, float temp_c)
{
  ec_reading_t result = {0};
  char response[64];
  ec_output_mask_t mask;

  result.status = ec_get_output_mask(ec, &mask);
  if (result.status != EZO_SUCCESS)
    return result;

  if (compensated)
    result.status = ezo_commandf(EZO_KIND_EC, ec, response, sizeof(response), "RT,%.2f", temp_c);
  else
    result.status = ezo_command(EZO_KIND_EC, ec, "R", response, sizeof(response));
  if (result.status != EZO_SUCCESS)
    return result;

  result.status = ec_parse_reading(response, mask, &result);
  return result;
}

ec_reading_t ec_get_reading(ezo_sensor_t *ec)
{
  return ec_read(ec, false, 0.0f);
}

ec_reading_t ec_get_reading_compensated(ezo_sensor_t *ec, float temp_c)
{
  return ec_read(ec, true, temp_c);
}

int ec_set_k(ezo_sensor_t *ec, float k)
{
  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "K,%.4f", k);
}

int ec_get_k(ezo_sensor_t *ec, float *k)
{
  char response[32];

  if (k == NULL)
    return EZO_ERROR;

  int status = ezo_command(EZO_KIND_EC, ec, "K,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  if (sscanf(response, "?K,%f", k) != 1)
    return EZO_ERROR;

  return EZO_SUCCESS;
}

int ec_set_tds_factor(ezo_sensor_t *ec, float factor)
{
  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "TDS,%.4f", factor);
}

int ec_get_tds_factor(ezo_sensor_t *ec, float *factor)
{
  char response[32];

  if (factor == NULL)
    return EZO_ERROR;

  int status = ezo_command(EZO_KIND_EC, ec, "TDS,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  if (sscanf(response, "?TDS,%f", factor) != 1)
    return EZO_ERROR;

  return EZO_SUCCESS;
}

int ec_calibrate_dry(ezo_sensor_t *ec)
{
  return ezo_command(EZO_KIND_EC, ec, "Cal,dry", NULL, 0);
}

int ec_calibrate(ezo_sensor_t *ec, float conductivity)
{
  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "Cal,%.2f", conductivity);
}

int ec_calibrate_low(ezo_sensor_t *ec, float conductivity)
{
  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "Cal,low,%.2f", conductivity);
}

int ec_calibrate_high(ezo_sensor_t *ec, float conductivity)
{
  return ezo_commandf(EZO_KIND_EC, ec, NULL, 0, "Cal,high,%.2f", conductivity);
}

// --- Wrappers
int ec_get_cal_status(ezo_sensor_t *ec, ezo_cal_status_t *cal) { return ezo_get_cal_status(ec, cal); }
int ec_clear_calibration(ezo_sensor_t *ec)                     { return ezo_clear_calibration(ec); }
int ec_get_info(ezo_sensor_t *ec, char *info, int len)         { return ezo_get_info(ec, info, len); }
int ec_get_status(ezo_sensor_t *ec, char *status, int len)     { return ezo_get_status(ec, status, len); }
int ec_sleep(ezo_sensor_t *ec)                                 { return ezo_sleep(ec); }


//#endif // DUMMY_SENSORS
#endif // ifndef DUMMY_SENSORS

// ─── Dummy sensor implementation ──────────────────────────────────────────────
// Compiled only when DUMMY_SENSORS is defined: make dummy
// All functions return plausible pool chemistry values with small random drift.
// No I2C hardware required — safe to run on any machine.

#ifdef DUMMY_SENSORS

/*
// Small random float drift in range [-range, +range]
static float dummy_drift(float range)
{
  return ((float)(rand() % 1000) / 1000.0f - 0.5f) * 2.0f * range;
}
*/
// ─── Bus / detect (dummy) ─────────────────────────────────────────────────────

int ezo_bus_available()
{
  static int seeded = 0;
  if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
  return 1;   // always available in dummy mode
}

void ezo_i2cdetect(bool usesyslog)
{
  DIAG_LOG(usesyslog, "\n");
  DIAG_LOG(usesyslog, "[DUMMY] Simulated I2C bus scan on %s\n\n", I2C_BUS);
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  DIAG_LOG(usesyslog, "60:              62 63 64       67 68               \n\n");
  DIAG_LOG(usesyslog, "-\n");
  DIAG_LOG(usesyslog, "Detected devices:\n");
  DIAG_LOG(usesyslog, "  0x62  confirmed: ORP    (default addr for: ORP)\n");
  DIAG_LOG(usesyslog, "  0x63  confirmed: PH     (default addr for: PH)\n");
  DIAG_LOG(usesyslog, "  0x64  confirmed: EC     (default addr for: EC)\n");
  DIAG_LOG(usesyslog, "  0x67  confirmed: PUMP   (default addr for: PUMP)\n");
  DIAG_LOG(usesyslog, "  0x68  confirmed: RTD    (default addr for: RTD)\n");
  DIAG_LOG(usesyslog, "---------------------------------------------\n");
}
/*
void ezo_i2cdetect()
{
  printf("\n[DUMMY] Simulated I2C bus scan on %s\n\n", I2C_BUS);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  printf("60:              62 63 64       67 68               \n\n");
  printf("Detected devices:\n");
  printf("  0x62  confirmed: ORP    (default addr for: ORP)\n");
  printf("  0x63  confirmed: pH     (default addr for: pH)\n");
  printf("  0x64  confirmed: EC     (default addr for: EC)\n")
  printf("  0x67  confirmed: PUMP   (default addr for: PUMP)\n");
  printf("  0x68  confirmed: RTD    (default addr for: RTD)\n");
  printf("\n");
}
*/

void simulate_read_time()
{
  // Sleep for 1 second
  sleep(1);
}

// ─── Shared EZO helpers (dummy) ───────────────────────────────────────────────

// Same rule as the real build: a NULL sensor or an invalid address is an error,
// so a bad call is caught on a dev machine as well as on the Pi.
#define DUMMY_SENSOR_OK(s)  i2c_addr_valid(ezo_sensor_addr(s))

int ezo_get_info(ezo_sensor_t *sensor, char *info, int len)
{
  if (!DUMMY_SENSOR_OK(sensor)) return EZO_ERROR;
  simulate_read_time();
  snprintf(info, len, "?I,DUMMY,1.00");
  return EZO_SUCCESS;
}

int ezo_get_status(ezo_sensor_t *sensor, char *status, int len)
{
  if (!DUMMY_SENSOR_OK(sensor)) return EZO_ERROR;
  simulate_read_time();
  snprintf(status, len, "?STATUS,P,5.00");
  return EZO_SUCCESS;
}

int ezo_clear_calibration(ezo_sensor_t *sensor)
{
  return DUMMY_SENSOR_OK(sensor) ? EZO_SUCCESS : EZO_ERROR;
}

int ezo_sleep(ezo_sensor_t *sensor)
{
  return DUMMY_SENSOR_OK(sensor) ? EZO_SUCCESS : EZO_ERROR;
}

int ezo_get_cal_status(ezo_sensor_t *sensor, ezo_cal_status_t *cal)
{
  if (!DUMMY_SENSOR_OK(sensor) || cal == NULL) return EZO_ERROR;
  simulate_read_time();
  cal->points = 3;
  return EZO_SUCCESS;
}

// ─── pH sensor (dummy) ────────────────────────────────────────────────────────

ph_reading_t ph_get_reading(ezo_sensor_t *ph)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  return (ph_reading_t){ 7.20f + dummy_drift(0.15f), EZO_SUCCESS };
}

ph_reading_t ph_get_reading_compensated(ezo_sensor_t *ph, float temp_c)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  (void)temp_c;
  // between 7.1 and 8.1 = Use 7.6 as the base, and 0.5 as the range
  return (ph_reading_t){ 7.60f + dummy_drift(0.5f), EZO_SUCCESS };
}

ph_reading_t ph_get_reading_filtered(ezo_sensor_t *ph)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  // Call dummy ph_get_reading 3x and return median — same logic as real version
  float r[3];
  for (int i = 0; i < 3; i++) r[i] = ph_get_reading(ph).value;
  if (r[0] > r[1]) { float t = r[0]; r[0] = r[1]; r[1] = t; }
  if (r[1] > r[2]) { float t = r[1]; r[1] = r[2]; r[2] = t; }
  if (r[0] > r[1]) { float t = r[0]; r[0] = r[1]; r[1] = t; }
  return (ph_reading_t){ r[1], EZO_SUCCESS };
}

int ph_calibrate(ezo_sensor_t *ph, ph_cal_point_t point, float ph_value)
{
  const char *point_str[] = {"low", "mid", "high"};
  if (!DUMMY_SENSOR_OK(ph)) return EZO_ERROR;
  if ((unsigned)point > PH_CAL_HIGH) return EZO_SYNTAX_ERR;
  simulate_read_time();
  printf("[DUMMY] pH calibrate %s at %.2f — OK\n", point_str[point], ph_value);
  return EZO_SUCCESS;
}

int ph_calibrate_mid(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_MID,  PH_REF_MID); }
int ph_calibrate_low(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_LOW,  PH_REF_LOW); }
int ph_calibrate_high(ezo_sensor_t *ph) { return ph_calibrate(ph, PH_CAL_HIGH, PH_REF_HIGH); }

int ph_get_cal_status(ezo_sensor_t *ph, ezo_cal_status_t *cal) { return ezo_get_cal_status(ph, cal); }
int ph_clear_calibration(ezo_sensor_t *ph)                     { return ezo_clear_calibration(ph); }
int ph_get_info(ezo_sensor_t *ph, char *info, int len)         { return ezo_get_info(ph, info, len); }
int ph_get_status(ezo_sensor_t *ph, char *status, int len)     { return ezo_get_status(ph, status, len); }
int ph_sleep(ezo_sensor_t *ph)                                 { return ezo_sleep(ph); }

int ph_calibrate_by_value(ezo_sensor_t *ph, float calibrationValue) {
    // Dynamically calculate the midpoints
    float low_mid_boundary  = (PH_REF_LOW + PH_REF_MID) / 2.0f;   // (4.00 + 7.00) / 2 = 5.50f
    float mid_high_boundary = (PH_REF_MID + PH_REF_HIGH) / 2.0f; // (7.00 + 10.00) / 2 = 8.50f

    //Anything below the low/mid midpoint
    if (calibrationValue < low_mid_boundary) {
        return ph_calibrate_low(ph);
    }
    // Anything between the low/mid midpoint and mid/high midpoint
    else if (calibrationValue >= low_mid_boundary && calibrationValue <= mid_high_boundary) {
        return ph_calibrate_mid(ph);
    }
    // Anything above the mid/high midpoint
    else {
        return ph_calibrate_high(ph);
    }
}

// ─── ORP sensor (dummy) ───────────────────────────────────────────────────────

orp_reading_t orp_get_reading(ezo_sensor_t *orp)
{
  if (!DUMMY_SENSOR_OK(orp)) return (orp_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  return (orp_reading_t){ 650.0f + dummy_drift(20.0f), EZO_SUCCESS };
}

int orp_calibrate(ezo_sensor_t *orp, float mv_value)
{
  if (!DUMMY_SENSOR_OK(orp)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] ORP calibrate at %.2f mV — OK\n", mv_value);
  return EZO_SUCCESS;
}

int orp_get_cal_status(ezo_sensor_t *orp, ezo_cal_status_t *cal) { return ezo_get_cal_status(orp, cal); }
int orp_clear_calibration(ezo_sensor_t *orp)                     { return ezo_clear_calibration(orp); }
int orp_get_info(ezo_sensor_t *orp, char *info, int len)         { return ezo_get_info(orp, info, len); }
int orp_get_status(ezo_sensor_t *orp, char *status, int len)     { return ezo_get_status(orp, status, len); }
int orp_sleep(ezo_sensor_t *orp)                                 { return ezo_sleep(orp); }

// ─── RTD temperature sensor (dummy) ──────────────────────────────────────────

rtd_reading_t rtd_get_reading(ezo_sensor_t *rtd)
{
  if (!DUMMY_SENSOR_OK(rtd)) return (rtd_reading_t){ 0.0f, RTD_SCALE_CELSIUS, EZO_ERROR };
  simulate_read_time();
  // Return between 28 and 29
  //return (rtd_reading_t){ 28.5f + dummy_drift(0.5f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

  // Return between -5 and 75
  //return (rtd_reading_t){ 35.0f + dummy_drift(40.0f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

  return (rtd_reading_t){ 30.0f + dummy_drift(1.0f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

}

int rtd_set_scale(ezo_sensor_t *rtd, rtd_scale_t scale)
{
  const char *scales[] = {"Celsius", "Fahrenheit", "Kelvin"};
  if (!DUMMY_SENSOR_OK(rtd)) return EZO_ERROR;
  if ((unsigned)scale > RTD_SCALE_KELVIN) return EZO_SYNTAX_ERR;
  printf("[DUMMY] RTD scale set to %s — OK\n", scales[scale]);
  return EZO_SUCCESS;
}

int rtd_calibrate(ezo_sensor_t *rtd, float known_temp)
{
  if (!DUMMY_SENSOR_OK(rtd)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] RTD calibrate at %.2f — OK\n", known_temp);
  return EZO_SUCCESS;
}

int rtd_get_cal_status(ezo_sensor_t *rtd, ezo_cal_status_t *cal) { return ezo_get_cal_status(rtd, cal); }
int rtd_clear_calibration(ezo_sensor_t *rtd)                     { return ezo_clear_calibration(rtd); }
int rtd_get_info(ezo_sensor_t *rtd, char *info, int len)         { return ezo_get_info(rtd, info, len); }
int rtd_get_status(ezo_sensor_t *rtd, char *status, int len)     { return ezo_get_status(rtd, status, len); }
int rtd_sleep(ezo_sensor_t *rtd)                                 { return ezo_sleep(rtd); }

// ─── Pressure sensor (dummy) ──────────────────────────────────────────────────

prs_reading_t prs_get_reading(ezo_sensor_t *prs)
{
  if (!DUMMY_SENSOR_OK(prs)) return (prs_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  // Return a plausible clean pool filter pressure around 15.0 psi
  return (prs_reading_t){ 15.0f + dummy_drift(1.0f), EZO_SUCCESS };
}

int prs_calibrate(ezo_sensor_t *prs, float psi_value)
{
  if (!DUMMY_SENSOR_OK(prs)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] PRS calibrate at %.2f psi — OK\n", psi_value);
  return EZO_SUCCESS;
}

int prs_calibrate_zero(ezo_sensor_t *prs)
{
  if (!DUMMY_SENSOR_OK(prs)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] PRS calibrate zero (atmospheric) — OK\n");
  return EZO_SUCCESS;
}

int prs_get_cal_status(ezo_sensor_t *prs, ezo_cal_status_t *cal) { return ezo_get_cal_status(prs, cal); }
int prs_clear_calibration(ezo_sensor_t *prs)                     { return ezo_clear_calibration(prs); }
int prs_get_info(ezo_sensor_t *prs, char *info, int len)         { return ezo_get_info(prs, info, len); }
int prs_get_status(ezo_sensor_t *prs, char *status, int len)     { return ezo_get_status(prs, status, len); }
int prs_sleep(ezo_sensor_t *prs)                                 { return ezo_sleep(prs); }


// ─── Conductivity sensor (EZO-EC) ─────────────────────────────────────────────
static ec_output_mask_t dummy_ec_output_mask = EC_OUTPUT_CONDUCTIVITY;

int ec_get_output_mask(ezo_sensor_t *ec, ec_output_mask_t *mask)
{
  if (!DUMMY_SENSOR_OK(ec) || mask == NULL)
    return EZO_ERROR;

  *mask = dummy_ec_output_mask;
  return EZO_SUCCESS;
}

int ec_set_output(ezo_sensor_t *ec, ec_output_t output, bool enabled)
{
  if (!DUMMY_SENSOR_OK(ec))
    return EZO_ERROR;

  if (enabled)
    dummy_ec_output_mask |= output;
  else
    dummy_ec_output_mask &= ~output;

  return EZO_SUCCESS;
}

ec_reading_t ec_get_reading(ezo_sensor_t *ec)
{
    ec_reading_t result = {0};

    if (!DUMMY_SENSOR_OK(ec))
    {
        result.status = EZO_ERROR;
        return result;
    }

    simulate_read_time();

    result.status = EZO_SUCCESS;

    /* Single underlying measurement (µS/cm); everything else derives from it.
       Salt pool: ~3.2 ppt salt ≈ 5800 µS/cm, drifting about ±150 µS/cm. */
    const float ec_us = 5800.0f + dummy_drift(150.0f);

    if (dummy_ec_output_mask & EC_OUTPUT_CONDUCTIVITY)
    {
        result.conductivity = ec_us;
        result.valid |= EC_OUTPUT_CONDUCTIVITY;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_TDS)
    {
        result.tds = ec_us * 0.54f;               /* ~3130 ppm */
        result.valid |= EC_OUTPUT_TDS;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_SALINITY)
    {
        result.salinity = (ec_us / 1000.0f) * 0.55f;   /* ~3.2 ppt */
        result.valid |= EC_OUTPUT_SALINITY;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_SPECIFIC_GRAVITY)
    {
        float salinity_ppt = (ec_us / 1000.0f) * 0.55f;
        result.specific_gravity = 1.000f + salinity_ppt * 0.00075f;  /* ~1.0024 */
        result.valid |= EC_OUTPUT_SPECIFIC_GRAVITY;
    }

    return result;
}

ec_reading_t ec_get_reading_compensated(ezo_sensor_t *ec, float temp_c)
{
  (void)temp_c;
  return ec_get_reading(ec);
}

int ec_set_k(ezo_sensor_t *ec, float k)
{
  (void)k;
  return DUMMY_SENSOR_OK(ec) ? EZO_SUCCESS : EZO_ERROR;
}

int ec_get_k(ezo_sensor_t *ec, float *k)
{
  if (!DUMMY_SENSOR_OK(ec) || k == NULL)
    return EZO_ERROR;

  *k = 1.0f;
  return EZO_SUCCESS;
}

int ec_set_tds_factor(ezo_sensor_t *ec, float factor)
{
  (void)factor;
  return DUMMY_SENSOR_OK(ec) ? EZO_SUCCESS : EZO_ERROR;
}

int ec_get_tds_factor(ezo_sensor_t *ec, float *factor)
{
  if (!DUMMY_SENSOR_OK(ec) || factor == NULL)
    return EZO_ERROR;

  *factor = 0.54f;
  return EZO_SUCCESS;
}

int ec_calibrate_dry(ezo_sensor_t *ec)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC dry calibration — OK\n");
  return EZO_SUCCESS;
}

int ec_calibrate(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_calibrate_low(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC low calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_calibrate_high(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC high calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_get_cal_status(ezo_sensor_t *ec, ezo_cal_status_t *cal) { return ezo_get_cal_status(ec, cal); }
int ec_clear_calibration(ezo_sensor_t *ec)                     { return ezo_clear_calibration(ec); }
int ec_get_info(ezo_sensor_t *ec, char *info, int len)         { return ezo_get_info(ec, info, len); }
int ec_get_status(ezo_sensor_t *ec, char *status, int len)     { return ezo_get_status(ec, status, len); }
int ec_sleep(ezo_sensor_t *ec)                                 { return ezo_sleep(ec); }




/* ═══════════════════════════ DUMMY_SENSORS ═══════════════════════════════
 *
 * Simulates the device, per address:
 *   - dispensed volume ramps with elapsed time (not a jump at the end)
 *   - continuous modes run until pump_stop(); stop keeps the PARTIAL volume
 *   - pause freezes the volume, resume continues it (idempotent, like the real wrapper)
 *   - total volume accumulates; Clear resets it; absolute total never resets
 *   - rates outside 0.5 .. DUMMY_PUMP_MAX_ML_PER_MIN return EZO_SYNTAX_ERR
 *   - pump_set_i2c_address() moves the simulated pump to the new address
 * Goes through i2c_lock_addr(), so lock behaviour is exercised without hardware.
 * Optional: -DDUMMY_PUMP_LATENCY_MS=300 adds per-command delay (held under the
 * address lock, like a real transaction).                                      */
 
#define DUMMY_PUMP_MAX_ML_PER_MIN    60.0f   /* "calibrated max": rate for D,<ml> and D,* (1 ml/s) */
#define DUMMY_PUMP_MIN_ML_PER_MIN    0.5f
#define DUMMY_PUMP_VOLTS             12.0f
#ifndef DUMMY_PUMP_LATENCY_MS
#define DUMMY_PUMP_LATENCY_MS        0
#endif
 
typedef struct {
  int       init, pumping, paused, continuous, cal_status;
  float     requested_ml;       /* what D,? reports */
  float     rate_ml_min;
  float     dispensed_ml;       /* what R reports */
  float     total_ml;           /* TV,? */
  float     absolute_total_ml;  /* ATV,? */
  long long last_ms;
} dp_t;
 
static dp_t            _dp[128];
static pthread_mutex_t _dp_mutex = PTHREAD_MUTEX_INITIALIZER;
 
static long long dp_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
 
static void dp_update(dp_t *d)           /* caller holds _dp_mutex */
{
  long long now = dp_now_ms();
 
  if (d->pumping && !d->paused) {
    float add = d->rate_ml_min * (float)(now - d->last_ms) / 60000.0f;
    if (!d->continuous && d->dispensed_ml + add >= d->requested_ml) {
      add = d->requested_ml - d->dispensed_ml;
      d->pumping = 0;
    }
    d->dispensed_ml      += add;
    d->total_ml          += add;
    d->absolute_total_ml += add;
  }
  d->last_ms = now;
}
 
/* Lock order: per-address I2C lock, then _dp_mutex.  Latency is spent under the
 * address lock only, so different pumps still run in parallel.  NULL on failure. */
static dp_t *dp_enter(int addr)
{
  if (!i2c_addr_valid(addr)) return NULL;
  if (i2c_lock_addr(addr) != 0) return NULL;
  if (DUMMY_PUMP_LATENCY_MS > 0) usleep(DUMMY_PUMP_LATENCY_MS * 1000);
 
  pthread_mutex_lock(&_dp_mutex);
  dp_t *d = &_dp[addr];
  if (!d->init) {
    memset(d, 0, sizeof(*d));
    d->init = 1;
    d->cal_status = 3;                   /* dummy ships calibrated */
    d->last_ms = dp_now_ms();
  }
  dp_update(d);
  return d;
}
 
static void dp_leave(int addr)
{
  pthread_mutex_unlock(&_dp_mutex);
  i2c_unlock_addr(addr);
}
 
/* Start a new dose, replacing any running one.  VERIFY: device behaviour for a
 * D command while dosing.                                                     */
static void dp_begin(dp_t *d, float requested_ml, float rate, int continuous)
{
  d->requested_ml = continuous ? 0.0f : requested_ml;
  d->rate_ml_min  = rate;
  d->continuous   = continuous;
  d->dispensed_ml = 0.0f;
  d->paused       = 0;
  d->pumping      = 1;
}
 
static int dp_rate_ok(float rate)
{
  return rate >= DUMMY_PUMP_MIN_ML_PER_MIN && rate <= DUMMY_PUMP_MAX_ML_PER_MIN;
}
 
/* Args are validated by the caller; this locks and starts.  Returns the EZO status. */
static int dp_start(int addr, float requested_ml, float rate, int continuous, const char *what)
{
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  dp_begin(d, requested_ml, rate, continuous);
  printf("[DUMMY] 0x%02X %s\n", addr, what);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- start a dose ------------------------------------------------------ */
 
int pump_dose_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "dose %.2f ml", ml);
  return dp_start(addr, ml, DUMMY_PUMP_MAX_ML_PER_MIN, 0, what);
}
 
int pump_dose_volume_over_time(ezo_sensor_t *pump, float ml, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  float rate = ml / (float)minutes;
  if (!dp_rate_ok(rate)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "dose %.2f ml over %d min", ml, minutes);
  return dp_start(addr, ml, rate, 0, what);
}
 
int pump_dose_rate(ezo_sensor_t *pump, float ml_per_min, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml_per_min > 0.0f) || minutes < 1 || !dp_rate_ok(ml_per_min)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "rate %.2f ml/min for %d min", ml_per_min, minutes);
  return dp_start(addr, ml_per_min * (float)minutes, ml_per_min, 0, what);   /* VERIFY: D,? volume */
}
 
int pump_dose_rate_continuous(ezo_sensor_t *pump, float ml_per_min)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml_per_min > 0.0f) || !dp_rate_ok(ml_per_min)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "rate %.2f ml/min continuous", ml_per_min);
  return dp_start(addr, 0.0f, ml_per_min, 1, what);
}
 
int pump_dose_max_continuous(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  return dp_start(addr, 0.0f, DUMMY_PUMP_MAX_ML_PER_MIN, 1, "continuous at max rate");
}
 
/* ---- control ----------------------------------------------------------- */
 
int pump_stop(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->pumping = 0;                        /* dispensed_ml keeps the partial volume */
  d->paused  = 0;
  printf("[DUMMY] 0x%02X stop (%.2f ml dispensed)\n", addr, d->dispensed_ml);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
static int pump_set_paused(int addr, int want_paused)
{
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->paused = want_paused ? 1 : 0;
  printf("[DUMMY] 0x%02X %s\n", addr, want_paused ? "pause" : "resume");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_pause(ezo_sensor_t *pump)  { return pump_set_paused(ezo_sensor_addr(pump), 1); }
int pump_resume(ezo_sensor_t *pump) { return pump_set_paused(ezo_sensor_addr(pump), 0); }
 
int pump_is_paused(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return -1;
  int p = d->paused;
  dp_leave(addr);
  return p;
}
 
/* ---- query ------------------------------------------------------------- */
 
pump_dose_status_t pump_get_dose_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  pump_dose_status_t r = { 0.0f, 0, EZO_ERROR };
  dp_t *d = dp_enter(addr);
  if (!d) return r;
  r.requested_volume_ml = d->requested_ml;
  r.is_pumping = (d->pumping && !d->paused) ? 1 : 0;      /* VERIFY: flag while paused */
  r.status = EZO_SUCCESS;
  dp_leave(addr);
  return r;
}
 
static float dp_read(int addr, size_t offset)
{
  dp_t *d = dp_enter(addr);
  if (!d) return -1.0f;
  float v = *(float *)((char *)d + offset);
  dp_leave(addr);
  return v;
}
 
float pump_get_dispensed_volume(ezo_sensor_t *pump)      { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, dispensed_ml)); }
float pump_get_total_volume(ezo_sensor_t *pump)          { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, total_ml)); }
float pump_get_absolute_total_volume(ezo_sensor_t *pump) { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, absolute_total_ml)); }
float pump_get_max_flow_rate(ezo_sensor_t *pump)         { return i2c_addr_valid(ezo_sensor_addr(pump)) ? DUMMY_PUMP_MAX_ML_PER_MIN : -1.0f; }
float pump_get_voltage(ezo_sensor_t *pump)               { return i2c_addr_valid(ezo_sensor_addr(pump)) ? DUMMY_PUMP_VOLTS : -1.0f; }
 
int pump_clear_total_volume(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->total_ml = 0.0f;                    /* absolute total is never cleared */
  printf("[DUMMY] 0x%02X clear total volume\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- calibration ------------------------------------------------------- */
 
int pump_get_calibration_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return -1;
  int c = d->cal_status;
  dp_leave(addr);
  return c;
}
 
/* The dummy only approximates which calibration bit a procedure sets. */
int pump_set_calibration_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->cal_status |= 1;
  printf("[DUMMY] 0x%02X calibrate %.2f ml\n", addr, ml);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_clear_calibration(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->cal_status = 0;
  printf("[DUMMY] 0x%02X calibration cleared\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- device ------------------------------------------------------------ */
 
int pump_find(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  printf("[DUMMY] 0x%02X find (LED blink)\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_set_i2c_address(ezo_sensor_t *pump, int new_addr)
{
  int addr = ezo_sensor_addr(pump);
 
  if (!i2c_addr_valid(addr) || !i2c_addr_valid(new_addr)) return EZO_SYNTAX_ERR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
 
  int rc;
  if (new_addr == addr)        rc = EZO_SUCCESS;
  else if (_dp[new_addr].init) rc = EZO_ERROR;           /* occupied, like the probe */
  else {
    _dp[new_addr] = *d;                                  /* the pump "moves" */
    memset(d, 0, sizeof(*d));
    printf("[DUMMY] 0x%02X -> 0x%02X\n", addr, new_addr);
    pump->address = (unsigned char)new_addr;             /* same as the real function */
    rc = EZO_SUCCESS;
  }
  dp_leave(addr);
  return rc;
}
 
int pump_get_info(ezo_sensor_t *pump, char *info, int len)
{
  int addr = ezo_sensor_addr(pump);
 
  if (info == NULL || len <= 0) return EZO_ERROR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  snprintf(info, len, "?I,PMP,DUMMY");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_get_device_status(ezo_sensor_t *pump, char *status, int len)
{
  int addr = ezo_sensor_addr(pump);
 
  if (status == NULL || len <= 0) return EZO_ERROR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  snprintf(status, len, "?STATUS,P,5.00");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_sleep(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  dp_leave(addr);
  return EZO_SUCCESS;
}



#endif // DUMMY_SENSORS











#ifdef OLD_CODE

// CODE works fine.  But was getting messy with the writes some function using generic some there own
// Code above all uses generic write function.  Not ful tested yet.


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <math.h>
#include <errno.h>

#include "i2c.h"
#include "utils.h"
#include "ezo.h"
#include "i2c_bus.h"

const char* i2c_get_driver_name(i2c_sensor_type_t type)
{
  switch(type){
    case I2C_SENSOR_PTE7300:
      return "PTE7300";
    break;
    case I2C_SENSOR_HSC:
      return "HSC/SSC";
    break;
    default:
      return "";
    break;
  }
}

// ─── Real implementation ──────────────────────────────────────────────────────
// All code in this block is compiled only when DUMMY_SENSORS is NOT defined.
// To build with fake sensors: make dummy
// To build normally:          make

#ifndef DUMMY_SENSORS

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

// Set to 0 if bring-up shows PTE7300 registers are actually big-endian on
// your unit — see the header comment on PTE7300 byte order. Quick check
// with i2c-tools from the shell (adjust bus/addr as needed):
//   i2cset -y 1 0x6c 0x22 0x93 0x8b i   # write START, LSB-first as this driver does
//   i2cget -y 1 0x6c 0x2e w             # i2c_smbus word read is LSB-first too
// If the pressure reading only makes sense with the two data bytes swapped,
// flip this to 0.
#define PTE7300_LITTLE_ENDIAN 1

int pte7300_validate_status(unsigned short raw_status);

// ─── Generic transport ────────────────────────────────────────────────────────
// Every transaction goes through i2c_open()/i2c_close() (i2c_bus.c): i2c_open()
// takes the per-address lock, opens `bus_path` and selects the slave address;
// i2c_close() releases the lock.  The address is validated first because
// i2c_open() treats a negative address as "no lock, no ioctl", which is meant for
// bus-level use only (scans, availability), never for talking to a device.
//
// The I2C_RDWR paths carry the address in each message, so the I2C_SLAVE that
// i2c_open() issues is redundant for them.  It is harmless unless a kernel driver
// has claimed the address (EBUSY), in which case the transfer is refused.

int i2c_bus_available(const char *bus_path)
{
  int fd = i2c_open(bus_path, -1);      // proves the bus device opens; no address, no lock
  if (fd < 0) return 0;
  i2c_close(fd);
  return 1;
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len < 0 || len > 32 || (len > 0 && data == NULL)) return I2C_ERROR;

  unsigned char buf[1 + 32];
  buf[0] = reg;
  if (len > 0) memcpy(&buf[1], data, len);

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return I2C_ERROR;

  struct i2c_msg msg = { .addr = (unsigned short)address, .flags = 0,
                          .len = (unsigned short)(1 + len), .buf = buf };
  struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return I2C_ERROR;

  struct i2c_msg msgs[2];
  msgs[0].addr  = (unsigned short)address;
  msgs[0].flags = 0;
  msgs[0].len   = 1;
  msgs[0].buf   = &reg;

  msgs[1].addr  = (unsigned short)address;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len   = (unsigned short)len;
  msgs[1].buf   = data;

  struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);   // also selects the slave address
  if (fd < 0) return I2C_ERROR;

  int ret = read(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);   // also selects the slave address
  if (fd < 0) return I2C_ERROR;

  int ret = write(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

// 1 if something ACKs at `address`, else 0.  An invalid address is "not there"
// (0), never an error code: I2C_ERROR is -1, which a caller would read as true.
int i2c_probe_address(const char *bus_path, int address)
{
  if (!i2c_addr_valid(address)) return 0;

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return 0;

  unsigned char dummy;
  int ok = (read(fd, &dummy, 1) >= 0);
  i2c_close(fd);
  return ok;
}

// ─── Known-device table (extend this as you add more generic I2C sensors) ───
// Only sensors with a fixed/default factory address are worth listing here —
// HSC/SSC addresses are order-code specific, so they're deliberately absent.

typedef struct {
  int         addr;
  const char *name;
} i2c_addr_map_t;

static const i2c_addr_map_t i2c_known_devices[] = {
  { PTE7300_ADDR_DEFAULT, "PTE7300 pressure (non-CRC)" },
  { PTE7300_ADDR_CRC,     "PTE7300 pressure (CRC)" },
  { 0,                    NULL                          }
};

const char *i2c_name_from_addr(int addr)
{
  for (int i = 0; i2c_known_devices[i].name != NULL; i++)
    if (i2c_known_devices[i].addr == addr)
      return i2c_known_devices[i].name;

  // See if it's an ezo device
  return ezo_name_from_addr(addr);
}

// Active identification, used only by deepscan.  Unlike the scan itself (which
// sends no data), this WRITES to the device, so it must not be pointed at a
// chip in the wrong protocol:
//   - The PTE7300 answers at two addresses.  0x6C is the plain register protocol;
//     0x6D is the CRC-framed protocol.  The probe below is a plain register read,
//     which at 0x6D is a malformed frame, so 0x6D is never queried.
//   - The HSC/SSC probe is a bare multi-byte read.  It is never sent to 0x6C,
//     where a register-mapped PTE7300 lives.
static const char *i2c_query_device_type(int addr)
{
  if (addr == PTE7300_ADDR_CRC) return NULL;

  // 1. Try PTE7300 probe: read 2 bytes from the STATUS register (0x36)
  unsigned char pte_buf[2];
  if (i2c_read_reg(I2C_GENERIC_BUS, addr, PTE7300_REG_STATUS, pte_buf, 2) == I2C_SUCCESS) {
    unsigned short status_reg;
#if PTE7300_LITTLE_ENDIAN
    status_reg = (unsigned short)(pte_buf[0] | (pte_buf[1] << 8));
#else
    status_reg = (unsigned short)((pte_buf[0] << 8) | pte_buf[1]);
#endif
    if (pte7300_validate_status(status_reg) == I2C_SUCCESS) {
      return "PTE7300";
    }
  }

  // The PTE7300's own address ends here, whatever the status said.
  if (addr == PTE7300_ADDR_DEFAULT) return NULL;

  // 2. Try Honeywell HSC/SSC probe: registerless 4-byte read
  unsigned char hsc_buf[4];
  if (i2c_read_bytes(I2C_GENERIC_BUS, addr, hsc_buf, 4) == I2C_SUCCESS) {
    unsigned char status_bits = hsc_buf[0] & HSC_STATUS_MASK; // 0xC0 mask
    if (status_bits == HSC_STATUS_NORMAL || status_bits == HSC_STATUS_STALE) {
      return "HSC/SSC";
    }
  }

  return NULL;
}

/* Same probing rule as i2cdetect's default (auto) mode:
 *   SMBus Read Byte   for 0x30-0x37 and 0x50-0x5F  (EEPROM ranges, where a Quick
 *                      Write can corrupt data)
 *   SMBus Quick Write for everything else.
 * (The previous comment and ranges here were inverted.)
 * Returns 1 = ACK, 0 = no answer, -1 = address claimed by a kernel driver
 * (EBUSY; i2cdetect prints "UU").                                              */
static int i2c_probe_address_safe(int fd, int addr)
{
  if (ioctl(fd, I2C_SLAVE, addr) < 0)
    return (errno == EBUSY) ? -1 : 0;

  if ((addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x5F)) {
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args = {
      .read_write = I2C_SMBUS_READ,
      .command = 0,
      .size = I2C_SMBUS_BYTE,
      .data = &data
    };
    return (ioctl(fd, I2C_SMBUS, &args) >= 0) ? 1 : 0;
  }

  struct i2c_smbus_ioctl_data args = {
    .read_write = I2C_SMBUS_WRITE,
    .command = 0,
    .size = I2C_SMBUS_QUICK,
    .data = NULL
  };
  return (ioctl(fd, I2C_SMBUS, &args) >= 0) ? 1 : 0;
}

/* Helper to check if address belongs to standard EZO default range */
static bool is_potential_ezo_address(int addr)
{
  /* Standard EZO default range: 0x61 (DO), 0x62 (ORP), 0x63 (pH), 
     0x64 (EC), 0x65 (Hum), 0x66 (RTD), 0x67 (Pump), 0x68 (PRS), etc. */
  return (addr >= 0x61 && addr <= 0x6B);
}

// What this puts on the wire, compared with `i2cdetect -y 1`:
//   scan only      : identical per address (see i2c_probe_address_safe).
//   deepscan adds  : real data transactions (i2c_query_device_type, and the EZO
//                    "i" command), which i2cdetect never sends.  That is the part
//                    that can upset a device; aquachemd warns about it.
// The per-address locks only exclude other THREADS of this process.  `aquachemd
// scan` run from a shell is a second process, so it can still interleave with a
// running daemon, exactly as i2cdetect can.
void i2c_detect(bool deepscan, bool usesyslog)
{
  // addr = -1: one fd walks every address, so it takes no lock and no I2C_SLAVE
  // of its own; each probe takes that address's lock itself.
  int fd = i2c_open(I2C_GENERIC_BUS, -1);
  if (fd < 0) { perror("open i2c"); return; }

  DIAG_LOG(usesyslog, "\nScanning I2C bus %s...\n\n", I2C_GENERIC_BUS);
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    char line[64];
    int len = snprintf(line, sizeof(line), "%02x: ", row * 16);

    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) {
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      if (i2c_lock_addr(addr) != 0) {          // busy in another thread: skip it
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      int found = i2c_probe_address_safe(fd, addr);
      i2c_unlock_addr(addr);

      if (found < 0) {
        len += snprintf(line + len, sizeof(line) - len, "UU ");
      } else if (found == 0) {
        len += snprintf(line + len, sizeof(line) - len, "-- ");
      } else {
        len += snprintf(line + len, sizeof(line) - len, "%02x ", addr);
        detected[count++] = addr;
      }
    }
    DIAG_LOG(usesyslog, "%s\n", line);
  }
  i2c_close(fd);

  if (count == 0) { DIAG_LOG(usesyslog, "\nNo devices found.\n"); return; }

  DIAG_LOG(usesyslog, "\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known   = i2c_name_from_addr(addr);
    const char *queried = NULL;

    /* SAFETY FIX: Only query active device details if address matches EZO ranges 
       to avoid corrupting third-party sensors/RTCs/expanders */
    if (deepscan) {
      if (is_potential_ezo_address(addr)) {
        usleep(10000); // 10ms bus recovery pause before active query
        queried = ezo_query_device_type(addr);
      } else {
        usleep(10000); // 10ms bus recovery pause
        queried = i2c_query_device_type(addr);
      }
    }

    if (queried) {
      DIAG_LOG(usesyslog, "  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    } else if (known) {
      DIAG_LOG(usesyslog, "  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    } else {
      DIAG_LOG(usesyslog, "  0x%02x  unknown device\n", addr);
    }
  }
}

/*
void i2c_generic_detect(const char *bus_path)
{
  int fd = open(bus_path, O_RDWR);
  if (fd < 0) { perror("open i2c"); return; }

  printf("\nScanning I2C bus %s (generic sensors)...\n\n", bus_path);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    printf("%02x: ", row * 16);
    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) { printf("   "); continue; }
      if (ioctl(fd, I2C_SLAVE, addr) < 0) { printf("   "); continue; }

      unsigned char buf;
      if (read(fd, &buf, 1) < 0)
        printf("-- ");
      else
      {
        printf("%02x ", addr);
        detected[count++] = addr;
      }
    }
    printf("\n");
  }
  close(fd);

  if (count == 0) { printf("\nNo devices found.\n"); return; }

  printf("\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known = i2c_name_from_addr(addr);
    if (known)
      printf("  0x%02x  likely: %s (by default address, unconfirmed)\n", addr, known);
    else
      printf("  0x%02x  unknown device (could be HSC/SSC — those have no fixed default)\n", addr);
  }
  printf("\n");
}
*/
// ─── Shared scaling helper ─────────────────────────────────────────────────
// Linear map from raw counts to engineering units — used by every sensor
// family below (PTE7300's ±16000 span, HSC/SSC's transfer function span,
// and both sensors' temperature outputs).

static float i2c_scale(long raw, long min_counts, long max_counts, float min_value, float max_value)
{
  if (max_counts == min_counts) return NAN;   // misconfigured sensor (e.g. init never ran) — never silently return ±inf
  return min_value + (float)(raw - min_counts) * (max_value - min_value) / (float)(max_counts - min_counts);
}



// ─── PTE7300 pressure sensor ──────────────────────────────────────────────────


// ─── CRC4/CRC8 (ported from Sensata's official PTE7300_I2C Arduino library) ──
// Bit-serial CRC exactly as in their reference implementation — translated
// line-for-line from PTE7300_I2C.cpp's calc_crc4()/calc_crc8(), not
// reverse-engineered, so the framing matches what the ASIC's CRC-protected
// command channel actually expects.

static unsigned char pte7300_crc4(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (i >= len - 1 && j < 4) break;
      if (((shifter >> 3) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
      shifter &= 0x0F;
    }
  }
  return shifter & 0x0F;
}

static unsigned char pte7300_crc8(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (((shifter >> 7) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
    }
  }
  return shifter;
}

// CRC-protected command write — mirrors writeRegisterCRC() from Sensata's
// library for the single-word command case (number=1). Their library
// defaults CRC mode ON for everything; our working hypothesis is the ASIC
// requires it specifically for command-channel writes, even though it's
// been accepting plain non-CRC reads fine.
static int pte7300_write_cmd_crc(i2c_sensor_t *s, unsigned int cmd)
{
  unsigned char lo, hi;
#if PTE7300_LITTLE_ENDIAN
  lo = (unsigned char)(cmd & 0xFF);
  hi = (unsigned char)((cmd >> 8) & 0xFF);
#else
  hi = (unsigned char)(cmd & 0xFF);
  lo = (unsigned char)((cmd >> 8) & 0xFF);
#endif

  // node byte: 7-bit address shifted into an 8-bit form with read=0, crc=1 —
  // this is where the datasheet's "0xDA including CRC" comes from: for
  // s->address=0x6C, this evaluates to exactly 0xDA.
  unsigned char node = (unsigned char)(((s->address << 1) & 0xFC) | 0x02);

  unsigned char header[2] = { PTE7300_REG_CMD, 0x10 };   // 0x10 = ((1 word * 2)-1)<<4
  unsigned char crc4 = pte7300_crc4(0x03, 0x0F, header, 2);
  unsigned char len_crc4_byte = (unsigned char)(0x10 | (crc4 & 0x0F));

  unsigned char all[5] = { node, PTE7300_REG_CMD, len_crc4_byte, lo, hi };
  unsigned char crc8 = pte7300_crc8(0xD5, 0xFF, all, sizeof(all));

  // Wire frame after the CRC-mode slave address (0x6D): [reg][len/crc4][lo][hi][crc8]
  unsigned char payload[4] = { len_crc4_byte, lo, hi, crc8 };

  // One chip, two addresses (0x6C plain, 0x6D CRC) but the locks are per ADDRESS,
  // so also hold the plain address while a command goes out on the CRC one;
  // otherwise it could interleave with a register read of the same chip.
  // (Skipped if the sensor is configured directly at the CRC address, which
  // i2c_write_reg() is about to lock itself.)
  int hold_plain = (s->address != PTE7300_ADDR_CRC);
  if (hold_plain && i2c_lock_addr(s->address) != 0) return I2C_ERROR;
  int rc = i2c_write_reg(I2C_GENERIC_BUS, PTE7300_ADDR_CRC, PTE7300_REG_CMD, payload, sizeof(payload));
  if (hold_plain) i2c_unlock_addr(s->address);
  return rc;
}



#define PTE7300_USE_CRC_COMMANDS 1   // set to 0 to fall back to plain (non-CRC) command writes

static int pte7300_write_cmd(i2c_sensor_t *s, unsigned int cmd)
{
#if PTE7300_USE_CRC_COMMANDS
  return pte7300_write_cmd_crc(s, cmd);
#else
  unsigned char data[2];
#if PTE7300_LITTLE_ENDIAN
  data[0] = (unsigned char)(cmd & 0xFF);
  data[1] = (unsigned char)((cmd >> 8) & 0xFF);
#else
  data[0] = (unsigned char)((cmd >> 8) & 0xFF);
  data[1] = (unsigned char)(cmd & 0xFF);
#endif
  return i2c_write_reg(I2C_GENERIC_BUS, s->address, PTE7300_REG_CMD, data, 2);
#endif
}

static int pte7300_read_i16(i2c_sensor_t *s, unsigned char reg, short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (short)(data[0] | (data[1] << 8));
#else
  *out = (short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

static int pte7300_read_u16(i2c_sensor_t *s, unsigned char reg, unsigned short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (unsigned short)(data[0] | (data[1] << 8));
#else
  *out = (unsigned short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

int pte7300_validate_status(unsigned short raw_status)
{

  // Empirically observed on this unit alongside independently-verified
  // correct pressure/temperature readings. NOT a decode of documented bit
  // semantics -- Sensata's bit table for 0x36 remains unconfirmed, and even
  // EPFL's hardware-tested driver doesn't attempt to interpret this register.
  // Treat this as "seen good before," not "understood."
  if (raw_status == 0xC00E || raw_status == 0xC01E) {
    return I2C_SUCCESS;
  }

  /* This below is simply a guess, I can;t find any actual documentation on it......
  // Extract the system layer and the device state layer
  uint8_t i2c_status_flags = (raw_status >> 14) & 0x03; // Top 2 bits
  uint8_t internal_diagnostic = raw_status & 0xFF;       // Lower 8 bits

  if (i2c_status_flags == 0x03) {
    // 0x03 (11b) is expected when reading register space 0x36
    if (internal_diagnostic == 0x0E || internal_diagnostic == 0x1E) {
        // Sensor is perfectly healthy.
        // 0x1E simply means an internal measurement cycle was active during the read.
        return I2C_SUCCESS;
    } else {
        // Handle unexpected lower-byte fault flags here if they occur
        return I2C_ERROR;
    }
  }*/
  return I2C_ERROR;
}

// Logging-only — see NOTE ON STATUS BITS in i2c.h. Nothing gates on this.
static void pte7300_decode_status(unsigned short status)
{
  LOG(LOG_DEBUG, "--- PTE7300 STATUS DECODER (Raw: 0x%04X) ---", status);

  if (status == 0x0000 || status == 0x0001) {
    LOG(LOG_INFO, "  [OK] System Normal - No diagnostic faults latched");
    return;
  }

  // --- High Byte System & DSP Diagnostics (Bits 15..8) ---
  if (status & 0x8000) LOG(LOG_ERR,   "  [!] Bit 15 (0x8000): GLOBAL DIAGNOSTIC FAULT - ASIC in error state");
  if (status & 0x4000) LOG(LOG_ERR,   "  [!] Bit 14 (0x4000): COMMAND ERROR - Invalid command or bad sequence");
  if (status & 0x2000) LOG(LOG_NOTICE,   "  [*] Bit 13 (0x2000): DSP BUSY - Conversion actively in progress");
  if (status & 0x1000) LOG(LOG_ERR,   "  [!] Bit 12 (0x1000): EEPROM/RAM FAULT - Memory CRC parity error");
  if (status & 0x0800) LOG(LOG_ERR,   "  [!] Bit 11 (0x0800): EXECUTION REJECTED - Command ignored by state machine");
  if (status & 0x0400) LOG(LOG_ERR,   "  [!] Bit 10 (0x0400): SUPPLY VOLTAGE FAULT - VDD under-voltage drop");
  if (status & 0x0200) LOG(LOG_ERR,   "  [!] Bit  9 (0x0200): THERMAL WARNING - Silicon temp exceeds operating limit");
  if (status & 0x0100) LOG(LOG_ERR,   "  [!] Bit  8 (0x0100): MATH FAULT - DSP raw count overflow/underflow");

  // --- Low Byte Analog Front-End & Sensor Element (Bits 7..0) ---
  if (status & 0x0080) LOG(LOG_ERR,   "  [!] Bit  7 (0x0080): INTERNAL DIAGNOSTIC - Reserved ASIC flag");
  if (status & 0x0040) LOG(LOG_ERR,   "  [!] Bit  6 (0x0040): BRIDGE DRIVE FAULT - Strain-gauge supply failure");
  if (status & 0x0020) LOG(LOG_ERR,   "  [!] Bit  5 (0x0020): SENSOR DIE FAULT - Open/Short circuit on element");
  if (status & 0x0010) LOG(LOG_ERR,   "  [!] Bit  4 (0x0010): PRESSURE AFE FAULT - Pressure ADC signal out-of-bounds");
  if (status & 0x0008) LOG(LOG_ERR,   "  [!] Bit  3 (0x0008): TEMP AFE FAULT - Diode ADC signal out-of-bounds");
  if (status & 0x0004) LOG(LOG_ERR,   "  [!] Bit  2 (0x0004): CALIBRATION FAULT - Factory EEPROM trim unreadable");
  if (status & 0x0002) LOG(LOG_NOTICE,   "  [*] Bit  1 (0x0002): MODE - Continuous/Cyclic mode active");
  if (status & 0x0001) LOG(LOG_NOTICE,   "  [*] Bit  0 (0x0001): STATE - Measurement ready / Run state active");

  LOG(LOG_DEBUG, "---------------------------------------------");
}

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;   // fixed by hardware, not part-dependent
  s->max_counts =  16000;

  // RESET's own write result isn't checked here — the ASIC power-cycles
  // itself immediately on receiving it and may NACK the transaction's STOP
  // phase as a side effect of that, which isn't a real failure. The
  // meaningful check is whether the sensor responds afterward.
  pte7300_write_cmd(s, PTE7300_CMD_RESET);
  usleep(50000);   // boot delay: give the ASIC time to reload EEPROM trim data

  unsigned short status_reg = 0;
  int ret = pte7300_read_u16(s, PTE7300_REG_STATUS, &status_reg);

  if (ret != I2C_SUCCESS) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X not responding post-reset", s->address);
    return I2C_ERROR;
  }

  if ( pte7300_validate_status(status_reg) != I2C_SUCCESS ) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X returned error status post-reset: 0x%04X", s->address, status_reg);
    pte7300_decode_status(status_reg);
    return I2C_ERROR;
  }
  
  LOG(LOG_INFO, "PTE7300 init success at 0x%02X (post-reset status: 0x%04X)", s->address, status_reg);
  return I2C_SUCCESS;
  
}



static i2c_reading_t pte7300_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  short raw_pressure = 0, raw_temp = 0;
  unsigned short raw_status = 0;

  int status = pte7300_read_i16(s, PTE7300_REG_PRESSURE, &raw_pressure);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  status = pte7300_read_i16(s, PTE7300_REG_TEMP, &raw_temp);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  pte7300_read_u16(s, PTE7300_REG_STATUS, &raw_status);   // best-effort, non-fatal

  if ( pte7300_validate_status(raw_status) != I2C_SUCCESS ) {
    LOG(LOG_WARNING, "PTE7300 at 0x%02X: status 0x%04X not in the accepted set (raw pressure=%d, temp=%d); "
                     "returning value 0 with status SUCCESS", s->address, raw_status, raw_pressure, raw_temp);
    pte7300_decode_status(raw_status);
    result.status = I2C_SUCCESS;
    return result;
  }

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, -16000, 16000, -40.0f, 125.0f);

  if (!isfinite(result.value) || !isfinite(result.temp_c)) {
    LOG(LOG_ERR, "PTE7300 at 0x%02X: computed non-finite value (pressure=%f temp=%f) — "
                 "sensor likely misconfigured (min_counts=%d max_counts=%d); check init call site",
        s->address, result.value, result.temp_c, s->min_counts, s->max_counts);
    result.status = I2C_ERROR;
    return result;
  }

  result.status = I2C_SUCCESS;
  return result;
}

static int pte7300_sleep(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_SLEEP); }
static int pte7300_reset(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_RESET); }

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  return pte7300_write_cmd(s, PTE7300_CMD_IDLE);
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;

  unsigned short lo, hi;
  int status = pte7300_read_u16(s, PTE7300_REG_SERIAL_LO, &lo);
  if (status != I2C_SUCCESS) return status;
  status = pte7300_read_u16(s, PTE7300_REG_SERIAL_HI, &hi);
  if (status != I2C_SUCCESS) return status;

  *serial = ((unsigned int)hi << 16) | lo;
  return I2C_SUCCESS;
}

// ─── Honeywell HSC/SSC pressure sensor ───────────────────────────────────────

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

static i2c_reading_t hsc_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  // No command needed — the device free-runs; just read the last conversion.
  // 4 bytes gets both pressure and temperature in one transaction.
  unsigned char buf[4];
  int status = i2c_read_bytes(I2C_GENERIC_BUS, s->address, buf, sizeof(buf));
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  unsigned char status_bits = buf[0] & HSC_STATUS_MASK;
  if (status_bits == HSC_STATUS_DIAGNOSTIC)   { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_COMMAND_MODE) { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_STALE)        { result.status = I2C_PENDING; /* fall through, value still returned below */ }

  unsigned int raw_pressure = ((buf[0] & 0x3F) << 8) | buf[1];                       // 14-bit
  unsigned int raw_temp     = ((unsigned int)buf[2] << 3) | (buf[3] >> 5);           // 11-bit

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, 0, HSC_TEMP_COUNTS_MAX, HSC_TEMP_MIN_C, HSC_TEMP_MAX_C);

  if (status_bits == HSC_STATUS_NORMAL) result.status = I2C_SUCCESS;
  return result;
}

// ─── Generic dispatch ─────────────────────────────────────────────────────────

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300:
      LOG(LOG_DEBUG, "i2c_sensor_init: PTE7300 at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      return i2c_sensor_init_pte7300(s, address, min_value, max_value);

    case I2C_SENSOR_HSC:
      LOG(LOG_DEBUG, "i2c_sensor_init: HSC at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      i2c_sensor_init_hsc_default(s, address, min_value, max_value);
      return I2C_SUCCESS;

    default:
      return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s)
{
  // Same underlying check regardless of family — both just need an ACK.
  return i2c_probe_address(I2C_GENERIC_BUS, s->address);
}

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_get_reading(s);
    case I2C_SENSOR_HSC:     return hsc_get_reading(s);
    default:                 return (i2c_reading_t){ 0.0f, 0.0f, I2C_ERROR };
  }
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_sleep(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // free-running, no sleep command over I2C
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_reset(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // no reset command over I2C
    default:                 return I2C_ERROR;
  }
}

#endif // ifndef DUMMY_SENSORS

// ─── Dummy sensor implementation ──────────────────────────────────────────────
// Compiled only when DUMMY_SENSORS is defined: make dummy
// Returns plausible values with small random drift, no hardware required.

#ifdef DUMMY_SENSORS

int i2c_bus_available(const char *bus_path) { (void)bus_path; return 1; }

void i2c_detect(bool deepscan, bool usesyslog)
{
  (void)deepscan;
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  DIAG_LOG(usesyslog, "00:                         -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "60: -- -- 62 63 -- -- 66 67 -- -- -- -- 6c 6d -- --\n");
  DIAG_LOG(usesyslog, "70: -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "\n");
  DIAG_LOG(usesyslog, "Detected devices:\n");
  DIAG_LOG(usesyslog, "  0x62  likely:    ORP     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x63  likely:    pH      (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x66  likely:    RTD     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x67  likely:    PUMP    (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6c  likely:    PTE7300 pressure (non-CRC)  (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6d  likely:    PTE7300 pressure (CRC)  (by default address, unconfirmed)\n");
}

void i2c_generic_detect(const char *bus_path)
{
  (void)bus_path;
  printf("\n[DUMMY] Simulated generic I2C bus scan\n\n");
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  printf("60:                          6c                    \n\n");
  printf("Detected devices:\n");
  printf("  0x6c  likely: PTE7300 pressure (non-CRC) (by default address, unconfirmed)\n\n");
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  (void)bus_path; (void)reg; (void)data; (void)len;
  return i2c_addr_valid(address) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  (void)bus_path; (void)reg;
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  (void)bus_path;
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  (void)bus_path; (void)data; (void)len;
  return i2c_addr_valid(address) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_probe_address(const char *bus_path, int address)
{
  (void)bus_path;
  return i2c_addr_valid(address) && (address == PTE7300_ADDR_DEFAULT);
}

// ─── Sensor instance setup (dummy) — same as real, no hardware touched ──────

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;
  s->max_counts =  16000;
  return I2C_SUCCESS;
}

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300: return i2c_sensor_init_pte7300(s, address, min_value, max_value);
    case I2C_SENSOR_HSC:     i2c_sensor_init_hsc_default(s, address, min_value, max_value); return I2C_SUCCESS;
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s) { (void)s; return 1; }

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  // Plausible clean pool filter pressure, same idea as prs_get_reading() in
  // ezo.c's dummy block — centered around 15 psi (or the unit's midpoint if
  // your span isn't psi-ish) with a little drift, clamped to the configured span.
  //float mid   = (s->min_value + s->max_value) / 2.0f;
  //float value = (s->max_value - s->min_value > 5.0f) ? 15.0f + dummy_drift(1.0f) : mid + dummy_drift((s->max_value - s->min_value) * 0.02f);
  //if (value < s->min_value) value = s->min_value;
  //if (value > s->max_value) value = s->max_value;

  // Center at 15.0 PSI with ±13.0 PSI drift gives range [2.0, 28.0] PSI
  float target_psi = 15.0f + dummy_drift(13.0f);

  // Convert PSI back to bar for the primary value field
  float value_bar = target_psi / 14.5037738f;

  return (i2c_reading_t){ value_bar, 25.0f + dummy_drift(1.0f), I2C_SUCCESS };
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  *serial = 0xDEADBEEF;
  return I2C_SUCCESS;
}

#endif // DUMMY_SENSORS



#ifdef __OLD__





#define _GNU_SOURCE
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "ezo.h"
#include "utils.h"
#include "i2c_bus.h"
#include "acd_types.h"

// ─── Real implementation ──────────────────────────────────────────────────────
// All code in this block is compiled only when DUMMY_SENSORS is NOT defined.
// To build with fake sensors: make dummy
// To build normally:          make

#ifndef DUMMY_SENSORS

#include <linux/i2c-dev.h>

// ─── Core I2C functions (private) ────────────────────────────────────────────

// Every bus transaction in this file starts here.  The address is validated first,
// because i2c_open() treats a negative address as "no lock, no ioctl", and a NULL
// sensor reaches us as -1 (see ezo_sensor_addr()).  i2c_open() then takes the
// per-address lock, which i2c_close() (via ezo_close) releases.
static int ezo_open(int address)
{
  if (!i2c_addr_valid(address)) return EZO_ERROR;
  return i2c_open(I2C_BUS, address);
}

static int ezo_close(int fd)
{
  return i2c_close(fd);
}

static int ezo_get_wait_ms(const char *cmd)
{
  if (strncasecmp(cmd, "R", 1) == 0 && strlen(cmd) == 1)
    return EZO_WAIT_READ;
  else if (strncasecmp(cmd, "RT,", 3) == 0)
    return EZO_WAIT_EC_TEMP;
  if (strncasecmp(cmd, "Cal", 3) == 0)
    return EZO_WAIT_CALIBRATE;
  if (strncasecmp(cmd, "D,", 2) == 0 ||
      strncasecmp(cmd, "DC,", 3) == 0 ||
      strncasecmp(cmd, "STOP", 4) == 0 ||
      strncasecmp(cmd, "P", 1) == 0)
    return EZO_WAIT_PUMP;
  return EZO_WAIT_GENERAL;
}

static int ezo_read(int fd, char *response, int len, int wait_ms)
{
  unsigned char buf[64] = {0};
  usleep(wait_ms * 1000);

  if (read(fd, buf, sizeof(buf)) < 0)
    return EZO_ERROR;

  if (buf[0] != EZO_SUCCESS)
    return buf[0];

  strncpy(response, (char *)&buf[1], len - 1);
  response[len - 1] = '\0';
  return EZO_SUCCESS;
}

static int ezo_send_cmd(int fd, const char *cmd)
{
  return write(fd, cmd, strlen(cmd));
}

static int ezo_read_retry(int fd, char *response, int len, int wait_ms)
{
  int status = ezo_read(fd, response, len, wait_ms);
  for (int i = 0; status == EZO_PENDING && i < EZO_RETRIES; i++)
    status = ezo_read(fd, response, len, EZO_RETRY_WAIT_MS);
  return status;
}

static int ezo_command(int address, const char *cmd, char *result, int result_len)
{
  int fd = ezo_open(address);
  if (fd < 0) return EZO_ERROR;

  int wait_ms = ezo_get_wait_ms(cmd);
  
  //ezo_send_cmd(fd, cmd);
  //int status = ezo_read(fd, result, result_len, wait_ms);

  if (ezo_send_cmd(fd, cmd) < 0)
  {
    ezo_close(fd);
    return EZO_ERROR;
  }

  int status = ezo_read_retry(fd, result, result_len, wait_ms);

  ezo_close(fd);
  return status;
}



// ─── I2C detect ──────────────────────────────────────────────────────────────

typedef struct {
  int         addr;
  const char *name;
} ezo_addr_map_t;

static const ezo_addr_map_t ezo_known_devices[] = {
  { 0x61, "DO"    },
  { 0x62, "ORP"   },
  { 0x63, "pH"    },
  { 0x64, "EC"    },
  { 0x66, "RTD"   },
  { 0x67, "PUMP"  },
  { 0x68, "FLOW"  },
  { 0x69, "CO2"   },
  { 0x6A, "O2"    },
  { 0x6B, "PRS"   },
  { 0x70, "HUM"   },
  { 0,    NULL    }
};

const char *ezo_name_from_addr(int addr)
{
  for (int i = 0; ezo_known_devices[i].name != NULL; i++)
    if (ezo_known_devices[i].addr == addr)
      return ezo_known_devices[i].name;
  return NULL;
}

const char *ezo_query_device_type(int addr)
{
  static char type_buf[16];

  int fd = ezo_open(addr);          // takes the per-address lock and sets I2C_SLAVE
  if (fd < 0) return NULL;

  ezo_send_cmd(fd, "i");
  usleep(EZO_WAIT_GENERAL * 1000);

  unsigned char buf[64] = {0};
  if (read(fd, buf, sizeof(buf)) < 0 || buf[0] != EZO_SUCCESS)
  {
    ezo_close(fd);
    return NULL;
  }
  ezo_close(fd);

  char *start = strchr((char *)&buf[1], ',');
  if (!start) return NULL;
  start++;
  char *end = strchr(start, ',');
  if (!end) end = start + strlen(start);

  int len = end - start;
  if (len <= 0 || len >= (int)sizeof(type_buf)) return NULL;
  strncpy(type_buf, start, len);
  type_buf[len] = '\0';
  return type_buf;
}

/*
This will probe EZO devices well, but potentially break generic i2c sensors forcing them to need a reset.
*/

void ezo_i2cdetect()
{
  // One fd walks every address, so it is opened with addr = -1 (no lock, no
  // I2C_SLAVE) and each probe takes that address's lock itself.  A device that is
  // mid-transaction on another thread is skipped until it is free.
  int fd = i2c_open(I2C_BUS, -1);
  if (fd < 0) { perror("open i2c"); return; }

  printf("\nScanning I2C bus %s...\n\n", I2C_BUS);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    printf("%02x: ", row * 16);
    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) { printf("   "); continue; }
      if (i2c_lock_addr(addr) != 0)   { printf("   "); continue; }

      unsigned char buf;
      int sel = ioctl(fd, I2C_SLAVE, addr);
      int rd  = (sel < 0) ? -1 : (int)read(fd, &buf, 1);
      i2c_unlock_addr(addr);

      if (sel < 0) { printf("   "); continue; }
      if (rd < 0)
        printf("-- ");
      else
      {
        printf("%02x ", addr);
        detected[count++] = addr;
      }
    }
    printf("\n");
  }
  i2c_close(fd);

  if (count == 0) { printf("\nNo devices found.\n"); return; }

  printf("\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known   = ezo_name_from_addr(addr);
    const char *queried = ezo_query_device_type(addr);

    if (queried)
      printf("  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    else if (known)
      printf("  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    else {/*
      const char *known = i2c_name_from_addr(addr);
      if (known)
        printf("  0x%02x  likely: %s (by default address, unconfirmed)\n", addr, known);
      else*/
        printf("  0x%02x  unknown device\n", addr);
    }
  }
  printf("\n");
}

// ─── Bus availability ─────────────────────────────────────────────────────────

int ezo_bus_available()
{
  int fd = i2c_open(I2C_BUS, -1);   // just proves the bus device opens
  if (fd < 0) return 0;
  i2c_close(fd);
  return 1;
}

// ─── Shared EZO helpers ───────────────────────────────────────────────────────
// Every helper takes the sensor struct.  A NULL sensor, or one with an invalid
// address, fails in ezo_open() with EZO_ERROR before anything touches the bus.

int ezo_get_info(ezo_sensor_t *sensor, char *info, int len)
{
  return ezo_command(ezo_sensor_addr(sensor), "i", info, len);
}

int ezo_get_status(ezo_sensor_t *sensor, char *status, int len)
{
  return ezo_command(ezo_sensor_addr(sensor), "Status", status, len);
}

int ezo_clear_calibration(ezo_sensor_t *sensor)
{
  char response[32];
  return ezo_command(ezo_sensor_addr(sensor), "Cal,clear", response, sizeof(response));
}

int ezo_get_cal_status(ezo_sensor_t *sensor, ezo_cal_status_t *cal)
{
  char response[32];
  if (cal == NULL) return EZO_ERROR;

  int status = ezo_command(ezo_sensor_addr(sensor), "Cal,?", response, sizeof(response));
  if (status == EZO_SUCCESS)
  {
    int points = 0;
    if (sscanf(response, "?CAL,%d", &points) == 1)
      cal->points = points;
    else
      cal->points = EZO_ERROR;
  }
  return status;
}

int ezo_sleep(ezo_sensor_t *sensor)
{
  int fd = ezo_open(ezo_sensor_addr(sensor));
  if (fd < 0) return EZO_ERROR;
  int rc = (ezo_send_cmd(fd, "Sleep") < 0) ? EZO_ERROR : EZO_SUCCESS;
  ezo_close(fd);
  return rc;
}

// ─── pH sensor ────────────────────────────────────────────────────────────────

ph_reading_t ph_get_reading(ezo_sensor_t *ph)
{
  ph_reading_t result = {0.0f, EZO_ERROR};
  char response[32];
  result.status = ezo_command(ezo_sensor_addr(ph), "R", response, sizeof(response));
  if (result.status == EZO_SUCCESS)
    result.value = atof(response);
  return result;
}

ph_reading_t ph_get_reading_compensated(ezo_sensor_t *ph, float temp_c)
{
  ph_reading_t result = {0.0f, EZO_ERROR};
  int addr = ezo_sensor_addr(ph);
  char cmd[32];
  char response[32];
  snprintf(cmd, sizeof(cmd), "T,%.2f", temp_c);
  ezo_command(addr, cmd, response, sizeof(response));
  result.status = ezo_command(addr, "R", response, sizeof(response));
  if (result.status == EZO_SUCCESS)
    result.value = atof(response);
  return result;
}

// Median of 3 readings — discards outliers from interference spikes
ph_reading_t ph_get_reading_filtered(ezo_sensor_t *ph)
{
  float readings[3];
  int valid = 0;
  for (int i = 0; i < 3; i++)
  {
    ph_reading_t r = ph_get_reading(ph);
    if (r.status == EZO_SUCCESS)
      readings[valid++] = r.value;
  }
  if (valid == 0) return (ph_reading_t){0.0f, EZO_ERROR};
  if (valid < 3)  return (ph_reading_t){readings[0], EZO_SUCCESS};
  if (readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  if (readings[1] > readings[2]) { float t = readings[1]; readings[1] = readings[2]; readings[2] = t; }
  if (readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  return (ph_reading_t){readings[1], EZO_SUCCESS};
}

int ph_calibrate(ezo_sensor_t *ph, ph_cal_point_t point, float ph_value)
{
  char cmd[32];
  char response[32];
  const char *point_str[] = {"low", "mid", "high"};
  if ((unsigned)point > PH_CAL_HIGH) return EZO_SYNTAX_ERR;
  snprintf(cmd, sizeof(cmd), "Cal,%s,%.2f", point_str[point], ph_value);
  return ezo_command(ezo_sensor_addr(ph), cmd, response, sizeof(response));
}

int ph_calibrate_mid(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_MID,  PH_REF_MID); }
int ph_calibrate_low(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_LOW,  PH_REF_LOW); }
int ph_calibrate_high(ezo_sensor_t *ph) { return ph_calibrate(ph, PH_CAL_HIGH, PH_REF_HIGH); }

int ph_get_cal_status(ezo_sensor_t *ph, ezo_cal_status_t *cal) { return ezo_get_cal_status(ph, cal); }
int ph_clear_calibration(ezo_sensor_t *ph)                     { return ezo_clear_calibration(ph); }
int ph_get_info(ezo_sensor_t *ph, char *info, int len)         { return ezo_get_info(ph, info, len); }
int ph_get_status(ezo_sensor_t *ph, char *status, int len)     { return ezo_get_status(ph, status, len); }
int ph_sleep(ezo_sensor_t *ph)                                 { return ezo_sleep(ph); }

int ph_calibrate_by_value(ezo_sensor_t *ph, float calibrationValue) {
    // Dynamically calculate the midpoints
    float low_mid_boundary  = (PH_REF_LOW + PH_REF_MID) / 2.0f;   // (4.00 + 7.00) / 2 = 5.50f
    float mid_high_boundary = (PH_REF_MID + PH_REF_HIGH) / 2.0f; // (7.00 + 10.00) / 2 = 8.50f

    //Anything below the low/mid midpoint
    if (calibrationValue < low_mid_boundary) {
        return ph_calibrate_low(ph);
    }
    // Anything between the low/mid midpoint and mid/high midpoint
    else if (calibrationValue >= low_mid_boundary && calibrationValue <= mid_high_boundary) {
        return ph_calibrate_mid(ph);
    }
    // Anything above the mid/high midpoint
    else {
        return ph_calibrate_high(ph);
    }
}

// ─── ORP sensor ───────────────────────────────────────────────────────────────

orp_reading_t orp_get_reading(ezo_sensor_t *orp)
{
  orp_reading_t result = {0.0f, EZO_ERROR};
  char response[32];
  result.status = ezo_command(ezo_sensor_addr(orp), "R", response, sizeof(response));
  if (result.status == EZO_SUCCESS)
    result.value = atof(response);
  return result;
}

int orp_calibrate(ezo_sensor_t *orp, float mv_value)
{
  char cmd[32];
  char response[32];
  snprintf(cmd, sizeof(cmd), "Cal,%.2f", mv_value);
  return ezo_command(ezo_sensor_addr(orp), cmd, response, sizeof(response));
}

int orp_get_cal_status(ezo_sensor_t *orp, ezo_cal_status_t *cal) { return ezo_get_cal_status(orp, cal); }
int orp_clear_calibration(ezo_sensor_t *orp)                     { return ezo_clear_calibration(orp); }
int orp_get_info(ezo_sensor_t *orp, char *info, int len)         { return ezo_get_info(orp, info, len); }
int orp_get_status(ezo_sensor_t *orp, char *status, int len)     { return ezo_get_status(orp, status, len); }
int orp_sleep(ezo_sensor_t *orp)                                 { return ezo_sleep(orp); }

// ─── RTD temperature sensor ───────────────────────────────────────────────────

static int rtd_command(int addr, const char *cmd, char *result, int result_len)
{
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;
  int wait_ms = (strncasecmp(cmd, "R", 1) == 0 && strlen(cmd) == 1)
    ? EZO_WAIT_RTD : ezo_get_wait_ms(cmd);
  ezo_send_cmd(fd, cmd);
  int status = ezo_read(fd, result, result_len, wait_ms);
  ezo_close(fd);
  return status;
}

rtd_reading_t rtd_get_reading(ezo_sensor_t *rtd)
{
  rtd_reading_t result = {0.0f, RTD_SCALE_CELSIUS, EZO_ERROR};
  char response[32];
  result.status = rtd_command(ezo_sensor_addr(rtd), "R", response, sizeof(response));
  if (result.status == EZO_SUCCESS)
    result.value = atof(response);
  return result;
}

int rtd_set_scale(ezo_sensor_t *rtd, rtd_scale_t scale)
{
  char response[32];
  const char *cmd;
  switch (scale)
  {
    case RTD_SCALE_FAHRENHEIT: cmd = "S,f"; break;
    case RTD_SCALE_KELVIN:     cmd = "S,k"; break;
    default:                   cmd = "S,c"; break;
  }
  return rtd_command(ezo_sensor_addr(rtd), cmd, response, sizeof(response));
}

int rtd_calibrate(ezo_sensor_t *rtd, float known_temp)
{
  char cmd[32];
  char response[32];
  snprintf(cmd, sizeof(cmd), "Cal,%.2f", known_temp);
  return rtd_command(ezo_sensor_addr(rtd), cmd, response, sizeof(response));
}

int rtd_get_cal_status(ezo_sensor_t *rtd, ezo_cal_status_t *cal) { return ezo_get_cal_status(rtd, cal); }
int rtd_clear_calibration(ezo_sensor_t *rtd)                     { return ezo_clear_calibration(rtd); }
int rtd_get_info(ezo_sensor_t *rtd, char *info, int len)         { return ezo_get_info(rtd, info, len); }
int rtd_get_status(ezo_sensor_t *rtd, char *status, int len)     { return ezo_get_status(rtd, status, len); }
int rtd_sleep(ezo_sensor_t *rtd)                                 { return ezo_sleep(rtd); }

// ─── Pressure sensor (EZO-PRS) ────────────────────────────────────────────────

prs_reading_t prs_get_reading(ezo_sensor_t *prs)
{
  prs_reading_t result = {0.0f, EZO_ERROR};
  char response[32];
  result.status = ezo_command(ezo_sensor_addr(prs), "R", response, sizeof(response));
  if (result.status == EZO_SUCCESS)
    result.value = atof(response);
  return result;
}

int prs_calibrate(ezo_sensor_t *prs, float psi_value)
{
  char cmd[32];
  char response[32];
  snprintf(cmd, sizeof(cmd), "Cal,%.2f", psi_value);
  return ezo_command(ezo_sensor_addr(prs), cmd, response, sizeof(response));
}

int prs_calibrate_zero(ezo_sensor_t *prs)
{
  char response[32];
  return ezo_command(ezo_sensor_addr(prs), "Cal,0", response, sizeof(response));
}

int prs_get_cal_status(ezo_sensor_t *prs, ezo_cal_status_t *cal) { return ezo_get_cal_status(prs, cal); }
int prs_clear_calibration(ezo_sensor_t *prs)                     { return ezo_clear_calibration(prs); }
int prs_get_info(ezo_sensor_t *prs, char *info, int len)         { return ezo_get_info(prs, info, len); }
int prs_get_status(ezo_sensor_t *prs, char *status, int len)     { return ezo_get_status(prs, status, len); }
int prs_sleep(ezo_sensor_t *prs)                                 { return ezo_sleep(prs); }



// ─── EZO-PMP dosing pump ─────────────────────────────────────────────────────



#define PMP_RETRY_WAIT_MS    100      /* re-read interval while device answers 254 */
#define PMP_MAX_RETRIES      8        /* 100 ms first wait + 8 x 100 ms = 900 ms   */
#ifndef PMP_RESTART_WAIT_MS
#define PMP_RESTART_WAIT_MS  1000     /* after I2C,<n> the device reboots          */
#endif
 
/* snprintf that reports truncation, so a clipped number is never sent. */
static int pmp_build(char *buf, size_t n, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
static int pmp_build(char *buf, size_t n, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return (len < 0 || (size_t)len >= n) ? -1 : 0;
}
 
/* One command/response exchange on an already-open fd.
 * ESPHome waits 400 ms for every PMP command and treats 254 as "keep waiting".
 * We read after EZO_WAIT_PUMP (100 ms) and re-read on 254, so fast commands stay
 * fast and slow ones are not reported as failures.                             */
static int pmp_xfer(int fd, const char *cmd, char *result, int result_len)
{
  if (result && result_len > 0) result[0] = '\0';
 
  if (ezo_send_cmd(fd, cmd) < 0) return EZO_ERROR;
 
  int status = ezo_read(fd, result, result_len, EZO_WAIT_PUMP);
  for (int i = 0; status == EZO_PENDING && i < PMP_MAX_RETRIES; i++)
    status = ezo_read(fd, result, result_len, PMP_RETRY_WAIT_MS);
  return status;
}
 
/* One locked transaction: ezo_open (validate + lock) -> exchange -> ezo_close (unlock). */
static int pump_command(int addr, const char *cmd, char *result, int result_len)
{
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;
 
  int status = pmp_xfer(fd, cmd, result, result_len);
  ezo_close(fd);
  return status;
}
 
/* text after the n-th comma (n >= 1), or NULL */
static const char *pmp_field(const char *s, int n)
{
  for (; n > 0; n--) {
    s = strchr(s, ',');
    if (!s) return NULL;
    s++;
  }
  return s;
}
 
static int pmp_to_float(const char *s, float *out)
{
  if (!s) return -1;
  char *end = NULL;
  float v = strtof(s, &end);
  if (end == s) return -1;
  *out = v;
  return 0;
}
 
/* Query returning a single number.  skip_prefix=1 for "?XX,<n>" replies,
 * 0 for bare numbers ("R").  Returns -1.0f on any failure.                  */
static float pmp_query_float(int addr, const char *cmd, int skip_prefix)
{
  char resp[32];
  float v = 0.0f;
 
  if (pump_command(addr, cmd, resp, sizeof(resp)) != EZO_SUCCESS) return -1.0f;
  return (pmp_to_float(skip_prefix ? pmp_field(resp, 1) : resp, &v) == 0) ? v : -1.0f;
}
 
/* ---- start a dose ------------------------------------------------------ */
 
int pump_dose_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[32], resp[32];
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;                 /* never send <= 0 or NaN */
  if (pmp_build(cmd, sizeof(cmd), "D,%.2f", ml) < 0) return EZO_SYNTAX_ERR;
  return pump_command(addr, cmd, resp, sizeof(resp));
}
 
/* "Dose over time": the device takes WHOLE minutes (ESPHome sends an int), so
 * this is not usable for second-scale doses.                                  */
int pump_dose_volume_over_time(ezo_sensor_t *pump, float ml, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[32], resp[32];
  if (!(ml > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  if (pmp_build(cmd, sizeof(cmd), "D,%.2f,%d", ml, minutes) < 0) return EZO_SYNTAX_ERR;
  return pump_command(addr, cmd, resp, sizeof(resp));
}
 
/* Constant flow rate for a fixed number of whole minutes. */
int pump_dose_rate(ezo_sensor_t *pump, float ml_per_min, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[32], resp[32];
  if (!(ml_per_min > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  if (pmp_build(cmd, sizeof(cmd), "DC,%.2f,%d", ml_per_min, minutes) < 0) return EZO_SYNTAX_ERR;
  return pump_command(addr, cmd, resp, sizeof(resp));
}
 
/* Constant flow rate until pump_stop().  The device needs the second field
 * ("DC,<rate>" alone is incomplete) and caps continuous runs at 20 days, so a
 * dead daemon leaves it running: prefer pump_dose_volume() for chemical dosing. */
int pump_dose_rate_continuous(ezo_sensor_t *pump, float ml_per_min)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[32], resp[32];
  if (!(ml_per_min > 0.0f)) return EZO_SYNTAX_ERR;
  if (pmp_build(cmd, sizeof(cmd), "DC,%.2f,*", ml_per_min) < 0) return EZO_SYNTAX_ERR;
  return pump_command(addr, cmd, resp, sizeof(resp));
}
 
/* Continuous at the pump's maximum (calibrated) rate, until pump_stop(). */
int pump_dose_max_continuous(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  return pump_command(addr, "D,*", resp, sizeof(resp));
}
 
/* ---- control ----------------------------------------------------------- */
 
int pump_stop(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  return pump_command(addr, "X", resp, sizeof(resp));
}
 
/* P is a TOGGLE on the device (pause and resume are the same command), so a
 * blind P twice would resume.  Read P,? and toggle only when needed, all on one
 * fd (one lock hold).  If the state can't be read nothing is sent.
 * VERIFY: P while idle.                                                       */
static int pump_set_paused(int addr, int want_paused)
{
  char resp[32];
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;
 
  int rc = pmp_xfer(fd, "P,?", resp, sizeof(resp));
  if (rc == EZO_SUCCESS) {
    const char *f = pmp_field(resp, 1);                    /* "?P,<0|1>" */
    if (!f)
      rc = EZO_NO_DATA;
    else if ((atoi(f) == 1) != (want_paused != 0))
      rc = pmp_xfer(fd, "P", resp, sizeof(resp));
  }
 
  ezo_close(fd);
  return rc;
}
 
int pump_pause(ezo_sensor_t *pump)  { return pump_set_paused(ezo_sensor_addr(pump), 1); }
int pump_resume(ezo_sensor_t *pump) { return pump_set_paused(ezo_sensor_addr(pump), 0); }
 
/* 1 paused, 0 not paused, -1 error */
int pump_is_paused(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  float v = 0.0f;
 
  if (pump_command(addr, "P,?", resp, sizeof(resp)) != EZO_SUCCESS) return -1;
  if (pmp_to_float(pmp_field(resp, 1), &v) != 0) return -1;
  return ((int)v == 1) ? 1 : 0;
}
 
/* ---- query ------------------------------------------------------------- */
 
pump_dose_status_t pump_get_dose_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  pump_dose_status_t result = { 0.0f, 0, EZO_ERROR };
  char resp[32];
 
  /* D,? is the dispense-state query.  ("Status" is the generic EZO restart-reason
   * command and says nothing about whether the pump is running.)               */
  result.status = pump_command(addr, "D,?", resp, sizeof(resp));
  if (result.status != EZO_SUCCESS) return result;
 
  /* "?D,<volume>,<flag>": the flag is the LAST field and is what ESPHome keys on
   * (== 1 means dosing).  The volume is parsed leniently since it may not be
   * numeric while continuous (VERIFY: could be "*").                           */
  const char *first = pmp_field(resp, 1);
  const char *last  = strrchr(resp, ',');
  if (!first || last < first) {
    result.status = EZO_NO_DATA;
    return result;
  }
 
  float vol = 0.0f;
  result.requested_volume_ml = (pmp_to_float(first, &vol) == 0) ? vol : 0.0f;
  result.is_pumping = (atoi(last + 1) == 1);
  return result;
}
 
/* Volume dispensed by the current/last dose.
 * VERIFY: value after a completed dose, and after X.                          */
float pump_get_dispensed_volume(ezo_sensor_t *pump)        { return pmp_query_float(ezo_sensor_addr(pump), "R", 0); }
float pump_get_total_volume(ezo_sensor_t *pump)            { return pmp_query_float(ezo_sensor_addr(pump), "TV,?", 1); }
float pump_get_absolute_total_volume(ezo_sensor_t *pump)   { return pmp_query_float(ezo_sensor_addr(pump), "ATV,?", 1); }
float pump_get_max_flow_rate(ezo_sensor_t *pump)           { return pmp_query_float(ezo_sensor_addr(pump), "DC,?", 1); }
float pump_get_voltage(ezo_sensor_t *pump)                 { return pmp_query_float(ezo_sensor_addr(pump), "PV,?", 1); }
 
int pump_clear_total_volume(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  return pump_command(addr, "Clear", resp, sizeof(resp));
}
 
/* ---- calibration ------------------------------------------------------- */
 
/* 0 uncalibrated, 1 fixed volume, 2 volume/time, 3 both; -1 on error. */
int pump_get_calibration_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  float v = pmp_query_float(addr, "Cal,?", 1);
  return (v < 0.0f) ? -1 : (int)v;
}
 
/* VERIFY the datasheet procedure before calling (dispense, measure, then Cal,<ml>). */
int pump_set_calibration_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[32], resp[32];
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  if (pmp_build(cmd, sizeof(cmd), "Cal,%.2f", ml) < 0) return EZO_SYNTAX_ERR;
  return pump_command(addr, cmd, resp, sizeof(resp));
}
 
int pump_clear_calibration(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  return pump_command(addr, "Cal,clear", resp, sizeof(resp));
}
 
/* ---- device ------------------------------------------------------------ */
 
/* Blinks the LED.  ESPHome treats it as a 60 s command; VERIFY whether any
 * following command cancels it.  We only wait for the ack.                    */
int pump_find(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  char resp[32];
  return pump_command(addr, "Find", resp, sizeof(resp));
}
 
/* One-time provisioning (two pumps both ship at 0x67).  The device restarts at
 * the new address and sends no usable reply, so success means the write was
 * accepted.  Refuses if something already answers at new_addr.  On success
 * pump->address is updated (not thread-safe against other users of the same
 * struct: do this at setup/provisioning time), then save it to the config.
 * VERIFY on the bench with ONE pump on the bus.                               */
int pump_set_i2c_address(ezo_sensor_t *pump, int new_addr)
{
  int addr = ezo_sensor_addr(pump);
 
  char cmd[16];
 
  if (!i2c_addr_valid(addr) || !i2c_addr_valid(new_addr)) return EZO_SYNTAX_ERR;
  if (new_addr == addr) return EZO_SUCCESS;
  if (i2c_probe_address(I2C_BUS, new_addr)) return EZO_ERROR;   /* occupied */
  if (pmp_build(cmd, sizeof(cmd), "I2C,%d", new_addr) < 0) return EZO_SYNTAX_ERR;
 
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;
  int rc = (ezo_send_cmd(fd, cmd) < 0) ? EZO_ERROR : EZO_SUCCESS;
  ezo_close(fd);
 
  if (rc == EZO_SUCCESS) {
    pump->address = (unsigned char)new_addr;     /* keep the struct in step with the device */
    usleep(PMP_RESTART_WAIT_MS * 1000);
  }
  return rc;
}
 
/* The generic EZO helpers go through ezo_open() themselves, so they take the same
 * per-address lock.                                                           */
int pump_get_info(ezo_sensor_t *pump, char *info, int len)            { return ezo_get_info(pump, info, len); }
int pump_get_device_status(ezo_sensor_t *pump, char *status, int len) { return ezo_get_status(pump, status, len); }
int pump_sleep(ezo_sensor_t *pump)                                    { return ezo_sleep(pump); }



// ─── Conductivity sensor (EZO-EC) ─────────────────────────────────────────────

static int ec_command(int addr, const char *cmd, char *result, int result_len)
{
  return ezo_command(addr, cmd, result, result_len);
  /*
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;

  int wait_ms = ezo_get_wait_ms(cmd);

  if (strcasecmp(cmd, "R") == 0)
    wait_ms = EZO_WAIT_EC;
  else if (strncasecmp(cmd, "RT,", 3) == 0)
    wait_ms = EZO_WAIT_EC_TEMP;

  if (ezo_send_cmd(fd, cmd) < 0)
  {
    ezo_close(fd);
    return EZO_ERROR;
  }

  int status = ezo_read_retry(fd, result, result_len, wait_ms);

  ezo_close(fd);
  return status;
  */
}

/*
static int ec_command(int addr, const char *cmd, char *result, int result_len)
{
  int fd = ezo_open(addr);
  if (fd < 0) return EZO_ERROR;

  int wait_ms = ezo_get_wait_ms(cmd);

  if (strcmp(cmd, "R") == 0)
    wait_ms = EZO_WAIT_EC;
  else if (strncasecmp(cmd, "RT,", 3) == 0)
    wait_ms = EZO_WAIT_EC_TEMP;

  //ezo_send_cmd(fd, cmd);
  if (ezo_send_cmd(fd, cmd) < 0) { ezo_close(fd); return EZO_ERROR; }
  int status = ezo_read(fd, result, result_len, wait_ms);

  // Re read on pending.
  for (int i = 0; status == EZO_PENDING && i < 5; i++)
    status = ezo_read(fd, result, result_len, 100);

  ezo_close(fd);
  return status;
}
*/
static int ec_parse_output_mask(const char *response,
                                ec_output_mask_t *mask)
{
  const char *p = response;

  *mask = 0;

  /*
   * Response is normally:
   *
   *     ?O,EC,TDS,S,SG
   *
   * The parameters are always reported in the order:
   *
   *     EC, TDS, S, SG
   */

  if (*p == '?')
    p++;

  if (strncasecmp(p, "O,", 2) == 0)
    p += 2;
  else
    return EZO_ERROR;

  char copy[64];
  snprintf(copy, sizeof(copy), "%s", p);

  char *saveptr = NULL;
  char *token = strtok_r(copy, ",", &saveptr);

  while (token != NULL)
  {
    if (strcasecmp(token, "EC") == 0)
      *mask |= EC_OUTPUT_CONDUCTIVITY;
    else if (strcasecmp(token, "TDS") == 0)
      *mask |= EC_OUTPUT_TDS;
    else if (strcasecmp(token, "S") == 0)
      *mask |= EC_OUTPUT_SALINITY;
    else if (strcasecmp(token, "SG") == 0)
      *mask |= EC_OUTPUT_SPECIFIC_GRAVITY;

    token = strtok_r(NULL, ",", &saveptr);
  }

  return EZO_SUCCESS;
}

int ec_get_output_mask(ezo_sensor_t *ec, ec_output_mask_t *mask)
{
  char response[64];

  if (mask == NULL)
    return EZO_ERROR;

  int status = ec_command(ezo_sensor_addr(ec), "O,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  return ec_parse_output_mask(response, mask);
}

int ec_set_output(ezo_sensor_t *ec, ec_output_t output, bool enabled)
{
  const char *parameter;

  switch (output)
  {
    case EC_OUTPUT_CONDUCTIVITY:
      parameter = "EC";
      break;

    case EC_OUTPUT_TDS:
      parameter = "TDS";
      break;

    case EC_OUTPUT_SALINITY:
      parameter = "S";
      break;

    case EC_OUTPUT_SPECIFIC_GRAVITY:
      parameter = "SG";
      break;

    default:
      return EZO_ERROR;
  }

  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd), "O,%s,%d", parameter, enabled ? 1 : 0);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}

static int ec_parse_reading(const char *response, ec_output_mask_t mask, ec_reading_t *reading)
{
  char copy[64];
  char *saveptr = NULL;
  char *token;
  int field = 0;

  if (reading == NULL)
    return EZO_ERROR;

  memset(reading, 0, sizeof(*reading));

  snprintf(copy, sizeof(copy), "%s", response);

  token = strtok_r(copy, ",", &saveptr);

  while (token != NULL)
  {
    float value = strtof(token, NULL);

    switch (field)
    {
      case 0:
        if (mask & EC_OUTPUT_CONDUCTIVITY)
        {
          reading->conductivity = value;
          reading->valid |= EC_OUTPUT_CONDUCTIVITY;
        }
        break;

      case 1:
        if (mask & EC_OUTPUT_TDS)
        {
          reading->tds = value;
          reading->valid |= EC_OUTPUT_TDS;
        }
        break;

      case 2:
        if (mask & EC_OUTPUT_SALINITY)
        {
          reading->salinity = value;
          reading->valid |= EC_OUTPUT_SALINITY;
        }
        break;

      case 3:
        if (mask & EC_OUTPUT_SPECIFIC_GRAVITY)
        {
          reading->specific_gravity = value;
          reading->valid |= EC_OUTPUT_SPECIFIC_GRAVITY;
        }
        break;

      default:
        break;
    }

    field++;
    token = strtok_r(NULL, ",", &saveptr);
  }

  return reading->valid ? EZO_SUCCESS : EZO_NO_DATA;
}

ec_reading_t ec_get_reading(ezo_sensor_t *ec)
{
  ec_reading_t result = {0};
  char response[64];

  ec_output_mask_t mask;

  result.status = ec_get_output_mask(ec, &mask);

  if (result.status != EZO_SUCCESS)
    return result;

  result.status = ec_command(ezo_sensor_addr(ec), "R", response, sizeof(response));

  if (result.status != EZO_SUCCESS)
    return result;

  result.status = ec_parse_reading(response, mask, &result);

  return result;
}

ec_reading_t ec_get_reading_compensated(ezo_sensor_t *ec, float temp_c)
{
  ec_reading_t result = {0};
  char cmd[32];
  char response[64];

  ec_output_mask_t mask;

  result.status = ec_get_output_mask(ec, &mask);

  if (result.status != EZO_SUCCESS)
    return result;

  snprintf(cmd, sizeof(cmd), "RT,%.2f", temp_c);

  result.status = ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));

  if (result.status != EZO_SUCCESS)
    return result;

  result.status = ec_parse_reading(response, mask, &result);

  return result;
}

int ec_set_k(ezo_sensor_t *ec, float k)
{
  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd), "K,%.4f", k);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}

int ec_get_k(ezo_sensor_t *ec, float *k)
{
  char response[32];

  if (k == NULL)
    return EZO_ERROR;

  int status = ec_command(ezo_sensor_addr(ec), "K,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  if (sscanf(response, "?K,%f", k) != 1)
    return EZO_ERROR;

  return EZO_SUCCESS;
}

int ec_set_tds_factor(ezo_sensor_t *ec, float factor)
{
  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd), "TDS,%.4f", factor);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}

int ec_get_tds_factor(ezo_sensor_t *ec, float *factor)
{
  char response[32];

  if (factor == NULL)
    return EZO_ERROR;

  int status = ec_command(ezo_sensor_addr(ec), "TDS,?", response, sizeof(response));

  if (status != EZO_SUCCESS)
    return status;

  if (sscanf(response, "?TDS,%f", factor) != 1)
    return EZO_ERROR;

  return EZO_SUCCESS;
}

int ec_calibrate_dry(ezo_sensor_t *ec)
{
  char response[32];

  return ec_command(ezo_sensor_addr(ec), "Cal,dry", response, sizeof(response));
}

int ec_calibrate(ezo_sensor_t *ec, float conductivity)
{
  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd), "Cal,%.2f", conductivity);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}

int ec_calibrate_low(ezo_sensor_t *ec, float conductivity)
{
  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd), "Cal,low,%.2f", conductivity);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}

int ec_calibrate_high(ezo_sensor_t *ec, float conductivity)
{
  char cmd[32];
  char response[32];

  snprintf(cmd, sizeof(cmd),"Cal,high,%.2f",conductivity);

  return ec_command(ezo_sensor_addr(ec), cmd, response, sizeof(response));
}
// --- Wrappers
int ec_get_cal_status(ezo_sensor_t *ec, ezo_cal_status_t *cal) { return ezo_get_cal_status(ec, cal); }
int ec_clear_calibration(ezo_sensor_t *ec)                     { return ezo_clear_calibration(ec); }
int ec_get_info(ezo_sensor_t *ec, char *info, int len)         { return ezo_get_info(ec, info, len); }
int ec_get_status(ezo_sensor_t *ec, char *status, int len)     { return ezo_get_status(ec, status, len); }
int ec_sleep(ezo_sensor_t *ec)                                 { return ezo_sleep(ec); }


//#endif // DUMMY_SENSORS
#endif // ifndef DUMMY_SENSORS

// ─── Dummy sensor implementation ──────────────────────────────────────────────
// Compiled only when DUMMY_SENSORS is defined: make dummy
// All functions return plausible pool chemistry values with small random drift.
// No I2C hardware required — safe to run on any machine.

#ifdef DUMMY_SENSORS

/*
// Small random float drift in range [-range, +range]
static float dummy_drift(float range)
{
  return ((float)(rand() % 1000) / 1000.0f - 0.5f) * 2.0f * range;
}
*/
// ─── Bus / detect (dummy) ─────────────────────────────────────────────────────

int ezo_bus_available()
{
  static int seeded = 0;
  if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
  return 1;   // always available in dummy mode
}

void ezo_i2cdetect(bool usesyslog)
{
  DIAG_LOG(usesyslog, "\n");
  DIAG_LOG(usesyslog, "[DUMMY] Simulated I2C bus scan on %s\n\n", I2C_BUS);
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  DIAG_LOG(usesyslog, "60:              62 63 64       67 68               \n\n");
  DIAG_LOG(usesyslog, "-\n");
  DIAG_LOG(usesyslog, "Detected devices:\n");
  DIAG_LOG(usesyslog, "  0x62  confirmed: ORP    (default addr for: ORP)\n");
  DIAG_LOG(usesyslog, "  0x63  confirmed: PH     (default addr for: PH)\n");
  DIAG_LOG(usesyslog, "  0x64  confirmed: EC     (default addr for: EC)\n");
  DIAG_LOG(usesyslog, "  0x67  confirmed: PUMP   (default addr for: PUMP)\n");
  DIAG_LOG(usesyslog, "  0x68  confirmed: RTD    (default addr for: RTD)\n");
  DIAG_LOG(usesyslog, "---------------------------------------------\n");
}
/*
void ezo_i2cdetect()
{
  printf("\n[DUMMY] Simulated I2C bus scan on %s\n\n", I2C_BUS);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  printf("60:              62 63 64       67 68               \n\n");
  printf("Detected devices:\n");
  printf("  0x62  confirmed: ORP    (default addr for: ORP)\n");
  printf("  0x63  confirmed: pH     (default addr for: pH)\n");
  printf("  0x64  confirmed: EC     (default addr for: EC)\n")
  printf("  0x67  confirmed: PUMP   (default addr for: PUMP)\n");
  printf("  0x68  confirmed: RTD    (default addr for: RTD)\n");
  printf("\n");
}
*/

void simulate_read_time()
{
  // Sleep for 1 second
  sleep(1);
}

// ─── Shared EZO helpers (dummy) ───────────────────────────────────────────────

// Same rule as the real build: a NULL sensor or an invalid address is an error,
// so a bad call is caught on a dev machine as well as on the Pi.
#define DUMMY_SENSOR_OK(s)  i2c_addr_valid(ezo_sensor_addr(s))

int ezo_get_info(ezo_sensor_t *sensor, char *info, int len)
{
  if (!DUMMY_SENSOR_OK(sensor)) return EZO_ERROR;
  simulate_read_time();
  snprintf(info, len, "?I,DUMMY,1.00");
  return EZO_SUCCESS;
}

int ezo_get_status(ezo_sensor_t *sensor, char *status, int len)
{
  if (!DUMMY_SENSOR_OK(sensor)) return EZO_ERROR;
  simulate_read_time();
  snprintf(status, len, "?STATUS,P,5.00");
  return EZO_SUCCESS;
}

int ezo_clear_calibration(ezo_sensor_t *sensor)
{
  return DUMMY_SENSOR_OK(sensor) ? EZO_SUCCESS : EZO_ERROR;
}

int ezo_sleep(ezo_sensor_t *sensor)
{
  return DUMMY_SENSOR_OK(sensor) ? EZO_SUCCESS : EZO_ERROR;
}

int ezo_get_cal_status(ezo_sensor_t *sensor, ezo_cal_status_t *cal)
{
  if (!DUMMY_SENSOR_OK(sensor) || cal == NULL) return EZO_ERROR;
  simulate_read_time();
  cal->points = 3;
  return EZO_SUCCESS;
}

// ─── pH sensor (dummy) ────────────────────────────────────────────────────────

ph_reading_t ph_get_reading(ezo_sensor_t *ph)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  return (ph_reading_t){ 7.20f + dummy_drift(0.15f), EZO_SUCCESS };
}

ph_reading_t ph_get_reading_compensated(ezo_sensor_t *ph, float temp_c)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  (void)temp_c;
  // between 7.1 and 8.1 = Use 7.6 as the base, and 0.5 as the range
  return (ph_reading_t){ 7.60f + dummy_drift(0.5f), EZO_SUCCESS };
}

ph_reading_t ph_get_reading_filtered(ezo_sensor_t *ph)
{
  if (!DUMMY_SENSOR_OK(ph)) return (ph_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  // Call dummy ph_get_reading 3x and return median — same logic as real version
  float r[3];
  for (int i = 0; i < 3; i++) r[i] = ph_get_reading(ph).value;
  if (r[0] > r[1]) { float t = r[0]; r[0] = r[1]; r[1] = t; }
  if (r[1] > r[2]) { float t = r[1]; r[1] = r[2]; r[2] = t; }
  if (r[0] > r[1]) { float t = r[0]; r[0] = r[1]; r[1] = t; }
  return (ph_reading_t){ r[1], EZO_SUCCESS };
}

int ph_calibrate(ezo_sensor_t *ph, ph_cal_point_t point, float ph_value)
{
  const char *point_str[] = {"low", "mid", "high"};
  if (!DUMMY_SENSOR_OK(ph)) return EZO_ERROR;
  if ((unsigned)point > PH_CAL_HIGH) return EZO_SYNTAX_ERR;
  simulate_read_time();
  printf("[DUMMY] pH calibrate %s at %.2f — OK\n", point_str[point], ph_value);
  return EZO_SUCCESS;
}

int ph_calibrate_mid(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_MID,  PH_REF_MID); }
int ph_calibrate_low(ezo_sensor_t *ph)  { return ph_calibrate(ph, PH_CAL_LOW,  PH_REF_LOW); }
int ph_calibrate_high(ezo_sensor_t *ph) { return ph_calibrate(ph, PH_CAL_HIGH, PH_REF_HIGH); }

int ph_get_cal_status(ezo_sensor_t *ph, ezo_cal_status_t *cal) { return ezo_get_cal_status(ph, cal); }
int ph_clear_calibration(ezo_sensor_t *ph)                     { return ezo_clear_calibration(ph); }
int ph_get_info(ezo_sensor_t *ph, char *info, int len)         { return ezo_get_info(ph, info, len); }
int ph_get_status(ezo_sensor_t *ph, char *status, int len)     { return ezo_get_status(ph, status, len); }
int ph_sleep(ezo_sensor_t *ph)                                 { return ezo_sleep(ph); }

int ph_calibrate_by_value(ezo_sensor_t *ph, float calibrationValue) {
    // Dynamically calculate the midpoints
    float low_mid_boundary  = (PH_REF_LOW + PH_REF_MID) / 2.0f;   // (4.00 + 7.00) / 2 = 5.50f
    float mid_high_boundary = (PH_REF_MID + PH_REF_HIGH) / 2.0f; // (7.00 + 10.00) / 2 = 8.50f

    //Anything below the low/mid midpoint
    if (calibrationValue < low_mid_boundary) {
        return ph_calibrate_low(ph);
    }
    // Anything between the low/mid midpoint and mid/high midpoint
    else if (calibrationValue >= low_mid_boundary && calibrationValue <= mid_high_boundary) {
        return ph_calibrate_mid(ph);
    }
    // Anything above the mid/high midpoint
    else {
        return ph_calibrate_high(ph);
    }
}

// ─── ORP sensor (dummy) ───────────────────────────────────────────────────────

orp_reading_t orp_get_reading(ezo_sensor_t *orp)
{
  if (!DUMMY_SENSOR_OK(orp)) return (orp_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  return (orp_reading_t){ 650.0f + dummy_drift(20.0f), EZO_SUCCESS };
}

int orp_calibrate(ezo_sensor_t *orp, float mv_value)
{
  if (!DUMMY_SENSOR_OK(orp)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] ORP calibrate at %.2f mV — OK\n", mv_value);
  return EZO_SUCCESS;
}

int orp_get_cal_status(ezo_sensor_t *orp, ezo_cal_status_t *cal) { return ezo_get_cal_status(orp, cal); }
int orp_clear_calibration(ezo_sensor_t *orp)                     { return ezo_clear_calibration(orp); }
int orp_get_info(ezo_sensor_t *orp, char *info, int len)         { return ezo_get_info(orp, info, len); }
int orp_get_status(ezo_sensor_t *orp, char *status, int len)     { return ezo_get_status(orp, status, len); }
int orp_sleep(ezo_sensor_t *orp)                                 { return ezo_sleep(orp); }

// ─── RTD temperature sensor (dummy) ──────────────────────────────────────────

rtd_reading_t rtd_get_reading(ezo_sensor_t *rtd)
{
  if (!DUMMY_SENSOR_OK(rtd)) return (rtd_reading_t){ 0.0f, RTD_SCALE_CELSIUS, EZO_ERROR };
  simulate_read_time();
  // Return between 28 and 29
  //return (rtd_reading_t){ 28.5f + dummy_drift(0.5f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

  // Return between -5 and 75
  //return (rtd_reading_t){ 35.0f + dummy_drift(40.0f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

  return (rtd_reading_t){ 30.0f + dummy_drift(1.0f), RTD_SCALE_CELSIUS, EZO_SUCCESS };

}

int rtd_set_scale(ezo_sensor_t *rtd, rtd_scale_t scale)
{
  const char *scales[] = {"Celsius", "Fahrenheit", "Kelvin"};
  if (!DUMMY_SENSOR_OK(rtd)) return EZO_ERROR;
  if ((unsigned)scale > RTD_SCALE_KELVIN) return EZO_SYNTAX_ERR;
  printf("[DUMMY] RTD scale set to %s — OK\n", scales[scale]);
  return EZO_SUCCESS;
}

int rtd_calibrate(ezo_sensor_t *rtd, float known_temp)
{
  if (!DUMMY_SENSOR_OK(rtd)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] RTD calibrate at %.2f — OK\n", known_temp);
  return EZO_SUCCESS;
}

int rtd_get_cal_status(ezo_sensor_t *rtd, ezo_cal_status_t *cal) { return ezo_get_cal_status(rtd, cal); }
int rtd_clear_calibration(ezo_sensor_t *rtd)                     { return ezo_clear_calibration(rtd); }
int rtd_get_info(ezo_sensor_t *rtd, char *info, int len)         { return ezo_get_info(rtd, info, len); }
int rtd_get_status(ezo_sensor_t *rtd, char *status, int len)     { return ezo_get_status(rtd, status, len); }
int rtd_sleep(ezo_sensor_t *rtd)                                 { return ezo_sleep(rtd); }

// ─── Pressure sensor (dummy) ──────────────────────────────────────────────────

prs_reading_t prs_get_reading(ezo_sensor_t *prs)
{
  if (!DUMMY_SENSOR_OK(prs)) return (prs_reading_t){ 0.0f, EZO_ERROR };
  simulate_read_time();
  // Return a plausible clean pool filter pressure around 15.0 psi
  return (prs_reading_t){ 15.0f + dummy_drift(1.0f), EZO_SUCCESS };
}

int prs_calibrate(ezo_sensor_t *prs, float psi_value)
{
  if (!DUMMY_SENSOR_OK(prs)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] PRS calibrate at %.2f psi — OK\n", psi_value);
  return EZO_SUCCESS;
}

int prs_calibrate_zero(ezo_sensor_t *prs)
{
  if (!DUMMY_SENSOR_OK(prs)) return EZO_ERROR;
  simulate_read_time();
  printf("[DUMMY] PRS calibrate zero (atmospheric) — OK\n");
  return EZO_SUCCESS;
}

int prs_get_cal_status(ezo_sensor_t *prs, ezo_cal_status_t *cal) { return ezo_get_cal_status(prs, cal); }
int prs_clear_calibration(ezo_sensor_t *prs)                     { return ezo_clear_calibration(prs); }
int prs_get_info(ezo_sensor_t *prs, char *info, int len)         { return ezo_get_info(prs, info, len); }
int prs_get_status(ezo_sensor_t *prs, char *status, int len)     { return ezo_get_status(prs, status, len); }
int prs_sleep(ezo_sensor_t *prs)                                 { return ezo_sleep(prs); }


// ─── Conductivity sensor (EZO-EC) ─────────────────────────────────────────────
static ec_output_mask_t dummy_ec_output_mask = EC_OUTPUT_CONDUCTIVITY;

int ec_get_output_mask(ezo_sensor_t *ec, ec_output_mask_t *mask)
{
  if (!DUMMY_SENSOR_OK(ec) || mask == NULL)
    return EZO_ERROR;

  *mask = dummy_ec_output_mask;
  return EZO_SUCCESS;
}

int ec_set_output(ezo_sensor_t *ec, ec_output_t output, bool enabled)
{
  if (!DUMMY_SENSOR_OK(ec))
    return EZO_ERROR;

  if (enabled)
    dummy_ec_output_mask |= output;
  else
    dummy_ec_output_mask &= ~output;

  return EZO_SUCCESS;
}

ec_reading_t ec_get_reading(ezo_sensor_t *ec)
{
    ec_reading_t result = {0};

    if (!DUMMY_SENSOR_OK(ec))
    {
        result.status = EZO_ERROR;
        return result;
    }

    simulate_read_time();

    result.status = EZO_SUCCESS;

    /* Single underlying measurement (µS/cm); everything else derives from it.
       Salt pool: ~3.2 ppt salt ≈ 5800 µS/cm, drifting about ±150 µS/cm. */
    const float ec_us = 5800.0f + dummy_drift(150.0f);

    if (dummy_ec_output_mask & EC_OUTPUT_CONDUCTIVITY)
    {
        result.conductivity = ec_us;
        result.valid |= EC_OUTPUT_CONDUCTIVITY;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_TDS)
    {
        result.tds = ec_us * 0.54f;               /* ~3130 ppm */
        result.valid |= EC_OUTPUT_TDS;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_SALINITY)
    {
        result.salinity = (ec_us / 1000.0f) * 0.55f;   /* ~3.2 ppt */
        result.valid |= EC_OUTPUT_SALINITY;
    }

    if (dummy_ec_output_mask & EC_OUTPUT_SPECIFIC_GRAVITY)
    {
        float salinity_ppt = (ec_us / 1000.0f) * 0.55f;
        result.specific_gravity = 1.000f + salinity_ppt * 0.00075f;  /* ~1.0024 */
        result.valid |= EC_OUTPUT_SPECIFIC_GRAVITY;
    }

    return result;
}

ec_reading_t ec_get_reading_compensated(ezo_sensor_t *ec, float temp_c)
{
  (void)temp_c;
  return ec_get_reading(ec);
}

int ec_set_k(ezo_sensor_t *ec, float k)
{
  (void)k;
  return DUMMY_SENSOR_OK(ec) ? EZO_SUCCESS : EZO_ERROR;
}

int ec_get_k(ezo_sensor_t *ec, float *k)
{
  if (!DUMMY_SENSOR_OK(ec) || k == NULL)
    return EZO_ERROR;

  *k = 1.0f;
  return EZO_SUCCESS;
}

int ec_set_tds_factor(ezo_sensor_t *ec, float factor)
{
  (void)factor;
  return DUMMY_SENSOR_OK(ec) ? EZO_SUCCESS : EZO_ERROR;
}

int ec_get_tds_factor(ezo_sensor_t *ec, float *factor)
{
  if (!DUMMY_SENSOR_OK(ec) || factor == NULL)
    return EZO_ERROR;

  *factor = 0.54f;
  return EZO_SUCCESS;
}

int ec_calibrate_dry(ezo_sensor_t *ec)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC dry calibration — OK\n");
  return EZO_SUCCESS;
}

int ec_calibrate(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_calibrate_low(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC low calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_calibrate_high(ezo_sensor_t *ec, float conductivity)
{
  if (!DUMMY_SENSOR_OK(ec)) return EZO_ERROR;
  printf("[DUMMY] EC high calibration at %.2f µS/cm — OK\n", conductivity);
  return EZO_SUCCESS;
}

int ec_get_cal_status(ezo_sensor_t *ec, ezo_cal_status_t *cal) { return ezo_get_cal_status(ec, cal); }
int ec_clear_calibration(ezo_sensor_t *ec)                     { return ezo_clear_calibration(ec); }
int ec_get_info(ezo_sensor_t *ec, char *info, int len)         { return ezo_get_info(ec, info, len); }
int ec_get_status(ezo_sensor_t *ec, char *status, int len)     { return ezo_get_status(ec, status, len); }
int ec_sleep(ezo_sensor_t *ec)                                 { return ezo_sleep(ec); }




/* ═══════════════════════════ DUMMY_SENSORS ═══════════════════════════════
 *
 * Simulates the device, per address:
 *   - dispensed volume ramps with elapsed time (not a jump at the end)
 *   - continuous modes run until pump_stop(); stop keeps the PARTIAL volume
 *   - pause freezes the volume, resume continues it (idempotent, like the real wrapper)
 *   - total volume accumulates; Clear resets it; absolute total never resets
 *   - rates outside 0.5 .. DUMMY_PUMP_MAX_ML_PER_MIN return EZO_SYNTAX_ERR
 *   - pump_set_i2c_address() moves the simulated pump to the new address
 * Goes through i2c_lock_addr(), so lock behaviour is exercised without hardware.
 * Optional: -DDUMMY_PUMP_LATENCY_MS=300 adds per-command delay (held under the
 * address lock, like a real transaction).                                      */
 
#define DUMMY_PUMP_MAX_ML_PER_MIN    60.0f   /* "calibrated max": rate for D,<ml> and D,* (1 ml/s) */
#define DUMMY_PUMP_MIN_ML_PER_MIN    0.5f
#define DUMMY_PUMP_VOLTS             12.0f
#ifndef DUMMY_PUMP_LATENCY_MS
#define DUMMY_PUMP_LATENCY_MS        0
#endif
 
typedef struct {
  int       init, pumping, paused, continuous, cal_status;
  float     requested_ml;       /* what D,? reports */
  float     rate_ml_min;
  float     dispensed_ml;       /* what R reports */
  float     total_ml;           /* TV,? */
  float     absolute_total_ml;  /* ATV,? */
  long long last_ms;
} dp_t;
 
static dp_t            _dp[128];
static pthread_mutex_t _dp_mutex = PTHREAD_MUTEX_INITIALIZER;
 
static long long dp_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
 
static void dp_update(dp_t *d)           /* caller holds _dp_mutex */
{
  long long now = dp_now_ms();
 
  if (d->pumping && !d->paused) {
    float add = d->rate_ml_min * (float)(now - d->last_ms) / 60000.0f;
    if (!d->continuous && d->dispensed_ml + add >= d->requested_ml) {
      add = d->requested_ml - d->dispensed_ml;
      d->pumping = 0;
    }
    d->dispensed_ml      += add;
    d->total_ml          += add;
    d->absolute_total_ml += add;
  }
  d->last_ms = now;
}
 
/* Lock order: per-address I2C lock, then _dp_mutex.  Latency is spent under the
 * address lock only, so different pumps still run in parallel.  NULL on failure. */
static dp_t *dp_enter(int addr)
{
  if (!i2c_addr_valid(addr)) return NULL;
  if (i2c_lock_addr(addr) != 0) return NULL;
  if (DUMMY_PUMP_LATENCY_MS > 0) usleep(DUMMY_PUMP_LATENCY_MS * 1000);
 
  pthread_mutex_lock(&_dp_mutex);
  dp_t *d = &_dp[addr];
  if (!d->init) {
    memset(d, 0, sizeof(*d));
    d->init = 1;
    d->cal_status = 3;                   /* dummy ships calibrated */
    d->last_ms = dp_now_ms();
  }
  dp_update(d);
  return d;
}
 
static void dp_leave(int addr)
{
  pthread_mutex_unlock(&_dp_mutex);
  i2c_unlock_addr(addr);
}
 
/* Start a new dose, replacing any running one.  VERIFY: device behaviour for a
 * D command while dosing.                                                     */
static void dp_begin(dp_t *d, float requested_ml, float rate, int continuous)
{
  d->requested_ml = continuous ? 0.0f : requested_ml;
  d->rate_ml_min  = rate;
  d->continuous   = continuous;
  d->dispensed_ml = 0.0f;
  d->paused       = 0;
  d->pumping      = 1;
}
 
static int dp_rate_ok(float rate)
{
  return rate >= DUMMY_PUMP_MIN_ML_PER_MIN && rate <= DUMMY_PUMP_MAX_ML_PER_MIN;
}
 
/* Args are validated by the caller; this locks and starts.  Returns the EZO status. */
static int dp_start(int addr, float requested_ml, float rate, int continuous, const char *what)
{
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  dp_begin(d, requested_ml, rate, continuous);
  printf("[DUMMY] 0x%02X %s\n", addr, what);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- start a dose ------------------------------------------------------ */
 
int pump_dose_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "dose %.2f ml", ml);
  return dp_start(addr, ml, DUMMY_PUMP_MAX_ML_PER_MIN, 0, what);
}
 
int pump_dose_volume_over_time(ezo_sensor_t *pump, float ml, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml > 0.0f) || minutes < 1) return EZO_SYNTAX_ERR;
  float rate = ml / (float)minutes;
  if (!dp_rate_ok(rate)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "dose %.2f ml over %d min", ml, minutes);
  return dp_start(addr, ml, rate, 0, what);
}
 
int pump_dose_rate(ezo_sensor_t *pump, float ml_per_min, int minutes)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml_per_min > 0.0f) || minutes < 1 || !dp_rate_ok(ml_per_min)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "rate %.2f ml/min for %d min", ml_per_min, minutes);
  return dp_start(addr, ml_per_min * (float)minutes, ml_per_min, 0, what);   /* VERIFY: D,? volume */
}
 
int pump_dose_rate_continuous(ezo_sensor_t *pump, float ml_per_min)
{
  int addr = ezo_sensor_addr(pump);
 
  char what[48];
  if (!(ml_per_min > 0.0f) || !dp_rate_ok(ml_per_min)) return EZO_SYNTAX_ERR;
  snprintf(what, sizeof(what), "rate %.2f ml/min continuous", ml_per_min);
  return dp_start(addr, 0.0f, ml_per_min, 1, what);
}
 
int pump_dose_max_continuous(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  return dp_start(addr, 0.0f, DUMMY_PUMP_MAX_ML_PER_MIN, 1, "continuous at max rate");
}
 
/* ---- control ----------------------------------------------------------- */
 
int pump_stop(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->pumping = 0;                        /* dispensed_ml keeps the partial volume */
  d->paused  = 0;
  printf("[DUMMY] 0x%02X stop (%.2f ml dispensed)\n", addr, d->dispensed_ml);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
static int pump_set_paused(int addr, int want_paused)
{
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->paused = want_paused ? 1 : 0;
  printf("[DUMMY] 0x%02X %s\n", addr, want_paused ? "pause" : "resume");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_pause(ezo_sensor_t *pump)  { return pump_set_paused(ezo_sensor_addr(pump), 1); }
int pump_resume(ezo_sensor_t *pump) { return pump_set_paused(ezo_sensor_addr(pump), 0); }
 
int pump_is_paused(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return -1;
  int p = d->paused;
  dp_leave(addr);
  return p;
}
 
/* ---- query ------------------------------------------------------------- */
 
pump_dose_status_t pump_get_dose_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  pump_dose_status_t r = { 0.0f, 0, EZO_ERROR };
  dp_t *d = dp_enter(addr);
  if (!d) return r;
  r.requested_volume_ml = d->requested_ml;
  r.is_pumping = (d->pumping && !d->paused) ? 1 : 0;      /* VERIFY: flag while paused */
  r.status = EZO_SUCCESS;
  dp_leave(addr);
  return r;
}
 
static float dp_read(int addr, size_t offset)
{
  dp_t *d = dp_enter(addr);
  if (!d) return -1.0f;
  float v = *(float *)((char *)d + offset);
  dp_leave(addr);
  return v;
}
 
float pump_get_dispensed_volume(ezo_sensor_t *pump)      { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, dispensed_ml)); }
float pump_get_total_volume(ezo_sensor_t *pump)          { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, total_ml)); }
float pump_get_absolute_total_volume(ezo_sensor_t *pump) { return dp_read(ezo_sensor_addr(pump), offsetof(dp_t, absolute_total_ml)); }
float pump_get_max_flow_rate(ezo_sensor_t *pump)         { return i2c_addr_valid(ezo_sensor_addr(pump)) ? DUMMY_PUMP_MAX_ML_PER_MIN : -1.0f; }
float pump_get_voltage(ezo_sensor_t *pump)               { return i2c_addr_valid(ezo_sensor_addr(pump)) ? DUMMY_PUMP_VOLTS : -1.0f; }
 
int pump_clear_total_volume(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->total_ml = 0.0f;                    /* absolute total is never cleared */
  printf("[DUMMY] 0x%02X clear total volume\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- calibration ------------------------------------------------------- */
 
int pump_get_calibration_status(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return -1;
  int c = d->cal_status;
  dp_leave(addr);
  return c;
}
 
/* The dummy only approximates which calibration bit a procedure sets. */
int pump_set_calibration_volume(ezo_sensor_t *pump, float ml)
{
  int addr = ezo_sensor_addr(pump);
 
  if (!(ml > 0.0f)) return EZO_SYNTAX_ERR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->cal_status |= 1;
  printf("[DUMMY] 0x%02X calibrate %.2f ml\n", addr, ml);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_clear_calibration(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  d->cal_status = 0;
  printf("[DUMMY] 0x%02X calibration cleared\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
/* ---- device ------------------------------------------------------------ */
 
int pump_find(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  printf("[DUMMY] 0x%02X find (LED blink)\n", addr);
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_set_i2c_address(ezo_sensor_t *pump, int new_addr)
{
  int addr = ezo_sensor_addr(pump);
 
  if (!i2c_addr_valid(addr) || !i2c_addr_valid(new_addr)) return EZO_SYNTAX_ERR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
 
  int rc;
  if (new_addr == addr)        rc = EZO_SUCCESS;
  else if (_dp[new_addr].init) rc = EZO_ERROR;           /* occupied, like the probe */
  else {
    _dp[new_addr] = *d;                                  /* the pump "moves" */
    memset(d, 0, sizeof(*d));
    printf("[DUMMY] 0x%02X -> 0x%02X\n", addr, new_addr);
    pump->address = (unsigned char)new_addr;             /* same as the real function */
    rc = EZO_SUCCESS;
  }
  dp_leave(addr);
  return rc;
}
 
int pump_get_info(ezo_sensor_t *pump, char *info, int len)
{
  int addr = ezo_sensor_addr(pump);
 
  if (info == NULL || len <= 0) return EZO_ERROR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  snprintf(info, len, "?I,PMP,DUMMY");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_get_device_status(ezo_sensor_t *pump, char *status, int len)
{
  int addr = ezo_sensor_addr(pump);
 
  if (status == NULL || len <= 0) return EZO_ERROR;
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  snprintf(status, len, "?STATUS,P,5.00");
  dp_leave(addr);
  return EZO_SUCCESS;
}
 
int pump_sleep(ezo_sensor_t *pump)
{
  int addr = ezo_sensor_addr(pump);
 
  dp_t *d = dp_enter(addr);
  if (!d) return EZO_ERROR;
  dp_leave(addr);
  return EZO_SUCCESS;
}



#endif // DUMMY_SENSORS



#endif // OLD_CODE

#endif