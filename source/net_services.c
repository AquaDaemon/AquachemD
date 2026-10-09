
#define _POSIX_C_SOURCE 200809L // for strdup
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "mongoose.h"

#include "aquachemd.h"
#include "utils.h"
#include "config.h"
#include "net_services.h"
#include "mqtt_discovery.h"
#include "net_interface.h"
#include "json_messages.h"
#include "state_manager.h"
#include "acd_scheduler.h"
#include "acd_timer.h"
#include "sensor_stats.h"
#include "web_config.h"
#include "version.h"


static struct aquachemdata *_aquachemd_data;
//static char *_web_root;
//static acd_runstate_t _netsrunstate;

static acd_thread_t _netsrunstate = {
    .parent_id = 0,
    .id = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER, 
    .cond = PTHREAD_COND_INITIALIZER, 
    .state = ACD_STARTING
};

//static pthread_t _net_thread_id = 0;
//static bool _keepNetServicesRunning = false;
static struct mg_mgr _mgr;
static int _mqtt_exit_flag = false;

static struct mg_http_serve_opts _http_server_opts;
static struct mg_http_serve_opts _http_server_opts_nocache;

void reset_last_mqtt_status();
void broadcast_aquachemdstate(struct mg_connection *nc, bool force_all);
void start_mqtt(struct mg_mgr *mgr);
static void ws_send(struct mg_connection *nc, const char *msg);
bool broadcast_systemd_logmessages(bool acdMgrActive);


#ifdef USE_SYSTEMD
  #include <systemd/sd-journal.h>
  #define JOURNAL_FAIL_RETRY 5
  #define BLANK_READ_LIMIT 100
  #define WS_LOG_LENGTH 400
  static const char *MSG_JOURNAL_GIVEUP = "Giving up on journal, don't expect to see logs";
  static const char *MSG_JOURNAL_OPEN_FAILED = "Failed to open journal";
  void ws_send_logmsg(struct mg_connection *nc, char *msg);
#endif


#define FAST_SUFFIX_3_CI(str, len, SUFFIX) ( \
    (len) >= 3 && \
    tolower((unsigned char)(str)[(len)-3]) == tolower((unsigned char)(SUFFIX)[0]) && \
    tolower((unsigned char)(str)[(len)-2]) == tolower((unsigned char)(SUFFIX)[1]) && \
    tolower((unsigned char)(str)[(len)-1]) == tolower((unsigned char)(SUFFIX)[2]) \
)

#define FAST_SUFFIX_4_CI(str, len, SUFFIX) ( \
    (len) >= 4 && \
    tolower((unsigned char)(str)[(len)-4]) == tolower((unsigned char)(SUFFIX)[0]) && \
    tolower((unsigned char)(str)[(len)-3]) == tolower((unsigned char)(SUFFIX)[1]) && \
    tolower((unsigned char)(str)[(len)-2]) == tolower((unsigned char)(SUFFIX)[2]) && \
    tolower((unsigned char)(str)[(len)-1]) == tolower((unsigned char)(SUFFIX)[3]) \
)

static struct {
    volatile bool is_dirty;
    acd_key_t *key;
    uint32_t runtime; 
    float dose_ml;
} _dose_event;

void reset_dose_event()
{
  _dose_event.is_dirty = false;
  _dose_event.key = NULL;
  _dose_event.runtime = 0;
  _dose_event.dose_ml = 0;
}

void post_dosing_event(acd_key_t *key, uint32_t runtime, float total_ml)
{
   // Store info and set flag for next run.
   
   _dose_event.key = key;
   _dose_event.runtime = runtime;
   _dose_event.dose_ml = total_ml;
   
   _dose_event.is_dirty = true;
   //_aquachemd_data.is_dirty = true;
}

struct mg_connection *mg_next(struct mg_mgr *s, struct mg_connection *conn) {
  return conn == NULL ? s->conns : conn->next;
}

static void net_signal_handler(int sig_num) {
  intHandler(sig_num); // Force signal handler to aquachemd.c
}

static void mg_logger(char ch, void *param) {
  
  static char buf[256];
  static size_t len;
  buf[len++] = ch;
  if (ch == '\n' || len >= sizeof(buf)) {
    //syslog(LOG_INFO, "%.*s", (int) len, buf); // Send logs
    LOG(LOG_INFO, buf);
    len = 0;
    memset(buf, 0, sizeof(buf));
  }
}


//  helper functions

/*
static int is_websocket(const struct mg_connection *nc) {
  return IS_WEBSOCKET(nc);
}

static int is_mqttconnecting(const struct mg_connection *nc) {
  return IS_MQTT_CONNECTING(nc);
}

static inline int is_websocket_acdmanager(const struct mg_connection *nc) {
  return GET_AQD_FLAGS(nc) & AQD_MG_CON_WS_AQM;
}

static inline int is_mqtt(const struct mg_connection *nc) {
  return GET_AQD_FLAGS(nc) & (AQD_MG_CON_MQTT | AQD_MG_CON_MQTT_CONNECTING);
}
*/

static bool is_websocket(const struct mg_connection *nc) {
  return IS_WEBSOCKET(nc);
}

static bool is_mqttconnecting(const struct mg_connection *nc) {
  // The macro returns 8, but assigning/returning it as a bool casts it to true (1)
  return IS_MQTT_CONNECTING(nc); 
}

static inline bool is_websocket_acdmanager(const struct mg_connection *nc) {
  return (GET_AQD_FLAGS(nc) & AQD_MG_CON_WS_AQM) != 0;
}

static inline bool is_mqtt(const struct mg_connection *nc) {
  return (GET_AQD_FLAGS(nc) & (AQD_MG_CON_MQTT | AQD_MG_CON_MQTT_CONNECTING)) != 0;
}





static inline void set_mqttconnecting(struct mg_connection *nc) {
  uintptr_t f = GET_AQD_FLAGS(nc);
  SET_AQD_FLAGS(nc, f | AQD_MG_CON_MQTT_CONNECTING);
}

static inline void set_websocket_acdmanager(struct mg_connection *nc) {
  uintptr_t f = GET_AQD_FLAGS(nc);
  SET_AQD_FLAGS(nc, f | AQD_MG_CON_WS_AQM);
}

static inline void set_mqttconnected(struct mg_connection *nc) {
  uintptr_t f = GET_AQD_FLAGS(nc);
  f |= AQD_MG_CON_MQTT;
  f &= ~AQD_MG_CON_MQTT_CONNECTING;
  SET_AQD_FLAGS(nc, f);
}

void log_mg_str(int level, char *name, struct mg_str str) {
  char buf[256];
  size_t len = str.len < sizeof(buf) - 1 ? str.len : sizeof(buf) - 1;
  memcpy(buf, str.buf, len);
  buf[len] = '\0';
  LOG(level, "%s: %s", name, buf);
}


bool process_sensor_request(char *buffer, size_t buf_size, char *ws_request, float calibrationValue, bool is_calibration)
{
  int cal_rtn=EZO_SUCCESS;
  int read_rtn=EZO_SUCCESS;
  float value = 0.0f;
  if (_aquachemd_data == NULL) {
    goto f_end;
  }

  //LOG(LOG_ERR,"process_sensor_request() %s '%s'\n",is_calibration?"calibration":"reading", ws_request);

  if (strcasecmp(ws_request, "cal_ph") == 0) {
    ezo_sensor_t ph = { .address = EZO_PH_ADDR }; // Need to pass ADDR from UI in future
    if (is_calibration) {
      LOG(LOG_NOTICE, "Calibrating pH - %.2f",calibrationValue);
      cal_rtn = ph_calibrate_by_value(&ph, calibrationValue);
    }
    if (cal_rtn == EZO_SUCCESS) {
      LOG(LOG_NOTICE, "Reading pH");
      ph_reading_t reading = ph_get_reading(&ph);
      if (reading.status == EZO_SUCCESS){value=reading.value; LOG(LOG_NOTICE, "pH reading %.2f",value);}
      else {LOG(LOG_ERR, "Failed to read pH sensor, status %d", reading.status);}
      read_rtn = reading.status;
    }

  } else if (strcasecmp(ws_request, "cal_orp") == 0) {
    ezo_sensor_t orp = { .address = EZO_ORP_ADDR }; // Need to pass ADDR from UI in future
    if (is_calibration) {
      LOG(LOG_NOTICE, "Calibrating ORP - %.2f",calibrationValue);
      cal_rtn = orp_calibrate(&orp, calibrationValue);
    }
    if (cal_rtn == EZO_SUCCESS) {
      LOG(LOG_NOTICE, "Reading ORP");
      orp_reading_t reading = orp_get_reading(&orp);
      if (reading.status == EZO_SUCCESS){value=reading.value; LOG(LOG_NOTICE, "ORP reading %.2f",value);}
      else {LOG(LOG_ERR, "Failed to read ORP sensor, status %d", reading.status);}
      read_rtn = reading.status;
    }

  } else if (strcasecmp(ws_request, "cal_rtd") == 0) {
    ezo_sensor_t rtd = { .address = EZO_RTD_ADDR }; // Need to pass ADDR from UI in future
    
    if (is_calibration) {
      LOG(LOG_NOTICE, "Calibrating Temperature - %.2f",calibrationValue);
      cal_rtn = rtd_calibrate(&rtd, calibrationValue);
    }
    if (cal_rtn == EZO_SUCCESS) {
      LOG(LOG_NOTICE, "Reading Temperature");
      rtd_reading_t reading = rtd_get_reading(&rtd);
      if (reading.status == EZO_SUCCESS){value=reading.value; LOG(LOG_NOTICE, "Temperature reading %.2f",value);}
      else {LOG(LOG_ERR, "Failed to read Temperature sensor, status %d", reading.status);}
      read_rtn = reading.status;
    }

  } else if (strcasecmp(ws_request, "cal_prs") == 0) {
    ezo_sensor_t prs = { .address = EZO_PRS_ADDR }; // Need to pass ADDR from UI in future
    
    if (is_calibration) {
      LOG(LOG_NOTICE, "Calibrating Pressure - %.2f",calibrationValue);
      cal_rtn = prs_calibrate(&prs, calibrationValue);
    }
    if (cal_rtn == EZO_SUCCESS) {
      LOG(LOG_NOTICE, "Reading Pressure");
      prs_reading_t reading = prs_get_reading(&prs);
      if (reading.status == EZO_SUCCESS){value=reading.value; LOG(LOG_NOTICE, "Pressure reading %.2f",value);}
      else {LOG(LOG_ERR, "Failed to read Pressure sensor, status %d", reading.status);}
      read_rtn = reading.status;
    }
  }

  f_end:

  if (cal_rtn != EZO_SUCCESS) {
    snprintf(buffer,buf_size, "{\"type\":\"instant_sensor_message\",\"status\":\"error\",\"error\":\"%d\",\"error_msg\":\"error calibrating sensor\"}",cal_rtn);
    return false;
  } else if (read_rtn != EZO_SUCCESS) {
    snprintf(buffer,buf_size, "{\"type\":\"instant_sensor_message\",\"status\":\"error\",\"error\":\"%d\",\"error_msg\":\"error reading sensor\"}",read_rtn);
    return false;
  } else {
    snprintf(buffer,buf_size, "{\"type\":\"instant_sensor_message\",\"status\":\"ok\",\"request_type\":\"%s\",\"action_type\":\"%s\",\"value\":\"%.2f\"}",
             ws_request,
             is_calibration?"calibration":"reading",
             value);
    return true;
  }
}



