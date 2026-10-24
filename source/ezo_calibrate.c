
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "ezo_calibrate.h"
#include "utils.h"          // LOG

#define CAL_FILE_FORMAT    1
#define CAL_FILE_LINE_MAX  256


const char* ezo_device_type_to_str(ezo_device_type_t type) 
{
  switch (type){
    case EZO_PH:  return "PH";
    case EZO_ORP: return "ORP";
    case EZO_PRS: return "PRS";
    case EZO_RTD: return "RTD";
    case EZO_EC:  return "EC";
    case EZO_PMP:  return "PMP";
    default:      return "Unknown";
  }
}

bool ezo_calibrate_parse_args(int argc, char *argv[], ezo_calibrate_t *parsed) {
    // Ensure minimum arguments: program + "calibrate" + device + action/value
    if (argc < 4) {
        return false;
    }

    if (strcmp(argv[1], "calibrate") != 0) {
        return false;
    }

    // 1. Parse Device Type (argv[2])
    if (strcmp(argv[2], "ph") == 0) {
        parsed->device = EZO_PH;
    } else if (strcmp(argv[2], "orp") == 0) {
        parsed->device = EZO_ORP;
    } else if (strcmp(argv[2], "prs") == 0) {
        parsed->device = EZO_PRS;
    } else if (strcmp(argv[2], "rtd") == 0) {
        parsed->device = EZO_RTD;
    } else if (strcmp(argv[2], "ec") == 0) {
        parsed->device = EZO_EC;
     } else if (strcmp(argv[2], "pmp") == 0) {
        parsed->device = EZO_PMP;
    } else {
        return false; // Unknown device
    }

    // 2. Parse Actions based on argv[3]
    char *arg3 = argv[3];

    if (strcmp(arg3, "clear") == 0) {
        parsed->action = EZO_ACTION_CLEAR;
    } 
    else if (strcmp(arg3, "export") == 0) {
        if (argc < 5) return false; // Missing filename
        parsed->action = EZO_ACTION_EXPORT;
        parsed->filename = argv[4];
    } 
    else if (strcmp(arg3, "import") == 0) {
        if (argc < 5) return false; // Missing filename
        parsed->action = EZO_ACTION_IMPORT;
        parsed->filename = argv[4];
    }
    else if (strcmp(arg3, "status") == 0) {
        parsed->action = EZO_ACTION_STATUS;
    } 
    else if (strcmp(arg3, "read") == 0) {
        parsed->action = EZO_ACTION_READING;
    }
    else if (strcmp(arg3, "dose") == 0) {
        parsed->action = EZO_ACTION_DOSE;
    } 
    else {
        // This is a calibration command (either option-based or float-based)
        parsed->action = EZO_ACTION_CALIBRATE;

        switch (parsed->device) {
            case EZO_PH:
            case EZO_EC:
                // Map option string to option enum
                if (strcmp(arg3, "low") == 0) {
                    parsed->option = EZO_OPT_LOW;
                } else if (strcmp(arg3, "mid") == 0) {
                    parsed->option = EZO_OPT_MID;
                } else if (strcmp(arg3, "high") == 0) {
                    parsed->option = EZO_OPT_HIGH;
                } else if (strcmp(arg3, "dry") == 0) {
                    parsed->option = EZO_OPT_DRY;
                } else {
                    return false; // Unknown option string
                }
                
                // EC takes an extra float value: e.g. ec <dry|low|high> <µS value>
                if (parsed->device == EZO_EC) {
                    if (argc < 5) return false;
                    parsed->float_value = strtof(argv[4], NULL);
                }
                break;

            case EZO_ORP:
            case EZO_PRS:
            case EZO_RTD:
            case EZO_PMP:
                // Direct float value calibration
                parsed->float_value = strtof(arg3, NULL);
                break;
        }
    }

    return true;
}


