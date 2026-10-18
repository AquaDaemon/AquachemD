
#include "aquachemd.h"
#include "mqtt_discovery.h"
#include "net_services.h"
#include "net_interface.h"
#include "utils.h"
#include "config.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

/**
 * TEMPLATES
 */


static const char *HASS_DEVICE_TEMPLATE = 
    "\"device\":{"
        "\"identifiers\":[\"" AQUACHEMD_NAME "\"],"
        "\"sw_version\":\"" AQUACHEMD_VERSION "\","
        "\"model\":\"" AQUACHEMD_NAME "\","
        "\"name\":\"" AQUACHEMD_SHORT_NAME "\","
        "\"manufacturer\":\"" AQUADAEMON "\","
        "%s" // Connections placeholder
        "\"suggested_area\":\"pool\""
    "}";

static const char *HASS_AVAIL_TEMPLATE = 
    "\"availability\":{"
        "\"payload_available\":\"1\","
        "\"payload_not_available\":\"0\","
        "\"topic\":\"%s/" MQTT_LWM_TOPIC "\""
    "}";

static const char *HASS_AVAIL_SENSOR_TEMPLATE = 
    "\"availability\":["
        "{\"topic\":\"%s/" MQTT_LWM_TOPIC "\",\"payload_available\":\"1\",\"payload_not_available\":\"0\"}," // System Alive
        "{\"topic\":\"%s/%s/state\",\"value_template\":\"{{ 'online' if value == '1' else 'offline' }}\"}" // Sensor logic
    "]";

static const char *HASS_AVAIL_PUMP_TEMPLATE = 
    "\"availability\":["
        "{\"topic\":\"%s/" MQTT_LWM_TOPIC "\",\"payload_available\":\"1\",\"payload_not_available\":\"0\"}," // System Alive
        "{\"topic\":\"%s/%s/state\",\"value_template\":\"{{ 'offline' if value == '3' else 'online' }}\"}" // Sensor logic
    "]";

// Template for standard Sensors (Temp, pH, ORP, Problem)
static const char *HASS_SENSOR_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"sensor\","
        "\"unique_id\":\"aquachemd_%s\","
        "\"name\":\"%s\","
    // "\"state_topic\":\"%s/%s/" MQTT_TL_STATE "\","
        "\"state_topic\":\"%s/%s\","
        "\"value_template\":\"{{ value_json | float }}\","
        "\"icon\":\"%s\""
        "%s" // Extra JSON (unit, device_class, state_class) injected here
    "}";

// Template for standard Sensors (Temp, pH, ORP, Problem)
static const char *HASS_BINARY_SENSOR_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"binary_sensor\","
        "\"unique_id\":\"aquachemd_%s\","
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s/" MQTT_TL_STATE "\","
    //    "\"state_topic\":\"%s/%s\","
        "\"payload_on\": \"0\","
        "\"payload_off\": \"1\","
        "\"icon\":\"%s\""
        "%s" // Extra JSON (unit, device_class, state_class) injected here
    "}";

static const char *HASS_NUMBER_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"number\","
        "\"unique_id\":\"aquachemd_%s\","
        "\"name\":\"%s\","
        "\"command_topic\":\"%s/%s/set\","
        "\"state_topic\":\"%s/%s\","
        "\"min\":%f,"
        "\"max\":%f,"
        "\"step\":%f,"
        "\"unit_of_measurement\":\"%s\","
        "\"state_class\":\"measurement\","
        "\"mode\":\"slider\","
        "\"icon\":\"%s\""
    "}";

    // Template for Select entities - Now accepts a custom value_template
static const char *HASS_SELECT_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"select\","
        "\"unique_id\":\"aquachemd_%s\","
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s/" MQTT_TL_STATE "\","
        "\"command_topic\":\"%s/%s/set\","
        "\"options\":[%s],"
        "\"value_template\":\"%s\"," 
        "\"command_template\":\"{%% set mapper = { 'off':'0', 'on':'1', 'enabled':'2', 'disabled':'3' } %%}{{ mapper[value] }}\","
        "\"icon\":\"%s\""
    "}";