void send_mqtt(struct mg_connection *nc, const char *toppic, const char *message)
{
  //static uint16_t msg_id = 0;

  if (toppic == NULL)
    return;

  struct mg_mqtt_opts pub_opts = {.topic = mg_str(toppic),
                                .message = mg_str(message),
                                .qos = 1,
                                .retain = true};
  uint16_t msg_id = mg_mqtt_pub(nc, &pub_opts);

  LOG(LOG_INFO, "MQTT: Published id=%d: %s %s\n", msg_id, toppic, message);
}


void send_ws(struct mg_connection *nc, const char *msg)
{
  int size = strlen(msg);
  
  mg_ws_send(nc, msg, size, WEBSOCKET_OP_TEXT);
  
  //LOG(NET_LOG,LOG_DEBUG, "WS: Sent %d characters '%s'\n",size, msg);
}

/**
 * Sends a formatted JSON reply over a WebSocket.
 * @param c       The mongoose connection.
 * @param success True for success, false for error.
 * @param msg     Optional custom message (can be NULL).
 */
void send_ws_reply(struct mg_connection *c, bool success, const char *msg) {
  if (success) {
        // Use "ok" as the default success message if NULL
    mg_ws_printf(c, WEBSOCKET_OP_TEXT, JSON_GOOD_FMT, (msg == NULL) ? "ok" : msg);
  } else {
        // Use "unknown error" as the default failure message if NULL
    mg_ws_printf(c, WEBSOCKET_OP_TEXT, JSON_ERROR_FMT, (msg == NULL) ? "unknown error" : msg);
  }
}

/*
  Quicker and more accurate for us than normal strncmp, since we check for the trailing / at right position
  check Spa against uri /Spa/set /Spa_mode/set / Spa_heater/set
*/
bool uri_strcmp(const char *uri, const char *string) {
  int i;
  int len = strlen(string);

  // Check the trailing / on length first.
  if (uri[len] != '/') {
    return false;
  }

  // Now check all characters
  for (i=0; i < len; i++) {
    if ( uri[i] != string[i] ){
      return false;
    } 
  }

  return true;
}

void serve_file(struct mg_connection *nc, struct mg_http_message *http_msg)
{
  // Anything with .json should not be cached.
  // NSF FIX mg_http_serve_dir fails when directory does not exist and holds up webbrowser.
  if (FAST_SUFFIX_4_CI(http_msg->uri.buf, http_msg->uri.len, "json"))
  {
    /*
    if (_acdconfig_.web_directory != NULL && strncmp(http_msg->uri.buf, "/config.json", 12) == 0)
    {
      mg_http_serve_file(nc, http_msg, _acdconfig_.web_directory, &_http_server_opts_nocache);
      LOG(LOG_NOTICE, "Using %s for web config\n", _acdconfig_.web_directory);
    }
    else*/
    {
      mg_http_serve_dir(nc, http_msg, &_http_server_opts_nocache);
    }
  }
  else // can cache anything here.
  {
    if (http_msg->uri.len <= 12 && strncmp(http_msg->uri.buf, "/acdmanager", 10) == 0) {
      char buf[256];
      snprintf(buf, 256, "%s/acdmanager.html", _acdconfig_.web_directory);
      mg_http_serve_file(nc, http_msg, buf, &_http_server_opts);
    } else {
      mg_http_serve_dir(nc, http_msg, &_http_server_opts);
    }
  }
}

typedef enum {uActioned, uBad, uDevices, uHomebridgeDevices, uConfig, uSaveConfig, uSaveWebConfig, uSaveSchedules,  uSchedules, uACDmanager, uDoseStats, uCalibrate, uInstantReading} uriAtype;

#define NO_DEVICE         "No matching Device found"
#define INVALID_VALUE     "Invalid value"
#define UNKNOWN_REQUEST   "Didn't understand request"
#define REJECTED_REQUEST  "Request was rejected"
//#define ACTIONED_REQUEST  "Request was actioned"

