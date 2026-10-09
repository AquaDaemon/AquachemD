

// X-macro config table — one entry per parameter

/**
 * CFG_ENTRY( name, field, default, type, mask, group, ui_metadata )
 * 1. name        : Key in the .conf file
 * 2. field       : Member in struct acdconfig
 * 3. default     : The literal default value
 * 4. type        : Data type (used for SET_VAL_ ## type)
 * 5. mask        : Behavior flags (CFG_READONLY, etc.)
 * 6. bit_flag    : The specific bit/mask to toggle within the 'field' (only used for CFG_BITMASK type)
 * 7. ui_metadata : JSON string for UI dropdowns
 */

CFG_ENTRY( "main_label",             main_label,             "AquachemD",          CFG_STRING,  0,                  0,                NULL )

CFG_ENTRY( "master_off_as_interlock_global", master_off_as_interlock, true,    CFG_BOOL,    0,                  0,                CFG_O_BOOL )


CFG_ENTRY( "single_instance",        singleton,               true,                CFG_BOOL,    0,                  0,                CFG_O_BOOL )

/* ---  GLOBAL SYSTEM & LOGGING --- */
CFG_ENTRY( "listen_address",         listen_address,         "http://0.0.0.0:80",  CFG_STRING,  CFG_ADVANCED, 0,                NULL )
CFG_ENTRY( "log_level",              log_level,              LOG_NOTICE,           CFG_TXT_INT, 0,                                  0,                CFG_O_log_level )
CFG_ENTRY( "mg_log_level",           mg_log_level,           0,                    CFG_INT,     CFG_READONLY|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "log_sensor_readings",    log_sensor_readings,    false,                CFG_BOOL,    0,                                  0,                CFG_O_BOOL )
/* ---  WEB & DIRECTORIES --- */
CFG_ENTRY( "web_directory",          web_directory,          "/var/www/aquachemd", CFG_STRING,  CFG_READONLY|CFG_HIDE,      0,     NULL )
//CFG_ENTRY( "web_config",             web_config,             "",                   CFG_STRING,  CFG_GRP_ADVANCED|CFG_READONLY|CFG_HIDE,      0,     NULL )

/* ---  MQTT COMMUNICATION --- */
CFG_ENTRY( "mqtt_server",            mqtt_server,            NULL,                 CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "mqtt_user",              mqtt_user,              NULL,                 CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "mqtt_passwd",            mqtt_passwd,            NULL,                 CFG_STRING,  CFG_ADVANCED|CFG_PASSWD_MASK,  0,                NULL )
CFG_ENTRY( "mqtt_aquachemd_topic",   mqtt_aquachemd_topic,   "aquachemd",          CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "mqtt_aqualinkd_topic",   mqtt_aqualinkd_topic,   "aqualinkd",          CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "mqtt_discovery_topic",   mqtt_discovery_topic,   "homeassistant",      CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "mqtt_discovery_use_mac", mqtt_discovery_use_mac, true,                 CFG_BOOL,    CFG_ADVANCED,                                  0,                CFG_O_BOOL )
CFG_ENTRY( "mqtt_timed_update",      mqtt_timed_update,      true,                 CFG_BOOL,    CFG_ADVANCED,                                  0,                CFG_O_BOOL )
CFG_ENTRY( "mqtt_repost_sensors",    mqtt_repost_sensors,    false,                CFG_BOOL,    CFG_ADVANCED,                                  0,                CFG_O_BOOL )
CFG_ENTRY( "mqtt_discovery_strict_availability",mqtt_strict_avail,false,           CFG_BOOL,    CFG_ADVANCED,                                  0,               CFG_O_BOOL )

/* --- REMOTE INSTANCE --- */
/*
remote_instance_label=Spa House
remote_instance_url=http://spa-aquachemd.local:88
remote_instance_mqtt_topic=aquachemd_spa
remote_instance_id_suffix=S2

CFG_ENTRY( "remote_instance_label",      remote_instance_label,            NULL,    CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "remote_instance_url",        remote_instance_url,              NULL,    CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "remote_instance_mqtt_topic", remote_instance_mqtt_topic,       NULL,    CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
CFG_ENTRY( "remote_instance_id_suffix",  remote_instance_id_suffix,        NULL,    CFG_STRING,  CFG_ADVANCED,                  0,                NULL )
*/

/* ---  GPIO CONFIGURATION --- */
CFG_ENTRY( "gpio_chip",              gpio_chip,              "/dev/gpiochip0",     CFG_STRING,  CFG_ADVANCED,                       0,                NULL )