// Template for the Timer Sensor
static const char *HASS_TIMER_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"sensor\","
        "\"unique_id\":\"aquachemd_%s_timer\","
        "\"name\":\"%s Timer\","
        "\"state_topic\":\"%s/%s/" MQTT_TL_TIMER "/" MQTT_TL_DURATION "\","
        "\"unit_of_measurement\":\"s\","
        "\"device_class\":\"duration\","
        "\"state_class\":\"measurement\","
        "\"icon_template\":\"{{ 'mdi:timer-sand' if states(entity_id)|int > 0 else 'mdi:timer-outline' }}\""
    "}";

// Template for the settable Timer Duration (number slider) - lets HA both set
// and reflect the dose duration, in sync with any other interface (web/mqtt/HomeKit)

static const char *HASS_NUMBER_TIMER_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"number\","
        "\"unique_id\":\"aquachemd_%s_timer_set\","
        "\"name\":\"%s Timer\","
        "\"command_topic\":\"%s/%s/" MQTT_TL_TIMER "/set\","
        "\"state_topic\":\"%s/%s/" MQTT_TL_TIMER "/" MQTT_TL_DURATION "\","
        "\"min\":0,"
        "\"max\":%d,"
        "\"step\":1,"
        "\"unit_of_measurement\":\"s\","
        "\"mode\":\"slider\","
        "\"icon\":\"mdi:timer-outline\""
    "}";


// Template for dosing events - FIXED STATE_CLASS
static const char *HASS_DOSE_TEMPLATE = 
    "{"
        "%s,%s,"
        "\"type\":\"sensor\","
        "\"unique_id\":\"aquachemd_%s_dose\","
        "\"name\":\"%s Last Dose\","
        "\"state_topic\":\"%s/%s/%s\"," 
        "\"unit_of_measurement\":\"mL\","
        "\"device_class\":\"volume\","
        "\"state_class\":\"total_increasing\"," // Changed from measurement
        "\"icon\":\"mdi:flask-plus-outline\""
    "}";



