#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include <stdint.h>
#include "acd_types.h"

struct aquachemdata; 
struct acd_condition;

void parse_config_file(struct aquachemdata *acdata);
void free_config();
bool write_config_file (struct aquachemdata *acdata);
void print_config (struct aquachemdata *acdata);
bool build_aquachem_config_json(char *buffer, size_t buf_size);

int save_aquachem_config_json(const char* inBuf, int inSize, char* outBuf, int outSize, struct aquachemdata *acdata);

#define MAXCFGLINE 256

struct acdconfig
{
#if MG_TLS > 0
  char *cert_dir;
  char *mqtt_cert_dir;
#endif
  bool singleton;
  char *main_label;
  char *config_file;
  char *listen_address;
  unsigned int log_level;
  unsigned int mg_log_level;
  bool log_sensor_readings;
  char *web_directory;
  bool master_off_as_interlock;

  char *mqtt_aquachemd_topic;
  char *mqtt_aqualinkd_topic;
  char *mqtt_discovery_topic;
  char *mqtt_server;
  char *mqtt_user;
  char *mqtt_passwd;
  bool mqtt_discovery_use_mac;
  //bool mqtt_post_conditions;
  //bool convert_mqtt_temp;
  bool mqtt_timed_update;
  bool mqtt_repost_sensors;
  bool mqtt_strict_avail;

  char *gpio_chip;
  
  bool temp_comp_ph;
  bool temp_comp_ec;
  char *temp_comp_id;
  int temp_comp_reading_min;
  int temp_comp_reading_max;

  int sensor_poll_time;

  //bool post_condition;

  bool log_zerorun_pump_events;

  uint32_t ph_default_dose_time;
  uint32_t orp_default_dose_time;
  uint32_t h2o_default_dose_time;
  uint32_t ph_max_dose_time;
  uint32_t orp_max_dose_time;
  uint32_t h2o_max_dose_time;

  uint32_t switch_max_runtime; // Default max runtime for switches etc

  //bool ph_average_dose_calc;
  //bool orp_average_dose_calc;

  runtime_range_t ph_steps[MAX_DOSING_RANGES];
  uint8_t ph_step_count;

  runtime_range_t orp_steps[MAX_DOSING_RANGES];
  uint8_t orp_step_count;

  acd_key_t *keys;
};

typedef enum cfg_value_type {
  CFG_STRING,
  CFG_INT,
  CFG_FLOAT,
  CFG_HEX,
  CFG_BOOL,
  CFG_BITMASK,
  CFG_TXT_INT,
  CFG_CUSTOM
} cfg_value_type;

typedef struct cfgParam {
  void *value_ptr;
  cfg_value_type value_type;
  uint16_t config_mask;      // Could change this is 8
  char *name;
  char *metadata;
  uint16_t bit_flag;
} cfgParam;

//#define CFG_PERSISTANT (1 << 0)
#define CFG_ADVANCED (1 << 1)       // Show as advanced
#define CFG_READONLY (1 << 2)       // Show as no edit in UI     
#define CFG_HIDE (1 << 3)           // Hide in ui.
//#define CFG_FORCE_RESTART (1 << 4)
#define CFG_PASSWD_MASK (1 << 5)
#define CFG_MULTIPLE (1 << 6)
#define CFG_IS_ALLOCATED (1 << 7)   // Code needs this, not config template

#define PASSWD_MASK_TEXT "********"
//#define UNKNOWN -1
//#define STR_FULL_LENGTH 0

#ifndef CONFIG_C
extern struct acdconfig _acdconfig_;
#else
//struct acdconfig _acdconfig_;
// Initialize config with safe defaults
struct acdconfig _acdconfig_ = {
    .log_level      = LOG_INFO,  // Start with INFO so we see boot messages
    .listen_address = "0.0.0.0:8080",
    .config_file    = "/etc/aquachemd.conf",
    .ph_step_count  = 0,
    .orp_step_count = 0
};
#endif

// Count entries in the config table at compile time
#define CFG_ENTRY(...) +1
enum { 
    CFG_PARAM_COUNT = (0
#include "config_table.h"
    )
};
#undef CFG_ENTRY

#ifndef CONFIG_C
extern cfgParam _cfgParams[CFG_PARAM_COUNT];
#else
cfgParam _cfgParams[CFG_PARAM_COUNT];
#endif

#endif //CONFIG_H_
