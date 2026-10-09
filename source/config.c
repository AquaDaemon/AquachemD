
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <strings.h> // For strncasecmp



#define CONFIG_C
#include "config.h"
#include "utils.h"
#include "aquachemd.h"
#include "cJSON.h"
#include "sensor_stats.h"
#include "uom.h"

void set_config_defaults();
char* replace_or_append_suffix(const char *orig, const char *suffixes[], const char *append_text);
void assign_missing_ids(struct aquachemdata *acdata);
void validate_config(struct aquachemdata *acdata);
static inline bool key_ends_with_ic_impl(const char *str, const char *suffix, size_t suf_len);

#define SET_VAL_CFG_STRING(field, def)  _acdconfig_.field = def
#define SET_VAL_CFG_INT(field, def)     _acdconfig_.field = def
#define SET_VAL_CFG_BOOL(field, def)    _acdconfig_.field = def
#define SET_VAL_CFG_FLOAT(field, def)   _acdconfig_.field = def
#define SET_VAL_CFG_HEX(field, def)     _acdconfig_.field = def
#define SET_VAL_CFG_BITMASK(field, def) _acdconfig_.field = def
#define SET_VAL_CFG_TXT_INT(field, def) _acdconfig_.field = def
// Define the initialization macro to do nothing
#define SET_VAL_CFG_CUSTOM(field, def)  /* Handled by specialized logic */



#define STARTS_WITH_IC(str, prefix) \
    (strncasecmp((str), (prefix), sizeof(prefix) - 1) == 0)

#define KEY_ENDS_WITH_IC(str, suffix) \
    key_ends_with_ic_impl((str), (suffix), sizeof(suffix) - 1)



static inline bool key_ends_with_ic_impl(const char *str, const char *suffix, size_t suf_len) {
    if (!str || !suffix) return false;

    // Find '=' delimiter; if absent, use full string length
    const char *eq = strchr(str, '=');
    size_t key_len = eq ? (size_t)(eq - str) : strlen(str);

    if (key_len < suf_len) return false;

    // Compare only the last `suf_len` characters before '='
    return strncasecmp(str + key_len - suf_len, suffix, suf_len) == 0;
}


// Helper to stop cJSON to handle a float correctly.
static void cJSON_AddFloat(cJSON *object, const char *name, float value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", value); // %g automatically drops trailing zeros
    cJSON_AddRawToObject(object, name, buf);
}


typedef struct {
    char *label;
    char *topic_path;
    char *char_value;
    char *ID;
    float value;
    float value2;
    float value3;
    float value4;
    int   pin;
    bool  pin_mode;
    bool  pin_state;
    //bool  is_global;
    acd_scope_t scope;
    unsigned char address;
    acd_type_t pending_type;
    uint8_t flags;
    acd_uom_t uom;
    acd_uom_t uom2;
} acd_staging_t;

static acd_staging_t _staging;


void add_condition_mqtt(const acd_staging_t *st);
void add_condition_gpio(const acd_staging_t *st);
void add_pump(const acd_staging_t *st);
void add_gpio(const acd_staging_t *st);
void add_sensor_ezo(const acd_staging_t *st);
void add_sensor_d1w(const acd_staging_t *st);
void add_sensor_mqtt(const acd_staging_t *st);
void add_sensor_sysfs(const acd_staging_t *st);
void add_sensor_i2c(const acd_staging_t *st);


void init_cfg_parameters() {
    LOG(LOG_DEBUG, "Initializing config with %d entries", CFG_PARAM_COUNT);
    int i = 0;

#define CFG_ENTRY(cfg_name, cfg_field, cfg_def, cfg_type, cfg_mask, cfg_flag, cfg_meta) \
    if (i < CFG_PARAM_COUNT) {                                  \
        _cfgParams[i].name        = cfg_name;                   \
        _cfgParams[i].value_ptr   = (((cfg_mask) & CFG_MULTIPLE) && (cfg_type != CFG_CUSTOM)) ? NULL : &_acdconfig_.cfg_field; \
        _cfgParams[i].value_type  = cfg_type;                   \
        _cfgParams[i].config_mask = cfg_mask;                   \
        _cfgParams[i].bit_flag    = cfg_flag;                   \
        _cfgParams[i].metadata    = cfg_meta;                   \
                                                                \
        if (!((cfg_mask) & CFG_MULTIPLE)) {                     \
            SET_VAL_ ## cfg_type(cfg_field, cfg_def);           \
        }                                                       \
        i++;                                                    \
    }

#undef CONFIG_TABLE_H_ 
#include "config_table.h"
#undef CFG_ENTRY
}

void clear_staging() {
    if (_staging.label) { free(_staging.label); _staging.label = NULL; }
    if (_staging.ID) { free(_staging.ID); _staging.ID = NULL; }
    if (_staging.topic_path) { free(_staging.topic_path); _staging.topic_path = NULL; }
    if (_staging.char_value) { free(_staging.char_value); _staging.char_value = NULL; }
    _staging.pin = 0;
    _staging.pin_mode = 0;
    _staging.pin_state = 0;
    _staging.value = 0;
    _staging.value2 = 0;
    _staging.value3 = 0;
    _staging.value4 = 0;
    _staging.pending_type = ACD_TYPE_NONE;
    _staging.flags = 0;
    //_staging.scope = ACD_SCOPE_GLOBAL;
    _staging.scope = ACD_SCOPE_UNKNOWN;
    _staging.uom = UOM_NONE;
    _staging.uom2 = UOM_NONE;
}

void action_staging() {
    if (!_staging.label) return;

    LOG(LOG_DEBUG,"Adding device type %d\n",_staging.pending_type);

    switch (_staging.pending_type) {
        case ACD_TYPE_EZO_PH:
        case ACD_TYPE_EZO_ORP:
        case ACD_TYPE_EZO_TEMP:
        case ACD_TYPE_EZO_PRS:
        case ACD_TYPE_EZO_EC:
            add_sensor_ezo(&_staging);
            break;
        case ACD_TYPE_MQTT_TEMP:
            add_sensor_mqtt(&_staging);
            break;
        case ACD_TYPE_D1W_TEMP:
            add_sensor_d1w(&_staging);
            break;
        case ACD_TYPE_GPIO_PMP:
        case ACD_TYPE_EZO_PMP:
            add_pump(&_staging);
            break;
        case ACD_TYPE_GPIO_OUTPUT:
        case ACD_TYPE_GPIO_INPUT:
            add_gpio(&_staging);
            break;
        case ACD_TYPE_MQTT_COND:
            add_condition_mqtt(&_staging);
            break;
        case ACD_TYPE_GPIO_COND:
            add_condition_gpio(&_staging);
            break;
        case ACD_TYPE_SYSFS_VALUE:
            add_sensor_sysfs(&_staging);
            break;
        case ACD_TYPE_MQTT_VALUE:
            add_sensor_mqtt(&_staging);
            break;
        case ACD_TYPE_I2C_PRS:
            add_sensor_i2c(&_staging);
            break;
        default:
            LOG(LOG_ERR, "Didn't create config entry for %s", _staging.label);
            break;
    }
    
    // Ensure cleanup happens after extraction
    clear_staging();
}



bool parse_dose_range(runtime_range_t *steps, uint8_t *count, const char *value, bool sort_descending) {
    if (*count >= MAX_DOSING_RANGES) return false;
    float threshold;
    uint32_t seconds;
    if (sscanf(value, "%f:%u", &threshold, &seconds) != 2) return false;

    int idx = *count;
    steps[idx].threshold = threshold;
    steps[idx].seconds = seconds;
    (*count)++;

    for (int j = idx; j > 0; j--) {
        bool swap = (sort_descending) ? (steps[j].threshold > steps[j - 1].threshold) : (steps[j].threshold < steps[j - 1].threshold);
        if (swap) {
            runtime_range_t temp = steps[j];
            steps[j] = steps[j - 1];
            steps[j - 1] = temp;
        } else break;
    }
    return true;
}

bool setConfigValue(struct aquachemdata *acdata, char *param, char *value) {
    value = cleanwhitespace(value);

    LOG(LOG_DEBUG,"Parsing config parm %s\n",param);

    for (int i = 0; i < CFG_PARAM_COUNT; i++) {
        if (strncasecmp(param, _cfgParams[i].name, strlen(_cfgParams[i].name)) == 0) {
            if (_cfgParams[i].metadata != NULL && strncasestr(_cfgParams[i].metadata, value, STR_FULL_LENGTH) == NULL) {
                return false;
            }
            if (strlen(value) <= 0) return true;

            // --- INTERCEPT ALL RANGE ARRAYS AND DYNAMIC ENTRY PARSERS ---
            if (_cfgParams[i].config_mask & CFG_MULTIPLE) {
                if (_cfgParams[i].value_type == CFG_CUSTOM) {
                    if (strncasecmp(param, "ph_dose_range", 13) == 0) {
                        return parse_dose_range(_acdconfig_.ph_steps, &_acdconfig_.ph_step_count, value, true);
                    } 
                    if (strncasecmp(param, "orp_dose_range", 14) == 0) {
                        return parse_dose_range(_acdconfig_.orp_steps, &_acdconfig_.orp_step_count, value, false);
                    }
                    return false;
                }

                // Dynamic Linked List Staging Parser
                if (strstr(param, "_label") != NULL) {
                    action_staging();
                    clear_staging();
                    LOG(LOG_DEBUG,"Start new device %s\n",value);
                    _staging.label = strdup(value);

                    /*if (STARTS_WITH_IC(param, "gpio_switch")) _staging.pending_type = ACD_TYPE_GPIO_OUTPUT;
                    else*/
                    if (STARTS_WITH_IC(param, "mqtt_condition")) _staging.pending_type = ACD_TYPE_MQTT_COND;
                    else if (STARTS_WITH_IC(param, "gpio_condition")) _staging.pending_type = ACD_TYPE_GPIO_COND;
                    else if (STARTS_WITH_IC(param, "sysfs_sensor")) _staging.pending_type = ACD_TYPE_SYSFS_VALUE;
                    else if (STARTS_WITH_IC(param, "gpio_input")) _staging.pending_type = ACD_TYPE_GPIO_INPUT;
                    else if (STARTS_WITH_IC(param, "gpio_output")) _staging.pending_type = ACD_TYPE_GPIO_OUTPUT;
                    //else if (STARTS_WITH_IC(param, "ezo_doser")) {_staging.pending_type = ACD_TYPE_EZO_PMP; _staging.address = EZO_PMP_ADDR;}
                }
                else if (strstr(param, "_type")) {
                    if (strcasecmp(value, "ezo") == 0) {
                        if (STARTS_WITH_IC(param, "ph_sensor"))       {_staging.pending_type = ACD_TYPE_EZO_PH;   _staging.address = EZO_PH_ADDR;}
                        else if (STARTS_WITH_IC(param, "orp_sensor")) {_staging.pending_type = ACD_TYPE_EZO_ORP;  _staging.address = EZO_ORP_ADDR;}
                        else if (STARTS_WITH_IC(param, "temp_sensor")){_staging.pending_type = ACD_TYPE_EZO_TEMP; _staging.address = EZO_RTD_ADDR;}
                        else if (STARTS_WITH_IC(param, "prs_sensor")) {_staging.pending_type = ACD_TYPE_EZO_PRS;  _staging.address = EZO_PRS_ADDR;}
                        else if (STARTS_WITH_IC(param, "ec_sensor"))  {_staging.pending_type = ACD_TYPE_EZO_EC; _staging.address = EZO_EC_ADDR;}   
                    }
                    else if (STARTS_WITH_IC(param, "ezo_doser_type")) {
                            _staging.pending_type = ACD_TYPE_EZO_PMP;
                            _staging.address = EZO_PMP_ADDR;
                            setMASK(_staging.flags, parse_pump_type(value));
                    }
                    else if (strcasecmp(value, "d1w") == 0) _staging.pending_type = ACD_TYPE_D1W_TEMP;
                    else if (strcasecmp(value, "mqtt") == 0) _staging.pending_type = ACD_TYPE_MQTT_TEMP;
                    //else if (strcasecmp(value, "input") == 0) _staging.pending_type = ACD_TYPE_GPIO_INPUT;
                    //else if (strcasecmp(value, "output") == 0) _staging.pending_type = ACD_TYPE_GPIO_OUTPUT;
                    else if (strstr(param, "gpio_doser_type")) {
                        _staging.pending_type = ACD_TYPE_GPIO_PMP;
                        setMASK(_staging.flags, parse_pump_type(value));
                    }
                    else if (strcasecmp(value, "PTE7300") == 0) {
                      _staging.pending_type = ACD_TYPE_I2C_PRS;
                      _staging.pin_mode = I2C_SENSOR_PTE7300;
                      _staging.address = PTE7300_ADDR_DEFAULT;
                    }
                    
                }
                else if (KEY_ENDS_WITH_IC(param, "_ID")) {
                    if (_staging.ID) free(_staging.ID);
                    _staging.ID = strdup(value);
                }
                else if (strncasecmp(param, "mqtt_condition_topic", 20) == 0) {
                    _staging.pending_type = ACD_TYPE_MQTT_COND;
                    if (_staging.topic_path) free(_staging.topic_path);
                    _staging.topic_path = strdup(value);
                }
                else if (strncasecmp(param, "sysfs_sensor_path", 17) == 0) {
                     _staging.pending_type = ACD_TYPE_SYSFS_VALUE;
                     if (_staging.topic_path) free(_staging.topic_path);
                    _staging.topic_path = strdup(value);
                }
                else if (strstr(param, "_address")) {
                    _staging.address = (unsigned char)strtoul(value, NULL, 16);
                }
                else if (strncasecmp(param, "temp_sensor_path", 16) == 0) {
                    if (_staging.topic_path) free(_staging.topic_path);
                    _staging.topic_path = strdup(value);
                }
                else if (strncasecmp(param, "temp_sensor_topic", 17) == 0) {
                    if (_staging.topic_path) free(_staging.topic_path);
                    _staging.topic_path = strdup(value);
                }
                else if (strncasecmp(param, "temp_sensor_offset", 18) == 0) {
                    _staging.value = strtof(value, NULL);
                }
                else if (strncasecmp(param, "temp_sensor_scale", 17) == 0) {
                    _staging.value2 = strtof(value, NULL);
                }
                else if (strstr(param, "_sensor_statistics")) {
                    /* For NEW average caculation */
                    _staging.value3 = parse_duration_to_seconds(value);
                    if (_staging.value3 > 0) {
                      setMASK(_staging.flags, CALC_AVERAGE);
                    }
                }
                else if (KEY_ENDS_WITH_IC(param, "_interlock_scope") ||
                         KEY_ENDS_WITH_IC(param, "_severity") ) {
                    _staging.scope = parse_acd_scope(value);
                }
                else if (strncasecmp(param, "mqtt_sensor_topic", 17) == 0) {
                    if (_staging.topic_path) free(_staging.topic_path);
                    _staging.topic_path = strdup(value);
                    _staging.pending_type = ACD_TYPE_MQTT_VALUE;
                }
                else if (strncasecmp(param, "gpio_condition_pin_mode", 23) == 0) {
                    _staging.pin_mode = parse_gpio_active(value);
                    _staging.pending_type = ACD_TYPE_GPIO_COND;
                }
                else if (strncasecmp(param, "gpio_condition_required_state", 29) == 0) {
                    _staging.pin_state = parse_gpio_req(value);
                    _staging.pending_type = ACD_TYPE_GPIO_COND;
                }
                else if (strncasecmp(param, "gpio_condition_pin", 18) == 0) {
                    _staging.pin = (int)strtoul(value, NULL, 10);
                    _staging.pending_type = ACD_TYPE_GPIO_COND;
                }
                else if (KEY_ENDS_WITH_IC(param, "_required_state")) { // All GPIO_XXX
                    _staging.pin_state = parse_gpio_req(value);
                }
                else if (KEY_ENDS_WITH_IC(param, "_pin_mode")) { // All GPIO_XXX
                    _staging.pin_mode = parse_gpio_active(value);
                }
                else if (KEY_ENDS_WITH_IC(param, "_pin")) {  // All GPIO_XXX
                    _staging.pin = (int)strtoul(value, NULL, 10);
                }
                else if (KEY_ENDS_WITH_IC(param, "_doser_tank_total_volume")) {
                    _staging.value2 = strtof(value, NULL);
                }
                else if (KEY_ENDS_WITH_IC(param, "_doser_tank_size")) { // NSF NEED TO REMOVE AND DEPRICATE.
                    _staging.value2 = strtof(value, NULL);
                }
                else if (KEY_ENDS_WITH_IC(param, "_doser_tank_uom")) {
                    _staging.uom2 = parse_uom(value);
                }
                else if (KEY_ENDS_WITH_IC(param, "_doser_tank_min_volume")) {
                    _staging.value3 = strtof(value, NULL);
                }
                else if (strncasecmp(param, "mqtt_condition_value", 20) == 0) {
                    if (_staging.char_value) free(_staging.char_value);
                    _staging.char_value = strdup(value);
                    _staging.pending_type = ACD_TYPE_MQTT_COND;
                }   
                else if (KEY_ENDS_WITH_IC(param, "_doser_ml_per_second")) {
                    _staging.value = strtof(value, NULL);
                }
                else if (KEY_ENDS_WITH_IC(param, "_doser_running_dose_max_ml")) {
                    _staging.value4 = strtof(value, NULL);
                }
                else if (strstr(param, "condition_met_delay")) {
                    _staging.value = strtof(value, NULL);
                }
                else if (strncasecmp(param, "sysfs_sensor_regex", 18) == 0) {
                    if (_staging.char_value) free(_staging.char_value);
                    _staging.char_value = strdup(value);
                }
                else if (strstr(param, "_sensor_uom")) {
                    // temp_sensor_uom
                    // sysfs_sensor_uom
                    _staging.uom = parse_uom(value);
                }
                else if (strncasecmp(param, "sysfs_sensor_scale", 18) == 0) {
                    _staging.value2 = strtof(value, NULL);
                }
                else if (strncasecmp(param, "sysfs_sensor_offset", 19) == 0) {
                    _staging.value = strtof(value, NULL);
                }
                else if (strncasecmp(param, "prs_sensor_min_value", 20) == 0) {
                    _staging.value2 = strtof(value, NULL);
                }
                else if (strncasecmp(param, "prs_sensor_max_value", 20) == 0) {
                    _staging.value3 = strtof(value, NULL);
                }
                else if (strncasecmp(param, "ec_sensor_k", 11) == 0) {
                    _staging.value = strtof(value, NULL);
                }
                else if (strncasecmp(param, "ec_sensor_tds_factor", 20) == 0) {
                    _staging.value2 = strtof(value, NULL);
                }
                return true;
            }

            // Standard Entries
            switch (_cfgParams[i].value_type) {
                case CFG_STRING:
                    if (_cfgParams[i].config_mask & CFG_IS_ALLOCATED) {
                        free(*(char **)_cfgParams[i].value_ptr);
                        *(char **)_cfgParams[i].value_ptr = NULL;
                    }
                    *(char **)_cfgParams[i].value_ptr = cleanalloc(value, STR_FULL_LENGTH);
                    _cfgParams[i].config_mask |= CFG_IS_ALLOCATED;
                    break;
                case CFG_INT:
                    *(int *)_cfgParams[i].value_ptr = strtoul(value, NULL, 10);
                    break;
                case CFG_BOOL:
                    *(bool *)_cfgParams[i].value_ptr = parse_bool(value);
                    break;
                case CFG_HEX:
                    *(unsigned char *)_cfgParams[i].value_ptr = strtoul(value, NULL, 16);
                    break;
                case CFG_FLOAT:
                    *(float *)_cfgParams[i].value_ptr = strtof(value, NULL);
                    break;
                case CFG_TXT_INT:
                    if (_cfgParams[i].value_ptr == &_acdconfig_.log_level) {
                        *(int *)_cfgParams[i].value_ptr = log_str_to_priority(value);
                    }
                    break;
                default: break;
            }
            return true;
        }
    }
    return false;
}