uriAtype action_URI(const char *URI, int uri_length, float value, bool convertTemp, char **rtnmsg) 
{
  //uriAtype rtn = uBad;
  //bool found = false;
  int i;
  char *ri1 = (char *)URI;
  char *ri2 = NULL;
  char *ri3 = NULL;
  char *ri4 = NULL;

  LOG(LOG_DEBUG, "URI Request '%.*s': value %.2f\n", uri_length, URI, value);

  // Split up the URI into parts.
  for (i=1; i < uri_length; i++) {
    if ( URI[i] == '/' ) {
      if (ri2 == NULL) {
        ri2 = (char *)&URI[++i];
      } else if (ri3 == NULL) {
        ri3 = (char *)&URI[++i];
      } else if (ri4 == NULL) {
        ri4 = (char *)&URI[++i];
        break;
      }
    }
  }

  if (strncmp(ri1, "devices", 7) == 0) {
    return uDevices;
  //} else if (strncmp(ri1, "status", 6) == 0) {
  //  return uStatus;
  } else if (strncmp(ri1, "homebridge", 10) == 0) {
    return uHomebridgeDevices;
  } else if (strncmp(ri1, "schedules/set", 13) == 0) {
    return uSaveSchedules;
  } else if (strncmp(ri1, "schedules", 9) == 0) {
    return uSchedules;
  } else if (strncmp(ri1, "config/set", 10) == 0) {
    return uSaveConfig;
  } else if (strncmp(ri1, "webconfig/set", 13) == 0) {
    return uSaveWebConfig;
  } else if (strncmp(ri1, "config", 6) == 0) {
    return uConfig;
  } else if (strncmp(ri1, "acdmanager", 10) == 0 || strncmp(ri1, "manager", 7) == 0/*&& from == NET_WS*/) { // Only valid from websocket.
    return uACDmanager;
  } else if (strncmp(ri1, "setloglevel", 11) == 0 /*&& from == NET_WS*/) { // Only valid from websocket.
    set_loglevel(round(value));
    return uACDmanager; // Want to resent updated status
  } else if (strncmp(ri1, "dosestats", 9) == 0) {
    return uDoseStats;
  } else if (strncmp(ri1, "calibrate", 9) == 0) {
    return uCalibrate;
  } else if (strncmp(ri1, "instantreading", 9) == 0) {
    return uInstantReading;
  } else if (strncmp(ri1, "scan", 4) == 0 || strncmp(ri1, "deepscan", 8) == 0) {
    bool is_deep = (strncmp(ri1, "deepscan", 8) == 0);
    //gpio_detect(is_deep, true);
    //w1_detect(true);
    //ezo_i2cdetect(true);
    scan_sensors(true, is_deep);
    return uActioned;
    /*
  } else if (strncmp(ri1, "reset_stats", 11) == 0) {
    if ( reset_sensors_average_by_duration(_aquachemd_data, ri2) ) {
      return uActioned;
    } */
  // Handle "aquachemd/reset_stats" at moment aquachemd can be anything as we don;t check.
  } else if (ri2 && (strncmp(ri2, "reset_stats", 11) == 0 || strncmp(ri2, "reset_sensor_stats", 18) == 0)) {
  //} else if (ri2 && strncmp(ri2, "reset_sensor_stats", 18) == 0) { // NSF Need to move to this in future
      for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
        if (uri_strcmp(ri1, curr->ID)) {
          // If aquachemd is the ID then expect value and use reset by any matching values.
          if (ACD_TYPE_MASTER == curr->type) {
            reset_sensors_average_by_hours(_aquachemd_data, value);
          } else {
            reset_sensor_average(curr);
          }
          return uActioned;
        }
      }
  } else if (ri2 && strncmp(ri2, "reset_dose_stats", 16) == 0) {
    for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
      if (uri_strcmp(ri1, curr->ID)) {
        if (isMASKSET(curr->flags, PH_PUMP) || isMASKSET(curr->flags, ORP_PUMP) || isMASKSET(curr->flags, H2O_PUMP)) {
          reset_dose_running_total(curr);
          _aquachemd_data->is_dirty = true;
          return uActioned;
        } else {
          *rtnmsg = REJECTED_REQUEST; // not a doser -- nothing to reset here
          return uBad;
        }
      }
    }
  } else if (ri2 && (strncasecmp(ri2, "set", 3) == 0)) {
    for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
      if (uri_strcmp(ri1, curr->ID) && (value == ACD_LED_OFF || value == ACD_LED_ON || value == ACD_LED_ENABLED)) {
        LOG(LOG_INFO, "Request to set '%s' to '%s'", curr->label, acd_state_to_str(value));
        curr->is_dirty = true; // Force an update to be sent to the device, even if the state is already set to the requested value.
        if (state_change_request(_aquachemd_data, curr, value)) {
          return uActioned;
        } else {
          *rtnmsg = REJECTED_REQUEST;
        }
      }
    }
  } else if (ri2 && strncasecmp(ri2, "timer", 5) == 0 && 
             ri3 && strncasecmp(ri3, "set", 3) == 0) {
    for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
      if (uri_strcmp(ri1, curr->ID) && (value > 0)) {
        LOG(LOG_INFO, "Request to set '%s' to run for %f seconds", curr->label, value);
        curr->is_dirty = true; // Force an update to be sent to the device, even if the state is already set to the requested value.
        if (state_change_request_extended(_aquachemd_data, curr, ACD_LED_ON, (uint32_t)value)) {
          return uActioned;
        } else {
          *rtnmsg = REJECTED_REQUEST;
        }
      }
    }
  } else if (ri2 && (strncasecmp(ri2, "timer", 5) == 0) && 
             ri3 && (strncasecmp(ri3, "default", 7) == 0) && 
             ri4 && (strncasecmp(ri4, "set", 3) == 0)) {
    for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
      if (uri_strcmp(ri1, curr->ID) && (value > 0)) {
        LOG(LOG_INFO, "Request to set '%s' default run time to %f seconds", curr->label, value);
        set_pump_default_duration(curr, (uint32_t)value);
        _aquachemd_data->is_dirty = true;
        return uActioned;
      }
    }
  } else if (ri2 != NULL && (strncasecmp(ri2, "level", 5) == 0)) {
    for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
      if (uri_strcmp(ri1, curr->ID)) {
        acd_uom_t uom = UOM_NONE;
        if (ri3 != NULL && (strncasecmp(ri3, "set", 3) == 0)){
          uom = UOM_PERCENT;
        } else if (ri3 != NULL && (strncasecmp(ri3, "percent", 7) == 0) &&
                  (ri4 != NULL && (strncasecmp(ri4, "set", 3) == 0))){
          uom = UOM_PERCENT;
        } else if (ri3 != NULL && (strncasecmp(ri3, "remaining", 9) == 0) &&
                  (ri4 != NULL && (strncasecmp(ri4, "set", 3) == 0))){
          uom = curr->data.tank.uom;
        }

        if (uom != UOM_NONE) {
          LOG(LOG_INFO, "Request to set '%s' tank volume to %f%% %s", curr->label, value, uom_to_str(uom));
          set_tank_volume(curr, uom, value);
          validate_pumps_against_tanks(_aquachemd_data);
          _aquachemd_data->is_dirty = true; // Force an update to be sent to the device, even if the state is already set to the requested value.
          return uActioned;
        }
      }
    }
    *rtnmsg = UNKNOWN_REQUEST;
  } else if (strncmp(ri1, "restart", 7) == 0 ) { // Only valid from websocket.
    LOG(LOG_NOTICE, "Received restart request!\n");
    raise(SIGRESTART);
    return uActioned;
  } else if (strncmp(ri1, "installrelease", 14) == 0 ) { // Only valid from websocket.
    if (ri2 != NULL) {
      LOG(LOG_NOTICE, "Received install release request, %s\n",ri2);
      set_upgrade_version(ri2);
      //_aqualink_data->upgrade_version = malloc( (sizeof(char*) * strlen(ri2)) + 1);
      //snprintf(_aqualink_data->upgrade_version, strlen(ri2)+1, ri2);
    } else {
      LOG(LOG_NOTICE, "Received install release request, but no version named, using latest!\n");
      //_aqualink_data->upgrade_version = "latest";
      set_upgrade_version("latest");
    }
    raise(SIGRUPGRADE);
    return uActioned;
  } else {
    *rtnmsg = UNKNOWN_REQUEST;
  }

  return uBad;
}

void action_web_request(struct mg_connection *nc, struct mg_http_message *http_msg)
{
  char buf[256];
  char *msg = NULL;
  float value = 0;

  log_mg_str(LOG_DEBUG, "URI request", http_msg->uri);
  log_mg_str(LOG_DEBUG, "Query request", http_msg->query);

  //const char* devices_json = get_devices_json(_aquachemd_data);

  if (strncmp(http_msg->uri.buf, "/api", 4) != 0)
  {
    serve_file(nc, http_msg);
    return;
  }
  
  // If query string.
  if (http_msg->query.len > 1)
  {
    // mg_get_http_var(&http_msg->query, "value", buf, sizeof(buf)); // Old mosquitto
    mg_http_get_var(&http_msg->query, "value", buf, sizeof(buf));
    value = atof(buf);
  }
  else if (http_msg->body.len > 1)
  {
    value = parse_payload_value(http_msg->body.buf, http_msg->body.len);
  }

  int len = mg_url_decode(http_msg->uri.buf, http_msg->uri.len, buf, 50, 0);

  struct mg_str *source = mg_http_get_header(http_msg, "X-AquachemD-Source");
  if (source != NULL && mg_strcmp(*source, mg_str("cron")) == 0) {
    // Request came from the cron
  }

  if (strncmp(http_msg->uri.buf, "/api/", 4) == 0)
  {
    switch (action_URI(&buf[5], len - 5, value, false, &msg)){
      case uActioned:
        mg_http_reply(nc, 200, CONTENT_TEXT, GET_RTN_OK);
        break;
      case uDevices:
      case uHomebridgeDevices:
        const char* devices_json = get_devices_json(_aquachemd_data);
        mg_http_reply(nc, 200, CONTENT_JSON, devices_json);
        break;
      case uACDmanager: // Ony for debugging, no need to support web request of this, only websocket.
        char message[1028];
        build_acdmanager_json(_aquachemd_data, message, sizeof(message));
        mg_http_reply(nc, 200, CONTENT_JSON, message);
        break;
      default:
        mg_http_reply(nc, 400, CONTENT_TEXT, GET_RTN_UNKNOWN);
        break;
    }
  }

}

void action_websocket_request(struct mg_connection *nc, struct mg_ws_message *wm) 
{
  char uri[URI_LEN];
  float val;
  char *msg = NULL;
  //char message[2048];
  //char message[8192];
  //char message[32768]; // 32 * 1024 = 32KiB
  char message[65536]; // 64 * 1024 - 64KiB

  log_mg_str(LOG_DEBUG, "WS: Websocket message", wm->data);

  if (!parse_json_uri_command(wm->data.buf, wm->data.len, uri, &val)) {
    LOG(LOG_ERR, "Failed to parse command payload '%.*s'\n",wm->data.len,wm->data.buf);
    send_ws_reply(nc, false, "Failed to parse command");
    return;
  }

  LOG(LOG_DEBUG, "WS: URI %s, Value: %.1f\n", uri, val);

  uriAtype type = action_URI(uri, strlen(uri), val, false, &msg);
  switch (type){
    case uActioned:
      send_ws_reply(nc, true, NULL);
      break;
    case uDevices:
      const char* devices_json = get_devices_json(_aquachemd_data);
      send_ws(nc, devices_json);
      break;
    case uSchedules:
      build_schedules_js(message, sizeof(message));
      ws_send(nc, message);
      break;
    case uSaveSchedules:
      save_schedules_js((char *)wm->data.buf, wm->data.len, message, sizeof(message));
      ws_send(nc, message);
      break;
    case uConfig:
      build_aquachem_config_json(message, sizeof(message));
      ws_send(nc, message);
      break;
    case uSaveConfig:
      save_aquachem_config_json((char *)wm->data.buf, wm->data.len, message, sizeof(message), _aquachemd_data);
      ws_send(nc, message);
      break;
    case uSaveWebConfig:
      save_web_config_json((char *)wm->data.buf, wm->data.len, message, sizeof(message), _aquachemd_data);
      ws_send(nc, message);
      break;
    case uACDmanager:
      if (!is_websocket_acdmanager(nc)){
        set_websocket_acdmanager(nc);
        _aquachemd_data->acdManagerActive = true;
        LOG(LOG_INFO, "Started AquachemD Manager");
      }
      build_acdmanager_json(_aquachemd_data, message, sizeof(message));
      ws_send(nc, message);
      break;
    case uDoseStats:
      if (val < 1) val = 1; // Minimum 1 day
      get_pump_summaries_json(val, true, message, sizeof(message));
      ws_send(nc, message);
      break;
    case uCalibrate:
    case uInstantReading:
      char *last_slash = strrchr(uri, '/') + 1;
      LOG(LOG_DEBUG, "EZO %s %s with %.2f\n", type==uCalibrate?"Calibrate":"Take instant reading", last_slash, val);
      process_sensor_request(message, sizeof(message), last_slash, val, type==uCalibrate?true:false);
      ws_send(nc, message);
      break;

    default:
      send_ws_reply(nc, false, msg);
      break;
  }

  // Send updated devices
  ws_send(nc, get_devices_json(_aquachemd_data)); 
}