float ezo_calibrate_action(ezo_calibrate_t *cal) {

  ezo_sensor_t sensor;
  float ezo_rtn = EZO_ERROR;

  switch (cal->device){
    case EZO_PH:  sensor.address = EZO_PH_ADDR; break;
    case EZO_ORP: sensor.address = EZO_ORP_ADDR; break;
    case EZO_PRS: sensor.address = EZO_PRS_ADDR; break;
    case EZO_RTD: sensor.address = EZO_RTD_ADDR; break;
    case EZO_EC:  sensor.address = EZO_EC_ADDR; break;
    case EZO_PMP: sensor.address = EZO_EC_ADDR; break;

    default:
      LOG(LOG_ERR, "Calibrating device %s, not supported!",ezo_device_type_to_str(cal->device));
      return EZO_ERROR;
    break;
  }

  switch (cal->action) {
    case EZO_ACTION_READING:
      switch (cal->device){
        case EZO_PH: { 
          ph_reading_t reading = ph_get_reading(&sensor);
          if (reading.status == EZO_SUCCESS){LOG(LOG_NOTICE, "pH reading %.2f",reading.value);  ezo_rtn = reading.value;}
          else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);            ezo_rtn = reading.status;}
          break;
         }
        case EZO_ORP: {
          orp_reading_t reading = orp_get_reading(&sensor);
          if (reading.status == EZO_SUCCESS){LOG(LOG_NOTICE, "ORP reading %.2f",reading.value);  ezo_rtn = reading.value;}
          else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);             ezo_rtn = reading.status;}
          break;
        }
        case EZO_PRS: {
          prs_reading_t reading = prs_get_reading(&sensor);
          if (reading.status == EZO_SUCCESS){LOG(LOG_NOTICE, "PRS reading %.2f",reading.value);  ezo_rtn = reading.value;}
          else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);             ezo_rtn = reading.status;}
          break;
        }
        case EZO_RTD: {
          rtd_reading_t reading = rtd_get_reading(&sensor);
          if (reading.status == EZO_SUCCESS){LOG(LOG_NOTICE, "RTD reading %.2f",reading.value);  ezo_rtn = reading.value;}
          else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);             ezo_rtn = reading.status;}
          break;
        }
        case EZO_EC:{
          ec_reading_t reading = ec_get_reading(&sensor);
          if (reading.status == EZO_SUCCESS){LOG(LOG_NOTICE, "EC reading %.2f",reading.conductivity);  ezo_rtn = reading.conductivity;}
          else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);             ezo_rtn = reading.status;}
          break;
        }
        default:
          LOG(LOG_ERR, "Reading not supported for %s", ezo_device_type_to_str(cal->device));
        break;
      }
      break;
    case EZO_ACTION_STATUS:
      ezo_cal_status_t cal_status;
      ezo_rtn = ezo_get_cal_status(&sensor, &cal_status);
      if (ezo_rtn == EZO_SUCCESS && cal_status.points != EZO_ERROR ) {
        LOG(LOG_NOTICE, "%s calibration status = %d points",ezo_device_type_to_str(cal->device),cal_status.points);
        ezo_rtn = cal_status.points;
      } else{
        LOG(LOG_ERR, "Failed to read %s calibration status",ezo_device_type_to_str(cal->device));
      }
      break;
    case EZO_ACTION_CALIBRATE:    
      switch (cal->device){
        case EZO_PH:
          if (cal->option == EZO_OPT_LOW) {
            LOG(LOG_NOTICE,"Calibrating pH low\n");
            ezo_rtn = ph_calibrate_low(&sensor);
          } else if (cal->option == EZO_OPT_MID) {
            LOG(LOG_NOTICE,"Calibrating pH mid\n");
            ezo_rtn = ph_calibrate_mid(&sensor);
          } else if (cal->option == EZO_OPT_HIGH) {
            LOG(LOG_NOTICE,"Calibrating pH high\n");
            ezo_rtn = ph_calibrate_high(&sensor);
          } else if (cal->option == EZO_OPT_NONE) {
            LOG(LOG_NOTICE,"Calibrating pH to %.2f\n", cal->float_value);
            ezo_rtn = ph_calibrate_by_value(&sensor, cal->float_value);
          }
        break;
        case EZO_ORP:
          LOG(LOG_NOTICE,"Calibrating ORP %.2f\n", cal->float_value);
          ezo_rtn =  orp_calibrate(&sensor, cal->float_value);
        break;
        case EZO_PRS:
          LOG(LOG_NOTICE,"Calibrating PRS %.2f\n", cal->float_value);
          ezo_rtn =  prs_calibrate(&sensor, cal->float_value);
        break;
        case EZO_RTD:
          LOG(LOG_NOTICE,"Calibrating RTD %.2f\n", cal->float_value);
          ezo_rtn =  rtd_calibrate(&sensor, cal->float_value);
        break;
        case EZO_EC:
          if (cal->option == EZO_OPT_DRY) {
            LOG(LOG_NOTICE,"Calibrating EC dry\n");
            ezo_rtn =  ec_calibrate_dry(&sensor);
          } else if (cal->option == EZO_OPT_LOW) {
            LOG(LOG_NOTICE,"Calibrating EC low to %.2f\n", cal->float_value);
            ezo_rtn =  ec_calibrate_low(&sensor, cal->float_value);
          } else if (cal->option == EZO_OPT_HIGH) {
            LOG(LOG_NOTICE,"Calibrating EC high to %.2f\n", cal->float_value);
            ezo_rtn =  ec_calibrate_high(&sensor, cal->float_value);
          } else if (cal->option == EZO_OPT_NONE) {
            LOG(LOG_NOTICE,"Calibrating EC to %.2f\n", cal->float_value);
            ezo_rtn =  ec_calibrate_by_value(&sensor, cal->float_value);
          }
        break;
        case EZO_PMP:
          LOG(LOG_NOTICE,"Calibrating PMP %.2f\n", cal->float_value);
          ezo_rtn =  pump_set_calibration_volume(&sensor, cal->float_value);
        break;
        default:
          LOG(LOG_ERR, "Calibrating device type %d, not supported!",cal->device);
          ezo_rtn =  EZO_ERROR;
        break;
      }
      break;

    case EZO_ACTION_CLEAR: 
      LOG(LOG_NOTICE,"CLEAR calibration for Device %s\n", ezo_device_type_to_str(cal->device));
      switch (cal->device){
        case EZO_PH:  ezo_rtn = ph_clear_calibration(&sensor); break;
        case EZO_ORP: ezo_rtn = orp_clear_calibration(&sensor); break;
        case EZO_PRS: ezo_rtn = prs_clear_calibration(&sensor); break;
        case EZO_RTD: ezo_rtn = rtd_clear_calibration(&sensor); break;
        case EZO_EC:  ezo_rtn = ec_clear_calibration(&sensor); break;
        default:
          LOG(LOG_ERR, "Calibrating device type %s, not supported!",ezo_device_type_to_str(cal->device));
          ezo_rtn =  EZO_ERROR;
        break;
      }

      case EZO_ACTION_DOSE:
        if ( cal->device == EZO_PMP) {
          LOG(LOG_NOTICE,"Dosing 10ml from Device %s\n", ezo_device_type_to_str(cal->device));
          ezo_rtn = pump_dose_volume(&sensor, 10);
        } else {
          LOG(LOG_ERR, "Dosing for device %s not supported!", ezo_device_type_to_str(cal->device));
          ezo_rtn =  EZO_ERROR;
        }
      break;
    break;

    case EZO_ACTION_EXPORT:
      LOG(LOG_NOTICE,"Export calibration for Device %s to file: %s\n", ezo_device_type_to_str(cal->device), cal->filename);
      ezo_rtn =  ezo_save_calibration_file(&sensor, cal->filename);
      break;

    case EZO_ACTION_IMPORT:
      LOG(LOG_NOTICE,"Import calibration for Device %s from file: %s\n", ezo_device_type_to_str(cal->device), cal->filename);
      ezo_rtn =  ezo_load_calibration_file(&sensor, cal->filename);
      break;
  }

  // TAKE READING AND ezo_rtn =  THAT.
  if ( ezo_rtn == EZO_ERROR)
    return EZO_ERROR;

  // Take a new reading after any any calibration 
  if (cal->action == EZO_ACTION_CALIBRATE) {
    int orig_action = cal->action;
    cal->action = EZO_ACTION_STATUS;
    float result = ezo_calibrate_action(cal);
    cal->action = orig_action; // Set the action back to original setting
    return result;
  }
  
  return ezo_rtn;
}