void parse_config_file(struct aquachemdata *acdata) {
    FILE *fp;
    char bufr[MAXCFGLINE];
    char *b_ptr;
    bool log_debug = (_acdconfig_.log_level >= LOG_DEBUG);

    init_cfg_parameters();
    if (log_debug) _acdconfig_.log_level = LOG_DEBUG;

    if ((fp = fopen(_acdconfig_.config_file, "r")) != NULL) {
        while (!feof(fp)) {
            if (fgets(bufr, MAXCFGLINE, fp) != NULL) {
                b_ptr = &bufr[0];
                char *indx;
                while (isspace(*b_ptr)) b_ptr++;
                if (b_ptr[0] != '\0' && b_ptr[0] != '#') {
                    indx = strchr(b_ptr, '=');
                    if (indx != NULL) {
                        if (!setConfigValue(acdata, b_ptr, indx + 1)) {
                            char *end = b_ptr + strlen(b_ptr) - 1;
                            while (end > b_ptr && isspace(*end)) end--;
                            LOG(LOG_ERR, "Unknown config parameter '%.*s'\n", (int)(end - b_ptr + 1), b_ptr);
                        } else if (log_debug) {
                            _acdconfig_.log_level = LOG_DEBUG;
                        }
                    }
                }
            }
        }
        action_staging();
        fclose(fp);
    } else {
        LOG(LOG_ERR, "Error opening config file '%s'\n",_acdconfig_.config_file);
        exit(EXIT_FAILURE);
    }
    acdata->keys->next = _acdconfig_.keys;
    // I know all of the below could be combined and be quicker startup
    // But keeping them seperate for future enhacments and code readability.
    assign_missing_ids(acdata);
    validate_config(acdata);
    print_config(acdata);
}

void free_config()
{
    LOG(LOG_NOTICE, "Cleaning config memory\n");

    // FREE THE OLD HARDWARE LINKED LIST
    // We must safely remove the existing custom blocks to prevent memory leaks.
    acd_key_t *curr = _acdconfig_.keys; // Start one from master.
    while (curr != NULL) {
        acd_key_t *next = curr->next;
        if (curr->label) free(curr->label);
        if (curr->ID) free(curr->ID);
        
        if (curr->type == ACD_TYPE_MQTT_COND || curr->type == ACD_TYPE_MQTT_TEMP) {
            if (curr->data.mqtt.topic) free(curr->data.mqtt.topic);
            if (curr->data.mqtt.target_value) free(curr->data.mqtt.target_value);
        }
        if (curr->type == ACD_TYPE_SYSFS_VALUE) {
            if (curr->data.sysfs.regex_pattern) free(curr->data.sysfs.regex_pattern);
        }
        
        free(curr);
        curr = next;
    }

    _acdconfig_.keys = NULL;
}



// Helper: Checks if a value is valid for a given config parameter without mutating memory.
static bool is_valid_config(const char *param, const char *value) {
    LOG(LOG_DEBUG, "Validating: '%s' option for '%s'", value, param);
    
    for (int i = 0; i < CFG_PARAM_COUNT; i++) {
        if (strcasecmp(param, _cfgParams[i].name) == 0) {
            
            
            // Check UI Metadata dropdown constraints (if any)
            // This doesn't work for things like bool where it's true/false in the json, but yes/no in the metadata object
            //if (_cfgParams[i].metadata != NULL && strcasestr(_cfgParams[i].metadata, value) == NULL) {
            if (_cfgParams[i].value_type != CFG_BOOL && _cfgParams[i].metadata != NULL && strcasestr(_cfgParams[i].metadata, value) == NULL) {
                LOG(LOG_WARNING, "Validation failed: '%s' is not a valid option for '%s'", value, param);
                return false; 
            }
            
            // Type-specific validation
            char *endptr;
            switch (_cfgParams[i].value_type) {
                case CFG_INT:
                //case CFG_TXT_INT:
                    strtol(value, &endptr, 10);
                    if (*endptr != '\0') return false; // Not a clean integer
                    break;
                case CFG_FLOAT:
                    strtof(value, &endptr);
                    if (*endptr != '\0') return false; // Not a clean float
                    break;
                case CFG_HEX:
                    strtoul(value, &endptr, 16);
                    if (*endptr != '\0') return false; // Not a clean hex
                    break;
                case CFG_BOOL:
                    if (strcasecmp(value, "true") != 0 && strcasecmp(value, "false") != 0 &&
                        strcmp(value, "1") != 0 && strcmp(value, "0") != 0 &&
                        strcasecmp(value, "yes") != 0 && strcasecmp(value, "no") != 0 &&
                        strcasecmp(value, "on") != 0 && strcasecmp(value, "off") != 0) {
                        LOG(LOG_WARNING, "Validation failed: '%s' is not a valid option for '%s'", value, param);
                        return false;
                    }
                    break;
                case CFG_TXT_INT:
                case CFG_STRING:
                    // Validate string ???
                    break;
                default:
                    break; // Strings and custom types pass basic checks
            }
            return true;
        }
    }
    
    // If it's a dynamic staging key (e.g., "mqtt_condition_label"), we allow it to pass 
    // because the daemon's parser handles these on boot.
    if (strstr(param, "_label") || strstr(param, "_type") || strstr(param, "_pin") || 
        strstr(param, "_topic") || strstr(param, "_address") || strstr(param, "_scope")) {
        return true;
    }

    LOG(LOG_WARNING, "Validation failed: Unknown parameter '%s'", param);
    return false;
}



// Helper to extract the active in-memory variable value as a string while we loop over the saving json config
static bool get_config_val_str(const char *name, char *buf, size_t buf_size) {
    for (int i = 0; i < CFG_PARAM_COUNT; i++) {
        if (strcasecmp(name, _cfgParams[i].name) == 0) {
            switch (_cfgParams[i].value_type) {
                case CFG_STRING:
                    snprintf(buf, buf_size, "%s", *(char **)_cfgParams[i].value_ptr ? *(char **)_cfgParams[i].value_ptr : "");
                    break;
                case CFG_INT:
                case CFG_TXT_INT:
                    snprintf(buf, buf_size, "%d", *(int *)_cfgParams[i].value_ptr);
                    break;
                case CFG_FLOAT:
                    snprintf(buf, buf_size, "%.6g", *(float *)_cfgParams[i].value_ptr);
                    break;
                case CFG_BOOL:
                    snprintf(buf, buf_size, "%s", bool_to_str(*(bool *)_cfgParams[i].value_ptr));
                    break;
                case CFG_HEX:
                    snprintf(buf, buf_size, "0x%02hhx", *(unsigned char *)_cfgParams[i].value_ptr);
                    break;
                default: 
                    buf[0] = '\0'; 
                    break;
            }
            return true;
        }
    }
    return false;
}

// Copies mode + ownership from the existing config file onto tmp_file,
// so that when tmp_file replaces it via rename(), the live file's
// permissions/ownership are preserved instead of reverting to whatever
// tmp_file was created with. Call this after writing tmp_file's contents
// and before rename(). Non-fatal on failure -- logs and lets the rename
// proceed with tmp_file's own permissions rather than aborting the save.
static void preserve_file_permissions(const char *existing_file, const char *tmp_file)
{
  struct stat st;

  if (stat(existing_file, &st) != 0) {
    if (errno == ENOENT) {
      // First-ever write -- nothing to preserve, tmp_file's own mode stands.
      return;
    }
    LOG(LOG_WARNING, "preserve_file_permissions: stat(%s) failed: %s",existing_file, strerror(errno));
    return;
  }

  if (chmod(tmp_file, st.st_mode & 07777) != 0) {
    LOG(LOG_WARNING, "preserve_file_permissions: chmod(%s) failed: %s",tmp_file, strerror(errno));
  }

  // chown to a different uid typically requires root/CAP_CHOWN -- if this
  // process isn't privileged, this may legitimately fail. That's fine;
  // log it rather than treat it as fatal.
  if (chown(tmp_file, st.st_uid, st.st_gid) != 0) {
    LOG(LOG_WARNING, "preserve_file_permissions: chown(%s) failed: %s",tmp_file, strerror(errno));
  }
}