bool action_mqtt_message(struct mg_connection *nc, struct mg_mqtt_message *msg) 
{
    char *rtnmsg = NULL;
    float value = 0;
    char *endptr;

    // 1. Logging using the original Mongoose strings
    log_mg_str(LOG_DEBUG, "MQTT: message topic", msg->topic);
    log_mg_str(LOG_DEBUG, "MQTT: message payload", msg->data);

    // Convert payload to float
    value = strtof(msg->data.buf, &endptr);

    // Handle Non-Numeric Payloads (Home Assistant compat)
    // If endptr didn't move, or if the payload is clearly a string command
    if (endptr == msg->data.buf) {
        // Create a temporary stack string for our trim comparison
        char payload[32];
        snprintf(payload, sizeof(payload), "%.*s", (int)msg->data.len, msg->data.buf);
        if (strtrimcasecmp(payload, "on") == 0 || 
            strtrimcasecmp(payload, "heat") == 0 || 
            strtrimcasecmp(payload, "cool") == 0) {
            value = 1.0f;
        }
        LOG(LOG_DEBUG, "MQTT: Map '%s' -> %.0f for topic %.*s\n", payload, value, (int)msg->topic.len, msg->topic.buf);
    }

    // Assuming the topic starts with base topic,  skip it to get the "Command" part
    size_t base_len = strlen(_acdconfig_.mqtt_aquachemd_topic);
    size_t offset = base_len + 1; // +1 to skip the trailing slash

    if (msg->topic.len > offset) {
        const char *uri_part = &msg->topic.buf[offset];
        size_t uri_len = msg->topic.len - offset;

        // 5. Execute Action
        if (action_URI(uri_part, uri_len, value, false, &rtnmsg) == uBad) {
          LOG(LOG_WARNING, "MQTT: Action failed '%.*s'", (int)msg->topic.len, msg->topic.buf);
        }
        return true;
    }

    return false;
}

bool action_mqtt_condition_message(acd_key_t *condition, struct mg_mqtt_message *mqtt_msg)
{
  LOG(LOG_INFO, "MQTT: Received message for condition '%s': %.*s\n", condition->label, mqtt_msg->data.len, mqtt_msg->data.buf);
  
  if (strncmp(mqtt_msg->data.buf, condition->data.mqtt.target_value, mqtt_msg->data.len) == 0) {
    if (ASSIGN_IF_CHANGED(condition->met, true, _aquachemd_data->is_dirty, condition->is_dirty)) {
      LOG(LOG_INFO, "MQTT Condition Change: %s is now SATISFIED\n", condition->label); 
    } 
    set_key_state(_aquachemd_data, condition, ACD_LED_ON);
    return true;
  } else {
    if (ASSIGN_IF_CHANGED(condition->met, false, _aquachemd_data->is_dirty, condition->is_dirty)) {
      LOG(LOG_INFO, "MQTT Condition Change: %s is now NOT MET\n", condition->label); 
    }
    set_key_state(_aquachemd_data, condition, ACD_LED_OFF);
    return true;
  }

  return false;

}

bool action_mqtt_sensor_message(acd_key_t *sensor, struct mg_mqtt_message *mqtt_msg)
{
  LOG(LOG_INFO, "MQTT: Received message for sensor '%s': %.*s\n", sensor->label, mqtt_msg->data.len, mqtt_msg->data.buf);
  
  // Value could be string or int.  We can use target_value to store the string, since that's only used for a MQTT condition.
  if (sensor->data.mqtt.target_value) free(sensor->data.mqtt.target_value);
  sensor->data.mqtt.target_value = strndup(mqtt_msg->data.buf, mqtt_msg->data.len);

  if (sensor->type == ACD_TYPE_MQTT_TEMP || sensor->type == ACD_TYPE_MQTT_VALUE) {
    // Known float or int value.
    float new_value = strtof(sensor->data.mqtt.target_value, NULL);
    LOG(LOG_INFO, "MQTT: Updating sensor '%s' value to %.2f\n", sensor->label, new_value);
    //sensor->state = ACD_LED_ON;
    ASSIGN_IF_CHANGED(sensor->value, new_value, _aquachemd_data->is_dirty, sensor->is_dirty); // Mark data as dirty so it gets sent to clients right away instead of waiting for next sensor read. 
    set_key_state(_aquachemd_data, sensor, ACD_LED_ON);
    return true;
  } else {
    LOG(LOG_WARNING, "MQTT: Received message for unsupported sensor type for sensor '%s'\n", sensor->label);
  }

  return false;
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
  struct mg_mqtt_message *mqtt_msg;
  struct mg_http_message *http_msg;
  struct mg_ws_message *ws_msg;
  char aq_topic[30];
  #ifdef AQ_TM_DEBUG 
    int tid; 
  #endif

  switch (ev) {
  //case MG_EV_HTTP_REQUEST:

  case MG_EV_HTTP_MSG:
    http_msg = (struct mg_http_message *)ev_data;

    //if ( strstr(http_msg->head.buf, "Upgrade: websocket")  ) {
    if ( mg_http_get_header(http_msg, "Sec-WebSocket-Key") != NULL) {
      LOG(LOG_DEBUG, "Enable websockets\n");
      mg_ws_upgrade(nc, http_msg, NULL);
      break;
    }

    action_web_request(nc, http_msg);
    LOG(LOG_DEBUG, "Served WEB request\n");
    break;
  
  case MG_EV_WS_OPEN:
    _aquachemd_data->open_websockets++;
    LOG(LOG_INFO, "++ Websocket joined\n");
    break;
  
  case MG_EV_WS_MSG:
    ws_msg = (struct mg_ws_message *)ev_data;
    action_websocket_request(nc, ws_msg);
    break;
  
  case MG_EV_CLOSE: 
    //LOG(LOG_WARNING, "Connection closed is_websocket( %d ) is_mqtt( %d ) is_mqttconnecting( %d )\n", is_websocket(nc), is_mqtt(nc), is_mqttconnecting(nc));
    if (is_websocket(nc)) {
      _aquachemd_data->open_websockets--;
      LOG(LOG_INFO, "-- Websocket left\n");
      if (is_websocket_acdmanager(nc)){
        // Check if this was the only acdmanager connection and if so close.
        struct mg_connection *c;
        bool active = false;

        for (c = mg_next(nc->mgr, NULL); c != NULL; c = mg_next(nc->mgr, c)){
          if (is_websocket(c) && is_websocket_acdmanager(c)){
            active = true;
            break;
          }
        }
        if (!active && _aquachemd_data->acdManagerActive){
          _aquachemd_data->acdManagerActive = false;
          LOG(LOG_INFO, "Stopped AquachemD Manager\n");
        }
      }
    }
    else if (is_mqtt(nc) || is_mqttconnecting(nc))
    {
      LOG(LOG_WARNING, "MQTT Connection closed\n");
      nc->is_closing = 1;
      _mqtt_exit_flag = true;
    }

    break;
  
  case MG_EV_ACCEPT:
    // IMPORTANT: Clean the flags since mongoose reuses memory.
    nc->fn_data = NULL;

    if (is_mqtt(nc)) {
      return;
    }
    // Only want HTTPS & WS connections
#if MG_TLS > 0
    if (nc->is_tls) {
      static char *crt;
      static char *key; 
      static char *ca;
      
      struct mg_tls_opts opts;
      memset(&opts, 0, sizeof(opts));

      if (crt == NULL || key == NULL) {
        LOG(LOG_NOTICE, "HTTPS: loading certs from : %s\n", _acdconfig_.cert_dir);
        crt = read_pem_file(false, "%s/crt.pem",_acdconfig_.cert_dir);
        key = read_pem_file(false, "%s/key.pem",_acdconfig_.cert_dir);
        ca = read_pem_file(true, "%s/ca.pem",_acdconfig_.cert_dir); // If this doesn't exist we don't care. If it exists, 2 way auth
      }
      opts.ca = mg_str(ca);    // Most cases this will be null, only get's set for 2 way auth (ie load cert and authority onto client)
      opts.cert = mg_str(crt);
      opts.key = mg_str(key);      
      mg_tls_init(nc, &opts);
    }
#endif
    break;

  case MG_EV_CONNECT: {
    set_mqttconnected(nc);
    //set_mqtt(nc);
    _mqtt_exit_flag = false;
    LOG(LOG_DEBUG, "MQTT: Connected to : %s\n", _acdconfig_.mqtt_server);
#if MG_TLS > 0
    if (nc->is_tls) {
      static char *crt;
      static char *key;
      static char *ca;
      
      struct mg_tls_opts opts;
      memset(&opts, 0, sizeof(opts));

      if (crt == NULL || key == NULL) {
        LOG(LOG_NOTICE, "MQTTS: loading certs from : %s\n", _acdconfig_.mqtt_cert_dir);
        crt = read_pem_file(false, "%s/crt.pem",_acdconfig_.cert_dir);
        key = read_pem_file(false, "%s/key.pem",_acdconfig_.cert_dir);
        ca = read_pem_file(true, "%s/ca.pem",_acdconfig_.cert_dir);
      }
      opts.cert = mg_str(crt);
      opts.key = mg_str(key);
      opts.ca = mg_str(ca);
      mg_tls_init(nc, &opts);
    }
#endif
  } break;

  case MG_EV_MQTT_OPEN:
    {
      //struct mg_mqtt_opts sub_opts
      static uint8_t qos=0;// PUT IN FUNCTION HEADDER can't be bothered with ack, so set to 0

      LOG(LOG_DEBUG, "MQTT: Connection open %lu\n", nc->id);

      snprintf(aq_topic, 29, "%s/#", _acdconfig_.mqtt_aquachemd_topic);
      //mqtt_subscribe(nc, aq_topic);
      struct mg_mqtt_opts sub_opts;
      memset(&sub_opts, 0, sizeof(sub_opts));
      sub_opts.topic = mg_str(aq_topic);
      sub_opts.qos = qos;
      LOG(LOG_INFO, "MQTT: Subscribing to '%s'\n", aq_topic);
      mg_mqtt_sub(nc, &sub_opts);

      // Any MQTT sensors we need to subscribe to?
      for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
        if (curr->type == ACD_TYPE_MQTT_TEMP || curr->type == ACD_TYPE_MQTT_COND || curr->type == ACD_TYPE_MQTT_VALUE) {
          memset(&sub_opts, 0, sizeof(sub_opts));
          sub_opts.topic = mg_str(curr->data.mqtt.topic);
          sub_opts.qos = qos;
          LOG(LOG_INFO, "MQTT: Subscribing to '%s'\n", curr->data.mqtt.topic);
          mg_mqtt_sub(nc, &sub_opts);
        }
      }
      
      LOG(LOG_INFO, "MQTT: sending Alive message (last will message)\n");
      snprintf(aq_topic, 24, "%s/%s", _acdconfig_.mqtt_aquachemd_topic,MQTT_LWM_TOPIC);
      send_mqtt(nc, aq_topic ,MQTT_ON);

      publish_mqtt_discovery( _aquachemd_data, nc);
    }
    break;

  case MG_EV_MQTT_CMD:
    //LOG(LOG_NOTICE, "MQTT: MG_EV_MQTT_CMD command, add code / need to replocate MG_EV_MQTT_PUBACK MG_EV_MQTT_SUBACK\n");
    break;
  //case MG_EV_MQTT_PUBLISH:
  case MG_EV_MQTT_MSG:
    bool found=false;
    mqtt_msg = (struct mg_mqtt_message *)ev_data;
    
    // We are only subscribed to aquachemd topic, (so not checking that).
    // Just check we have "set" as string end
    // NSF should handle reset_dose_total & reset_sensor_stats here, or simply leave them to WS and HTTP only
    if ( FAST_SUFFIX_3_CI(mqtt_msg->topic.buf, mqtt_msg->topic.len, "set"))
    {
      LOG(LOG_DEBUG, "MQTT: received (msg_id: %d), %.*s\n", mqtt_msg->id, mqtt_msg->topic.len, mqtt_msg->topic.buf);
      found=action_mqtt_message(nc, mqtt_msg);
    } else {
      //LOG(LOG_DEBUG, "MQTT: received, %.*s %.*s\n", mqtt_msg->topic.len, mqtt_msg->topic.buf, mqtt_msg->data.len, mqtt_msg->data.buf);
      for (acd_key_t *curr = _aquachemd_data->keys; curr != NULL; curr = curr->next) {
        if (curr->type == ACD_TYPE_MQTT_TEMP || curr->type == ACD_TYPE_MQTT_COND || curr->type == ACD_TYPE_MQTT_VALUE) {
          if (strncasecmp(mqtt_msg->topic.buf, curr->data.mqtt.topic, mqtt_msg->topic.len) == 0) {
            LOG(LOG_DEBUG, "MQTT: received (msg_id: %d), %.*s checking\n", mqtt_msg->id, mqtt_msg->topic.len, mqtt_msg->topic.buf);
            if (IS_CONDITION(curr->type)) {
              found=action_mqtt_condition_message(curr, mqtt_msg);
            } else {
              found=action_mqtt_sensor_message(curr, mqtt_msg);
            }
          }
        }
      }
    }
    if (!found) {
      LOG(LOG_DEBUG, "MQTT: received (msg_id: %d), %.*s ignoring\n", mqtt_msg->id, mqtt_msg->topic.len, mqtt_msg->topic.buf);
    }
    break;
  }
}