void publish_mqtt_discovery(struct aquachemdata *acdata, struct mg_connection *nc) {
    char device_json[512];
    char connections[128];

    char avail_json[256], final_msg[JSON_DISCOVERY_SIZE], topic[250];
    

    const net_iface *iface = get_first_valid_interface();

    // 1. Prepare shared fragments
    if (_acdconfig_.mqtt_discovery_use_mac) {
        snprintf(connections, sizeof(connections), 
                 "\"connections\":[[\"mac\",\"%s\"]],\"configuration_url\":\"%s\",", 
                 iface->mac, iface->url);
    } else {
        snprintf(connections, sizeof(connections), 
             "\"connections\":[[\"serial_number\",\"aquachemd_controller\"]],\"configuration_url\":\"%s\",", 
             iface->url);
    }

    snprintf(device_json, sizeof(device_json), HASS_DEVICE_TEMPLATE, connections);
    

    // 2. Iterate through keys
    for (acd_key_t *curr = acdata->keys; curr != NULL; curr = curr->next) {
        char extra_json[256] = ""; 
        const char *hass_type   = "sensor";
        const char *icon        = "mdi:gauge";
        const char *options     = "";
        const char *state_mapper = "";
        bool should_publish     = true;

        if (IS_INPUT(curr->type) && _acdconfig_.mqtt_strict_avail) {
          snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_SENSOR_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic, _acdconfig_.mqtt_aquachemd_topic, curr->ID);
        } else if (IS_OUTPUT(curr->type) && _acdconfig_.mqtt_strict_avail) {
          snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_PUMP_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic, _acdconfig_.mqtt_aquachemd_topic, curr->ID);
        } else {
          snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);
        }

        switch (curr->type) {
            case ACD_TYPE_MASTER:
                hass_type = "select";
                icon      = "mdi:shield-check";
                options   = "\"off\",\"enabled\"";
                state_mapper = "{% set mapper = {'0':'off','1':'enabled','2':'enabled','3':'off'} %}{{ mapper[value] if value in mapper else 'off' }}";
                break;

            case ACD_TYPE_GPIO_PMP:
            case ACD_TYPE_EZO_PMP:
                hass_type = "select";
                icon      = "mdi:pump";
                options   = "\"off\",\"on\",\"enabled\"";
                // Mapping: 0->off, 1->on, 2->enabled, 3->off
                state_mapper = "{% set mapper = {'0':'off','1':'on','2':'enabled','3':'off'} %}{{ mapper[value] if value in mapper else 'off' }}";
                break;
          
            case ACD_TYPE_MQTT_TEMP: // Will fall through to ACD_TYPE_EZO_TEMP when set to yes
                if (!_acdconfig_.mqtt_repost_sensors) {
                    should_publish = false;
                    break;
                }
            case ACD_TYPE_EZO_TEMP:
            case ACD_TYPE_D1W_TEMP:
            case ACD_TYPE_I2C_TEMP:
                snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"");
                icon = "mdi:thermometer";
                break;
    
            case ACD_TYPE_EZO_PH:
                snprintf(extra_json, sizeof(extra_json), 
                    ",\"device_class\":\"ph\",\"state_class\":\"measurement\"");
                icon = "mdi:flask-outline";
                break;

            case ACD_TYPE_EZO_ORP:
                snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"mV\",\"state_class\":\"measurement\"");
                icon = "mdi:lightning-bolt-outline";
                break;

            case ACD_TYPE_I2C_PRS:
            case ACD_TYPE_EZO_PRS:
                if (curr->uom && curr->uom != UOM_NONE) {
                  snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"%s\",\"device_class\":\"pressure\",\"state_class\":\"measurement\"",
                    uom_to_HA_str(curr->uom));
                } else {
                  snprintf(extra_json, sizeof(extra_json), 
                    ",\"device_class\":\"pressure\",\"state_class\":\"measurement\"");
                }
                icon = "mdi:gauge";
                break;

            case ACD_TYPE_MQTT_VALUE: // Will fall through to ACD_TYPE_SYSFS_VALUE when set to yes
                if (!_acdconfig_.mqtt_repost_sensors) {
                    should_publish = false;
                    break;
                }
            case ACD_TYPE_SYSFS_VALUE:
                if (IS_UOM_TEMPERATURE(curr->uom)) {
                  snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"%s\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"",
                    uom_to_HA_str(curr->uom));
                    icon = "mdi:thermometer";
                } else {
                   snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"",
                    uom_to_HA_str(curr->uom));
                    icon = "mdi:chemical-weapon";
                }
                break;

            case ACD_TYPE_MQTT_COND: // Will fall through to ACD_TYPE_GPIO_COND when set to yes
                if (!_acdconfig_.mqtt_repost_sensors) {
                    should_publish = false;
                    break;
                }
            case ACD_TYPE_SENSOR_COND: 
            case ACD_TYPE_GPIO_COND:
                // Flow / Level alert
                hass_type = "binary_sensor";
                snprintf(extra_json, sizeof(extra_json), ",\"device_class\":\"problem\"");
                //snprintf(extra_json, sizeof(extra_json), ""); 
                icon = "mdi:alert-circle-outline";
                // FIXED: Icon cannot be "", use a fallback icon
                break;

            case ACD_TYPE_VIR_TANK:
                // Level sensor
                hass_type = "sensor";
                snprintf(extra_json, sizeof(extra_json), 
                    ",\"unit_of_measurement\":\"%%\",\"state_class\":\"measurement\"");
                icon = "mdi:cup-water";
                break;
            
            case ACD_TYPE_GPIO_INPUT:
              hass_type = "binary_sensor";
              break;

            
            case ACD_TYPE_EZO_EC:
              if (curr->uom == UOM_MICROSIEMENS_CM) {
                snprintf(extra_json, sizeof(extra_json), ",\"unit_of_measurement\":\"%s\",\"device_class\":\"conductivity\",\"state_class\":\"measurement\"",
                         uom_to_HA_str(curr->uom));
                icon = "mdi:flash-outline";
              } else {
                snprintf(extra_json, sizeof(extra_json),
                        ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"",
                        uom_to_HA_str(curr->uom));
                icon = (curr->uom == UOM_PPM) ? "mdi:water-opacity" :(curr->uom == UOM_PSU) ? "mdi:waves" : "mdi:scale-balance";   // SG
              }
              break;
            
            case ACD_TYPE_GPIO_PULSE:
              if (curr->uom == UOM_LITERS_PER_MIN || curr->uom == UOM_GALLONS_PER_MIN) {
                snprintf(extra_json, sizeof(extra_json),
                         ",\"unit_of_measurement\":\"%s\",\"device_class\":\"volume_flow_rate\",\"state_class\":\"measurement\",\"suggested_display_precision\": 2",
                         uom_to_HA_str(curr->uom));
              } else {
                snprintf(extra_json, sizeof(extra_json),
                         ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"",
                uom_to_HA_str(curr->uom));
              }
              icon = "mdi:pipe";
              break;

            case ACD_TYPE_GPIO_OUTPUT:
              hass_type = "select";
              icon      = "mdi:toggle-switch-outline";
              options   = "\"off\",\"on\"";
              // 0->off, 1->on, 2/3 (enabled/disabled) -> off
              state_mapper = "{% set mapper = {'0':'off','1':'on','2':'off','3':'off'} %}{{ mapper[value] if value in mapper else 'off' }}";
              break;


            case ACD_TYPE_NONE:
              should_publish = false;
              break;
        }

        if (should_publish) {
            if (strcmp(hass_type, "select") == 0) {
                snprintf(final_msg, sizeof(final_msg), HASS_SELECT_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         options, state_mapper, icon);
            } else if (strcmp(hass_type, "binary_sensor") == 0) {
                snprintf(final_msg, sizeof(final_msg), HASS_BINARY_SENSOR_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         icon, extra_json);
            } else {
                snprintf(final_msg, sizeof(final_msg), HASS_SENSOR_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         icon, extra_json);
            }
            // Discovery topic path
            snprintf(topic, sizeof(topic), "%s/%s/aquachemd/aquachemd_%s/config", 
                     _acdconfig_.mqtt_discovery_topic, hass_type, curr->ID);

            send_mqtt(nc, topic, final_msg);

            // IF it's a Pump, also publish the Timer Sensor, and Dose sensor
            if (curr->type == ACD_TYPE_GPIO_PMP || curr->type == ACD_TYPE_EZO_PMP) {
                snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);
                // Timer
                snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s_timer/config", 
                         _acdconfig_.mqtt_discovery_topic, curr->ID);

                snprintf(final_msg, sizeof(final_msg), HASS_TIMER_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID);

                send_mqtt(nc, topic, final_msg);

                // Timer (settable number/slider) - shares the same duration state_topic
                // as the sensor above, so it stays in sync no matter which interface
                // (HA, web, raw MQTT, HomeKit) last set the value
                snprintf(topic, sizeof(topic), "%s/number/aquachemd/aquachemd_%s_timer_set/config", 
                         _acdconfig_.mqtt_discovery_topic, curr->ID);
 
                snprintf(final_msg, sizeof(final_msg), HASS_NUMBER_TIMER_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                         (curr->flags & PH_PUMP) ? _acdconfig_.ph_max_dose_time : _acdconfig_.orp_max_dose_time);
 
                send_mqtt(nc, topic, final_msg);

                // Doser
                /*
                const char *dose_suffix = ((curr->flags & PH_PUMP)  ? MQTT_TL_DOSE_PH : 
                                           (curr->flags & ORP_PUMP) ? MQTT_TL_DOSE_ORP : MQTT_TL_DOSE_UNKNOWN );
                */
                snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s_dose/config", 
                                                _acdconfig_.mqtt_discovery_topic, curr->ID);

                snprintf(final_msg, sizeof(final_msg), HASS_DOSE_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, curr->label,
                         _acdconfig_.mqtt_aquachemd_topic, curr->ID, MQTT_TL_LAST_DOSE);

                send_mqtt(nc, topic, final_msg);
            }

            // For Tank also post a way to set the level.
            if (curr->type == ACD_TYPE_VIR_TANK) {
                char full_label[64];
                snprintf(full_label, sizeof(full_label), "%s Remaining", curr->label);
                char topic_label[64];
                snprintf(topic_label, sizeof(topic_label), "%s/level/remaining", curr->ID);

                snprintf(topic, sizeof(topic), "%s/number/aquachemd/aquachemd_%s_remaining/config", 
                         _acdconfig_.mqtt_discovery_topic, curr->ID);
 
                snprintf(final_msg, sizeof(final_msg), HASS_NUMBER_TEMPLATE,
                         device_json, avail_json,
                         curr->ID, full_label,
                         _acdconfig_.mqtt_aquachemd_topic, topic_label,
                         _acdconfig_.mqtt_aquachemd_topic, topic_label,
                         0.0,
                         curr->data.tank.total_volume,
                         0.1,
                         uom_to_HA_str(curr->data.tank.uom),
                        "mdi:cup-water");
 
                send_mqtt(nc, topic, final_msg);
            }

            // IF has Average calculation, also publish the Average Sensor
            // Create a dummy device 
            if (isMASKSET(curr->flags, CALC_AVERAGE)) {
                char id[16];
                char label[32];
                char sensor_topic[32];
                snprintf(id, 16, "%s_average", curr->ID);
                snprintf(label, 32, "%s (average)", curr->label);
                snprintf(sensor_topic, 32, "%s/average", curr->ID);

                snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);

                snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s/config", 
                         _acdconfig_.mqtt_discovery_topic, id);

                snprintf(final_msg, sizeof(final_msg), HASS_SENSOR_TEMPLATE,
                         device_json, avail_json,
                         id, label,
                         _acdconfig_.mqtt_aquachemd_topic, sensor_topic,
                        icon, extra_json);

                send_mqtt(nc, topic, final_msg);
            }
        }
    }
}