int save_aquachem_config_json(const char* inBuf, int inSize, char* outBuf, int outSize, struct aquachemdata *acdata) {
    cJSON *root = cJSON_ParseWithLength(inBuf, inSize);
    if (!root) {
        LOG(LOG_ERR, "Failed to parse incoming config JSON.");
        snprintf(outBuf, outSize, "{\"status\":\"error\",\"message\":\"Invalid JSON payload.\"}");
        return -1;
    }

    // Write to a temporary file first so a crash doesn't corrupt the main config
    //const char *tmp_file = "/tmp/aquachemd.conf.tmp";
    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", _acdconfig_.config_file);
    FILE *fp = fopen(tmp_file, "w");
    if (!fp) {
        LOG(LOG_ERR, "Failed to open temporary config file for writing.");
        snprintf(outBuf, outSize, "{\"status\":\"error\",\"message\":\"Disk write error.\"}");
        cJSON_Delete(root);
        return -1;
    }

    bool validation_failed = false;

    // 1. PROCESS STANDARD GLOBAL FIELDS
    cJSON *fields = cJSON_GetObjectItem(root, "fields");
    if (cJSON_IsArray(fields)) {
        fprintf(fp, "# Global System Parameters\n");
        cJSON *field;
        char last_k = '\n';
        
        cJSON_ArrayForEach(field, fields) {
            cJSON *k = cJSON_GetObjectItem(field, "key");
            cJSON *v = cJSON_GetObjectItem(field, "value");
            cJSON *t = cJSON_GetObjectItem(field, "type");
            
            if (!k || !v || !cJSON_IsString(k)) continue;

            // Find the bitmask for this parameter ---
            int mask = 0;
            for (int i = 0; i < CFG_PARAM_COUNT; i++) {
                if (strcasecmp(k->valuestring, _cfgParams[i].name) == 0) {
                    mask = _cfgParams[i].config_mask;
                    break;
                }
            }

            // Spoof Protection ---
            // If the UI maliciously sent a hidden field, ignore the payload entirely.
            if (mask & CFG_HIDE) {
                continue; 
            }

            // save the first char of key, and if changes print new line
            if (k->valuestring[0] != '\0') {
                char current_first_char = k->valuestring[0]; 
                if (last_k != '\0' && current_first_char != last_k) {
                    fprintf(fp, "\n");
                }
                last_k = current_first_char;
            }

            // Handle array_text (Dosing ranges)
            if (t && cJSON_IsString(t) && strcmp(t->valuestring, "array_text") == 0) {
                if (cJSON_IsArray(v)) {
                    cJSON *arr_item;
                    cJSON_ArrayForEach(arr_item, v) {
                        if (cJSON_IsString(arr_item)) {
                            fprintf(fp, "%s=%s\n", k->valuestring, arr_item->valuestring);
                        }
                    }
                }
            } 
            // Handle all standard types
            else {
                char val_str[256] = {0};
                if (cJSON_IsString(v)) snprintf(val_str, sizeof(val_str), "%s", v->valuestring);
                else if (cJSON_IsNumber(v)) {
                    if (v->valueint == v->valuedouble) snprintf(val_str, sizeof(val_str), "%d", v->valueint);
                    else snprintf(val_str, sizeof(val_str), "%.6g", v->valuedouble);
                } else if (cJSON_IsBool(v)) {
                    snprintf(val_str, sizeof(val_str), "%s", bool_to_str(cJSON_IsTrue(v)));
                }

                // Password Mask and Read-Only Interception ---
                // If the UI sent back "********" OR the field is read-only, pull the REAL value from memory.
                if ((mask & CFG_READONLY) || ((mask & CFG_PASSWD_MASK) && strcmp(val_str, PASSWD_MASK_TEXT) == 0)) {
                    get_config_val_str(k->valuestring, val_str, sizeof(val_str));
                }

                if (is_valid_config(k->valuestring, val_str)) {
                    fprintf(fp, "%s=%s\n", k->valuestring, val_str);
                } else {
                    validation_failed = true;
                    break;
                }
            }
        }

        // Because hidden variables weren't in the JSON, we must write them to the file now.
        fprintf(fp, "\n# Hidden System Parameters\n");
        for (int i = 0; i < CFG_PARAM_COUNT; i++) {
            if ((_cfgParams[i].config_mask & CFG_HIDE) && !(_cfgParams[i].config_mask & CFG_MULTIPLE)) {
                char hidden_val[256] = {0};
                if (get_config_val_str(_cfgParams[i].name, hidden_val, sizeof(hidden_val))) {
                    fprintf(fp, "%s=%s\n", _cfgParams[i].name, hidden_val);
                }
            }
        }
    }

    fprintf(fp, "\n# Conditions & Sensors\n");

    // PROCESS CUSTOM HARDWARE BLOCKS
    cJSON *blocks = cJSON_GetObjectItem(root, "custom_blocks");
    if (cJSON_IsArray(blocks) && !validation_failed) {
        cJSON *block;
        cJSON_ArrayForEach(block, blocks) {
            fprintf(fp, "\n"); // Add spacing between blocks

            // --- THE FIX: Explicitly extract and write the Label FIRST ---
            cJSON *label = cJSON_GetObjectItem(block, "label");
            cJSON *type_id = cJSON_GetObjectItem(block, "block_type_id");

            if (label && cJSON_IsString(label) && type_id && cJSON_IsNumber(type_id))
            {
                int type = type_id->valueint;
                const char *lbl = label->valuestring;

                cJSON *ID = cJSON_GetObjectItem(block, "ID");
                const char *id = cJSON_GetStringValue(ID);

                const char *prefix = NULL;
                const char *sensor_type = NULL;

                // Resolve key prefix and optional sub-type
                switch (type) {
                    case ACD_TYPE_MQTT_COND:   prefix = "mqtt_condition"; break;
                    case ACD_TYPE_GPIO_COND:   prefix = "gpio_condition"; break;
                    case ACD_TYPE_EZO_PH:      prefix = "ph_sensor";   sensor_type = "ezo"; break;
                    case ACD_TYPE_EZO_ORP:     prefix = "orp_sensor";  sensor_type = "ezo"; break;
                    case ACD_TYPE_EZO_PRS:     prefix = "prs_sensor";  sensor_type = "ezo"; break;
                    case ACD_TYPE_EZO_TEMP:    prefix = "temp_sensor"; sensor_type = "ezo"; break;
                    case ACD_TYPE_EZO_EC:      prefix = "ec_sensor";   sensor_type = "ezo"; break;
                    case ACD_TYPE_MQTT_TEMP:   prefix = "temp_sensor"; sensor_type = "mqtt"; break;
                    case ACD_TYPE_D1W_TEMP:    prefix = "temp_sensor"; sensor_type = "d1w"; break;
                    case ACD_TYPE_SYSFS_VALUE: prefix = "sysfs_sensor"; break;
                    case ACD_TYPE_GPIO_PMP:    prefix = "gpio_doser"; break;
                    case ACD_TYPE_GPIO_OUTPUT: prefix = "gpio_output"; break;
                    case ACD_TYPE_GPIO_INPUT:  prefix = "gpio_input"; break;
                    case ACD_TYPE_MQTT_VALUE:  prefix = "mqtt_sensor"; break;
                    case ACD_TYPE_I2C_PRS:     prefix = "prs_sensor"; break;
                    //default: break;
                }
                // Write parameters sequentially if valid type matched
                if (prefix)
                {
                    fprintf(fp, "%s_label=%s\n", prefix, lbl);
                    // Only print ID if it exists and is not an empty string
                    if (id && id[0] != '\0') fprintf(fp, "%s_ID=%s\n", prefix, id);
                    if (sensor_type) fprintf(fp, "%s_type=%s\n", prefix, sensor_type);
                }
            }

            // --- NOW safely iterate through the rest of the fields ---
            cJSON *b_fields = cJSON_GetObjectItem(block, "fields");
            if (cJSON_IsArray(b_fields)) {
                cJSON *fld;
                cJSON_ArrayForEach(fld, b_fields) {
                    cJSON *k = cJSON_GetObjectItem(fld, "key");
                    cJSON *v = cJSON_GetObjectItem(fld, "value");
                    if (!k || !v || !cJSON_IsString(k)) continue;

                    char val_str[256] = {0};
                    if (cJSON_IsString(v)) {
                        snprintf(val_str, sizeof(val_str), "%s", v->valuestring);
                    } else if (cJSON_IsNumber(v)) {
                        if (v->valueint == v->valuedouble) snprintf(val_str, sizeof(val_str), "%d", v->valueint);
                        else snprintf(val_str, sizeof(val_str), "%.6g", v->valuedouble);
                    } else if (cJSON_IsBool(v)) {
                        //snprintf(val_str, sizeof(val_str), "%s", cJSON_IsTrue(v) ? "true" : "false");
                        snprintf(val_str, sizeof(val_str), "%s", bool_to_str(cJSON_IsTrue(v)));
                    }
                    
                    if (is_valid_config(k->valuestring, val_str)) {
                        // Skip re-writing the label if the UI accidentally included it in the fields array, or if the value is empty
                        if (strstr(k->valuestring, "_label") == NULL && strlen(val_str) > 0 ) {
                            fprintf(fp, "%s=%s\n", k->valuestring, val_str);
                        }
                    } else {
                        validation_failed = true;
                        break;
                    }
                }
            }
            if (validation_failed) break;
        }
    }


    fclose(fp);
    cJSON_Delete(root);

    // 3. APPLY OR REJECT
    if (validation_failed) {
        remove(tmp_file); // Clean up the garbage file
        snprintf(outBuf, outSize, "{\"status\":\"error\",\"message\":\"Configuration failed validation rules.\"}");
        LOG(LOG_ERR, "Web UI config update rejected due to validation failure.");
        return -1;
    } else {
        preserve_file_permissions(_acdconfig_.config_file, tmp_file);
        // Move temp file to actual config path (atomic operation on POSIX systems)
        if (rename(tmp_file, _acdconfig_.config_file) == 0) {
            snprintf(outBuf, outSize, "{\"status\":\"success\",\"message\":\"Configuration saved. Daemon is restarting...\"}");
            LOG(LOG_NOTICE, "Configuration updated via Web UI. Requesting soft restart.");
            
            // SIGNAL THE MAIN THREAD TO RELOAD
            //aquachemd_request_reload();
            // Above not working yet.
            LOG(LOG_WARNING, "AquachemD must be restarted for configuration changes to take effect!");
            return 0;
        } else {
            snprintf(outBuf, outSize, "{\"status\":\"error\",\"message\":\"Failed to apply config file.\"}");
            LOG(LOG_ERR, "Failed to rename temp config file over main config.");
            return -1;
        }
    }
}