#define MQTT_PUB_TOPIC_SIZE 250

static void ws_send(struct mg_connection *nc, const char *msg)
{
  int size = strlen(msg);
  
  mg_ws_send(nc, msg, size, WEBSOCKET_OP_TEXT);
  
  //LOG(LOG_DEBUG, "WS: Sent %d characters '%s'\n",size, msg);
}



void send_mqtt_int_msg(struct mg_connection *nc, char *ID, int value) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE+2];
  static char msg[11];

  sprintf(msg, "%d", value);
  sprintf(mqtt_pub_topic, "%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

void send_mqtt_float_msg(struct mg_connection *nc, char *ID, float value) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE+2];
  static char msg[11];

  sprintf(msg, "%.2f", value);
  sprintf(mqtt_pub_topic, "%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

void send_mqtt_average_float_msg(struct mg_connection *nc, char *ID, float value) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE+2];
  static char msg[11];

  sprintf(msg, "%.2f", value);
  sprintf(mqtt_pub_topic, "%s/%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID, MQTT_TL_AVERAGE);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

void send_mqtt_level_float_msg(struct mg_connection *nc, char *ID, char *subtopic, float value) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE+2];
  static char msg[11];

  sprintf(msg, "%.2f", value);
  sprintf(mqtt_pub_topic, "%s/%s/%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID, MQTT_TL_LEVEL, subtopic);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

/*
// replaced with send_mqtt_acd_state_msg()
void send_mqtt_key_status_msg(struct mg_connection *nc, acd_key_t *key) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE];
  static char msg[4];

  sprintf(mqtt_pub_topic, "%s/%s/status", _acdconfig_.mqtt_aquachemd_topic, key->ID);
  sprintf(msg, "%d", key->state);
  send_mqtt(nc, mqtt_pub_topic, msg);
}
*/

void send_mqtt_acd_state_msg(struct mg_connection *nc, char *ID, acd_state_t state) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE];
  static char msg[4];

  sprintf(mqtt_pub_topic, "%s/%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID, MQTT_TL_STATE);
  sprintf(msg, "%d", state);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

void send_mqtt_timer_state_msg(struct mg_connection *nc, char *ID, acd_state_t state, uint32_t duration, uint32_t default_duration)
{
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE];
  //static char msg[4];

  sprintf(mqtt_pub_topic, "%s/%s/%s", ID, MQTT_TL_TIMER, MQTT_TL_DURATION);
  send_mqtt_int_msg(nc, mqtt_pub_topic, duration);

  sprintf(mqtt_pub_topic, "%s/%s/%s", ID, MQTT_TL_TIMER, MQTT_TL_DEFAULT);
  send_mqtt_int_msg(nc, mqtt_pub_topic, default_duration);

  // Homekit needs duration and remaining sent before state.
  sprintf(mqtt_pub_topic, "%s/%s/%s", ID, MQTT_TL_TIMER, MQTT_TL_STATE);
  send_mqtt_int_msg(nc, mqtt_pub_topic, state);
}


void send_mqtt_string_msg(struct mg_connection *nc, const char *ID, const char *msg) {
  static char mqtt_pub_topic[MQTT_PUB_TOPIC_SIZE];

  sprintf(mqtt_pub_topic, "%s/%s", _acdconfig_.mqtt_aquachemd_topic, ID);
  send_mqtt(nc, mqtt_pub_topic, msg);
}

/*
void send_mqtt_temp_msg(struct mg_connection *nc, char *ID, float value)
{
  // Incase we need to do degc to f conversion, we can do it here.
  send_mqtt_float_msg(nc, ID, (float)value);
}
*/

void mqtt_broadcast_aquachemdstate(struct mg_connection *nc, bool force_all)
{

  char topic[250];
  char msg[11];

  // Post main ACD state
  if (force_all || _aquachemd_data->keys->is_dirty) {
    send_mqtt_acd_state_msg(nc, _aquachemd_data->keys->ID, _aquachemd_data->keys->state);
    send_mqtt_string_msg(nc, "display_message", _aquachemd_data->display_message);
    CLEAR_DIRTY(_aquachemd_data->keys->is_dirty);
  }

  // Post sensors & conditions if enabled.  
  for (acd_key_t *curr = _aquachemd_data->keys->next; curr != NULL; curr = curr->next) { 
    
    if (!curr->is_dirty && !force_all) {continue;}

    if (IS_CONDITION(curr->type) && _acdconfig_.post_condition) {
      send_mqtt_acd_state_msg(nc, curr->ID, curr->met?ACD_LED_ON:ACD_LED_OFF);
    } else if (IS_OUTPUT(curr->type)) {
      // For homekit, we need to set the timer information before the state information.
      if ( isMASKSET(curr->flags, TIMER_ACTIVE)) {
        uint32_t remaining_sec = get_timer_left_sec(curr);
        send_mqtt_timer_state_msg(nc, curr->ID, (remaining_sec > 0?ACD_LED_ON:ACD_LED_OFF), remaining_sec, curr->default_duration);
      } else {
        send_mqtt_timer_state_msg(nc, curr->ID, ACD_LED_OFF, 0, caculate_dose_time(_aquachemd_data, curr));
      }
      send_mqtt_acd_state_msg(nc, curr->ID, curr->state);
    } else {
      send_mqtt_acd_state_msg(nc, curr->ID, curr->state);
      if (curr->state == ACD_LED_ON) {
        send_mqtt_float_msg(nc, curr->ID, curr->value);
        if (isMASKSET(curr->flags,CALC_AVERAGE) ) {
          send_mqtt_average_float_msg(nc, curr->ID, curr->stats.average);
        }
      }
      //if (curr->type == ACD_TYPE_VIR_TANK) {
      //  if (curr->uom)
      if (IS_TANK_VOLUME_ENABLED(curr)) {
        send_mqtt_level_float_msg(nc, curr->ID, MQTT_TSL_REMAINING_VOLUME, curr->data.tank.remaining_volume);
        send_mqtt_level_float_msg(nc, curr->ID, MQTT_TSL_TOTAL_VOLUME, curr->data.tank.total_volume);
      }
    }
    CLEAR_DIRTY(curr->is_dirty);
  }

  {
    static float last_ph = 0;
    static float last_orp = 0;
    // Post to aqualinkd if enabled (only pH & ORP)
    if (_acdconfig_.mqtt_aqualinkd_topic != NULL)
    {
      for (acd_key_t *curr = _aquachemd_data->keys->next; curr != NULL; curr = curr->next)
      {
        if (curr->type == ACD_TYPE_EZO_PH && curr->index == MASTER_ID && curr->state == ACD_LED_ON && curr->value != last_ph)
        {
          sprintf(msg, "%.2f", curr->value);
          sprintf(topic, "%s/CHEM/pH/set", _acdconfig_.mqtt_aqualinkd_topic);
          send_mqtt(nc, topic, msg);
          last_ph = curr->value;
          LOG(LOG_DEBUG, "MQTT: Broadcasted pump pH %s to Aqualinkd\n", msg);
        }
        else if (curr->type == ACD_TYPE_EZO_ORP && curr->index == MASTER_ID && curr->state == ACD_LED_ON && curr->value != last_orp)
        {
          sprintf(msg, "%.2f", curr->value);
          sprintf(topic, "%s/CHEM/ORP/set", _acdconfig_.mqtt_aqualinkd_topic);
          send_mqtt(nc, topic, msg);
          last_orp = curr->value;
          LOG(LOG_DEBUG, "MQTT: Broadcasted pump ORP %s to Aqualinkd\n", msg);
        }
      }
    }
  }
}

