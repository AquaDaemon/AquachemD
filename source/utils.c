

#define _POSIX_C_SOURCE 200809L // clock_nanosleep()
#define SYSLOG_NAMES
#include <stdlib.h>
#include <syslog.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef USE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "utils.h"
#include "config.h"
#include "acd_types.h"
#include "gpio.h"
#include "version.h"


#define LOG_BUFFER_SIZE 1024


static bool _enable_journal = false;
static bool _enable_stdout  = true; // Default to true for safety

bool is_running_under_systemd() {
    // INVOCATION_ID is the modern standard for systemd services
    if (getenv("INVOCATION_ID") != NULL) {
        return true;
    }
    return false;
}

void init_logging_backend() {
#ifdef USE_SYSTEMD
    // Do we have a journal stream from systemd?
    if (getenv("JOURNAL_STREAM")) {
        _enable_journal = true;
        _enable_stdout  = false; // Systemd handles stdout usually
    }

    // Going to enable Journal even if runing from command line.
    _enable_journal = true;
#endif
   
#ifdef DUMMY_SENSORS
    _enable_journal = true; 
    _enable_stdout  = true; // We want both for testing
#endif
}




static void _do_log(const int msg_level, const char *format, va_list args) {
    va_list args_copy;

#ifdef USE_SYSTEMD
    if (_enable_journal) {
        va_copy(args_copy, args);
        sd_journal_printv(msg_level, format, args_copy);
        va_end(args_copy);
    }
#endif

    if (!_enable_stdout) return;

    char message[LOG_BUFFER_SIZE];
    va_copy(args_copy, args);
    int len = vsnprintf(message, sizeof(message), format, args_copy);
    va_end(args_copy);

    while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r' || message[len - 1] == ' ')) {
        message[--len] = '\0';
    }

    printf("%-8.8s: %s\n", log_priority_to_str(msg_level), message);
    fflush(stdout);
}

//  "V" Wrapper (The workhorse)
void VLOG(const int msg_level, const char *format, va_list args) {
    if (msg_level <= _acdconfig_.log_level) {
        _do_log(msg_level, format, args);
    }
}

// Updated LOG (Standard wrapper)
void LOG(const int msg_level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    VLOG(msg_level, format, args);
    va_end(args);
}

/* Always prints, bypassing _acdconfig_.log_level */
void FORCE_LOG(const int msg_level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    // We skip VLOG (which checks levels) and go straight to the implementation
    _do_log(msg_level, format, args);
    
    va_end(args);
}

/**
 * LOG_SYSTEM_ERR
 * Combines a custom user message with the system's string for a specific error number.
 * Usage: LOG_SYSTEM_ERR(errno, "Failed to bind to port %d", port);
 */
void LOG_SYSTEM_ERR(int errnum, const char *format, ...) {
    char user_msg[256];
    char combined_msg[512];
    
    // 1. Format the user's context
    va_list args;
    va_start(args, format);
    vsnprintf(user_msg, sizeof(user_msg), format, args);
    va_end(args);

    // 2. Append the system error string (strerror)
    // Using %s for the whole thing inside LOG() prevents double-formatting bugs
    snprintf(combined_msg, sizeof(combined_msg), "%s: %s (%d)", 
             user_msg, strerror(errnum), errnum);

    // 3. Send to the core logger as a simple string
    LOG(LOG_ERR, "%s", combined_msg);
}



void LOG_STARTUP_EVENT()
{
  FORCE_LOG(LOG_NOTICE, "Starting %s (%s) v%s\n", AQUACHEMD_NAME, AQUACHEMD_SHORT_NAME, AQUACHEMD_VERSION);

  sd_journal_send("MESSAGE=Starting %s (%s) v%s", AQUACHEMD_NAME, AQUACHEMD_SHORT_NAME, AQUACHEMD_VERSION,
                "PRIORITY=%i", LOG_NOTICE,
                "MESSAGE_ID=%s", SD_MESSAGE_STARTUP_ID,
                NULL);
}