bool build_aquachem_config_json(char *buffer, size_t buf_size) {
    cJSON *root = cJSON_CreateObject();
    char tmp_buf[32];
    if (!root) return false;

    cJSON_AddStringToObject(root, "type", "config_schema");
    cJSON *fields = cJSON_AddArrayToObject(root, "fields");

    // 1. GLOBAL SYSTEM PARAMETERS
    for (int i = 0; i < CFG_PARAM_COUNT; i++) {
        if (_cfgParams[i].config_mask & (CFG_HIDE | CFG_MULTIPLE)){
          if ( _cfgParams[i].value_ptr != &_acdconfig_.ph_steps &&
               _cfgParams[i].value_ptr != &_acdconfig_.orp_steps) {
            continue;
          }
        } 

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", _cfgParams[i].name);
        cJSON_AddBoolToObject(item, "readonly", (_cfgParams[i].config_mask & CFG_READONLY) ? true : false);
        cJSON_AddBoolToObject(item, "advanced", (_cfgParams[i].config_mask & CFG_ADVANCED) ? true : false);
        
        if (_cfgParams[i].metadata) {
            cJSON *opts = cJSON_Parse(_cfgParams[i].metadata);
            if (opts) cJSON_AddItemToObject(item, "options", opts);
        }

        switch (_cfgParams[i].value_type) {
            case CFG_STRING:
                cJSON_AddStringToObject(item, "type", "text");

                if (_cfgParams[i].config_mask & CFG_PASSWD_MASK) {
                  cJSON_AddStringToObject(item, "value", PASSWD_MASK_TEXT);
                } else {
                  cJSON_AddStringToObject(item, "value", *(char **)_cfgParams[i].value_ptr ? *(char **)_cfgParams[i].value_ptr : "");
                }
                break;
            case CFG_INT:
                cJSON_AddStringToObject(item, "type", "number");
                cJSON_AddNumberToObject(item, "value", *(int *)_cfgParams[i].value_ptr);
                break;
            case CFG_BOOL:
                cJSON_AddStringToObject(item, "type", "boolean");
                cJSON_AddBoolToObject(item, "value", *(bool *)_cfgParams[i].value_ptr);
                break;
            case CFG_FLOAT:
                cJSON_AddStringToObject(item, "type", "number");
                cJSON_AddFloat(item, "value", *(float *)_cfgParams[i].value_ptr);
                break;
            case CFG_TXT_INT:
                cJSON_AddStringToObject(item, "type", "select");
                if (_cfgParams[i].value_ptr == &_acdconfig_.log_level) {
                    cJSON_AddStringToObject(item, "value", log_priority_to_str(_acdconfig_.log_level));
                }
                break;
            case CFG_CUSTOM:
              if (_cfgParams[i].value_ptr == &_acdconfig_.ph_steps || _cfgParams[i].value_ptr == &_acdconfig_.orp_steps) {
                uint8_t step_cnt;
                runtime_range_t *steps;
                int decimal = 0;
                char range[12];
                if (_cfgParams[i].value_ptr == &_acdconfig_.ph_steps) {
                  step_cnt = _acdconfig_.ph_step_count;
                  steps = _acdconfig_.ph_steps;
                  decimal = 2;
                } else {
                  step_cnt = _acdconfig_.orp_step_count;
                  steps = _acdconfig_.orp_steps;
                  decimal = 0;
                }
                cJSON_AddStringToObject(item, "type", "array_text");
                cJSON *val_array = cJSON_CreateArray();
                for (int s = 0; s < step_cnt; s++) {
                  snprintf( range, sizeof(range), "%.*f:%d",decimal,steps[s].threshold, steps[s].seconds);
                  cJSON_AddItemToArray(val_array, cJSON_CreateString(range));
                }
                cJSON_AddItemToObject(item, "value", val_array);
              }
              break;
            default:
                cJSON_AddStringToObject(item, "type", "text");
                break;
        }
        cJSON_AddItemToArray(fields, item);
    }

    // 2. DYNAMIC CUSTOM HARDWARE BLOCKS 
    cJSON *custom_blocks = cJSON_AddArrayToObject(root, "custom_blocks");
    acd_key_t *curr = _acdconfig_.keys;
    
    while (curr != NULL) {
 
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "label", curr->label ? curr->label : "");
        cJSON_AddStringToObject(block, "ID", curr->ID ? curr->ID : "");
        cJSON_AddNumberToObject(block, "block_type_id", (double)curr->type);
        
        cJSON *block_fields = cJSON_AddArrayToObject(block, "fields");
        cJSON *f_item; 
        cJSON *s_item;
        cJSON *t_item;
        

        // Any virtual device is created from another sensor, so ignore
        if (isMASKSET(curr->flags, ACD_FLAG_VIRTUAL)) {
            //LOG(LOG_DEBUG, "Device %s is virtual, ignoring for config\n",curr->label);
            curr = curr->next;
            continue;
        }

        switch (curr->type) {
            case ACD_TYPE_MQTT_COND:
                cJSON_AddStringToObject(block, "driver_type", "mqtt_condition");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_condition_topic");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", curr->data.mqtt.topic ? curr->data.mqtt.topic : "");
                cJSON_AddItemToArray(block_fields, f_item);
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_condition_value");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", curr->data.mqtt.target_value ? curr->data.mqtt.target_value : "");
                cJSON_AddItemToArray(block_fields, f_item);
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_condition_severity");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_condition_met_delay");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->delay_on);
                cJSON_AddItemToArray(block_fields, f_item);
                break;

            case ACD_TYPE_GPIO_COND:
                cJSON_AddStringToObject(block, "driver_type", "gpio_condition");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_condition_pin");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->data.gpio.pin);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_condition_pin_mode");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_active_to_str(curr->data.gpio.active));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ACTIVE));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_condition_required_state");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_req_to_str(curr->data.gpio.required));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ONOFF));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_condition_severity");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_condition_met_delay");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->delay_on);
                cJSON_AddItemToArray(block_fields, f_item);
                break;

            case ACD_TYPE_EZO_TEMP:
            case ACD_TYPE_EZO_PH:
            case ACD_TYPE_EZO_ORP:
            case ACD_TYPE_EZO_PRS:
            case ACD_TYPE_EZO_EC:
                cJSON_AddStringToObject(block, "driver_type", "ezo_sensor");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", (curr->type == ACD_TYPE_EZO_PH) ? "ph_sensor_address" : 
                                                       (curr->type == ACD_TYPE_EZO_ORP) ? "orp_sensor_address" : 
                                                       (curr->type == ACD_TYPE_EZO_PRS) ? "prs_sensor_address" : 
                                                       (curr->type == ACD_TYPE_EZO_EC) ? "ec_sensor_address" : "temp_sensor_address");

                cJSON_AddStringToObject(f_item, "type", "text"); 
                cJSON_AddBoolToObject(f_item, "readonly", false);
                snprintf(tmp_buf, sizeof(tmp_buf), "0x%02x", curr->data.ezo.address);
                cJSON_AddStringToObject(f_item, "value", tmp_buf);
                cJSON_AddItemToArray(block_fields, f_item);

                if (curr->type == ACD_TYPE_EZO_EC) {
                    f_item = cJSON_CreateObject();
                    cJSON_AddStringToObject(f_item, "key", "ec_sensor_k");
                    cJSON_AddStringToObject(f_item, "type", "number");
                    cJSON_AddBoolToObject(f_item, "readonly", false);
                    cJSON_AddFloat(f_item, "value", curr->data.ezo.ec_sensor_k);
                    cJSON_AddItemToArray(block_fields, f_item);
                    f_item = cJSON_CreateObject();
                    cJSON_AddStringToObject(f_item, "key", "ec_sensor_tds_factor");
                    cJSON_AddStringToObject(f_item, "type", "number");
                    cJSON_AddBoolToObject(f_item, "readonly", false);
                    cJSON_AddFloat(f_item, "value", curr->data.ezo.ec_sensor_tds_factor);
                    cJSON_AddItemToArray(block_fields, f_item);
                }

                f_item = cJSON_CreateObject();
                s_item = cJSON_CreateObject();
                if (curr->type == ACD_TYPE_EZO_PH) {
                    cJSON_AddStringToObject(f_item, "key", "ph_sensor_interlock_scope");
                    cJSON_AddStringToObject(s_item, "key", "ph_sensor_statistics");
                }
                else if (curr->type == ACD_TYPE_EZO_ORP) {
                    cJSON_AddStringToObject(f_item, "key", "orp_sensor_interlock_scope");
                    cJSON_AddStringToObject(s_item, "key", "orp_sensor_statistics");
                }
                else if (curr->type == ACD_TYPE_EZO_PRS) {
                    cJSON_AddStringToObject(f_item, "key", "prs_sensor_interlock_scope");
                    cJSON_AddStringToObject(s_item, "key", "prs_sensor_statistics");
                }
                else if (curr->type == ACD_TYPE_EZO_EC) {
                    cJSON_AddStringToObject(f_item, "key", "ec_sensor_interlock_scope");
                    cJSON_AddStringToObject(s_item, "key", "ec_sensor_statistics");
                }
                else {
                    cJSON_AddStringToObject(f_item, "key", "temp_sensor_interlock_scope");
                    cJSON_AddStringToObject(s_item, "key", "temp_sensor_statistics");
                }

                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                cJSON_AddStringToObject(s_item, "type", "text");
                cJSON_AddBoolToObject(s_item, "readonly", false);
                duration_seconds_to_string(curr->stats.tau_seconds, tmp_buf, sizeof(tmp_buf));
                cJSON_AddStringToObject(s_item, "value", curr->stats.tau_seconds > 0.0 ? tmp_buf : "");
                cJSON_AddItemToArray(block_fields, s_item);
                break;

            case ACD_TYPE_I2C_PRS:
                cJSON_AddStringToObject(block, "driver_type", "i2c_sensor");

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_type");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", i2c_get_driver_name(curr->data.i2c.type));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_I2C_PRS_DRIVERS));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_address");
                cJSON_AddStringToObject(f_item, "type", "text"); 
                cJSON_AddBoolToObject(f_item, "readonly", false);
                snprintf(tmp_buf, sizeof(tmp_buf), "0x%02x", curr->data.i2c.address);  
                cJSON_AddStringToObject(f_item, "value", tmp_buf);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_statistics");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                duration_seconds_to_string(curr->stats.tau_seconds, tmp_buf, sizeof(tmp_buf));
                cJSON_AddStringToObject(f_item, "value", curr->stats.tau_seconds > 0.0 ? tmp_buf : "");
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_min_value");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.i2c.min_value);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "prs_sensor_max_value");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.i2c.max_value);
                cJSON_AddItemToArray(block_fields, f_item);
                
                break;
            

            case ACD_TYPE_MQTT_TEMP:
                cJSON_AddStringToObject(block, "driver_type", "mqtt_temp_sensor");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_topic");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", curr->data.mqtt.topic ? curr->data.mqtt.topic : "");
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                s_item = cJSON_CreateObject();
                cJSON_AddStringToObject(s_item, "key", "temp_sensor_statistics");
                cJSON_AddStringToObject(s_item, "type", "text");
                cJSON_AddBoolToObject(s_item, "readonly", false);
                duration_seconds_to_string(curr->stats.tau_seconds, tmp_buf, sizeof(tmp_buf));
                cJSON_AddStringToObject(s_item, "value", curr->stats.tau_seconds > 0.0 ? tmp_buf : "");
                cJSON_AddItemToArray(block_fields, s_item);
                break;

            case ACD_TYPE_D1W_TEMP:
                cJSON_AddStringToObject(block, "driver_type", "d1w_sensor");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_path");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", (curr->data.w1.path[0] != '\0') ? curr->data.w1.path : "");
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_offset");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.w1.offset);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_scale");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.w1.scale);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "temp_sensor_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                s_item = cJSON_CreateObject();
                cJSON_AddStringToObject(s_item, "key", "temp_sensor_statistics");
                cJSON_AddStringToObject(s_item, "type", "text");
                cJSON_AddBoolToObject(s_item, "readonly", false);
                duration_seconds_to_string(curr->stats.tau_seconds, tmp_buf, sizeof(tmp_buf));
                cJSON_AddStringToObject(s_item, "value", curr->stats.tau_seconds > 0.0 ? tmp_buf : "");
                cJSON_AddItemToArray(block_fields, s_item);
                break;

            case ACD_TYPE_GPIO_PMP:
                cJSON_AddStringToObject(block, "driver_type", "gpio_doser");
                
                //bool is_ph = (curr->flags & PH_PUMP);
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_type");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", pump_type_to_str(curr->flags));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_PMP_TYPE));
                cJSON_AddItemToArray(block_fields, f_item);
         
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_pin");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->data.gpio.pin);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_pin_mode");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_active_to_str(curr->data.gpio.active));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ACTIVE));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_ml_per_second");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->dose_stats.flow_rate);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_running_dose_max_ml");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->dose_stats.running_total_max_ml);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                s_item = cJSON_CreateObject();
                t_item = cJSON_CreateObject();

                cJSON_AddStringToObject(f_item, "key", "gpio_doser_tank_total_volume");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);

                cJSON_AddStringToObject(s_item, "key", "gpio_doser_tank_uom");
                cJSON_AddStringToObject(s_item, "type", "select");
                cJSON_AddItemToObject(s_item, "options", cJSON_Parse(CFG_O_TANK_UOM));
                cJSON_AddBoolToObject(s_item, "readonly", false);

                cJSON_AddStringToObject(t_item, "key", "gpio_doser_tank_min_volume");
                cJSON_AddStringToObject(t_item, "type", "number");
                cJSON_AddBoolToObject(t_item, "readonly", false);

                if (curr->child && curr->child->type == ACD_TYPE_VIR_TANK) {
                  cJSON_AddNumberToObject(f_item, "value", curr->child->data.tank.total_volume);
                  cJSON_AddStringToObject(s_item, "value", uom_to_fullstr(curr->child->data.tank.uom));
                  cJSON_AddNumberToObject(t_item, "value", curr->child->data.tank.min_volume);
                } else {
                  cJSON_AddStringToObject(s_item, "value", "");
                }
                cJSON_AddItemToArray(block_fields, f_item);
                cJSON_AddItemToArray(block_fields, s_item);
                cJSON_AddItemToArray(block_fields, t_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_doser_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                break;

            case ACD_TYPE_EZO_PMP:
                cJSON_AddStringToObject(block, "driver_type", "EZO pump");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "ezo_doser_address");
                cJSON_AddStringToObject(f_item, "type", "text"); 
                cJSON_AddBoolToObject(f_item, "readonly", false);
                snprintf(tmp_buf, sizeof(tmp_buf), "0x%02x", curr->data.ezo.address);
                cJSON_AddStringToObject(f_item, "value", tmp_buf);
                cJSON_AddItemToArray(block_fields, f_item);

                //bool is_ph = (curr->flags & PH_PUMP);
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "ezo_doser_type");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", pump_type_to_str(curr->flags));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_PMP_TYPE));
                cJSON_AddItemToArray(block_fields, f_item);
         
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "ezo_doser_ml_per_second");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->dose_stats.flow_rate);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "ezo_doser_running_dose_max_ml");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->dose_stats.running_total_max_ml);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                s_item = cJSON_CreateObject();
                t_item = cJSON_CreateObject();

                cJSON_AddStringToObject(f_item, "key", "ezo_doser_tank_total_volume");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);

                cJSON_AddStringToObject(s_item, "key", "ezo_doser_tank_uom");
                cJSON_AddStringToObject(s_item, "type", "select");
                cJSON_AddItemToObject(s_item, "options", cJSON_Parse(CFG_O_TANK_UOM));
                cJSON_AddBoolToObject(s_item, "readonly", false);

                cJSON_AddStringToObject(t_item, "key", "ezo_doser_tank_min_volume");
                cJSON_AddStringToObject(t_item, "type", "number");
                cJSON_AddBoolToObject(t_item, "readonly", false);

                if (curr->child && curr->child->type == ACD_TYPE_VIR_TANK) {
                  cJSON_AddNumberToObject(f_item, "value", curr->child->data.tank.total_volume);
                  cJSON_AddStringToObject(s_item, "value", uom_to_fullstr(curr->child->data.tank.uom));
                  cJSON_AddNumberToObject(t_item, "value", curr->child->data.tank.min_volume);
                } else {
                  cJSON_AddStringToObject(s_item, "value", "");
                }
                cJSON_AddItemToArray(block_fields, f_item);
                cJSON_AddItemToArray(block_fields, s_item);
                cJSON_AddItemToArray(block_fields, t_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "ezo_doser_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToArray(block_fields, f_item);

                break;

            case ACD_TYPE_GPIO_OUTPUT:
                cJSON_AddStringToObject(block, "driver_type", "GPIO Output");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_output_pin");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->data.gpio.pin);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_output_pin_mode");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_active_to_str(curr->data.gpio.active));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ACTIVE));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_output_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE_FULL));
                cJSON_AddItemToArray(block_fields, f_item);

                break;

            case ACD_TYPE_GPIO_INPUT:
                cJSON_AddStringToObject(block, "driver_type", "GPIO Input");

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_input_pin");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddNumberToObject(f_item, "value", curr->data.gpio.pin);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_input_pin_mode");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_active_to_str(curr->data.gpio.active));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ACTIVE));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_input_required_state");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", gpio_req_to_str(curr->data.gpio.required));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_ONOFF));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "gpio_input_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddItemToArray(block_fields, f_item);
                
                break;
            
            case ACD_TYPE_SYSFS_VALUE:
                cJSON_AddStringToObject(block, "driver_type", "sysfs_sensor");
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_path");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", (curr->data.sysfs.path[0] != '\0') ? curr->data.sysfs.path : "");
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_offset");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.sysfs.offset);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_scale");
                cJSON_AddStringToObject(f_item, "type", "number");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddFloat(f_item, "value", curr->data.sysfs.multiplier);
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_regex");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", (curr->data.sysfs.regex_pattern != NULL) ? curr->data.sysfs.regex_pattern : "");
                cJSON_AddItemToArray(block_fields, f_item);         
                
                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_uom");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_UOM_LIST));
                cJSON_AddStringToObject(f_item, "value", uom_to_fullstr(curr->uom));
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "sysfs_sensor_interlock_scope");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", acd_scope_to_str(curr->scope));
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_SCOPE));
                cJSON_AddItemToArray(block_fields, f_item);
                break;

            case ACD_TYPE_MQTT_VALUE:
                cJSON_AddStringToObject(block, "driver_type", "mqtt_value_sensor");

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_sensor_topic");
                cJSON_AddStringToObject(f_item, "type", "text");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddStringToObject(f_item, "value", curr->data.mqtt.topic ? curr->data.mqtt.topic : "");
                cJSON_AddItemToArray(block_fields, f_item);

                f_item = cJSON_CreateObject();
                cJSON_AddStringToObject(f_item, "key", "mqtt_sensor_uom");
                cJSON_AddStringToObject(f_item, "type", "select");
                cJSON_AddBoolToObject(f_item, "readonly", false);
                cJSON_AddItemToObject(f_item, "options", cJSON_Parse(CFG_O_UOM_LIST));
                cJSON_AddStringToObject(f_item, "value", uom_to_fullstr(curr->uom));
                cJSON_AddItemToArray(block_fields, f_item);
                break;

            

            case ACD_TYPE_VIR_TANK: // Virtual tank will never get here, but stop the compiler warning
            case ACD_TYPE_I2C_TEMP: // This is only virtually supported at present, so shouldn't get here.
            case ACD_TYPE_NONE:
            case ACD_TYPE_MASTER:
                break;
        }
        
        cJSON_AddItemToArray(custom_blocks, block);
        curr = curr->next;
    }



    // 3. MASTER DEFINITIONS FOR CREATING NEW BLOCKS
    cJSON *available_drivers = cJSON_AddArrayToObject(root, "available_drivers");
    cJSON *drv, *df_arr, *df_item;


    // Define: MQTT Condition Template
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_MQTT_COND);
    cJSON_AddStringToObject(drv, "driver_type", "mqtt_condition");
    cJSON_AddStringToObject(drv, "default_label", "New MQTT Condition");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_condition_topic");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);
    //cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_condition_value");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_condition_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);
    //cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_condition_met_delay");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: GPIO Condition Template
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_COND);
    cJSON_AddStringToObject(drv, "driver_type", "gpio_condition");
    cJSON_AddStringToObject(drv, "default_label", "New GPIO Condition");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_condition_pin");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_condition_pin_mode");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Active High");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ACTIVE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_condition_required_state");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "off");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ONOFF));
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_condition_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_condition_met_delay");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    // Define: 1-Wire Temperature Template
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_D1W_TEMP);
    cJSON_AddStringToObject(drv, "driver_type", "d1w_sensor");
    cJSON_AddStringToObject(drv, "default_label", "New 1-Wire Sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_path");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "/sys/bus/w1/devices/");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_offset");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_scale");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddFloat(df_item, "value", 1.0);
    cJSON_AddItemToArray(df_arr, df_item);
    //cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "None");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));

    cJSON_AddItemToArray(available_drivers, drv);

    // Define: MQTT Temp Sensor Template (Added)
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_MQTT_TEMP);
    cJSON_AddStringToObject(drv, "driver_type", "mqtt_temp_sensor");
    cJSON_AddStringToObject(drv, "default_label", "New MQTT Temp Sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_topic");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_uom");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_UOM_TEMPERATURE));
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    // Define: MQTT Value Sensor Template (Added)
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_MQTT_VALUE);
    cJSON_AddStringToObject(drv, "driver_type", "mqtt_value_sensor");
    cJSON_AddStringToObject(drv, "default_label", "New MQTT Value Sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_sensor_topic");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "mqtt_sensor_uom");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_UOM_LIST));
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    // Define: SYSFS Sensor Template (Added)
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_SYSFS_VALUE);
    cJSON_AddStringToObject(drv, "driver_type", "sysfs_sensor");
    cJSON_AddStringToObject(drv, "default_label", "New SysFS Sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_path");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_offset");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_scale");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddFloat(df_item, "value", 1.0);
    cJSON_AddItemToArray(df_arr, df_item);
    
    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_regex");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_uom");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_UOM_LIST));
    cJSON_AddItemToArray(df_arr, df_item);
    cJSON_AddItemToArray(available_drivers, drv);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "sysfs_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Local");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);


    // Define: GPIO Doser Template
    drv = cJSON_CreateObject();
    //cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_PMP);
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_PMP);
    cJSON_AddStringToObject(drv, "driver_type", "gpio_doser");
    cJSON_AddStringToObject(drv, "default_label", "New GPIO Chemical Doser");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_type");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "gpio");
    //cJSON_AddItemToObject(df_item, "options", cJSON_Parse("[\"ph\",\"orp\"]"));
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_PMP_TYPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_pin");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_pin_mode");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Active Low");
    //cJSON_AddItemToObject(df_item, "options", cJSON_Parse("[\"Active High\",\"Active Low\"]"));
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ACTIVE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_ml_per_second");
    cJSON_AddStringToObject(df_item, "type", "number");
    //cJSON_AddFloat(df_item, "value", 1.0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_running_dose_max_ml");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_tank_total_volume");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_tank_uom");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_TANK_UOM));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_tank_min_volume");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_doser_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: GPIO Output Template
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_OUTPUT);
    cJSON_AddStringToObject(drv, "driver_type", "GPIO");
    cJSON_AddStringToObject(drv, "default_label", "New GPIO Output");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_output_pin");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_output_pin_mode");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Active Low");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ACTIVE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_output_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Allow");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE_FULL));
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);

    // Define: GPIO  input  Template
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_INPUT);
    cJSON_AddStringToObject(drv, "driver_type", "GPIO");
    cJSON_AddStringToObject(drv, "default_label", "New GPIO Input");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_input_type");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Output");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_GPIO_DIRECTION));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_input_pin");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddNumberToObject(df_item, "value", 0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_input_pin_mode");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Active Low");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ACTIVE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_input_required_state");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "on");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_ONOFF));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "gpio_input_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: EZO ph
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_PH);
    cJSON_AddStringToObject(drv, "driver_type", "ezo");
    cJSON_AddStringToObject(drv, "default_label", "New EZO pH sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ph_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ph_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: EZO ORP
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_ORP);
    cJSON_AddStringToObject(drv, "driver_type", "ezo");
    cJSON_AddStringToObject(drv, "default_label", "New EZO ORP sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "orp_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "orp_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);

    // Define: EZO TEMP
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_TEMP);
    cJSON_AddStringToObject(drv, "driver_type", "ezo");
    cJSON_AddStringToObject(drv, "default_label", "New EZO Temp sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "temp_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);

    // Define: EZO PRS
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_PRS);
    cJSON_AddStringToObject(drv, "driver_type", "ezo");
    cJSON_AddStringToObject(drv, "default_label", "New EZO Pressure sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "prs_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "prs_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);

    // Define: EZO EC
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_EC);
    cJSON_AddStringToObject(drv, "driver_type", "ezo");
    cJSON_AddStringToObject(drv, "default_label", "New EZO Conductivity sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");