void mqtt_broadcast_aquachemd_dose_event(struct mg_connection *nc)
{
  struct mg_connection *c;
  char topic[MQTT_PUB_TOPIC_SIZE];
  //char rt_topic[MQTT_PUB_TOPIC_SIZE];
  //char msg[11];

  LOG(LOG_INFO, "Broadcasting Aquachemd dose event to MQTT\n");

  if (!_dose_event.is_dirty) { return; }
/*
  snprintf(topic, sizeof(topic), "%s/%s", _dose_event.key->ID, 
                          (_dose_event.key->flags & PH_PUMP)  ? MQTT_TL_DOSE_PH : 
                          (_dose_event.key->flags & ORP_PUMP) ? MQTT_TL_DOSE_ORP :
                          (_dose_event.key->flags & H2O_PUMP) ? MQTT_TL_DOSE_H2O : MQTT_TL_DOSE_UNKNOWN );
  */
  //snprintf(ld_topic, sizeof(topic), "%s/%s", _dose_event.key->ID,MQTT_TL_LAST_DOSE);
  //snprintf(rt_topic, sizeof(topic), "%s/%s", _dose_event.key->ID,MQTT_TL_RUNNING_DOSE);

  for (c = mg_next(nc->mgr, NULL); c != NULL; c = mg_next(nc->mgr, c)) {
    if (is_mqtt(c)) {
      snprintf(topic, sizeof(topic), "%s/%s", _dose_event.key->ID,MQTT_TL_LAST_DOSE);
      LOG(LOG_DEBUG, "MQTT send %.2f to topic %s/%s\n",_dose_event.dose_ml,_acdconfig_.mqtt_aquachemd_topic,topic);
      send_mqtt_float_msg(c, topic, _dose_event.dose_ml);

      if (IS_RUNNING_DOSE_ENABLED(_dose_event.key)) {
        snprintf(topic, sizeof(topic), "%s/%s", _dose_event.key->ID,MQTT_TL_RUNNING_DOSE);
        LOG(LOG_DEBUG, "MQTT send %.2f to topic %s/%s\n",_dose_event.key->dose_stats.running_total_ml,_acdconfig_.mqtt_aquachemd_topic,topic);
        send_mqtt_float_msg(c, topic, _dose_event.key->dose_stats.running_total_ml);
      }
    }
  }
  
  reset_dose_event();
}

void reset_last_mqtt_status()
{
}

void broadcast_aquachemdstate(struct mg_connection *nc, bool force_all) 
{
  struct mg_connection *c;
  static int mqtt_count=0;

  LOG(LOG_INFO, "Broadcasting Aquachemd state to websockets and MQTT\n");

  // Reconnect MQTT if needed.
  if (_mqtt_exit_flag == true) {
    mqtt_count++;
    //if (mqtt_count >= 10) {
    if (mqtt_count >= 1) { // changed to 1 when cleaned up all the MQTT spaming.  may want to delete count completely in the future
      start_mqtt(nc->mgr);
      mqtt_count = 0;
    }
  }

  for (c = mg_next(nc->mgr, NULL); c != NULL; c = mg_next(nc->mgr, c)) {
    if (is_websocket(c) /*&& !is_websocket_acdmanager(c)*/) {
      ws_send(c, get_devices_json(_aquachemd_data));
    } else if (is_mqtt(c)) {
      mqtt_broadcast_aquachemdstate(c, force_all);
    }
  }

  CLEAR_DIRTY(_aquachemd_data->is_dirty);
}





void start_mqtt(struct mg_mgr *mgr) {
  
  //generate_mqtt_id(_acdconfig_.mqtt_ID, MQTT_ID_LEN);

  //LOG(LOG_WARNING, "NOT Starting MQTT client, need to check code\n");
  if ( _acdconfig_.mqtt_server == NULL || _acdconfig_.mqtt_aquachemd_topic == NULL ) 
    return;

  char aq_topic[30];
  char *mqtt_ID = generate_mqtt_id();
  LOG(LOG_NOTICE, "Starting MQTT client to %s, id %s\n", _acdconfig_.mqtt_server, mqtt_ID);

  snprintf(aq_topic, 24, "%s/%s", _acdconfig_.mqtt_aquachemd_topic,MQTT_LWM_TOPIC);

  struct mg_mqtt_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.user = mg_str(_acdconfig_.mqtt_user);
    opts.pass = mg_str(_acdconfig_.mqtt_passwd);
    //opts.client_id = mg_str(_acdconfig_.mqtt_ID);
    opts.client_id = mg_str(mqtt_ID);

    //opts.keepalive = 5; // This seems to kill connection for some reason, and not sent heartbeat
    opts.clean = true;
    //opts.version = 4; // Maybe 5
    opts.message = mg_str(MQTT_OFF); // will_message
    opts.topic = mg_str(aq_topic); // will_topic
    

  struct mg_connection *nc = mg_mqtt_connect(mgr, _acdconfig_.mqtt_server, &opts, ev_handler, NULL);
  if ( nc == NULL ) {
    LOG(LOG_ERR, "Failed to create MQTT listener to %s\n", _acdconfig_.mqtt_server);
  } else {
    set_mqttconnecting(nc);
    reset_last_mqtt_status();
    _mqtt_exit_flag = false; // set here to stop multiple connects, if it fails truley fails it will get set to false.
  }
}

// mdns glue
static struct mg_dnssd_record s_http_rec;

static void mdns_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  (void) c;
  if (ev != MG_EV_MDNS_REQ) return;

  struct mg_mdns_req *req = (struct mg_mdns_req *) ev_data;
  uint16_t t = req->rr->atype;

  if (t == MG_DNS_RTYPE_PTR || t == MG_DNS_RTYPE_SRV || t == MG_DNS_RTYPE_TXT) {
    if (req->is_listing) return;  // generic service browse, not supported
    if (mg_strcmp(req->reqname, mg_str("_http._tcp")) == 0) {
      req->r = &s_http_rec;
      req->is_resp = true;
    }
  }
}

static bool start_mdns(struct mg_mgr *mgr, const char *hostname, const char *listen_url) {
  memset(&s_http_rec, 0, sizeof(s_http_rec));
  s_http_rec.srvcproto = mg_str("_http._tcp");
  s_http_rec.txt = mg_str_n("\0", 1);          // empty TXT record
  s_http_rec.port = mg_url_port(listen_url);   // "http://0.0.0.0:88" -> 88

  // hostname must stay valid for the life of the listener (literal or static)
  return mg_mdns_listen(mgr, mdns_ev_handler, (void *) hostname) != NULL;
}



bool network_service(struct mg_mgr *mgr, struct aquachemdata *acdata) {
  struct mg_connection *nc;
  _aquachemd_data = acdata;

  signal(SIGTERM, net_signal_handler);
  signal(SIGINT, net_signal_handler);
  signal(SIGRESTART, net_signal_handler);
  signal(SIGRUPGRADE, net_signal_handler);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  
  mg_log_set(_acdconfig_.mg_log_level);
  mg_log_set_fn(mg_logger, NULL);

  const char *nameserver = get_ip_address_of_nameserver();
  mg_mgr_init(mgr);

  if (nameserver != NULL)
    mgr->dns4.url = nameserver;

  LOG(LOG_NOTICE, "Starting web server on %s\n", _acdconfig_.listen_address);
  //nc = mg_bind(mgr, _acdconfig_.listen_address, ev_handler);
  nc = mg_http_listen(mgr, _acdconfig_.listen_address, ev_handler, mgr);
  if (nc == NULL) {
    LOG(LOG_ERR, "Failed to create listener on port %s\n",_acdconfig_.listen_address);
    return false;
  }

  
  // Start mDNS responder with hostname "mydevice" (resolves to mydevice.local)
  if (!start_mdns(mgr, AQUACHEMD_SHORT_NAME, _acdconfig_.listen_address)) {
    LOG(LOG_WARNING, "Failed to create mDNS responder '%s.local'\n",AQUACHEMD_SHORT_NAME);
    // non-fatal: the web server is already up
  } else {
    LOG(LOG_NOTICE, "mDNS responder started for '%s.local'\n",AQUACHEMD_SHORT_NAME);
  }

  // Set default web options
  _http_server_opts.root_dir = _acdconfig_.web_directory;
  _http_server_opts.extra_headers = CACHE; 
  _http_server_opts.ssi_pattern = NULL;

  _http_server_opts_nocache.root_dir = _acdconfig_.web_directory;
  _http_server_opts_nocache.extra_headers = NO_CACHE;
  _http_server_opts_nocache.ssi_pattern = NULL;

  return true;
}