/*
void publish_mqtt_discovery_device(struct aquachemdata *acdata, struct mg_connection *nc, acd_key_t *device, const char *device_json)
{
  char avail_json[256], final_msg[JSON_DISCOVERY_SIZE], topic[250];

  char extra_json[256] = "";
  const char *hass_type = "sensor";
  const char *icon = "mdi:gauge";
  const char *options = "";
  const char *state_mapper = "";
  bool should_publish = true;

  if (IS_INPUT(device->type) && _acdconfig_.mqtt_strict_avail)
  {
    snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_SENSOR_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic, _acdconfig_.mqtt_aquachemd_topic, device->ID);
  }
  else if (IS_OUTPUT(device->type) && _acdconfig_.mqtt_strict_avail)
  {
    snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_PUMP_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic, _acdconfig_.mqtt_aquachemd_topic, device->ID);
  }
  else
  {
    snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);
  }

  switch (device->type)
  {
    case ACD_TYPE_MASTER:
        hass_type = "select";
        icon = "mdi:shield-check";
        options = "\"off\",\"enabled\"";
        state_mapper = "{% set mapper = {'0':'off','1':'enabled','2':'enabled','3':'off'} %}{{ mapper[value] if value in mapper else 'off' }}";
        break;

    case ACD_TYPE_GPIO_PMP:
    case ACD_TYPE_EZO_PMP:
        hass_type = "select";
        icon = "mdi:pump";
        options = "\"off\",\"on\",\"enabled\"";
        // Mapping: 0->off, 1->on, 2->enabled, 3->off
        state_mapper = "{% set mapper = {'0':'off','1':'on','2':'enabled','3':'off'} %}{{ mapper[value] if value in mapper else 'off' }}";
        break;

    case ACD_TYPE_MQTT_TEMP:
        if (!_acdconfig_.mqtt_repost_sensors)
        {
            should_publish = false;
            break;
        }
    case ACD_TYPE_EZO_TEMP:
    case ACD_TYPE_D1W_TEMP:
        snprintf(extra_json, sizeof(extra_json),
                 ",\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"");
        icon = "mdi:thermometer";
        break;

    case ACD_TYPE_EZO_PH:
        snprintf(extra_json, sizeof(extra_json),
                 ",\"device_class\":\"ph\",\"state_class\":\"measurement\"");
        icon = "mdi:flask-outline";
        break;

    case ACD_TYPE_EZO_ORP:
        snprintf(extra_json, sizeof(extra_json),
                 ",\"unit_of_measurement\":\"mV\",\"state_class\":\"measurement\"");
        icon = "mdi:lightning-bolt-outline";
        break;

    case ACD_TYPE_SYSFS_VALUE:
        if (IS_UOM_TEMPERATURE(device->uom))
        {
            snprintf(extra_json, sizeof(extra_json),
                     ",\"unit_of_measurement\":\"%s\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"",
                     uom_to_HA_str(device->uom));
            icon = "mdi:thermometer";
        }
        else
        {
            snprintf(extra_json, sizeof(extra_json),
                     ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"",
                     uom_to_HA_str(device->uom));
            icon = "mdi:chemical-weapon";
        }

        icon = "mdi:thermometer";
        break;

    case ACD_TYPE_MQTT_COND:
        if (!_acdconfig_.mqtt_repost_sensors)
        {
            should_publish = false;
            break;
        }

    case ACD_TYPE_GPIO_COND:
        // Flow / Level alert
        hass_type = "binary_sensor";
        snprintf(extra_json, sizeof(extra_json), ",\"device_class\":\"problem\"");
        // snprintf(extra_json, sizeof(extra_json), "");
        icon = "mdi:alert-circle-outline";
        // FIXED: Icon cannot be "", use a fallback icon
        break;

    default:
        should_publish = false;
        break;
    }

    if (should_publish)
    {
        if (strcmp(hass_type, "select") == 0)
        {
            snprintf(final_msg, sizeof(final_msg), HASS_SELECT_TEMPLATE,
                     device_json, avail_json,
                     device->ID, device->label,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID,
                     options, state_mapper, icon);
        }
        else if (strcmp(hass_type, "binary_sensor") == 0)
        {
            snprintf(final_msg, sizeof(final_msg), HASS_BINARY_SENSOR_TEMPLATE,
                     device_json, avail_json,
                     device->ID, device->label,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID,
                     icon, extra_json);
        }
        else
        {
            snprintf(final_msg, sizeof(final_msg), HASS_SENSOR_TEMPLATE,
                     device_json, avail_json,
                     device->ID, device->label,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID,
                     icon, extra_json);
        }
        // Discovery topic path
        snprintf(topic, sizeof(topic), "%s/%s/aquachemd/aquachemd_%s/config",
                 _acdconfig_.mqtt_discovery_topic, hass_type, device->ID);

        send_mqtt(nc, topic, final_msg);

        // IF it's a Pump, also publish the Timer Sensor, and Dose sensor
        if (device->type == ACD_TYPE_GPIO_PMP || device->type == ACD_TYPE_EZO_PMP)
        {
            snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);
            // Timer
            snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s_timer/config",
                     _acdconfig_.mqtt_discovery_topic, device->ID);

            snprintf(final_msg, sizeof(final_msg), HASS_TIMER_TEMPLATE,
                     device_json, avail_json,
                     device->ID, device->label,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID);

            send_mqtt(nc, topic, final_msg);

            // Doser
            const char *dose_suffix = ((device->flags & PH_PUMP) ? MQTT_TL_DOSE_PH : (device->flags & ORP_PUMP) ? MQTT_TL_DOSE_ORP
                                                                                                            : MQTT_TL_DOSE_UNKNOWN);

            snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s_dose/config",
                     _acdconfig_.mqtt_discovery_topic, device->ID);

            snprintf(final_msg, sizeof(final_msg), HASS_DOSE_TEMPLATE,
                     device_json, avail_json,
                     device->ID, device->label,
                     _acdconfig_.mqtt_aquachemd_topic, device->ID, dose_suffix);

            send_mqtt(nc, topic, final_msg);
        }
    }
}
*/
#ifdef DO_NOT_COMPILE2