/*
    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ec_sensor_address");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    snprintf(tmp_buf, sizeof(tmp_buf), "0x%02x", EZO_EC_ADDR);  
    cJSON_AddStringToObject(df_item, "value", tmp_buf);
    cJSON_AddItemToArray(df_arr, df_item);
*/
    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ec_sensor_k");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddNumberToObject(df_item, "value", EC_DEFAULT_K);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ec_sensor_tds_factor");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddNumberToObject(df_item, "value", EC_DEFAULT_TDS_FACTOR);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ec_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ec_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: I2C PRS
    drv = cJSON_CreateObject();
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_I2C_PRS);
    cJSON_AddStringToObject(drv, "driver_type", "i2c sensor");
    cJSON_AddStringToObject(drv, "default_label", "New I2C Pressure sensor");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "prs_sensor_type");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_I2C_PRS_DRIVERS));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "prs_sensor_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "prs_sensor_statistics");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);


    // Define: EZO Doser Template
    drv = cJSON_CreateObject();
    //cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_GPIO_PMP);
    cJSON_AddNumberToObject(drv, "block_type_id", ACD_TYPE_EZO_PMP);
    cJSON_AddStringToObject(drv, "driver_type", "ezo_doser");
    cJSON_AddStringToObject(drv, "default_label", "New EZO Chemical Doser");
    df_arr = cJSON_AddArrayToObject(drv, "fields");

    
    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_address");
    cJSON_AddStringToObject(df_item, "type", "text");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    snprintf(tmp_buf, sizeof(tmp_buf), "0x%02x", EZO_PMP_ADDR);  
    cJSON_AddStringToObject(df_item, "value", tmp_buf);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_type");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "ezo");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_PMP_TYPE));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_ml_per_second");
    cJSON_AddStringToObject(df_item, "type", "number");
    //cJSON_AddFloat(df_item, "value", 1.0);
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_running_dose_max_ml");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_tank_total_volume");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_tank_uom");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddStringToObject(df_item, "value", "");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_TANK_UOM));
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_tank_min_volume");
    cJSON_AddStringToObject(df_item, "type", "number");
    cJSON_AddItemToArray(df_arr, df_item);

    df_item = cJSON_CreateObject();
    cJSON_AddStringToObject(df_item, "key", "ezo_doser_interlock_scope");
    cJSON_AddStringToObject(df_item, "type", "select");
    cJSON_AddBoolToObject(df_item, "readonly", false);
    cJSON_AddStringToObject(df_item, "value", "Global");
    cJSON_AddItemToObject(df_item, "options", cJSON_Parse(CFG_O_SCOPE));
    cJSON_AddItemToArray(df_arr, df_item);

    cJSON_AddItemToArray(available_drivers, drv);

    // 3. RENDER TO MEMORY BUFFER
    bool success = cJSON_PrintPreallocated(root, buffer, (int)buf_size, 0);
    if (!success) {
        snprintf(buffer, buf_size, "{\"error\":\"buffer_overflow\"}");
    }

    cJSON_Delete(root);
    return success;
}




// config.c — local to this file. Not the same as utils.c's acd_scope_to_str(),
// which prints the bare enum name for routine runtime logs; this one spells
// out what scope actually does for the specific node kind, since this is the
// one place a user is trying to understand their config from scratch.
static const char *config_scope_detail_str(acd_key_t *key)
{
  if (IS_CONDITION(key->type)) {
    switch (key->scope) {
      case ACD_SCOPE_GLOBAL: return "Global (Hard Interlock)";
      case ACD_SCOPE_LOCAL:  return "Local (Soft Limit)";
      default:                return "Allow";
    }
  } else {
    switch (key->scope) {
      case ACD_SCOPE_GLOBAL: return "Global (paused during interlock)";
      case ACD_SCOPE_LOCAL:  return "Local (always reads)";
      default:                return "Allow";
    }
  }
}

#define MAX_PRINTLEN 25