// The Mongoose poll timeout in milliseconds
#define POLL_TIMEOUT_MS        100 

// How often to force a heartbeat refresh (in seconds)
#define HEARTBEAT_INTERVAL_SEC 300 

// Calculate the number of ticks required (1000ms in a second)
#define HEARTBEAT_TICKS_TARGET ((HEARTBEAT_INTERVAL_SEC * 1000) / POLL_TIMEOUT_MS)



void *net_services_thread( void *ptr )
{
  _aquachemd_data = (struct aquachemdata *) ptr;
  //bool mqtt_started = false;
  static uint32_t heartbeat_ticks = 0;
  
  // Update state to RUNNING
  pthread_mutex_lock(&_netsrunstate.mutex);
  _netsrunstate.state = ACD_KEEPRUNNING;
  pthread_mutex_unlock(&_netsrunstate.mutex);

  reset_dose_event();

  if (!network_service(&_mgr, _aquachemd_data)) {
    //LOG(LOG_ERR, "Failed to start network services\n");
    // Not the best way to do this (have thread exit process), but forks for the moment.
    //_keepNetServicesRunning = false;
    _netsrunstate.state = ACD_FAILED;
    LOG(LOG_ERR, "Can not start webserver on port %s.\n", _acdconfig_.listen_address);
   // exit(EXIT_FAILURE);
    goto f_end;
  }

  start_mqtt(&_mgr);

  //while (_netsrunstate == ACD_KEEPRUNNING)
  while (atomic_load_explicit(&_netsrunstate.state, memory_order_relaxed) == ACD_KEEPRUNNING) 
  {
    
    bool force_all = false;
    int journald_fail = 0;

    // Start MQTT on first iteration after HTTP server is ready
    /*
    if (!mqtt_started) {
      start_mqtt(&_mgr);
      mqtt_started = true;
    }*/
    
    mg_mgr_poll(&_mgr, POLL_TIMEOUT_MS);

    if (++heartbeat_ticks >= HEARTBEAT_TICKS_TARGET) { 
      force_all = true;
      heartbeat_ticks = 0; // Reset timer
    }

    // Update overall status
    if (_aquachemd_data->is_dirty == true || force_all == true) {
      broadcast_aquachemdstate(_mgr.conns, force_all);
      CLEAR_DIRTY(_aquachemd_data->is_dirty);
    } else {
      //LOG(LOG_DEBUG, "No state change, not broadcasting\n");
    }

    // Post any dose events
    if (_dose_event.is_dirty) {
      mqtt_broadcast_aquachemd_dose_event(_mgr.conns);
      CLEAR_DIRTY(_dose_event.is_dirty);
    }
  
#ifdef USE_SYSTEMD
    // Update acdmanager if active.
    // Inside your continuous loop
    if (_aquachemd_data->acdManagerActive) {
      if (journald_fail < JOURNAL_FAIL_RETRY) {
        if (!broadcast_systemd_logmessages(true)) {
            journald_fail++;
            LOG(LOG_ERR, "Journal broadcast failed (attempt %d/%d)\n", journald_fail, JOURNAL_FAIL_RETRY);
            
            if (journald_fail == JOURNAL_FAIL_RETRY) {
              char msg[WS_LOG_LENGTH];
              build_logmsg_json(msg, sizeof(msg), LOG_ERR, MSG_JOURNAL_GIVEUP, strlen(MSG_JOURNAL_GIVEUP));
              ws_send_logmsg(_mgr.conns, msg);
            }
        } else {
            // Success! Reset failure counter so we can handle intermittent issues later
            journald_fail = 0;
        }
      }
    } else {
     // Manager inactive: tell the function to clean up and reset our local fail counter
      broadcast_systemd_logmessages(false);
      journald_fail = 0;
    }
  }
#endif // USE_SYSTEMD

f_end:
  LOG(LOG_INFO, "Stopping network services thread\n");
  //broadcast_systemd_logmessages(false);
  mg_mgr_free(&_mgr);

  pthread_mutex_lock(&_netsrunstate.mutex);
  _netsrunstate.state = ACD_FINISHED;
  pthread_cond_signal(&_netsrunstate.cond); // Wake up anyone waiting for exit
  pthread_mutex_unlock(&_netsrunstate.mutex);

  LOG(LOG_INFO, "Stopped network services thread\n");
  pthread_exit(0);

}

bool start_net_services(struct aquachemdata *acddata) 
{
    // Use the mutex to safely check state
    pthread_mutex_lock(&_netsrunstate.mutex);
    if (_netsrunstate.state == ACD_KEEPRUNNING) {
        pthread_mutex_unlock(&_netsrunstate.mutex);
        LOG(LOG_NOTICE, "Network services thread is already running\n");
        return true;
    }
    
    // Capture the thread ID of the parent (the thread calling this function)
    _netsrunstate.parent_id = pthread_self();

    // Set starting state
    _netsrunstate.state = ACD_STARTING;
    pthread_mutex_unlock(&_netsrunstate.mutex);

    LOG(LOG_NOTICE, "Starting network services thread\n");

    // Pass the pointer to our struct as the thread argument
    if (pthread_create(&_netsrunstate.id, NULL, net_services_thread, acddata) != 0) {
        pthread_mutex_lock(&_netsrunstate.mutex);
        _netsrunstate.state = ACD_FAILED;
        pthread_mutex_unlock(&_netsrunstate.mutex);
        LOG(LOG_ERR, "Could not create network thread\n");
        return false;
    }

    pthread_detach(_netsrunstate.id);
    return true;
}

/*
bool start_net_services(struct aquachemdata *acddata) 
{
  // Not the best way to see if we are running, but works for now.
  if (_net_thread_id != 0 && _netsrunstate == ACD_KEEPRUNNING) {
    LOG(LOG_NOTICE, "Network services thread is already running, not starting\n");
    return true;
  }

  //_keepNetServicesRunning = true;
  _netsrunstate = ACD_STARTING;

  LOG(LOG_NOTICE, "Starting network services thread\n");

  if( pthread_create( &_net_thread_id , NULL ,  net_services_thread, (void*)acddata) < 0) {
    LOG(LOG_ERR, "could not create network thread\n");
    return false;
  }

  pthread_detach(_net_thread_id);

  return true;
}
*/


void stop_net_services()
{
    // Just trigger the stop
    pthread_mutex_lock(&_netsrunstate.mutex);
    if (_netsrunstate.state == ACD_KEEPRUNNING) {
        _netsrunstate.state = ACD_CLEANUP;
        mg_wakeup(&_mgr, 0, NULL, 0); 
    }
    pthread_mutex_unlock(&_netsrunstate.mutex);
    LOG(LOG_INFO, "Network services stop requested\n");
}

// Return the thread ID so the caller can wait
pthread_t get_net_services_id() 
{
    return _netsrunstate.id;
}


/*. SD Jornal */
#ifdef USE_SYSTEMD

void ws_send_logmsg(struct mg_connection *nc, char *msg)
{
  struct mg_connection *c;
  for (c = mg_next(nc->mgr, NULL); c != NULL; c = mg_next(nc->mgr, c))
  {
    if (is_websocket_acdmanager(c))
    {
      ws_send(c, msg);
    }
  }
}

sd_journal *open_journal(void)
{
  sd_journal *j = NULL;
  int rtn;

#ifndef ACD_CONTAINER_BUILD
  rtn = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
#else
  rtn = sd_journal_open_directory(&j, "/var/log/journal", 0);
#endif

  if (rtn < 0)
  {
    // systemd returns negative error codes directly; it doesn't use errno
    LOG(LOG_ERR, "Failed to open journal\n");
    return NULL;
  }

  char filter[84];
  snprintf(filter, sizeof(filter), "SYSLOG_IDENTIFIER=%s", _aquachemd_data->self);

  if (sd_journal_add_match(j, filter, 0) < 0)
  {
    LOG_SYSTEM_ERR(errno, "Failed to set journal syslog filter\n");
    sd_journal_close(j);
    return NULL;
  }

  if (sd_journal_set_data_threshold(j, WS_LOG_LENGTH - 25) < 0)
  {
    LOG(LOG_WARNING, "Failed to set journal data threshold\n");
  }

  // must call sd_journal_get_fd to initialize
  // the internal inotify subsystem, otherwise sd_journal_process() is a no-op.
  int journal_fd = sd_journal_get_fd(j);
  if (journal_fd < 0)
  {
    LOG_SYSTEM_ERR(-journal_fd, "Failed to initialize journal inotify watcher\n");
  }

  return j;
}

