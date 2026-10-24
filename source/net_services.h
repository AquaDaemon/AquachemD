#ifndef NET_SERVICES_H_
#define NET_SERVICES_H_ 

#include "aquachemd.h"

bool start_net_services(struct aquachemdata *acddata);
void stop_net_services();
pthread_t get_net_services_id();

//acd_runstate_t get_net_services_state();

void send_mqtt(struct mg_connection *nc, const char *toppic, const char *message);

bool post_dosing_event(acd_key_t *key, uint32_t runtime, float total_ml);

#define URI_LEN 32

#define MQTT_ON "1"
#define MQTT_OFF "0"
#define MQTT_LWM_TOPIC "Alive"

// Mqtt Topic Levels.
#define MQTT_TL_STATE    "state"
#define MQTT_TL_TIMER    "timer"
#define MQTT_TL_DURATION "duration" // For timer
#define MQTT_TL_DEFAULT  "default"  // For timer
/*
#define MQTT_TL_DOSE_PH  "total_acid_ml"
#define MQTT_TL_DOSE_ORP "total_chlorine_ml"
#define MQTT_TL_DOSE_H2O "total_water_ml"
#define MQTT_TL_DOSE_UNKNOWN "total_dose_ml"
*/
#define MQTT_TL_LAST_DOSE "last_dose_ml"
#define MQTT_TL_RUNNING_DOSE "running_dose_ml"
#define MQTT_TL_AVERAGE   "average"
#define MQTT_TL_LEVEL     "level"

// MQTT Topic Sub Levels.
#define MQTT_TSL_REMAINING_VOLUME   "remaining"
#define MQTT_TSL_TOTAL_VOLUME  "volume"

//#define MQTT_TEMP_TOPIC "temp"
//#define MQTT_PH_TOPIC "ph"
//#define MQTT_ORP_TOPIC "orp"

#define CACHE    "Cache-Control: public, max-age=604800, immutable\r\n" // 7 days
#define NO_CACHE "Cache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n"
#define CONTENT_JSON NO_CACHE"Content-Type: application/json\r\n"
#define CONTENT_JS   NO_CACHE"Content-Type: text/javascript\r\n"
#define CONTENT_TEXT NO_CACHE "Content-Type: text/plain\r\n"

//#define JSON_GOOD_REPLY "{\"status\":\"success\"}"
//#define JSON_ERROR_REPLY "{\"status\":\"error\"}"
//#define JSON_ERROR_UNKNOWN_REQUEST "{\"status\":\"error\",\"message\":\"unknown request\"}"
#define JSON_ERROR_FMT "{\"status\":\"error\",\"message\":\"%s\"}"
#define JSON_GOOD_FMT "{\"status\":\"success\",\"message\":\"%s\"}"

#define GET_RTN_OK "Ok"
#define GET_RTN_UNKNOWN "Unknown command"
#define GET_RTN_NOT_CHANGED "Not Changed"
#define GET_RTN_ERROR "Error"

#endif // NET_SERVICES_H_