void print_config (struct aquachemdata *acdata)
{
  int i;
  char name[MAX_PRINTLEN];
  //uint32_t errors=0;

  // Should already be set, but just in case, make conditions available.
  //acdata->conditions = _acdconfig_.conditions; // Make conditions available in main data struct for future use.

  acdata->keys->next = _acdconfig_.keys; // Make sensors available in main data struct keys, first key is pre-set 


  // Anything that's not in the config table should be added as a special case here until it's added to the table. This is for handling things that need to be displayed in a special way or that aren't actually stored in the config struct but are still important to display.
  LOG(LOG_NOTICE, "%-*s = %s\n",MAX_PRINTLEN,"Configuration file", _acdconfig_.config_file);


  for ( i=0; i < CFG_PARAM_COUNT; i++) {

    // don't print mg_log_level if it's 0 since it's only used for debugging and would just add confusion to users looking at the config
    if (_cfgParams[i].value_ptr == &_acdconfig_.mg_log_level && *(int *)_cfgParams[i].value_ptr == 0) {
      continue;
    }

    // Print these after.
    if (_cfgParams[i].config_mask & CFG_MULTIPLE) {
      continue; 
    }

    strcsub(name, MAX_PRINTLEN, _cfgParams[i].name, '_', ' ');
    switch (_cfgParams[i].value_type) {
      case CFG_STRING:
        if (*(char **)_cfgParams[i].value_ptr == NULL)
          LOG(LOG_NOTICE, "%-*s =\n", MAX_PRINTLEN, name);
        else {
          if (isMASKSET(_cfgParams[i].config_mask ,CFG_PASSWD_MASK) )
            LOG(LOG_NOTICE, "%-*s = %s\n", MAX_PRINTLEN, name, PASSWD_MASK_TEXT);
          else
            LOG(LOG_NOTICE, "%-*s = %s\n", MAX_PRINTLEN, name, *(char **)_cfgParams[i].value_ptr);
        }
      break;
      case CFG_INT:
        if (*(int *)_cfgParams[i].value_ptr == UNKNOWN)
          LOG(LOG_NOTICE, "%-*s =\n", MAX_PRINTLEN, name);
        else
          LOG(LOG_NOTICE, "%-*s = %d\n", MAX_PRINTLEN, name, *(int *)_cfgParams[i].value_ptr);
      break;
      case CFG_BOOL:
        LOG(LOG_NOTICE, "%-*s = %s\n", MAX_PRINTLEN, name, bool_to_str(*(bool *)_cfgParams[i].value_ptr));
      break;
      case CFG_HEX:
        LOG(LOG_NOTICE, "%-*s = 0x%02hhx\n", MAX_PRINTLEN, name, *(unsigned char *)_cfgParams[i].value_ptr);
      break;
      case CFG_FLOAT:
        LOG(LOG_NOTICE, "%-*s = %f\n", MAX_PRINTLEN, name, *(float *)_cfgParams[i].value_ptr);
      break;
      case CFG_BITMASK:
        LOG(LOG_NOTICE, "%-*s = %s\n", MAX_PRINTLEN, name, (*(uint16_t *)_cfgParams[i].value_ptr & _cfgParams[i].bit_flag) == _cfgParams[i].bit_flag?bool_to_str(true):bool_to_str(false));
      break;
      case CFG_TXT_INT:
        if (_cfgParams[i].value_ptr == &_acdconfig_.log_level) {
          LOG(LOG_NOTICE, "%-*s = %s\n", MAX_PRINTLEN, name, log_priority_to_str(_acdconfig_.log_level));
        } else {
          LOG(LOG_NOTICE, "%-*s = NEED TO ADD CODE TO HANDLE THIS\n", MAX_PRINTLEN, name);
        }
     break;
      case CFG_CUSTOM:
        //LOG(LOG_WARNING, "print_config() ADD SPECIAL CONFIG FOR '%s'\n",_cfgParams[i].name);
      break;
    }
  }


  if (_acdconfig_.ph_step_count > 0) {
    for (int s = 0; s < _acdconfig_.ph_step_count; s++) {
      LOG(LOG_NOTICE, "%-*s = >= %.2f (%ds)\n",MAX_PRINTLEN, "pH Dosing Range", _acdconfig_.ph_steps[s].threshold, _acdconfig_.ph_steps[s].seconds);
    }
  }
  if (_acdconfig_.orp_step_count > 0) {
    for (int s = 0; s < _acdconfig_.orp_step_count; s++) {
      LOG(LOG_NOTICE, "%-*s = <= %.2f (%ds)\n",MAX_PRINTLEN, "ORP Dosing Range", _acdconfig_.orp_steps[s].threshold, _acdconfig_.orp_steps[s].seconds);
    }
  }

  for (acd_key_t *curr = _acdconfig_.keys; curr != NULL; curr = curr->next) {
    const char *type_str = "UNKNOWN";
    char buffer[64];
    const char *role = (curr->flags & PH_PUMP)  ? "pH" :
                       (curr->flags & ORP_PUMP) ? "ORP" :
                       (curr->flags & H2O_PUMP) ? "H2O" : "undefined";
    buffer[0] = '\0';

    switch (curr->type) {
      case ACD_TYPE_MQTT_COND:   type_str = "condition (MQTT)"; break;
      case ACD_TYPE_GPIO_COND:   type_str = "condition (GPIO)"; break;
      case ACD_TYPE_EZO_TEMP:    type_str = "sensor (EZO Temp)"; break;
      case ACD_TYPE_EZO_PH:      type_str = "sensor (EZO pH)"; break;
      case ACD_TYPE_EZO_ORP:     type_str = "sensor (EZO ORP)"; break;
      case ACD_TYPE_EZO_PRS:     type_str = "sensor (EZO Pressure)"; break; // was mislabeled "EZO PMP"
      case ACD_TYPE_EZO_EC:      type_str = "sensor (EZO Conductivity)"; break;
      case ACD_TYPE_D1W_TEMP:    type_str = "sensor (1-Wire Temp)"; break;
      case ACD_TYPE_MQTT_TEMP:   type_str = "sensor (MQTT Temp)"; break;
      case ACD_TYPE_SYSFS_VALUE: type_str = "sensor (System File)"; break;
      case ACD_TYPE_MQTT_VALUE:  type_str = "sensor (MQTT Value)"; break;
      case ACD_TYPE_VIR_TANK:    type_str = "virtual sensor (Tank)"; break;
      case ACD_TYPE_GPIO_OUTPUT: type_str = "Output (GPIO)"; break;
      case ACD_TYPE_GPIO_INPUT:  type_str = "Input  (GPIO)"; break;
      case ACD_TYPE_I2C_PRS:
        type_str = "sensor (I2C Pressure)";
        snprintf(buffer, sizeof(buffer), "(%s)", curr->data.i2c.type == I2C_SENSOR_PTE7300 ? "PTE7300" : "UNKNOWN");
        break;
      case ACD_TYPE_I2C_TEMP:
        type_str = "sensor (I2C Temp)";
        if (curr->data.i2c.type == I2C_SENSOR_PTE7300){ snprintf(buffer, sizeof(buffer), "(PTE7300)");}
        break;
      case ACD_TYPE_GPIO_PMP:
        type_str = "pump (GPIO)";
        snprintf(buffer, sizeof(buffer), "(%s)", role);
        break;
      case ACD_TYPE_EZO_PMP:
        type_str = "pump (EZO PMP)";
        snprintf(buffer, sizeof(buffer), "(%s)", role);
        break;
      case ACD_TYPE_MASTER:
        break;
      case ACD_TYPE_NONE:
        default: type_str = "sensor (UNKNOWN)";
        break;
    }

    // GPIO pin/mode/required-state moved to the INFO block below -- NOTICE line
    // now only carries role (pump) or delay (condition), same as every other type.
    if (IS_CONDITION(curr->type)) {
      int len = strlen(buffer);
      snprintf(&buffer[len], sizeof(buffer)-len, "(delay=%ds)", curr->delay_on);
    }

    LOG(LOG_NOTICE, "%-*s %d = %-8.8s| %s %s\n", MAX_PRINTLEN, type_str, curr->index, curr->ID, curr->label, buffer);

    if (curr->type == ACD_TYPE_GPIO_PMP || curr->type == ACD_TYPE_GPIO_COND ||
        curr->type == ACD_TYPE_GPIO_OUTPUT || curr->type == ACD_TYPE_GPIO_INPUT) {
      LOG(LOG_INFO, "%-*s   -> pin = %d, mode = %s, required state = %s, scope = %s\n", MAX_PRINTLEN, "",
          curr->data.gpio.pin, gpio_active_to_str(curr->data.gpio.active), gpio_req_to_str(curr->data.gpio.required),
          config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_MQTT_COND) {
      LOG(LOG_INFO, "%-*s   -> MQTT topic = %s, expected value = %s, scope = %s\n", MAX_PRINTLEN, "",  curr->data.mqtt.topic, curr->data.mqtt.target_value, config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_MQTT_TEMP) {
      LOG(LOG_INFO, "%-*s   -> MQTT topic = %s, scope = %s\n", MAX_PRINTLEN, "", curr->data.mqtt.topic, config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_EZO_TEMP || curr->type == ACD_TYPE_EZO_PH || curr->type == ACD_TYPE_EZO_ORP) {
      LOG(LOG_INFO, "%-*s   -> I2C address = 0x%02x, scope = %s\n", MAX_PRINTLEN, "", curr->data.ezo.address, config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_EZO_EC) {
      if (isMASKSET(curr->data.ezo.flags, EC_CONDUCTIVITY)) {
        LOG(LOG_INFO, "%-*s   -> I2C address = 0x%02x, k=%.1f, tds=%.2f, scope = %s\n", MAX_PRINTLEN, "",
            curr->data.ezo.address, curr->data.ezo.ec_sensor_k, curr->data.ezo.ec_sensor_tds_factor, config_scope_detail_str(curr));
      } else {
        LOG(LOG_INFO, "%-*s   -> I2C address = 0x%02x, scope = %s\n", MAX_PRINTLEN, "", curr->data.ezo.address, config_scope_detail_str(curr));
      }
}

    if (curr->type == ACD_TYPE_D1W_TEMP) {
      LOG(LOG_INFO, "%-*s   -> path = %s, scale = %.4f, offset = %.2f, scope = %s\n", MAX_PRINTLEN, "",
          curr->data.w1.path, curr->data.w1.scale, curr->data.w1.offset, config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_I2C_PRS || curr->type == ACD_TYPE_I2C_TEMP) {
      LOG(LOG_INFO, "%-*s   -> I2C address = 0x%02x, range = %.2f to %.2f, scope = %s\n", MAX_PRINTLEN, "",
          curr->data.i2c.address, curr->data.i2c.min_value, curr->data.i2c.max_value, config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_MQTT_VALUE) {
      LOG(LOG_INFO, "%-*s   -> MQTT topic = %s%s%s\n", MAX_PRINTLEN, "", curr->data.mqtt.topic,
          curr->data.mqtt.target_value ? ", expected value = " : "",
          curr->data.mqtt.target_value ? curr->data.mqtt.target_value : "");
    }

    if (curr->type == ACD_TYPE_SYSFS_VALUE) {
      LOG(LOG_INFO, "%-*s   -> path = %s%s%s, multiplier = %.4f, offset = %.2f, scope = %s\n", MAX_PRINTLEN, "",
          curr->data.sysfs.path,
          curr->data.sysfs.parser_type == PARSER_REGEX ? ", regex = " : "",
          curr->data.sysfs.parser_type == PARSER_REGEX ? curr->data.sysfs.regex_pattern : "",
          curr->data.sysfs.multiplier, curr->data.sysfs.offset,
          config_scope_detail_str(curr));
    }

    if (curr->type == ACD_TYPE_VIR_TANK) {
      LOG(LOG_INFO, "%-*s   -> total volume = %.2f %s, min volume = %.2f %s\n", MAX_PRINTLEN, "",
          curr->data.tank.total_volume, uom_to_str(curr->data.tank.uom), curr->data.tank.min_volume, uom_to_str(curr->data.tank.uom));
    }

    if (curr->type == ACD_TYPE_GPIO_PMP || curr->type == ACD_TYPE_EZO_PMP) {
      LOG(LOG_INFO, "%-*s   -> flow rate = %.2f ml/s, max per period = %.2f ml, scope = %s\n", MAX_PRINTLEN, "",
          curr->dose_stats.flow_rate, curr->dose_stats.running_total_max_ml, config_scope_detail_str(curr));
    }
  }
}

// Ensures key->scope is always a real, usable value (ALLOW/LOCAL/GLOBAL) by the
// time this returns -- never ACD_SCOPE_UNKNOWN. This is the single point where
// an unrecognised or missing interlock scope gets resolved, so nothing
// downstream (classify_key(), the permission table, any 1 << scope shift) ever
// has to handle ACD_SCOPE_UNKNOWN itself. Must be called once for every key
// after its scope has been parsed from config.
//
// Defaults differ by device type, and are chosen to fail closed:
//   Conditions        -> GLOBAL. An interlock that silently gates nothing on
//                         a typo is the single worst failure mode this system
//                         has -- it looks configured but protects nothing.
//   Pumps             -> LOCAL. Matches classify_key()'s existing fallback:
//                         still respects a hard interlock, just not degraded
//                         to the most restrictive dosing behaviour.
//   Sensors/Inputs,
//   Plain outputs      -> GLOBAL. Default to trusting/running less, not more,
//                         when we don't know what was actually intended.
//   Everything else
//   (MASTER, VIR_TANK) -> no interlock concept -- left untouched.
void validate_interlock(acd_key_t *key) {
  if (!key) return;

  //if (key->scope == ACD_SCOPE_ALLOW || key->scope == ACD_SCOPE_LOCAL || key->scope == ACD_SCOPE_GLOBAL) {
  if (key->scope == ACD_SCOPE_LOCAL || key->scope == ACD_SCOPE_GLOBAL ||
     (key->scope == ACD_SCOPE_ALLOW && key->type == ACD_TYPE_GPIO_OUTPUT)) // Only GPIO_OUTPUT can be set to Allow (from config)
  {
    return; // already a real value
  }

  acd_scope_t fallback;
  if (IS_CONDITION(key->type)) {
    fallback = ACD_SCOPE_GLOBAL;
  } else if (IS_PUMP(key->type)) {
    fallback = ACD_SCOPE_LOCAL;
  } else if (IS_INPUT(key->type)) {
    fallback = ACD_SCOPE_LOCAL;
  } else if (key->type == ACD_TYPE_GPIO_OUTPUT) {
     fallback = ACD_SCOPE_GLOBAL;
  } else {
    return; // no interlock concept for this type -- nothing to validate
  }

  LOG(LOG_WARNING, "%s: interlock scope is missing or not valid, defaulting to %s\n",
      key->label, acd_scope_to_str(fallback));
  key->scope = fallback;
}


void validate_config(struct aquachemdata *acdata) {

  // make sure a valid poll time. ie >= 1
  _acdconfig_.sensor_poll_time = (_acdconfig_.sensor_poll_time < 1) ? 1 : _acdconfig_.sensor_poll_time;

  // Any other main config checks........

  // Check keys.
  for (acd_key_t *curr = acdata->keys->next; curr != NULL; curr = curr->next) {
    validate_interlock(curr);
    // any other checks need to happen in keys..........
  } 
}



// The set of distinct ID prefixes. Each gets its own independent
// counter/max-index-seen slot in assign_missing_ids(). This list only needs
// to stay in sync with prefix_for_type() below -- nothing else should ever
// duplicate this mapping.
typedef enum {
  ID_PREFIX_CS = 0,   // Conditions
  ID_PREFIX_PH,
  ID_PREFIX_ORP,
  ID_PREFIX_PRS,
  ID_PREFIX_TEMP,
  ID_PREFIX_PMP,
  ID_PREFIX_SYS,
  ID_PREFIX_MQT,
  ID_PREFIX_TNK,
  ID_PREFIX_GPIO,
  ID_PREFIX_UNK,
  ID_PREFIX_EC,
  NUM_ID_PREFIXES
} id_prefix_index_t;

// Single source of truth for "what prefix does this device type use" --
// shared by ID generation, config-load validation, and anything in the UI
// that groups or colors devices by type family (e.g. "all TEMP sensors").
// This is the exact switch that used to live inline inside generate_id().
const char *prefix_for_type(acd_type_t type) {
  switch (type) {
    case ACD_TYPE_MQTT_COND:
    case ACD_TYPE_GPIO_COND:      return "CS";
    case ACD_TYPE_EZO_PH:         return "PH";
    case ACD_TYPE_EZO_ORP:        return "ORP";
    case ACD_TYPE_EZO_EC:         return "EC";
    case ACD_TYPE_EZO_PRS:
    case ACD_TYPE_I2C_PRS:        return "PRS";
    case ACD_TYPE_EZO_TEMP:
    case ACD_TYPE_MQTT_TEMP:
    case ACD_TYPE_D1W_TEMP:
    case ACD_TYPE_I2C_TEMP:       return "TEMP";
    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:        return "PMP";
    case ACD_TYPE_SYSFS_VALUE:    return "SYS";
    case ACD_TYPE_MQTT_VALUE:     return "MQT";
    case ACD_TYPE_VIR_TANK:       return "TNK";
    case ACD_TYPE_GPIO_OUTPUT:
    case ACD_TYPE_GPIO_INPUT:     return "GPIO";
    default:                      return "UNK";
  }
}

// Companion lookup for the counter array index. Kept as its own switch
// (rather than deriving it from prefix_for_type()'s string) purely so
// assigning IDs to an entire config doesn't need any string comparisons.
static id_prefix_index_t prefix_index_for_type(acd_type_t type) {
  switch (type) {
    case ACD_TYPE_MQTT_COND:
    case ACD_TYPE_GPIO_COND:      return ID_PREFIX_CS;
    case ACD_TYPE_EZO_PH:         return ID_PREFIX_PH;
    case ACD_TYPE_EZO_ORP:        return ID_PREFIX_ORP;
    case ACD_TYPE_EZO_EC:         return ID_PREFIX_EC;
    case ACD_TYPE_EZO_PRS:
    case ACD_TYPE_I2C_PRS:        return ID_PREFIX_PRS;
    case ACD_TYPE_EZO_TEMP:
    case ACD_TYPE_MQTT_TEMP:
    case ACD_TYPE_D1W_TEMP:
    case ACD_TYPE_I2C_TEMP:       return ID_PREFIX_TEMP;
    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:        return ID_PREFIX_PMP;
    case ACD_TYPE_SYSFS_VALUE:    return ID_PREFIX_SYS;
    case ACD_TYPE_MQTT_VALUE:     return ID_PREFIX_MQT;
    case ACD_TYPE_VIR_TANK:       return ID_PREFIX_TNK;
    case ACD_TYPE_GPIO_OUTPUT:
    case ACD_TYPE_GPIO_INPUT:     return ID_PREFIX_GPIO;
    default:                      return ID_PREFIX_UNK;
  }
}

// Assigns a fresh ID to a single node using the given (already-verified-safe)
// index. Counters now live entirely in assign_missing_ids() -- this function
// has no static state of its own anymore.
void generate_id(acd_key_t *node, int index) {
  char buf[32];
  node->index = index;
  snprintf(buf, sizeof(buf), "%s_%d", prefix_for_type(node->type), index);
  node->ID = strdup(buf);
}

// Parses "PREFIX_123" into prefix and index. False if it doesn't match that shape.
static bool parse_id_format(const char *id, char *prefix_out, size_t prefix_out_size, int *index_out) {
  const char *underscore = strrchr(id, '_');
  if (!underscore || underscore == id) return false;

  size_t prefix_len = underscore - id;
  if (prefix_len >= prefix_out_size) return false;
  memcpy(prefix_out, id, prefix_len);
  prefix_out[prefix_len] = '\0';

  const char *num_part = underscore + 1;
  if (*num_part == '\0') return false;
  for (const char *p = num_part; *p; p++) {
    if (!isdigit((unsigned char)*p)) return false;
  }
  *index_out = atoi(num_part);
  return true;
}

static bool id_already_used(struct aquachemdata *acdata, const char *id, acd_key_t *exclude) {
  for (acd_key_t *curr = acdata->keys; curr != NULL; curr = curr->next) {
    if (curr == exclude || !curr->ID) continue;
    if (strcmp(curr->ID, id) == 0) return true;
  }
  return false;
}

void assign_missing_ids(struct aquachemdata *acdata) {
  int max_index[NUM_ID_PREFIXES] = {0};

  for (acd_key_t *curr = acdata->keys->next; curr != NULL; curr = curr->next) {
    
    LOG(LOG_DEBUG, "Validating ID for %s, %s\n",curr->label,curr->ID);

    if (!curr->ID) continue;

    char actual_prefix[8];
    int index = 1;

    bool valid = parse_id_format(curr->ID, actual_prefix, sizeof(actual_prefix), &index) &&
                 strcmp(actual_prefix, prefix_for_type(curr->type)) == 0;

    if (!valid) {
      LOG(LOG_ERR, "%s: ID '%s' doesn't match expected form '%s_<n>' for this device type, regenerating\n",
          curr->label, curr->ID, prefix_for_type(curr->type));
      free(curr->ID);
      curr->ID = NULL;
      continue;
    }
    if (id_already_used(acdata, curr->ID, curr)) {
      LOG(LOG_ERR, "%s: duplicate ID '%s', regenerating\n", curr->label, curr->ID);
      free(curr->ID);
      curr->ID = NULL;
      continue;
    }

    //int index = ++max_index[prefix_index_for_type(curr->type)];
    //curr->index = index;

    id_prefix_index_t idx = prefix_index_for_type(curr->type);
    if (index > max_index[idx]) max_index[idx] = index;
    curr->index = index;
  }

  for (acd_key_t *curr = acdata->keys; curr != NULL; curr = curr->next) {
    if (curr->ID) continue;
    id_prefix_index_t idx = prefix_index_for_type(curr->type);
    generate_id(curr, ++max_index[idx]);
    LOG(LOG_DEBUG, "Created ID for %s, %s",curr->label,curr->ID);
    curr->is_dirty = true; // needs writing back to config now that it's pinned
  }
}

const char* hex_to_str(unsigned char c) {
    static char buf[6]; // Enough for "0x??\0"
    snprintf(buf, sizeof(buf), "0x%02x", c);
    return buf;
}
const char* int_to_str(int i) {
    static char buf[12]; // Sufficient for -2147483648\0
    snprintf(buf, sizeof(buf), "%d", i);
    return buf;
}


//char *generate_label(const char *base, acd_label_type_t type, const char *label) {
char *generate_label(const char *base, acd_type_t type, const char *label) {

  if (label && strlen(label) > 0) {
    return strdup(label);
  }

  char buf[128];

  if (type == ACD_TYPE_MQTT_VALUE || type == ACD_TYPE_D1W_TEMP) {
    const char *last_slash = strrchr(base, '/');
    if (last_slash != NULL) 
      snprintf(buf, sizeof(buf), "%s_%s", prefix_for_type(type), last_slash + 1);
    else 
      snprintf(buf, sizeof(buf), "%s_%s", prefix_for_type(type), base);
  } else {
    snprintf(buf, sizeof(buf), "%s_%s", prefix_for_type(type), base);
  }


  return strdup(buf);
}

// Priority groups — lower number = closer to head
static int node_priority(acd_type_t type) {
    if (type == ACD_TYPE_MASTER)                return 0;
    if (IS_CONDITION(type))                     return 1;
    if (type == ACD_TYPE_EZO_TEMP  ||
        type == ACD_TYPE_MQTT_TEMP ||
        type == ACD_TYPE_D1W_TEMP)              return 2;
    if (IS_INPUT(type))                         return 3;  // PH, ORP, etc.
    if (IS_OUTPUT(type))                        return 4;
    return 5;                                              // unknown/safety
}

void append_to_key_list(acd_key_t *new_node) {
    new_node->next = NULL;
    int new_prio = node_priority(new_node->type);

    // Insert at head if list is empty or new node beats the head
    if (_acdconfig_.keys == NULL ||
        node_priority(_acdconfig_.keys->type) > new_prio) {
        new_node->next = _acdconfig_.keys;
        _acdconfig_.keys = new_node;
        return;
    }

    // Walk until the next node has a strictly higher priority
    acd_key_t *curr = _acdconfig_.keys;
    while (curr->next != NULL && node_priority(curr->next->type) <= new_prio) {
        curr = curr->next;
    }
    new_node->next = curr->next;
    curr->next = new_node;
}

// Specialized function for MQTT Condition
void add_condition_mqtt(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;

    new_node->type = ACD_TYPE_MQTT_COND;

    new_node->label = generate_label(st->topic_path, new_node->type, st->label);
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->data.mqtt.topic = strdup(st->topic_path);
    new_node->data.mqtt.target_value = strdup(st->char_value ? st->char_value : "");
    new_node->met = false; // Initial state, not met.
    //new_node->scope = st->is_global ? ACD_ACTION_BLOCK : ACD_ACTION_LIMIT;
    new_node->scope = st->scope;
    new_node->delay_on = st->value; 

    append_to_key_list(new_node);
}

// Specialized function for GPIO Condition
void add_condition_gpio(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;

    new_node->type = ACD_TYPE_GPIO_COND;
    
    new_node->label = generate_label(int_to_str(st->pin), new_node->type, st->label);
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->data.gpio.pin = st->pin;
    new_node->data.gpio.active = st->pin_mode;
    new_node->data.gpio.required = st->pin_state;
    new_node->met = false; // Initial state, not met.
    //new_node->scope = st->is_global ? ACD_ACTION_BLOCK : ACD_ACTION_LIMIT;
    new_node->scope = st->scope;
    
    new_node->delay_on = st->value; 
    
    append_to_key_list(new_node);
}

// Specialized function for Atlas Scientific EZO
void add_sensor_ezo(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;

    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(hex_to_str(st->address), new_node->type, st->label);
    new_node->data.ezo.address = st->address;
    //new_node->scope = st->is_global ? ACD_SCOPE_GLOBAL : ACD_SCOPE_LOCAL;
    new_node->scope = st->scope;
    new_node->stats.tau_seconds = st->value3;

    if (new_node->scope != ACD_SCOPE_GLOBAL && new_node->scope != ACD_SCOPE_LOCAL) {
        LOG(LOG_WARNING, "Interlock scope for %s is not valid, using Global\n",new_node->label);
        new_node->scope = ACD_SCOPE_GLOBAL;
    }
    
    if (st->flags != 0) {
      new_node->flags = st->flags;
      //new_node->stats.ID = malloc(strlen(new_node->ID)+ 5);
      //sprintf(new_node->stats.ID, "STS_%s", new_node->ID);
    }

    if (new_node->type == ACD_TYPE_EZO_TEMP)
    {
      if (st->uom != UOM_NONE) {new_node->uom = st->uom;}
      else {new_node->uom = UOM_CELSIUS;}
    } else if (new_node->type == ACD_TYPE_EZO_PH) {
      new_node->uom = UOM_PH;
    } else if (new_node->type == ACD_TYPE_EZO_ORP) {
      new_node->uom = UOM_MV;
    } else if (new_node->type == ACD_TYPE_EZO_PRS) {
      new_node->uom = UOM_PSI;
    } else if (new_node->type == ACD_TYPE_EZO_EC) {
      new_node->uom = UOM_MICROSIEMENS_CM;
      new_node->data.ezo.ec_sensor_k = st->value > 0 ? st->value : 1.0;   // ec_sensor_k
      new_node->data.ezo.ec_sensor_tds_factor = st->value2 > 0 ? st->value2 : 0.54;  // ec_sensor_tds_factor
    }

    append_to_key_list(new_node);

    if (new_node->type == ACD_TYPE_EZO_EC) {
      new_node->data.ezo.flags = EC_CONDUCTIVITY;

      acd_key_t *child_TDS_node = malloc(sizeof(acd_key_t));
      if (!child_TDS_node) return;
      child_TDS_node->flags = ACD_FLAG_VIRTUAL;
      child_TDS_node->type = ACD_TYPE_EZO_EC;
      child_TDS_node->uom = UOM_PPM;
      child_TDS_node->data.ezo.address = new_node->data.ezo.address;
      child_TDS_node->scope = new_node->scope;
      child_TDS_node->data.ezo.flags = EC_TDS;
      new_node->child = child_TDS_node;
      char *child_TDS_label = replace_or_append_suffix(
        st->label, 
        (const char*[]){"conductivity", NULL}, 
        "Total Dissolved Solids"
      );
      child_TDS_node->label = generate_label(hex_to_str(st->address), child_TDS_node->type, child_TDS_label);
      append_to_key_list(child_TDS_node);
      LOG(LOG_NOTICE, "Added I2C Sensor: %s, from I2C Conductivity Sensor: %s", child_TDS_node->label, new_node->label);
      free(child_TDS_label); // Free the temporary label string after use

      acd_key_t *child_PSU_node = malloc(sizeof(acd_key_t));
      if (!child_PSU_node) return;
      child_PSU_node->flags = ACD_FLAG_VIRTUAL;
      child_PSU_node->type = ACD_TYPE_EZO_EC;
      child_PSU_node->uom = UOM_PSU;
      child_PSU_node->data.ezo.address = new_node->data.ezo.address;
      child_PSU_node->scope = new_node->scope;
      child_PSU_node->data.ezo.flags = EC_SALINITY;
      child_TDS_node->child = child_PSU_node;
      char *child_PSU_label = replace_or_append_suffix(
        st->label, 
        (const char*[]){"conductivity", NULL}, 
        "Salinity"
      );
      child_PSU_node->label = generate_label(hex_to_str(st->address), child_PSU_node->type, child_PSU_label);
      append_to_key_list(child_PSU_node);
      LOG(LOG_NOTICE, "Added I2C Sensor: %s, from I2C Conductivity Sensor: %s", child_PSU_node->label, new_node->label);
      free(child_PSU_label); // Free the temporary label string after use

      acd_key_t *child_SG_node = malloc(sizeof(acd_key_t));
      if (!child_SG_node) return;
      child_SG_node->flags = ACD_FLAG_VIRTUAL;
      child_SG_node->type = ACD_TYPE_EZO_EC;
      child_SG_node->uom = UOM_SPECIFIC_GRAVITY;
      child_SG_node->data.ezo.address = new_node->data.ezo.address;
      child_SG_node->scope = new_node->scope;
      child_SG_node->data.ezo.flags = EC_SPECIFIC_GRAVITY;
      child_PSU_node->child = child_SG_node;
      child_SG_node->child = NULL;
      char *child_SG_label = replace_or_append_suffix(
        st->label, 
        (const char*[]){"conductivity", NULL}, 
        "Specific Gravity"
      );
      child_SG_node->label = generate_label(hex_to_str(st->address), child_SG_node->type, child_SG_label);
      append_to_key_list(child_SG_node);
      LOG(LOG_NOTICE, "Added I2C Sensor: %s, from I2C Conductivity Sensor: %s", child_SG_node->label, new_node->label);
      free(child_SG_label); // Free the temporary label string after use


      /*
  EC_OUTPUT_CONDUCTIVITY     = 1 << 0,
  EC_OUTPUT_TDS              = 1 << 1,
  EC_OUTPUT_SALINITY         = 1 << 2,
  EC_OUTPUT_SPECIFIC_GRAVITY = 1 << 3
     */

    }
}

// Specialized function for MQTT Sensor
void add_sensor_mqtt(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;
  
    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(st->topic_path, new_node->type, st->label);
    new_node->data.mqtt.topic = strdup(st->topic_path);

    //new_node->scope = st->is_global ? ACD_SCOPE_GLOBAL : ACD_SCOPE_LOCAL;
    //new_node->scope = ACD_SCOPE_LOCAL;
    new_node->scope = ACD_SCOPE_ALLOW;


    if (st->flags != 0) {
      new_node->flags = st->flags;
      //new_node->stats.ID = malloc(strlen(new_node->ID)+ 5);
      //sprintf(new_node->stats.ID, "STS_%s", new_node->ID);
    }

    if (st->uom != UOM_NONE) {
      new_node->uom = st->uom;
    } else if ( new_node->type == ACD_TYPE_MQTT_TEMP) {
      new_node->uom = UOM_CELSIUS;
    }

    append_to_key_list(new_node);
}

// Specialized function for DS18B20 1-Wire
void add_sensor_d1w(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;
  
    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(st->topic_path, new_node->type, st->label);
    strcpy(new_node->data.w1.path, st->topic_path);
    new_node->data.w1.offset = st->value;  // Mapped from _staging.value
    new_node->data.w1.scale = st->value2;  // Mapped from _staging.value2
    //new_node->scope = st->is_global ? ACD_SCOPE_GLOBAL : ACD_SCOPE_LOCAL;
    new_node->scope = st->scope;
    new_node->stats.tau_seconds = st->value3;

    if (st->flags != 0) {
      new_node->flags = st->flags;
      //new_node->stats.ID = malloc(strlen(new_node->ID)+ 5);
      //sprintf(new_node->stats.ID, "STS_%s", new_node->ID);
    }


    if (st->uom != UOM_NONE) {
      new_node->uom = st->uom;
    } else {
      new_node->uom = UOM_CELSIUS;
    }

    append_to_key_list(new_node);
}

void add_gpio(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;

    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(int_to_str(st->pin), new_node->type, st->label);
    new_node->data.gpio.pin = st->pin;
    new_node->data.gpio.active = st->pin_mode;
    new_node->data.gpio.required = st->pin_state;
    new_node->met = false; // Initial state, not met.

    new_node->scope = st->scope;

    if (new_node->type==ACD_TYPE_GPIO_OUTPUT) {
      set_pump_default_duration(new_node,_acdconfig_.switch_max_runtime / 2); // Default to half of the max runtime for switches
    }
/*
    if (new_node->type==ACD_TYPE_GPIO_INPUT) {
      new_node->scope = ACD_SCOPE_ALLOW;
      new_node->delay_on = st->value;
    } else if (new_node->type==ACD_TYPE_GPIO_OUTPUT) {
      new_node->scope = st->scope;
      set_pump_default_duration(new_node,_acdconfig_.switch_max_runtime / 2); // Default to half of the max runtime for switches
    }
*/

    append_to_key_list(new_node);
}



// Specialized function for GPIO Output / Pump
void add_pump(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;
  
    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(int_to_str(st->pin), new_node->type, st->label);

    if (new_node->type == ACD_TYPE_GPIO_PMP) {
        new_node->data.gpio.pin = st->pin;
        new_node->data.gpio.active = st->pin_mode;
        new_node->data.gpio.required = st->pin_state;
    } else if (new_node->type == ACD_TYPE_EZO_PMP) {
        new_node->data.ezo.address = st->address;
    }

    new_node->dose_stats.flow_rate = st->value; // Mapped from _staging.value (ml_ps)
    new_node->dose_stats.running_total_max_ml = st->value4;

    //new_node->scope = st->is_global ? ACD_SCOPE_GLOBAL : ACD_SCOPE_LOCAL;
    new_node->scope = st->scope;
    
  
    //printf("**** Tank %s: running_total_max_ml=%f\n", new_node->label, new_node->dose_stats.running_total_max_ml);

    if (st->flags != 0) {
        new_node->flags = st->flags;
    }

    if isMASKSET(new_node->flags, PH_PUMP)       set_pump_default_duration(new_node,_acdconfig_.ph_default_dose_time);
    else if isMASKSET(new_node->flags, ORP_PUMP) set_pump_default_duration(new_node,_acdconfig_.orp_default_dose_time);
    else if isMASKSET(new_node->flags, H2O_PUMP) set_pump_default_duration(new_node,_acdconfig_.h2o_default_dose_time);
    
    append_to_key_list(new_node);

    // If we know the tank size, create a child key.
    if (st->value2 > 0) {
      // Add the appropiate temperature sensor for the PTE7300 if it doesn't already exist in the list.
      acd_key_t *new_tank = malloc(sizeof(acd_key_t));
      if (!new_tank) return;

      //*new_tank = *new_node; // Copy all value fields, enums, unions, and flags in one step

      // Set to NULL if the copy shouldn't share the same linked list/child relations
      new_tank->ID = NULL;
      new_tank->child = NULL; 
      new_tank->next  = NULL;
      new_node->child = new_tank; // Link the temperature sensor as a child of the pressure sensor

      new_tank->flags = new_node->flags;
      setMASK(new_tank->flags, ACD_FLAG_VIRTUAL);
      
      new_tank->scope = ACD_SCOPE_LOCAL;

      new_tank->type = ACD_TYPE_VIR_TANK;
      new_tank->uom = UOM_PERCENT;

      char *tank_label = isMASKSET(new_tank->flags, PH_PUMP)?"Acid Tank Level":"Chlorine Tank Level";

      new_tank->data.tank.total_volume = st->value2;
      new_tank->data.tank.min_volume = st->value3;

      //printf("**** Tank uom2: %d, %s\n", st->uom2, uom_to_str(st->uom2));

      if (st->uom2 == UOM_GALLONS) {
        new_tank->data.tank.uom = st->uom2;
      } else {
        LOG(LOG_NOTICE, "Tank %s volume unit not specified or not in gallons, defaulting to liters.",tank_label);
        new_tank->data.tank.uom = UOM_LITERS;
      }

      new_tank->label = generate_label(hex_to_str(st->address), new_tank->type, tank_label);

      append_to_key_list(new_tank);

      LOG(LOG_NOTICE, "Added Virtual Tank Level Sensor: %s, from Doser : %s", new_tank->label, new_node->label);
    
    }
}
/*
void create_child_key(acd_key_t key)
{
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;

    *new_node = *key; // Copy all value fields, enums, unions, and flags in one step

    // Set to NULL if the copy shouldn't share the same linked list/child relations
    new_node->child = NULL; 
    new_node->next  = NULL;
    key->child = new_node; // Link the temperature sensor as a child of the pressure sensor

    new_node->flags = ACD_FLAG_VIRTUAL;
    new_node->type = ACD_TYPE_I2C_TEMP;
    new_node->uom = UOM_CELSIUS;

    char *new_node = replace_or_append_suffix(
      st->label, 
      (const char*[]){"pressure", "psi", "bar", NULL}, 
      " Temperature"
    );

    new_node->label = generate_label(hex_to_str(st->address), ACD_FLAG_VIRTUAL, new_node);
    generate_sensor_id(new_node);
    append_to_key_list(new_node);

    LOG(LOG_NOTICE, "Added I2C Temperature Sensor: %s, from I2C Pressure Sensor: %s, Address: 0x%02x", new_i2c_temp->label, new_node->label, new_node->data.i2c.address);
    
    free(new_node); // Free the temporary label string after use
}
*/
void add_sensor_sysfs(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;
  
    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(st->topic_path, new_node->type, st->label);
    strcpy(new_node->data.sysfs.path, st->topic_path);
    new_node->data.sysfs.offset = st->value;  // Mapped from _staging.value
    new_node->data.sysfs.multiplier = st->value2;  // Mapped from _staging.value2

    new_node->scope = st->scope;

    if (st->char_value) {
      new_node->data.sysfs.regex_pattern = strdup(st->char_value);
      new_node->data.sysfs.parser_type = PARSER_REGEX;
    } else {
      new_node->data.sysfs.parser_type = PARSER_RAW;    
    }

    new_node->uom = st->uom;

    append_to_key_list(new_node);
}


// Specialized function for i2c input
void add_sensor_i2c(const acd_staging_t *st) {
    acd_key_t *new_node = malloc(sizeof(acd_key_t));
    if (!new_node) return;
  
    new_node->type = st->pending_type;
    new_node->ID = st->ID ? strdup(st->ID) : NULL;
    new_node->label = generate_label(hex_to_str(st->address), new_node->type, st->label);
    
    new_node->data.i2c.type = st->pin_mode;

    if (new_node->data.i2c.type != I2C_SENSOR_PTE7300) {
        // SOME ERROR
        // return.
    }  
    new_node->data.i2c.address = st->address;
    new_node->data.i2c.min_value = st->value2;
    new_node->data.i2c.max_value = st->value3;

    //new_node->scope = ACD_SCOPE_LOCAL;
    //new_node->scope = st->is_global ? ACD_SCOPE_GLOBAL : ACD_SCOPE_LOCAL;
    new_node->scope = st->scope;

    new_node->uom = UOM_PSI;

    //new_node->uom = st->uom;
    /*
    new_node->data.gpio.pin = st->pin;
    new_node->data.gpio.active = st->pin_mode;
    new_node->data.gpio.required = st->pin_state;
    new_node->flow_rate = st->value; // Mapped from _staging.value (ml_ps)
    */
    if (st->flags != 0) {
        new_node->flags = st->flags;
    }
  
    append_to_key_list(new_node);

    if (new_node->data.i2c.type == I2C_SENSOR_PTE7300) {
      // Add the appropiate temperature sensor for the PTE7300 if it doesn't already exist in the list.
      acd_key_t *new_i2c_temp = malloc(sizeof(acd_key_t));
      if (!new_i2c_temp) return;

      *new_i2c_temp = *new_node; // Copy all value fields, enums, unions, and flags in one step

      // Set to NULL if the copy shouldn't share the same linked list/child relations
      new_i2c_temp->ID = NULL;
      new_i2c_temp->child = NULL; 
      new_i2c_temp->next  = NULL;
      new_node->child = new_i2c_temp; // Link the temperature sensor as a child of the pressure sensor

      new_i2c_temp->flags = ACD_FLAG_VIRTUAL;
      new_i2c_temp->type = ACD_TYPE_I2C_TEMP;
      new_i2c_temp->uom = UOM_CELSIUS;

      char *new_i2c_temp_label = replace_or_append_suffix(
        st->label, 
        (const char*[]){"pressure", "psi", "bar", NULL}, 
        " Temperature"
      );

      new_i2c_temp->label = generate_label(hex_to_str(st->address), new_i2c_temp->type, new_i2c_temp_label);

      append_to_key_list(new_i2c_temp);

      LOG(LOG_NOTICE, "Added I2C Temperature Sensor: %s, from I2C Pressure Sensor: %s, Address: 0x%02x", new_i2c_temp->label, new_node->label, new_node->data.i2c.address);
    
      free(new_i2c_temp_label); // Free the temporary label string after use

      //LOG(LOG_NOTICE, "Added I2C Pressure Sensor: %s, Address: 0x%02x, Min: %.2f, Max: %.2f", new_node->label, new_node->data.i2c.address, new_node->data.i2c.min_value, new_node->data.i2c.max_value);
    }
}


char* replace_or_append_suffix(const char *orig, const char *suffixes[], const char *append_text) {
    if (!orig || !append_text) return NULL;

    size_t orig_len = strlen(orig);
    size_t base_len = orig_len;

    // 1. Search for a matching suffix at the end of orig (case-insensitive)
    if (suffixes) {
        for (size_t i = 0; suffixes[i] != NULL; i++) {
            size_t suf_len = strlen(suffixes[i]);
            if (orig_len >= suf_len) {
                if (strncasecmp(orig + (orig_len - suf_len), suffixes[i], suf_len) == 0) {
                    base_len = orig_len - suf_len;
                    break; // Stop after the first matching suffix
                }
            }
        }
    }

    // 2. Trim trailing whitespace left on the base string (e.g., "Filter " -> "Filter")
    while (base_len > 0 && isspace((unsigned char)orig[base_len - 1])) {
        base_len--;
    }

    // 3. Skip leading space in append_text if the base string ended up empty
    const char *append_ptr = append_text;
    if (base_len == 0) {
        while (*append_ptr && isspace((unsigned char)*append_ptr)) {
            append_ptr++;
        }
    }

    // 4. Allocate memory and construct the result string
    size_t append_len = strlen(append_ptr);
    char *new_str = malloc(base_len + append_len + 1);
    if (!new_str) return NULL;

    if (base_len > 0) {
        memcpy(new_str, orig, base_len);
    }
    strcpy(new_str + base_len, append_ptr);

    return new_str;
}