/*
void find_aquachemd_startup_in_journal(sd_journal *journal, int fallbacklines)
{
  // Try to find the specific startup ID
  sd_journal_add_match(journal, SD_MESSAGE_STARTUP_ID, 0);

  if (sd_journal_seek_tail(journal) >= 0 && sd_journal_previous(journal) > 0)
  {
    sd_journal_flush_matches(journal);

    char filter[84];
    snprintf(filter, sizeof(filter), "SYSLOG_IDENTIFIER=%s", _aquachemd_data->self);
    sd_journal_add_match(journal, filter, 0);
    return;
  }

  // Fallback path
  sd_journal_flush_matches(journal);

  // FIX: Re-apply standard identifier filter here so you don't leak all system logs
  char filter[84];
  snprintf(filter, sizeof(filter), "SYSLOG_IDENTIFIER=%s", _aquachemd_data->self);
  sd_journal_add_match(journal, filter, 0);

  sd_journal_seek_tail(journal);
  sd_journal_previous_skip(journal, (uint64_t)fallbacklines);
}
*/




static void _reset_journal_state(sd_journal **j, bool *active)
{
  if (*j)
  {
    sd_journal_close(*j);
    *j = NULL;
  }
  *active = false;
}

#define SD_FALLBACK_LINES 10

void find_aquachemd_lifecycle_start(sd_journal *journal)
{
  const int MAX_SEARCH_LINES = 200;
  //const int FALLBACK_LINES = 10;

  sd_journal_flush_matches(journal);
  
  // 1. Set up the OR filter for the unique startup/upgrade message UUIDs
  char match1[128], match2[128];
  snprintf(match1, sizeof(match1), "MESSAGE_ID=%s", SD_MESSAGE_STARTUP_ID);
  snprintf(match2, sizeof(match2), "MESSAGE_ID=%s", SD_MESSAGE_UPGRADE_ID);
  sd_journal_add_match(journal, match1, 0);
  sd_journal_add_disjunction(journal);
  sd_journal_add_match(journal, match2, 0);

  bool found = false;
  if (sd_journal_seek_tail(journal) >= 0) {
      // Walk back up to 200 steps to find the start of our current lifecycle
      for (int i = 0; i < MAX_SEARCH_LINES; i++) {
          int r = sd_journal_previous(journal);
          if (r <= 0) break; // Reached end of log file or error    
          found = true; 
          break; 
      }
  }

  // 2. Clear the UUID filters and cleanly bind to ONLY our daemon's logs
  sd_journal_flush_matches(journal);
  char filter[84];
  snprintf(filter, sizeof(filter), "SYSLOG_IDENTIFIER=%s", _aquachemd_data->self);
  sd_journal_add_match(journal, filter, 0);

  // 3. Adjust the cursor position based on search results
  if (found) {    
      // We are sitting exactly ON the startup message.
      // Step back one line within our daemon's filter context so that 
      // the main loop's first sd_journal_next() lands directly back ON it.
      sd_journal_previous(journal);
  }
  else {   
      // Fallback: If no markers found, seek to the tail of our daemon's logs
      // and back up by 11 lines so that the first next() returns line #10.
      sd_journal_seek_tail(journal);
      sd_journal_previous_skip(journal, (uint64_t)(SD_FALLBACK_LINES + 1));
  }
}

bool broadcast_systemd_logmessages(bool acdMgrActive)
{
  static sd_journal *journal = NULL;
  static bool active = false;
  static char *cursor = NULL;
  
  // CRITICAL: Tracks if this is the very first UI connection since the binary launched
  static bool first_connection_since_boot = true; 
  char json_msg[WS_LOG_LENGTH];

  // Handle Deactivation Request (All UI tabs closed / WS disconnected)
  if (!acdMgrActive)
  {
    if (active)
    {
      _reset_journal_state(&journal, &active);

      if (cursor)
      {
        free(cursor);
        cursor = NULL;
      }
    }
    return true;
  }

  // Initialization / Re-opening upon a new WebSocket connection
  if (!active)
  {
    journal = open_journal();
    if (!journal)
    {
      build_logmsg_json(json_msg, sizeof(json_msg), LOG_ERR, MSG_JOURNAL_OPEN_FAILED, strlen(MSG_JOURNAL_OPEN_FAILED));
      ws_send_logmsg(_mgr.conns, json_msg);
      return false;
    }

    if (cursor)
    {
      // Resuming mid-stream (e.g., transient network hiccup, cursor exists)
      sd_journal_seek_cursor(journal, cursor);
      sd_journal_next(journal);
    }
    else
    {
      // Fresh connection established (cursor is NULL)
      if (first_connection_since_boot)
      {
        LOG(LOG_DEBUG, "First WD Manager connection, searching for startup messages\n");
        // SCENARIO 2: Daemon just booted or completed an upgrade.
        // Trace back to the boot sequence origin and stream forward.
        find_aquachemd_lifecycle_start(journal);
        first_connection_since_boot = false; // Defuse so subsequent connections use Scenario 1
      }
      else
      {
        // SCENARIO 1: Standard operation/UI Refresh. 
        // Just grab the last 10 lines of our daemon logs to confirm the stream is alive.
        LOG(LOG_DEBUG, "New WD Manager connection, going back 10 messages\n");
        sd_journal_flush_matches(journal);
        char filter[84];
        snprintf(filter, sizeof(filter), "SYSLOG_IDENTIFIER=%s", _aquachemd_data->self);
        sd_journal_add_match(journal, filter, 0);
        
        sd_journal_seek_tail(journal);
        sd_journal_previous_skip(journal, SD_FALLBACK_LINES + 1);
      }
    }
    active = true;
  }

  // Sync with disk for live changes BEFORE reading
  int status = sd_journal_process(journal);
  if (status == SD_JOURNAL_INVALIDATE)
  {
    _reset_journal_state(&journal, &active);
    return true;
  }

  // Process and Stream Entries (handles both historical catches and live lines)
  int rtn;
  while ((rtn = sd_journal_next(journal)) > 0)
  {
    const void *log_data, *pri_data;
    size_t log_len, pri_len;

    if (sd_journal_get_data(journal, "MESSAGE", &log_data, &log_len) >= 0 &&
        sd_journal_get_data(journal, "PRIORITY", &pri_data, &pri_len) >= 0)
    {
      build_logmsg_json(json_msg, sizeof(json_msg), atoi((const char *)pri_data + 9), (const char *)log_data + 8, (int)log_len - 8);
      ws_send_logmsg(_mgr.conns, json_msg);

      if (cursor)
        free(cursor);
      sd_journal_get_cursor(journal, &cursor);
    }
  }

  if (rtn < 0)
  {
    _reset_journal_state(&journal, &active);
    return false;
  }

  return true;
}

/*
bool broadcast_systemd_logmessages(bool acdMgrActive)
{
  static sd_journal *journal = NULL;
  static bool active = false;
  static char *cursor = NULL;
  char json_msg[WS_LOG_LENGTH];

  // Handle Deactivation Request
  if (!acdMgrActive)
  {
    if (active)
    {
      // printf("*** broadcast_systemd_logmessages() - close journal\n");
      _reset_journal_state(&journal, &active);

      // NOTE: If you want to remember your exact place across deactivations,
      // comment out the two lines below. Freeing them forces a fresh tail/startup seek.
      if (cursor)
      {
        free(cursor);
        cursor = NULL;
      }
    }
    return true;
  }

  // Initialization / Re-opening
  if (!active)
  {
    // printf("*** broadcast_systemd_logmessages() - open_journal()\n");
    journal = open_journal();
    if (!journal)
    {
      build_logmsg_json(json_msg, sizeof(json_msg), LOG_ERR, MSG_JOURNAL_OPEN_FAILED, strlen(MSG_JOURNAL_OPEN_FAILED));
      ws_send_logmsg(_mgr.conns, json_msg);
      return false;
    }

    if (cursor)
    {
      sd_journal_seek_cursor(journal, cursor);
      sd_journal_next(journal);
    }
    else
    {
      sd_journal_seek_tail(journal);
      //find_aquachemd_startup_in_journal(journal, 10);
      find_aquachemd_lifecycle_start(journal);
    }
    active = true;
  }

  // Sync with disk for live changes BEFORE reading
  int status = sd_journal_process(journal);
  if (status == SD_JOURNAL_INVALIDATE)
  {
    // printf("***** Journal files rotated or deleted, forcing reopen...\n");
    _reset_journal_state(&journal, &active);
    return true;
  }

  // Process New Entries
  // printf("*** broadcast_systemd_logmessages() - Start New Entries\n");
  int rtn;
  while ((rtn = sd_journal_next(journal)) > 0)
  {
    //printf("*** sd_journal_next() - return=%d\n", rtn);
    const void *log_data, *pri_data;
    size_t log_len, pri_len;

    if (sd_journal_get_data(journal, "MESSAGE", &log_data, &log_len) >= 0 &&
        sd_journal_get_data(journal, "PRIORITY", &pri_data, &pri_len) >= 0)
    {

      build_logmsg_json(json_msg, sizeof(json_msg), atoi((const char *)pri_data + 9), (const char *)log_data + 8, (int)log_len - 8);
      ws_send_logmsg(_mgr.conns, json_msg);

      if (cursor)
        free(cursor);
      sd_journal_get_cursor(journal, &cursor);
    }
    else
    {
      // printf("*** sd_journal_get_data() - blank\n");
    }
  }
  // printf("*** broadcast_systemd_logmessages() - END New Entries return=%d\n", rtn);

  if (rtn < 0)
  {
    // printf("***** broadcast_systemd_logmessages() - ERROR reading journal\n");
    _reset_journal_state(&journal, &active);
    return false;
  }

  return true;
}
*/
#endif // USE_SYSTEMD