// Define the common fragments as simple strings. 
// Note: No need for #define here, we'll just use them in the larger strings.

static const char *HASS_DEVICE_TEMPLATE = 
    "\"device\": {"
        "\"identifiers\": [\"" AQUACHEMD_SHORT_NAME "\"],"
        "\"sw_version\": \"" AQUACHEMD_VERSION "\","
        "\"model\": \"" AQUACHEMD_NAME "\","
        "\"name\": \"" AQUACHEMD_SHORT_NAME "\","
        "\"manufacturer\": \"" AQUACHEMD_SHORT_NAME "\","
        "%s" // connections/url placeholder
        "\"suggested_area\": \"pool\""
    "}";

static const char *HASS_AVAIL_TEMPLATE = 
    "\"availability\": {"
        "\"payload_available\": \"1\","
        "\"payload_not_available\": \"0\","
        "\"topic\": \"%%s/" MQTT_LWM_TOPIC "\"" // Note the double %% for literal %
    "}";

// The full Switch Template
const char *HASSIO_SWITCH_DISCOVER = "{"
    "%s," // Device block (HASS_DEVICE_TEMPLATE)
    "%s," // Avail block (HASS_AVAIL_TEMPLATE)
    "\"type\": \"switch\","
    "\"unique_id\": \"aquachemd_%s\","
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s/status\","
    "\"command_topic\": \"%s/%s/set\","
    "\"payload_on\": \"1\","
    "\"payload_off\": \"0\","
    "\"icon\": \"%s\""