/* --- POLLING & CONVERSION --- */
CFG_ENTRY( "sensor_poll_time",       sensor_poll_time,       60,                   CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "mqtt_convert_to_degF",   convert_mqtt_temp,      false,                CFG_BOOL,    0,                                  0,                CFG_O_BOOL )

CFG_ENTRY( "ph_reading_temp_min",    ph_reading_temp_min,    1,                    CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "ph_reading_temp_max",    ph_reading_temp_max,    60,                   CFG_INT,     0,                                  0,                NULL )

/* --- DOSING RANGES (Restored CFG_CUSTOM) --- */
CFG_ENTRY( "ph_dose_range",          ph_steps,               NULL,                 CFG_CUSTOM,  CFG_MULTIPLE,                       0,                NULL )
CFG_ENTRY( "ph_default_dose_time",   ph_default_dose_time,   20,                   CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "ph_max_dose_time",       ph_max_dose_time,      60,                   CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "ph_average_dose_calc",   ph_average_dose_calc,   false,                CFG_BOOL,    0,                                  0,                CFG_O_BOOL )


CFG_ENTRY( "orp_dose_range",         orp_steps,              NULL,                 CFG_CUSTOM,  CFG_MULTIPLE,                       0,                NULL )
CFG_ENTRY( "orp_default_dose_time",  orp_default_dose_time,  60,                   CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "orp_max_dose_time",      orp_max_dose_time,     300,                  CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "orp_average_dose_calc",  orp_average_dose_calc,  false,                CFG_BOOL,    0,                                  0,                CFG_O_BOOL )

CFG_ENTRY( "h2o_default_dose_time",  h2o_default_dose_time,  300,                  CFG_INT,     0,                                  0,                NULL )
CFG_ENTRY( "h2o_max_dose_time",      h2o_max_dose_time,     600,                  CFG_INT,     0,                                  0,                NULL )


CFG_ENTRY( "switch_max_runtime",     switch_max_runtime,    7200,                  CFG_INT,     0,                                  0,                NULL )


/* --- SECURITY / TLS (Conditional) --- */
#if MG_TLS > 0
CFG_ENTRY( "cert_dir",               cert_dir,               NULL,                 CFG_STRING,  CFG_GRP_ADVANCED,                   0,                NULL )
#endif

CFG_ENTRY( "post_condition",         post_condition,         true,                 CFG_BOOL,    0,                                  0,                CFG_O_BOOL )
CFG_ENTRY( "temp_compensated_ph",    temp_compensated_ph,    true,                 CFG_BOOL,    0,                                  0,                CFG_O_BOOL )
CFG_ENTRY( "log_zerorun_pump_events",log_zerorun_pump_events,false,                CFG_BOOL,    0,                                  0,                CFG_O_BOOL )