/*************** 
 * Increase the verboseness of what's recorded.
 * 
DOSE DECISION
Pump: Acid
Sensor: pH
Reading: 7.72
Average: 7.70
Target: 7.70
Table range: >= 7.70
Requested: 10 sec
Maximum: 60 sec
Flow rate: 2.18 ml/sec
Estimated dose: 21.8 ml

Safety:
  Filter pump: OK
  Flow cell: OK
  Tank: OK
  Global interlock: OK

Decision: ALLOWED

-----

DOSE SKIPPED
Pump: Acid
Reason: FILTER_PUMP_NOT_RUNNING

-----

DOSE SKIPPED
Reason: MAX_PERIOD_VOLUME_REACHED
Current: 482 ml
Limit: 500 ml

*/


void LOG_PUMP_EVENT(acd_key_t *key, uint32_t seconds, float reading, float ml) 
{
  // key->value = value of sensor when pump started (ie ph or orp)
  // Always do a standard human-readable log
  LOG(LOG_NOTICE, "PUMP_EVENT: %s ran for %us (Value: %.2f) estimated %.2fml", key->ID, seconds, reading, ml);

#ifdef USE_SYSTEMD
  if (_enable_journal) {
    const char *chem_type = (key->flags & PH_PUMP)  ? "ACID" : 
                            (key->flags & ORP_PUMP) ? "CHLORINE" :
                            (key->flags & H2O_PUMP) ? "WATER" : "UNKNOWN";

    sd_journal_send("MESSAGE=Pump Dosing Event",
                    "MESSAGE_ID=%s", SD_PUMP_EVENT_ID,
                    "APP_EVENT=ACD-PMP-Event",
                    "PUMP_ID=%s", key->ID,
                    "PUMP_NAME=%s", key->label,
                    "PUMP_TYPE=%s", chem_type,
                    "RUNTIME_SEC=%u", seconds,
                    "DOSE_ML=%.2f", ml,
                    "SENSOR_VAL=%.2f", reading,
                    "PRIORITY=%i", LOG_INFO,
                    NULL);
  }
#endif
}

void LOG_TANK_LEVEL_SET_EVENT(acd_key_t *key)
{
    LOG(LOG_NOTICE, "Tank: %s level set to %.1f%%", key->label, key->data.tank.percent_full);

#ifdef USE_SYSTEMD
  if (_enable_journal) {
    const char *chem_type = (key->flags & PH_PUMP)  ? "ACID" : 
                            (key->flags & ORP_PUMP) ? "CHLORINE" :
                            (key->flags & H2O_PUMP) ? "WATER" : "UNKNOWN";

    sd_journal_send("MESSAGE=Tank Level Event",
                    "MESSAGE_ID=%s", SD_TANK_LEVEL_SET_EVENT_ID,
                    "APP_EVENT=ACD-TANK-LEVEL-Event",
                    "TANK_ID=%s", key->ID,
                    "TANK_NAME=%s", key->label,
                    "TANK_TYPE=%s", chem_type,
                    "LEVEL_PERCENT=%f", key->data.tank.percent_full,
                    "LEVEL_VOLUME=%f", key->data.tank.remaining_volume,
                    NULL);
  }
#endif
}

void LOG_TANK_LEVEL(acd_key_t *key)
{
  // For the moment just use the same as LOG_TANK_LEVEL_SET_EVENT
  LOG_TANK_LEVEL_SET_EVENT(key);
}


// sd_journal_get_data() returns "field=value" as a raw, length-delimited
// blob -- it is NOT guaranteed to be null-terminated, so treating it as a
// C string directly (e.g. via sscanf on it in place) is unsafe. Copy the
// value portion into a small local buffer and null-terminate it first.
static bool journal_get_float_field(sd_journal *j, const char *field, float *out)
{
  const void *data;
  size_t length;

  if (sd_journal_get_data(j, field, &data, &length) < 0) return false;

  size_t prefix_len = strlen(field) + 1;   // "field="
  if (length <= prefix_len) return false;

  char buf[64];
  size_t value_len = length - prefix_len;
  if (value_len >= sizeof(buf)) value_len = sizeof(buf) - 1;
  memcpy(buf, (const char *)data + prefix_len, value_len);
  buf[value_len] = '\0';

  char *endptr;
  float val = strtof(buf, &endptr);
  if (endptr == buf) return false;   // no digits parsed at all

  *out = val;
  return true;
}