"}";

static const char *HASSIO_SENSOR_DISCOVER = "{"
    "%s," // Device block
    "%s," // Avail block
    "\"type\": \"sensor\","
    "\"unique_id\": \"aquachemd_%s\"," // id + suffix (e.g. "_temp")
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s\","
    "\"value_template\": \"{{ value_json }}\","
    "\"unit_of_measurement\": \"%s\","
    "\"device_class\": \"%s\","  // Can be "temperature" or empty ""
    "\"state_class\": \"%s\","   // Usually "measurement"
    "\"icon\": \"%s\""
"}";

const char *HASSIO_BINARY_SENSOR_DISCOVER = "{"
    "%s," // Device block
    "%s," // Avail block
    "\"type\": \"sensor\","
    "\"unique_id\": \"aquachemd_%s\"," // id + suffix (e.g. "_temp")
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s\","
    "\"payload_on\": \"1\","
    "\"payload_off\": \"0\","
    "\"icon\": \"%s\""
"}";



void publish_mqtt_discovery(struct aquachemdata *acdata, struct mg_connection *nc) {
    char device_json[512];
    char avail_json[256];
    char final_msg[JSON_DISCOVERY_SIZE];
    char connections[128];
    char topic[250];

    const net_iface *iface = get_first_valid_interface();

    // Prepare the dynamic "connections" part
    if (_acdconfig_.mqtt_discovery_use_mac) {
        snprintf(connections, sizeof(connections), 
                 "\"connections\": [[\"mac\", \"%s\"]], \"configuration_url\": \"%s\",", 
                 iface->mac, iface->url);
    } else {
        snprintf(connections, sizeof(connections), 
                 "\"configuration_url\": \"%s\",", iface->url);
    }

    // Build the reusable "device" and "availability" blocks
    snprintf(device_json, sizeof(device_json), HASS_DEVICE_TEMPLATE, connections);
    snprintf(avail_json, sizeof(avail_json), HASS_AVAIL_TEMPLATE, _acdconfig_.mqtt_aquachemd_topic);

    for (acd_key_t *curr = acdata->keys; curr != NULL; curr = curr->next) {
      final_msg[0] = '\0';
      topic[0] = '\0';

      switch (curr->type) {
        case ACD_TYPE_MASTER:
        case ACD_TYPE_GPIO_PMP:
        case ACD_TYPE_EZO_PMP:
          snprintf(final_msg, sizeof(final_msg), HASSIO_SWITCH_DISCOVER,
                     device_json,
                     avail_json,
                     curr->ID, 
                     curr->label, 
                     _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                     _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                     "mdi:toggle-switch-variant");

          snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
          break;
          
        case ACD_TYPE_EZO_TEMP:
        case ACD_TYPE_D1W_TEMP:
        //case ACD_TYPE_MQTT_TEMP: // Data came from MQTT, no need to re post
          snprintf(final_msg, sizeof(final_msg), HASSIO_SENSOR_DISCOVER,
                    device_json,
                    avail_json,
                    curr->ID,
                    curr->label,
                    _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                    "°C",
                    "temperature",
                    "measurement",
                    "mdi:gauge");
          snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
          break;
    
        case ACD_TYPE_EZO_PH:
          snprintf(final_msg, sizeof(final_msg), HASSIO_SENSOR_DISCOVER,
                    device_json,
                    avail_json,
                    curr->ID,
                    curr->label,
                    _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                    "pH",
                    "",
                    "measurement",
                    "mdi:water-outline");
          snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
          break;

        case ACD_TYPE_EZO_ORP:
          snprintf(final_msg, sizeof(final_msg), HASSIO_SENSOR_DISCOVER,
                    device_json,
                    avail_json,
                    curr->ID,
                    curr->label,
                    _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                    "orp",
                    "",
                    "measurement",
                    "mdi:water-outline");
          snprintf(topic, sizeof(topic), "%s/sensor/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
          break;
        //case ACD_TYPE_MQTT_COND: // No point, already gained from MQTT
        case ACD_TYPE_GPIO_COND:
          snprintf(final_msg, sizeof(final_msg), HASSIO_SENSOR_DISCOVER,
                    device_json,
                    avail_json,
                    curr->ID,
                    curr->label,
                    _acdconfig_.mqtt_aquachemd_topic, curr->ID,
                    "mdi:water-outline");
          snprintf(topic, sizeof(topic), "%s/binary_sensor/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
          break;
      }

      if (final_msg[0] != '\0' && topic[0] != '\0') {
        send_mqtt(nc, topic, final_msg);
      }

    }
}








#endif




#ifdef DO_NOT_COMPILE


#define HASS_DEVICE "\"identifiers\": " \
                        "[\"" AQUACHEMD_SHORT_NAME "\"]," \
                        " \"sw_version\": \"" AQUACHEMD_VERSION "\"," \
                        " \"model\": \"" AQUACHEMD_NAME "\"," \
                        " \"name\": \"" AQUACHEMD_SHORT_NAME "\"," \
                        " \"manufacturer\": \"" AQUACHEMD_SHORT_NAME "\"," \
                        "%s" \
                        " \"suggested_area\": \"pool\""

#define HASS_AVAILABILITY "\"payload_available\" : \"1\"," \
                          "\"payload_not_available\" : \"0\"," \
                          "\"topic\": \"%s/" MQTT_LWM_TOPIC "\""


const char *HASSIO_SWITCH_DISCOVER = "{"
    "\"device\": {" HASS_DEVICE "},"
    "\"availability\": {" HASS_AVAILABILITY "},"
    "\"type\": \"switch\","
    "\"unique_id\": \"aquachemd_%s\","
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s/status\","
    "\"command_topic\": \"%s/%s/set\","
    "\"payload_on\": \"1\","
    "\"payload_off\": \"0\","
    "\"icon\": \"%s\""
"}";

const char *HASSIO_TEMP_SENSOR_DISCOVER = "{"
    "\"device\": {" HASS_DEVICE "},"
    "\"availability\": {" HASS_AVAILABILITY "},"
    "\"type\": \"sensor\","
    "\"state_class\": \"measurement\","
    "\"unique_id\": \"aqualinkd_%s_temp\","
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s\","
    "\"value_template\": \"{{ value_json }}\","
    "\"unit_of_measurement\": \"%s\","
    "\"device_class\": \"temperature\","
    "\"icon\": \"%s\""
"}";

const char *HASSIO_SENSOR_DISCOVER = "{"
    "\"device\": {" HASS_DEVICE "},"
    "\"availability\": {" HASS_AVAILABILITY "},"
    "\"type\": \"sensor\","
    "\"state_class\": \"measurement\","
    "\"unique_id\": \"aqualinkd_%s\","
    "\"name\": \"%s\","
    "\"state_topic\": \"%s/%s\","
    "\"value_template\": \"{{ value_json }}\","
    "\"unit_of_measurement\": \"%s\","
    "\"icon\": \"%s\""
"}";


void publish_mqtt_discovery(struct aquachemdata *acdata, struct mg_connection *nc)
{
  char topic[250];
  char msg[JSON_DISCOVERY_SIZE];
  //char idbuf[128];
  char connections[128];

  const net_iface *iface = get_first_valid_interface();

  LOG(LOG_NOTICE, "Publishing MQTT discovery - NOT IMPLIMENTED\n");

  if (_acdconfig_.mqtt_discovery_use_mac) {
    sprintf(connections, "\"connections\": [[\"mac\", \"%s\"]],\"configuration_url\": \"%s\",", iface->mac, iface->url);
  } else {
    connections[0] = '\0';
    sprintf(connections,"\"configuration_url\": \"%s\",", iface->url);
  }

  // First thing we will publish names/labels to aquachemd topics. (only once at startup)

  for (acd_key_t *curr = acdata->keys; curr != NULL; curr = curr->next) { 
    //sprintf(topic, "%s/%s/label", _acdconfig_.mqtt_aquachemd_topic, curr->ID);
    //send_mqtt(nc, topic, curr->label);

    switch(curr->type) {
      case ACD_TYPE_MASTER:
        sprintf(msg, HASSIO_SWITCH_DISCOVER,
             connections,
             _acdconfig_.mqtt_aquachemd_topic,
             curr->ID, 
             curr->label, 
             _acdconfig_.mqtt_aquachemd_topic,curr->ID,
             _acdconfig_.mqtt_aquachemd_topic,curr->ID,
             "mdi:toggle-switch-variant");
        sprintf(topic, "%s/switch/aquachemd/aquachemd_%s/config", _acdconfig_.mqtt_discovery_topic, curr->ID);
        send_mqtt(nc, topic, msg);
      break;
      default:
      break;
    }

    
  }
}

#endif