/* --- DYNAMIC MULTIPLE BLOCKS (Sensors, Conditions, Dosers) --- */
CFG_ENTRY( "mqtt_condition_label",         keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_condition_ID",            keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_condition_topic",         keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_condition_value",         keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "mqtt_condition_scope_global",  keys,             NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_condition_severity",keys,             NULL,                CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_condition_met_delay",     keys,             NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "gpio_condition_label",         keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_ID",            keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_pin",           keys,             NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_pin_mode",      keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_required_state",keys,             NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "gpio_condition_scope_global",  keys,             NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_severity",         keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_condition_met_delay",     keys,             NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "ph_sensor_label",        keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ph_sensor_ID",           keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ph_sensor_type",         keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ph_sensor_address",      keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "ph_sensor_scope_global", keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ph_sensor_interlock_scope",        keys,                   NULL,                 CFG_STRING,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ph_sensor_statistics",   keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "orp_sensor_label",       keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "orp_sensor_ID",          keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "orp_sensor_type",        keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "orp_sensor_address",     keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "orp_sensor_scope_global",keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "orp_sensor_interlock_scope",       keys,                   NULL,                 CFG_STRING,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "orp_sensor_statistics",  keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "prs_sensor_label",       keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_ID",          keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_type",        keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_address",     keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "prs_sensor_scope_global",keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_interlock_scope", keys,               NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_statistics",  keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
// For I2C (not EZO)
CFG_ENTRY( "prs_sensor_min_value",   keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "prs_sensor_max_value",   keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "ec_sensor_label",          keys,                 NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_ID",             keys,                 NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_type",           keys,                 NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_address",        keys,                 NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_interlock_scope",keys,                 NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_statistics",     keys,                 NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//0.1, 1, 10
CFG_ENTRY( "ec_sensor_k",              keys,                 NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ec_sensor_tds_factor",     keys,                 NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "mqtt_sensor_label",       keys,                  NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_sensor_ID",          keys,                  NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_sensor_topic",       keys,                  NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "mqtt_sensor_scope_global",keys,                  NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "mqtt_sensor_uom",         keys,                  NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "temp_sensor_label",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_ID",         keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_type",       keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_address",    keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_topic",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_path",       keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_offset",     keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_scale",      keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "temp_sensor_scope_global",keys,                  NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_interlock_scope",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_statistics", keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "temp_sensor_uom",        keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "gpio_doser_label",         keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_ID",            keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_type",          keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_pin",           keys,                   NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "gpio_doser_address",       keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_pin_mode",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "gpio_doser_required_state",keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_ml_per_second", keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_tank_total_volume", keys,               NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_tank_min_volume", keys,                 NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_tank_uom",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_running_dose_max_ml", keys,             NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "gpio_doser_scope_global",  keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_doser_interlock_scope",keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "ezo_doser_label",         keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_ID",            keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_type",          keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "ezo_doser_pin",           keys,                   NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_address",       keys,                   NULL,                 CFG_HEX,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "ezo_doser_pin_mode",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "ezo_doser_required_state",keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_ml_per_second", keys,                   NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_tank_total_volume", keys,               NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_tank_min_volume", keys,                 NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_tank_uom",      keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_running_dose_max_ml", keys,             NULL,                 CFG_FLOAT,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
//CFG_ENTRY( "ezo_doser_scope_global",  keys,                   NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "ezo_doser_interlock_scope",keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


CFG_ENTRY( "gpio_input_label",          keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_input_ID",             keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_input_pin",            keys,             NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_input_pin_mode",       keys,             NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_input_required_state", keys,             NULL,                 CFG_BOOL,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_input_interlock_scope",keys,             NULL,                 CFG_STRING,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "gpio_output_label",          keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_output_ID",             keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_output_pin",            keys,                   NULL,                 CFG_INT,     CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_output_pin_mode",       keys,                   NULL,                 CFG_STRING,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "gpio_output_interlock_scope",keys,                   NULL,                 CFG_STRING,    CFG_MULTIPLE|CFG_HIDE,              0,                NULL )

CFG_ENTRY( "sysfs_sensor_label",      keys,                   NULL,                 CFG_STRING, CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_ID",         keys,                   NULL,                 CFG_STRING, CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_path",       keys,                   NULL,                 CFG_STRING, CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_offset",     keys,                   NULL,                 CFG_FLOAT,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_scale",      keys,                   NULL,                 CFG_FLOAT,  CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_regex",      keys,                   NULL,                 CFG_STRING, CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_interlock_scope",keys,               NULL,                 CFG_STRING,   CFG_MULTIPLE|CFG_HIDE,              0,                NULL )
CFG_ENTRY( "sysfs_sensor_uom",        keys,                   NULL,                 CFG_STRING, CFG_MULTIPLE|CFG_HIDE,              0,                NULL )


/* --- JSON Metadata Definitions --- */
#define CFG_O_log_level "[\"DEBUG\",\"INFO\",\"NOTICE\",\"WARNING\",\"ERROR\"]"
#define CFG_O_BOOL "[\"Yes\",\"No\"]"
#define CFG_O_ONOFF "[\"on\",\"off\"]"
#define CFG_O_ACTIVE "[\"Active High\",\"Active Low\"]"
#define CFG_O_PMP_TYPE "[\"pH\",\"ORP\",\"H2O\"]"

#define CFG_O_UOM_LIST "[\"celsius\",\"fahrenheit\",\"pH\",\"mV\",\"psi\",\"percent\",\"RPM\",\"bytes\",\"seconds\",\"minutes\",\"ratio\",\"milliliters\",\"liters\",\"gallons\",\"\"]"
#define CFG_O_UOM_TEMPERATURE "[\"°C\",\"°F\",\"\"]"

#define CFG_O_TANK_UOM "[\"gallons\",\"liters\",\"\"]"

#define CFG_O_I2C_PRS_DRIVERS "[\"PTE7300\",\"HSC/SSC\"]"

#define CFG_O_GPIO_DIRECTION "[\"Input\",\"Output\"]"

#define CFG_O_SCOPE      "[\"Local\",\"Global\"]"
#define CFG_O_SCOPE_FULL "[\"Allow\",\"Local\",\"Global\"]"


//#define CFG_O_STATS "[\"None\",\"Daily\",\"Weekly\"]"