bool READ_LAST_TANK_LEVEL_EVENT(acd_key_t *key)
{
#ifndef USE_SYSTEMD
  (void)key;
  return false;
#else
  if (!key) return false;
  if (!_enable_journal) return false;

  sd_journal *j = NULL;
  int ret = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
  if (ret < 0) {
    LOG(LOG_ERR, "READ_LAST_TANK_LEVEL_EVENT: failed to open journal: %s", strerror(-ret));
    return false;
  }

  char match_id[128];
  snprintf(match_id, sizeof(match_id), "MESSAGE_ID=%s", SD_TANK_LEVEL_SET_EVENT_ID);
  sd_journal_add_match(j, match_id, 0);

  char match_tank[256];
  snprintf(match_tank, sizeof(match_tank), "TANK_ID=%s", key->ID);
  sd_journal_add_match(j, match_tank, 0);
  // Two add_match calls on different field names AND together -- this
  // finds the intersection (this event type, for this specific tank).

  sd_journal_seek_tail(j);
  if (sd_journal_previous(j) <= 0) {
    LOG(LOG_NOTICE, "READ_LAST_TANK_LEVEL_EVENT: no prior tank level event found for %s", key->ID);
    sd_journal_close(j);
    return false;
  }

  float percent_full = 0.0f, remaining = 0.0f;
  bool got_level    = journal_get_float_field(j, "LEVEL_PERCENT", &percent_full);
  bool got_level_ml = journal_get_float_field(j, "LEVEL_VOLUME", &remaining);

  sd_journal_close(j);

  if (!got_level || !got_level_ml) {
    LOG(LOG_ERR, "READ_LAST_TANK_LEVEL_EVENT: found event for %s but couldn't parse level fields", key->ID);
    return false;
  }

  key->data.tank.percent_full = percent_full;
  key->data.tank.remaining_volume = remaining;
  key->value    = percent_full;
  key->is_dirty = true;

  LOG(LOG_DEBUG, "READ_LAST_TANK_LEVEL_EVENT: restored %s to %.1f%% (%.1f %s) from journal",
      key->label, percent_full, remaining, uom_to_display_str(key->data.tank.uom));

  return true;
#endif
}

void set_loglevel( int level)
{
  _acdconfig_.log_level = level;
}
int get_loglevel()
{
  return _acdconfig_.log_level;
}


//Move existing pointer
char *cleanwhitespace(char *str)
{
  char *end;

  if (str == NULL)
    return str;

  // Trim leading space
  while(isspace(*str)) str++;

  if(*str == 0)  // All spaces?
    return str;

  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && isspace(*end)) end--;

  // Write new null terminator
  if (end != (str + strlen(str) - 1) )
    *(end+1) = 0;

  return str;
}

// Return a new string with whitespace trimmed and memory allocated for it. Caller must free.
// str: input string to clean and allocate
// length: if STR_FULL_LENGTH, allocate for entire cleaned string; otherwise allocate for specified length
char *cleanalloc(char *str, int length)
{
  if (str == NULL)
    return NULL;

  str = cleanwhitespace(str);

  size_t str_len = strlen(str);
  char  *result;

  if (length == STR_FULL_LENGTH || length <= 0)
  {
    // Allocate for string + null terminator (already present but be explicit)
    result = (char *)malloc(str_len + 1);
    if (!result) return NULL;
    strcpy(result, str);
  }
  else
  {
    result = (char *)malloc(length + 1);   // +1 to guarantee room for null
    if (!result) return NULL;
    strncpy(result, str, length);
    result[length] = '\0';                 // ensure null terminated
  }

  // Guarantee null termination — fastest check is last byte
  if (result[str_len] != '\0')
    result[str_len] = '\0';

  return result;
}