static uint32_t cal_crc32(const unsigned char *p, size_t n)
{
  uint32_t crc = 0xFFFFFFFFu;
  while (n--)
  {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// "?i,EC,2.10" -> device "EC", firmware "2.10".  Case-insensitive on the "?i," so the
// dummy build's "?I,DUMMY,1.00" parses too.
static bool cal_parse_info(const char *info, char *dev, size_t dev_sz, char *fw, size_t fw_sz)
{
  if (strncasecmp(info, "?i,", 3) != 0) return false;
  const char *d = info + 3;
  const char *comma = strchr(d, ',');
  size_t dn = comma ? (size_t)(comma - d) : strlen(d);
  if (dn == 0 || dn >= dev_sz) return false;
  memcpy(dev, d, dn);
  dev[dn] = '\0';
  snprintf(fw, fw_sz, "%s", comma ? comma + 1 : "");
  return true;
}

static bool cal_text_ok(const char *s)          // header values: printable, no spaces needed
{
  if (*s == '\0') return false;
  for (; *s; s++)
    if ((unsigned char)*s < 0x21 || (unsigned char)*s > 0x7E) return false;
  return true;
}

int ezo_save_calibration_file(ezo_sensor_t *sensor, const char *filename)
{
  if (filename == NULL || *filename == '\0') return EZO_ERROR;

  // Who is this?  Without it the file could later be loaded into the wrong circuit.
  char info[64], dev[16], fw[16];
  int status = ezo_get_info(sensor, info, sizeof(info));
  if (status != EZO_SUCCESS)
  {
    LOG(LOG_ERR, "EZO calibration save: could not read the circuit's info ('i'), status %d\n", status);
    return status;
  }
  if (!cal_parse_info(info, dev, sizeof(dev), fw, sizeof(fw)) || !cal_text_ok(dev))
  {
    LOG(LOG_ERR, "EZO calibration save: unrecognised device info '%s'\n", info);
    return EZO_ERROR;
  }

  ezo_cal_data_t cal;
  status = ezo_download_calibration(sensor, &cal);
  if (status != EZO_SUCCESS) return status;

  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", filename) >= (int)sizeof(tmp)) return EZO_ERROR;

  FILE *fp = fopen(tmp, "w");
  if (fp == NULL)
  {
    LOG(LOG_ERR, "EZO calibration save: cannot write %s: %s\n", tmp, strerror(errno));
    return EZO_ERROR;
  }

  bool ok = true;
  ok = ok && fprintf(fp, "# AquachemD EZO calibration export\n") > 0;
  ok = ok && fprintf(fp, "format=%d\n", CAL_FILE_FORMAT) > 0;
  ok = ok && fprintf(fp, "device=%s\n", dev) > 0;
  ok = ok && fprintf(fp, "firmware=%s\n", fw) > 0;
  ok = ok && fprintf(fp, "chunks=%d\n", cal.chunks) > 0;
  ok = ok && fprintf(fp, "bytes=%zu\n", cal.length - (size_t)cal.chunks) > 0;   // strings only, not the '\n's
  ok = ok && fprintf(fp, "crc32=%08X\n", (unsigned)cal_crc32((const unsigned char *)cal.data, cal.length)) > 0;

  const char *p = cal.data, *end = cal.data + cal.length;
  for (int i = 1; ok && p < end; i++)
  {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    ok = ok && fprintf(fp, "%d:%.*s\n", i, (int)(nl - p), p) > 0;
    p = nl + 1;
  }

  ok = ok && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
  if (fclose(fp) != 0) ok = false;
  if (ok && rename(tmp, filename) != 0) ok = false;
  if (!ok)
  {
    LOG(LOG_ERR, "EZO calibration save: writing %s failed: %s\n", filename, strerror(errno));
    unlink(tmp);
    return EZO_ERROR;
  }

  LOG(LOG_NOTICE, "Calibration for device EZO 0x%02x saved to %s (%d strings)\n", ezo_sensor_addr(sensor), filename, cal.chunks);

  return EZO_SUCCESS;
}

int ezo_load_calibration_file(ezo_sensor_t *sensor, const char *filename)
{
  if (filename == NULL || *filename == '\0') return EZO_ERROR;

  FILE *fp = fopen(filename, "r");
  if (fp == NULL)
  {
    LOG(LOG_ERR, "EZO calibration load: cannot open %s: %s\n", filename, strerror(errno));
    return EZO_CAL_FILE_BAD;
  }

  ezo_cal_data_t cal;
  memset(&cal, 0, sizeof(cal));

  char fdev[16] = "", ffw[16] = "";
  int  format = 0, hdr_chunks = -1;
  long hdr_bytes = -1;
  bool have_crc = false;
  uint32_t file_crc = 0;

  char line[CAL_FILE_LINE_MAX];
  int lineno = 0;
  const char *why = NULL;

  while (why == NULL && fgets(line, sizeof(line), fp) != NULL)
  {
    lineno++;
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
    else if (!feof(fp)) { why = "line too long"; break; }
    if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
    if (n == 0 || line[0] == '#') continue;

    if (line[0] >= '0' && line[0] <= '9')                 // "<n>:<string>"
    {
      char *colon;
      long idx = strtol(line, &colon, 10);
      if (*colon != ':' || idx != cal.chunks + 1) { why = "bad or out-of-order string line"; break; }
      const char *text = colon + 1;
      size_t tn = strlen(text);
      if (tn == 0 || tn > EZO_CAL_CHUNK_MAX) { why = "string empty or too long"; break; }
      for (const char *q = text; *q; q++)
        if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7E) { why = "string has non-printable characters"; break; }
      if (why) break;
      if (cal.chunks >= EZO_CAL_MAX_CHUNKS || cal.length + tn + 1 >= sizeof(cal.data)) { why = "too many strings"; break; }
      memcpy(cal.data + cal.length, text, tn);
      cal.length += tn;
      cal.data[cal.length++] = '\n';
      cal.chunks++;
      continue;
    }

    char *eq = strchr(line, '=');                          // "key=value"
    if (eq == NULL) { why = "unrecognised line"; break; }
    *eq = '\0';
    const char *k = line, *v = eq + 1;
    if      (strcmp(k, "format")   == 0) format = atoi(v);
    else if (strcmp(k, "device")   == 0) { if (cal_text_ok(v) && strlen(v) < sizeof(fdev)) snprintf(fdev, sizeof(fdev), "%s", v); else why = "bad device"; }
    else if (strcmp(k, "firmware") == 0) snprintf(ffw, sizeof(ffw), "%s", v);
    else if (strcmp(k, "chunks")   == 0) hdr_chunks = atoi(v);
    else if (strcmp(k, "bytes")    == 0) hdr_bytes = atol(v);
    else if (strcmp(k, "crc32")    == 0) { file_crc = (uint32_t)strtoul(v, NULL, 16); have_crc = true; }
    else why = "unknown header key";
  }
  bool read_err = ferror(fp) != 0;
  fclose(fp);

  if (read_err) why = "read error";
  if (why == NULL && format != CAL_FILE_FORMAT)            why = "unsupported format";
  if (why == NULL && fdev[0] == '\0')                      why = "no device";
  if (why == NULL && (cal.chunks < 1 || cal.chunks != hdr_chunks)) why = "string count does not match header";
  if (why == NULL && hdr_bytes != (long)(cal.length - (size_t)cal.chunks)) why = "byte count does not match header";
  if (why == NULL && (!have_crc || file_crc != cal_crc32((const unsigned char *)cal.data, cal.length)))
                                                           why = "CRC mismatch (file damaged or edited)";
  if (why != NULL)
  {
    LOG(LOG_ERR, "EZO calibration load: %s: %s (line %d)\n", filename, why, lineno);
    return EZO_CAL_FILE_BAD;
  }

  // The file is sound.  Is it for this circuit?  Checked before anything is sent.
  char info[64], dev[16], fw[16];
  int status = ezo_get_info(sensor, info, sizeof(info));
  if (status != EZO_SUCCESS) return status;
  if (!cal_parse_info(info, dev, sizeof(dev), fw, sizeof(fw)))
  {
    LOG(LOG_ERR, "EZO calibration load: unrecognised device info '%s'\n", info);
    return EZO_ERROR;
  }
  if (strcasecmp(dev, fdev) != 0)
  {
    LOG(LOG_ERR, "EZO calibration load: %s is for a %s circuit, this one is %s - not loaded\n",
        filename, fdev, dev);
    return EZO_CAL_DEVICE_MISMATCH;
  }
  if (strcmp(fw, ffw) != 0)
    LOG(LOG_WARNING, "EZO calibration load: file is from firmware %s, circuit has %s\n", ffw, fw);

  status = ezo_upload_calibration(sensor, &cal);
  if (status == EZO_SUCCESS)
    LOG(LOG_NOTICE, "Calibration for device EZO 0x%02x restored from %s (%d strings)\n", ezo_sensor_addr(sensor), filename, cal.chunks);
  
  return status;
  
}