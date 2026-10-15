#ifndef UTILS_H_
#define UTILS_H_

#include <syslog.h>
#include <strings.h>
#include <ctype.h>
//#include <stdbool.h>
//#include <stdint.h>
//#include <time.h>
#include "acd_types.h"


void LOG(const int msg_level, const char * format, ...);
void FORCE_LOG(const int msg_level, const char * format, ...);
void LOG_SYSTEM_ERR(int errnum, const char *format, ...);
void LOG_PUMP_EVENT(acd_key_t *key, uint32_t seconds, float reading, float ml);
void LOG_TANK_LEVEL_SET_EVENT(acd_key_t *key);
void LOG_TANK_LEVEL(acd_key_t *key);
bool READ_LAST_TANK_LEVEL_EVENT(acd_key_t *key);
void LOG_STARTUP_EVENT();

#define DIAG_LOG(usesyslog, fmt, ...) \
  do { \
    if (usesyslog) LOG(LOG_NOTICE, fmt, ##__VA_ARGS__); \
    else            printf(fmt, ##__VA_ARGS__); \
  } while (0)

bool is_running_under_systemd();
void init_logging_backend();

void set_loglevel( int level);
int get_loglevel();

char *cleanwhitespace(char *str);

bool parse_bool(const char *str);
const char *bool_to_str(bool val);

gpio_active_t parse_gpio_active(char *str);
const char *gpio_active_to_str(gpio_active_t val);

gpio_req_t parse_gpio_req(char *str);
const char *gpio_req_to_str(gpio_req_t val);

uint8_t parse_pump_type(char *str);
const char *pump_type_to_str(uint8_t val);

acd_type_t parse_gpio_in_out(char *type);
const char *gpio_in_out_to_str(acd_type_t type);

gpio_dir_t parse_gpio_direction(char *type);
const char *gpio_direction_to_str(gpio_dir_t direction);

acd_scope_t parse_acd_scope(const char *str);
const char* acd_scope_to_str( acd_scope_t scope);
const char* acd_action_to_str( acd_scope_t scope); // Opposite of above


acd_operator_t parse_operator(const char *str);
const char *operator_to_str(acd_operator_t val);
bool operator_eval(acd_operator_t op, double lhs, double rhs);


/*
uint8_t parse_statistics(const char *str);
const char *statistics_to_str(uint8_t val);
const char *time_range_to_str(uint8_t val);
*/
/*
bool text2bool(char *str);
char *bool2text(bool val);
char *gpioactive2text(gpio_active_t val);
gpio_active_t text2gpioactive(char *str);
*/

float temp_c_to_f(float celsius);
float temp_f_to_c(float fahrenheit);
float temp_c_to_k(float celsius);

#define STR_FULL_LENGTH  -1. // For function strncasefind(), cleanalloc(), etc.
const char *strncasestr(const char *haystack, const char *needle, int length);
char *cleanalloc(char *str, int length);
char *strcsub(char *dst, int dst_len, const char *src, char find, char replace);
int strtrimcasecmp(const char *haystack, const char *needle);
void precise_delay(long nanoseconds);

const char* acd_state_to_str(acd_state_t state);
const char* acd_state_to_set_attrib(acd_state_t status);
const char* acd_condition_met_to_str(bool met);

// Monotonic clock in nanoseconds. Unaffected by wall-clock changes (NTP, DST,
// manual set), so safe for measuring elapsed intervals.
uint64_t mono_now_ns(void);

#define round(x) ((x)>=0?(long)((x)+0.5):(long)((x)-0.5))  
//#define roundf(a) (float) ((a*100)/100) // 2 decimal places
//#define roundf3(a) (float) ((a*1000)/1000) // 3 decimal places


#ifdef DUMMY_SENSORS
float dummy_drift(float range);
#endif

#define STR_MATCH(s1, s2) (strtrimcasecmp(s1, s2) == 0)

#define isMASKSET(bitmask, mask) (((bitmask) & (mask)) == (mask))
#define setMASK(bitmask, mask)    ((bitmask) |= (mask))
#define removeMASK(bitmask, mask) ((bitmask) &= ~(mask))

#define ACD_MAX(x, y) (((x) > (y)) ? (x) : (y))
#define ACD_MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ACD_CLAMP(value, min_val, max_val) ((value) > (max_val) ? (max_val) : ((value) < (min_val) ? (min_val) : (value)))


/*
#define LOG_EMERG       0       // system is unusable 
#define LOG_ALERT       1       // action must be taken immediately 
#define LOG_CRIT        2       // critical conditions 
#define LOG_ERR         3       // error conditions
#define LOG_WARNING     4       // warning conditions 
#define LOG_NOTICE      5       // normal but significant condition 
#define LOG_INFO        6       // informational 
#define LOG_DEBUG       7       // debug-level messages 
*/


static inline const char* log_priority_to_str(int level) {
    switch (level) {
        case LOG_ERR:     return "Error";
        case LOG_WARNING: return "Warning";
        case LOG_NOTICE:  return "Notice";
        case LOG_INFO:    return "Info";
        case LOG_DEBUG:   return "Debug";
        default:          return "Unknown";
    }
}

static inline int log_str_to_priority(const char *name) {
    if (!name) return -1; // Safety check

    if (strcasecmp(name, "Error") == 0)   return LOG_ERR;
    if (strcasecmp(name, "Warning") == 0) return LOG_WARNING;
    if (strcasecmp(name, "Notice") == 0)  return LOG_NOTICE;
    if (strcasecmp(name, "Info") == 0)    return LOG_INFO;
    if (strcasecmp(name, "Debug") == 0)   return LOG_DEBUG;

    return -1; // Or return a default like LOG_DEBUG
}


#endif // UTILS_H_