// Search for needle in haystack, case-insensitive, length-limited.
// length: max bytes of haystack to search, or STR_FULL_LENGTH for full string.
// Returns pointer to first match in haystack, or NULL if not found / error.
const char *strncasestr(const char *haystack, const char *needle, int length)
{
  // Null guard
  if (!haystack || !needle) return NULL;

  // Empty needle matches at start (consistent with strstr behaviour)
  if (!*needle) return haystack;

  // Determine actual search window
  size_t haystack_len = (length == STR_FULL_LENGTH) ? strlen(haystack) : (size_t)length;
  size_t needle_len   = strlen(needle);

  // Can't match if needle is longer than the search window
  if (needle_len > haystack_len) return NULL;

  // Pre-compute first character of needle in both cases to avoid
  // tolower() on every byte — we only call strncasecmp on a first-char hit
  unsigned char first_lo = (unsigned char)tolower((unsigned char)needle[0]);
  unsigned char first_hi = (unsigned char)toupper((unsigned char)needle[0]);

  // Last position in haystack where a match could start
  const char *end = haystack + haystack_len - needle_len;

  for (const char *h = haystack; h <= end; h++)
  {
    unsigned char c = (unsigned char)*h;

    // Fast first-character filter
    if (c != first_lo && c != first_hi)
      continue;

    // First char matched — check the rest
    if (strncasecmp(h, needle, needle_len) == 0)
      return h;
  }

  return NULL;
}

/**
 * strtrimcasecmp: Perfectly matches two strings, ignoring case and 
 * surrounding whitespace. Returns 0 on success, -1 on mismatch.
 */
int strtrimcasecmp(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return -1;

    // Skip leading whitespace
    while (isspace((unsigned char)*haystack)) haystack++;
    while (isspace((unsigned char)*needle)) needle++;

    // Find ends to calculate trimmed length
    const char *h_end = haystack + strlen(haystack) - 1;
    const char *n_end = needle + strlen(needle) - 1;

    // Move backwards to skip trailing whitespace
    while (h_end > haystack && isspace((unsigned char)*h_end)) h_end--;
    while (n_end > needle && isspace((unsigned char)*n_end)) n_end--;

    // Calculate effective lengths
    size_t h_len = (h_end >= haystack) ? (size_t)(h_end - haystack + 1) : 0;
    size_t n_len = (n_end >= needle) ? (size_t)(n_end - needle + 1) : 0;

    // Must be same length and not empty for a perfect match
    if (h_len != n_len || h_len == 0)
        return -1;

    // Compare actual content
    return strncasecmp(haystack, needle, h_len);
}

// Replace all occurrences of 'find' with 'replace' in 'src', copying to 'dst'.
// dst: destination buffer to write result (must be pre-allocated by caller)
// dst_len is the total size of the destination buffer, including space for null terminator.
// src: input string to process
// find: character to replace, use '-', not "-", to avoid confusion with string literals
// replace: character to replace with, use ' '
// Returns dst on success

char *strcsub(char *dst, int dst_len, const char *src, char find, char replace) 
{
  int i;
  int len = strlen(src);

  // Standard safety: Ensure we don't overflow the destination buffer
  if (dst_len > 0) {
    len = ACD_MIN(len, dst_len - 1); // -1 to leave room for \0
  }

  for (i = 0; i < len; i++) {
    dst[i] = (src[i] == find) ? replace : src[i];
  }
  dst[i] = '\0';

  return dst;
}


bool parse_bool(const char *str)
{
    if (!str) return false;
    
    // If your cleanwhitespace() function modifies the string inline, 
    // you can cast or pass a mutable copy, otherwise standard const char* works.
    char *clean = cleanwhitespace((char *)str);
    
    if (strcasecmp(clean, "YES") == 0 || 
        strcasecmp(clean, "ON") == 0 || 
        strcasecmp(clean, "TRUE") == 0 || 
        strcasecmp(clean, "1") == 0 ||
        strcasecmp(clean, "ENABLE") == 0) 
    {
        return true;
    }
    
    return false;
}

const char *bool_to_str(bool val)
{
    return val ? "YES" : "NO";
}

gpio_active_t parse_gpio_active(char *str)
{
    str = cleanwhitespace(str);
    if (strcasecmp(str, "ACTIVE_LOW") == 0 || strcasecmp(str, "ACTIVE LOW") == 0) {
        return GPIO_ACTIVE_LOW;
    }
    // Explicitly check for High so typos don't accidentally invert your hardware logic
    return GPIO_ACTIVE_HIGH; 
}

const char *gpio_active_to_str(gpio_active_t val)
{
    return (val == GPIO_ACTIVE_HIGH) ? "Active High" : "Active Low";
}

gpio_req_t parse_gpio_req(char *str)
{
    str = cleanwhitespace(str);
    if (strcasecmp(str, "ON") == 0 || strcasecmp(str, "YES") == 0 || strcasecmp(str, "HIGH") == 0) {
        return GPIO_REQ_ON;
    }
    return GPIO_REQ_OFF;
}

const char *gpio_req_to_str(gpio_req_t val)
{
    return (val == GPIO_REQ_OFF) ? "off" : "on";
}

// Convert Celsius to Fahrenheit
float temp_c_to_f(float celsius)
{
  return (celsius * 1.8 + 32);
}

// Convert Fahrenheit to Celsius
float temp_f_to_c(float fahrenheit)
{
  return ((fahrenheit-32) / 1.8);
}

// Convert Celsius to Kelvin
float temp_c_to_k(float celsius)
{
  return celsius + 273.15;
}

uint8_t parse_pump_type(char *str)
{
  str = cleanwhitespace(str);
  if (strcasecmp(str, "ph") == 0 || strcasecmp(str, "acid") == 0) {
    return PH_PUMP;
  } else if (strcasecmp(str, "orp") == 0 || strcasecmp(str, "chlorine") == 0) {
    return ORP_PUMP;
  } else if (strcasecmp(str, "h2o") == 0 || strcasecmp(str, "water") == 0) {
    return H2O_PUMP;
  }

  return PH_PUMP;
}

const char *pump_type_to_str(uint8_t val)
{
  //return (val == PH_PUMP) ? "pH" : "ORP";
  //return (val & PH_PUMP) ? "pH" : "ORP";

  if (isMASKSET(val, PH_PUMP))
    return "pH";
  else if (isMASKSET(val, ORP_PUMP))
    return "ORP";
  else if (isMASKSET(val, H2O_PUMP))
    return "H2O";

  return "";
}


acd_type_t parse_gpio_in_out(char *type)
{
  if (strcasecmp(type, "output") == 0 || strcasecmp(type, "out") == 0) {
    return ACD_TYPE_GPIO_OUTPUT;
  }
  return ACD_TYPE_GPIO_INPUT;
}

const char *gpio_in_out_to_str(acd_type_t type)
{
  if (type == ACD_TYPE_GPIO_INPUT)
    return "input";
  else if (type == ACD_TYPE_GPIO_OUTPUT)
    return "output";

  return "unknown";
}

gpio_dir_t parse_gpio_direction(char *type)
{
  if (strcasecmp(type, "output") == 0 || strcasecmp(type, "out") == 0) {
    return GPIO_INPUT;
  }
  return GPIO_OUTPUT;
}
const char *gpio_direction_to_str(gpio_dir_t direction)
{
  if (direction == GPIO_INPUT)
    return "input";
  else if (direction == GPIO_OUTPUT)
    return "output";

  return "unknown";
}

/*
uint8_t parse_statistics(const char *str)
{
    if (!str) return 0x00;
    
    // If your cleanwhitespace() function modifies the string inline, 
    // you can cast or pass a mutable copy, otherwise standard const char* works.
    char *clean = cleanwhitespace((char *)str);
    
    if (strcasecmp(clean, "DAY") == 0 || 
        strcasecmp(clean, "DAILY") == 0)
    {
      return AVG_DAILY;
    } else if (strcasecmp(clean, "WEEK") == 0 || 
               strcasecmp(clean, "WEEKLY") == 0)  
    {
      return AVG_WEEKLY;
    }

    // None is also an option, but that's returns 0x00
   
    return 0x00;
}

// For config.
const char *statistics_to_str(uint8_t val)
{
  if (isMASKSET(val, AVG_DAILY)) {return "Daily";}
  if (isMASKSET(val, AVG_WEEKLY)) {return "Weekly";}
  return "None";
}

// For UI / MQTT
const char *time_range_to_str(uint8_t val)
{
  if (isMASKSET(val, AVG_DAILY)) {return "24h";}
  if (isMASKSET(val, AVG_WEEKLY)) {return "7d";} 
  return "";
}
*/


#include <time.h>

void precise_delay(long nanoseconds) {
    struct timespec ts;
    ts.tv_sec = nanoseconds / 1000000000L;
    ts.tv_nsec = nanoseconds % 1000000000L;
    
    // TIMER_ABSTIME = 0 means relative sleep
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}



const char* acd_state_to_str(acd_state_t state) {
    switch (state) {
        case ACD_LED_OFF:      return "OFF";
        case ACD_LED_ON:       return "ON";
        case ACD_LED_ENABLED:  return "ENABLED";
        case ACD_LED_DISABLED: return "DISABLED";
        case ACD_LED_DELAY:    return "DELAY";
        case ACD_LED_UNKNOWN: 
        default:               return "UNKNOWN";
    }
}

acd_scope_t parse_acd_scope(const char *str) {
    if (!str) return ACD_SCOPE_UNKNOWN;

    char *clean = cleanwhitespace((char *)str);

    if (strcasecmp(clean, "Allow") == 0) {
        return ACD_SCOPE_ALLOW;
    } else if (strcasecmp(clean, "Local") == 0) {
        return ACD_SCOPE_LOCAL;
    } else if (strcasecmp(clean, "Global") == 0) {
        return ACD_SCOPE_GLOBAL;
    } else  if (strcasecmp(clean, "None") == 0) {
        return ACD_SCOPE_ALLOW; // May need to change this and add ACD_SCOPE_NONE?????
    }
    
    return ACD_SCOPE_UNKNOWN; // Default to global if unknown
}

const char* acd_scope_to_str( acd_scope_t scope) {
  switch (scope) {
    case ACD_SCOPE_ALLOW:      return "Allow";
    case ACD_SCOPE_LOCAL:      return "Local";
    case ACD_SCOPE_GLOBAL:     return "Global";
    default:                   return "UNKNOWN";
  }
}
// Simply the opposite of above.
const char* acd_action_to_str( acd_scope_t scope) {
  switch (scope) {
    case ACD_ACTION_ALLOW:     return "Allow";
    case ACD_ACTION_LIMIT:     return "Limit";
    case ACD_ACTION_BLOCK:     return "Block";
    default:                   return "UNKNOWN";
  }
}


const char* acd_condition_met_to_str(bool met) {
  if (met)
    return "SAFE";
  
  return "UNSAFE";
}

/**
 * Converts internal status integers to "Set" command strings for the UI.
 * This tells the UI which command to send back to the controller.
 */
const char* acd_state_to_set_attrib(acd_state_t status) {
  switch (status) {
    case ACD_LED_OFF:       return "set_off";
    case ACD_LED_ON:        return "set_on";
    case ACD_LED_ENABLED:   return "set_enabled";
    case ACD_LED_DISABLED:  return "set_disabled";
    default:                return "";
  }
}



#ifdef DUMMY_SENSORS
// Small random float drift in range [-range, +range]
float dummy_drift(float range)
{
  return ((float)(rand() % 1000) / 1000.0f - 0.5f) * 2.0f * range;
}
#endif