/****************

Make sure to have the below in /boot/firmware/config.txt to enable I2C on Raspberry Pi:

dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=100000

# To enable 1 wire
dtoverlay=w1-gpio

# Install i2c-tools for testing and debugging:
sudo apt install i2c-tools

# Load i2c-dev module
sudo modprobe i2c-dev

# Verify /dev/i2c-1 appears
ls /dev/i2c*

# Make it survive reboot (We shouldn't need to do this, but testing we do)
echo "i2c-dev" | sudo tee /etc/modules-load.d/i2c.conf

Usage:
  ./aquachemd                       - normal mode, loop reading sensors
  ./aquachemd scan                  - scan I2C bus and identify devices, scan GPIO chips
  ./aquachemd deepscan              - scan I2C bus and identify devices, scan GPIO chips with line details
  ./aquachemd calibrate mid         - calibrate pH mid-point (7.00)
  ./aquachemd calibrate low         - calibrate pH low-point (4.00)
  ./aquachemd calibrate high        - calibrate pH high-point (10.00)
  ./aquachemd calibrate orp <mv>    - calibrate ORP at specified mV value

*/

#define _POSIX_C_SOURCE 200809L // for clock_gettime and clock_nanosleep
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <math.h>

#include "version.h"
#include "aquachemd.h"
#include "utils.h"
#include "ezo.h"
#include "1wire.h"
#include "sysfs.h"
#include "utils.h"
#include "config.h"
#include "net_services.h"
#include "acd_timer.h"
#include "gpio_monitor.h"
#include "state_manager.h"
#include "sensor_stats.h"

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef WITH_GPIOD
#include "gpio.h"
#endif

//pthread_t _daemon_thread_id;
//acd_runstate_t _runstate;

static acd_thread_t _thread_control = {
    .parent_id = 0,
    .id = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .state = ACD_STARTING
};

bool start_upgrade();
bool acquire_singleton_lock(void);
void release_singleton_lock(void);


// 100% Async-Signal-Safe mapping function
static const char* get_async_sig_name(int sig_num) {
    switch (sig_num) {
        case SIGINT:  return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGQUIT: return "SIGQUIT";
        case SIGHUP:  return "SIGHUP";
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGRESTART:     return "SIGRESTART";
        case SIGRUPGRADE:    return "SIGUPGRADE";
        //case SIGCLEANUPEXIT: return "SIGCLEANUPEXIT";
        default:      return "UNKNOWN_SIGNAL";
    }
}


void intHandler(int sig_num)
{
  pthread_mutex_lock(&_thread_control.mutex);
  _thread_control.state = ACD_CLEANUP;
  pthread_mutex_unlock(&_thread_control.mutex);
  /*******************
   * 
   * safety guard so the stop task can't be skipped if the process is killed mid-dose. 
   * signal handler calling pump_stop() / relay_off() on SIGTERM/SIGINT 
   * 
   */

  LOG(LOG_INFO, "Exception Raised %s\n",get_async_sig_name(sig_num));

  devices_emergency_stop();

  // Wake up daemon thread
  //pthread_kill(_daemon_thread_id, SIGCLEANUPEXIT);

  if (sig_num == SIGRUPGRADE) {
    LOG(LOG_NOTICE, "Starting upgrade\n");
    if (! start_upgrade()) {
      LOG(LOG_ERR, "%s upgrade failed!\n",AQUACHEMD_SHORT_NAME);
    }
    return; // Let the upgrade process terminate us.
  }

  if (sig_num == SIGRESTART) {
    if (is_running_under_systemd()) {
      //LOG(LOG_WARNING, "Restarting %s!\n",AQUACHEMD_SHORT_NAME);
      // If we are deamonized, we need to use the system
      if(fork() == 0) {
        setsid(); // Escape the initial cgroup session completely
        sleep(2);
        LOG(LOG_WARNING, "Restarting %s!\n",AQUACHEMD_SHORT_NAME);
        //LOG(LOG_NOTICE, "Starting upgrade\n");
        char *newargv[] = {"/bin/systemctl", "restart", "aquachemd", NULL};
        char *newenviron[] = { NULL };
        execve(newargv[0], newargv, newenviron);
        //exit (EXIT_SUCCESS);
        _exit(127);
      }
    } else {
      LOG(LOG_ERR, "Can't restart %s, not running as daemon!\n",AQUACHEMD_SHORT_NAME);
      return;
    }
  }

  aquachemd_force_sensor_poll(); // If waiting force the next poll, that will allow this to execute quicker
  LOG(LOG_NOTICE, "Received request to stop %s!\n",AQUACHEMD_SHORT_NAME);

  //exit(EXIT_SUCCESS);
}

void setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = intHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);  // Ctrl+C
    sigaction(SIGTERM, &sa, NULL);  // kill / systemd stop
    sigaction(SIGHUP,  &sa, NULL);  // terminal closed / daemon reload
    sigaction(SIGQUIT, &sa, NULL);  // Ctrl+backslash
    sigaction(SIGPIPE, &sa, NULL);  // broken pipe
}

// Keep this private to aquachemd.c
//static acd_thread_t _main_thread_ctl; 

void aquachemd_request_reload(void) {
    // Release memory order guarantees that the newly written aquachemd.conf file 
    // changes are fully flushed to disk/OS caches before the state becomes visible.
    atomic_store_explicit(&_thread_control.state, ACD_RELOAD, memory_order_release);
    LOG(LOG_NOTICE, "Subsystem requested an application reload.\n");
}

void aquachemd_force_sensor_poll(void) {
    pthread_mutex_lock(&_thread_control.mutex);
    pthread_cond_signal(&_thread_control.cond);
    pthread_mutex_unlock(&_thread_control.mutex);
    LOG(LOG_DEBUG, "Sensor poll forced by external thread.\n");
}

typedef enum {
  ACD_MSG_CLEAR = -1,
  ACD_MSG_NONE,
  ACD_MSG_STARTING,
  ACD_MSG_CONDITION_FAILED,
  ACD_MSG_SENSOR_READ_FAILED
} display_message_t;

/**
 * Updates the global display message based on a message type and an optional string.
 * @param type    The display_message_t enum (e.g., ACD_MSG_SENSOR_READ_FAILED).
 * @param message An additional string to append (e.g., "Temp 1"). Can be NULL.
 */
void update_display_message(struct aquachemdata *acddata, display_message_t type, const char *message) {
    // 1. Check for the "Clear" condition first
    // If type is CLEAR/NONE and the message is NULL, empty the buffer and exit.

    if ((type == ACD_MSG_CLEAR || type == ACD_MSG_NONE) && message == NULL) {
      if (acddata->display_message[0] != '\0') {
        acddata->is_dirty = true;
      }
      acddata->display_message[0] = '\0';
      return;
    }

    // Only proceed if the current message is empty
    if (acddata->display_message[0] != '\0') {
      return;
    }

    const char *prefix = "";
    const char *suffix = (message == NULL) ? "" : message;

    switch (type) {
        case ACD_MSG_CONDITION_FAILED:
          prefix = "Check failed: ";
          break;
        case ACD_MSG_SENSOR_READ_FAILED:
          prefix = "Sensor read err: ";
          break;
        //case ACD_MSG_AQUACHEM_OFF
        //  prefix = "AquachemD is off";
        //  break;
        case ACD_MSG_STARTING:
          prefix = "Starting : ";
          break;
        case ACD_MSG_NONE:
        default:
          prefix = "";
          break;
    }

    snprintf(acddata->display_message, DISPLAY_MSG_SIZE, "%s%s", prefix, suffix);

    LOG(LOG_DEBUG, "Display Message Updated: %s", acddata->display_message);
}



double elapsed_ms(const struct timespec *start_time) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long delta_sec = now.tv_sec - start_time->tv_sec;
    long delta_nsec = now.tv_nsec - start_time->tv_nsec;

    if (delta_nsec < 0) {
        delta_sec -= 1;
        delta_nsec += 1000000000L;
    }

    return (delta_sec * 1000.0) + (delta_nsec / 1000000.0);
}
/*
ezo_pump_request(acd_key_t *key, acd_state_t state)
{
  if (state == ACD_LED_ON)
    setMASK(key->flags, EZO_PUMP_REQUEST_ON);
  else
    setMASK(key->flags, EZO_PUMP_REQUEST_OFF);

  aquachemd_force_sensor_poll();
}
*/
/*
check_ezo_pump_requests_status(struct aquachemdata *acdata, acd_key_t *key)
{
  if (key->state == ACD_LED_ON) {
    if ( isMASKSET(key->flags, EZO_PUMP_REQUEST_OFF)) {

    } else {
    pump_status_t pump_reading = pump_get_status();
    if (pump_reading.status == EZO_SUCCESS) {
      if (!pump_reading.is_pumping) {
        // Pump is off, set off
        // NSF NEED TO LOG THE PUMP RUN.
        float dose_ml = pump_get_dispensed_volume();
        set_key_state(acdata, key, ACD_LED_ENABLED);
      }
    }
    } else {
      LOG(LOG_WARNING, "EZO Pump '%s' read failed (status %d)\n", curr->label, pump_reading.status);
      //update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, curr->label);
      sensor_read_error(acdata, key);
    }
  } else if ( isMASKSET(key->flags, EZO_PUMP_REQUEST_ON)) {
    // Turn on dose.  
    if (pump_dose_volume_at_rate(key->requested_dose, key->dose_stats.flow_rate) == EZO_SUCCESS) {
      removeMASK(key->flags, EZO_PUMP_REQUEST_ON);
      set_key_state(acdata, key, ACD_LED_ON);
    } else {
      // ERROR
      LOG(LOG_ERR, "EZO Pump '%s' failed to turn on");
      sensor_read_error(acdata, key);
    }
  }
}
*/


void printHelp()
{
  printf("%s %s\n", AQUACHEMD_SHORT_NAME, AQUACHEMD_VERSION);
  
  printf("\t-h         (this message)\n");
  printf("\t---- Normal Runmode ----\n");
  printf("\t-c <file>  (Configuration file)\n");
  printf("\t-v         (Debug logging)\n");
  printf("\t---- Scan mode ----\n");
  printf("\tscan       (Scan system for devices & chips)\n");
  printf("\tdeepscan   (Scan system for devices & chips & scan GPIO chips in detail)\n");
  printf("\t---- Calibration mode ----\n");
  printf("\tcalibrate  (calibrate help)\n");
  printf("\tcalibrate ph  <low|mid|high>\n");
  printf("\tcalibrate orp <mv_value>\n");
  printf("\tcalibrate rtd <°C_value>\n");
  printf("\tcalibrate prs <psi_value>\n");
}


void scan_sensors(bool activeSystem, bool deepScan) 
{
  DIAG_LOG(activeSystem, "==============================================================\n");
  DIAG_LOG(activeSystem,"                   **** Started Scan **** \n");

#ifdef WITH_GPIOD
  DIAG_LOG(activeSystem, "==============================================================\n");
  gpio_detect(deepScan, activeSystem);
#endif
  
DIAG_LOG(activeSystem, "==============================================================\n");
  w1_detect(activeSystem);
  
  DIAG_LOG(activeSystem, "==============================================================\n");
  i2c_detect(deepScan, activeSystem);
  
  DIAG_LOG(activeSystem, "==============================================================\n");
  sysfs_detect(activeSystem);

  DIAG_LOG(activeSystem,"---------------------------------------------------------------\n");

  if (deepScan && activeSystem) {
    DIAG_LOG(activeSystem,"                    **** WARNING  **** \n");
    DIAG_LOG(activeSystem,"DeepScan can make some I2C sensors inoperable, you may need \n");
    DIAG_LOG(activeSystem,"to restart AquachemD to reset them\n");
    DIAG_LOG(activeSystem,"---------------------------------------------------------------\n");
  }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
  struct aquachemdata acddata = {0};
  struct timespec next_wake;

  snprintf(acddata.display_message, DISPLAY_MSG_SIZE, "Starting: %s v%s", AQUACHEMD_SHORT_NAME, AQUACHEMD_VERSION);
  CLEAR_DIRTY(acddata.is_dirty);

  char *bname = basename(argv[0]);
  if (bname == NULL || strlen(bname) == 0) {
    bname = "aquachemd"; // Hardcoded fallback
  }
  // Safely copy into the fixed buffer
  strncpy(acddata.self, bname, sizeof(acddata.self) - 1);
  acddata.self[sizeof(acddata.self) - 1] = '\0';

  if (!ezo_bus_available())
  {
    LOG(LOG_ERR, "Error: I2C bus %s not available.\n", I2C_BUS);
    LOG(LOG_ERR, "Run: sudo modprobe i2c-dev\n");
    LOG(LOG_ERR, "To persist across reboots: echo i2c-dev | sudo tee /etc/modules-load.d/i2c.conf\n");
    return 1;
  }


  // ── Scan mode ───────────────────────────────────────────────────────────────
  if (argc >= 2 && (strcasecmp(argv[1], "scan") == 0 || strcasecmp(argv[1], "deepscan") == 0))
  {
    /*
#ifdef WITH_GPIOD
    int deep = strcasecmp(argv[1], "deepscan") == 0;
    gpio_detect(deep, false);
#endif
    w1_detect(true);
    ezo_i2cdetect(false);*/
    int deep = strcasecmp(argv[1], "deepscan") == 0;
    scan_sensors(false, deep);
    return 0;
  }

  // ── Calibration mode ────────────────────────────────────────────────────────
  if (argc >= 2 && strcasecmp(argv[1], "calibrate") == 0)
  {
    int idx=2;
    if (argc < 3)
    {
      fprintf(stderr, "Usage: %s calibrate ph  <low|mid|high>\n", argv[0]);
      fprintf(stderr, "       %s calibrate orp <mv_value>\n", argv[0]);
      fprintf(stderr, "       %s calibrate rtd <°C_value>\n", argv[0]);
      fprintf(stderr, "       %s calibrate prs <psi_value>\n", argv[0]);
      return 1;
    }

    if (strcasecmp(argv[idx], "ph") == 0) {idx=3;}

    if (strcasecmp(argv[idx], "mid") == 0)
    {
      printf("Calibrating pH mid-point (7.00)...\n");
      //return ph_calibrate_mid() == EZO_SUCCESS ? 0 : 1;
      return ph_calibrate_mid(&(ezo_sensor_t){ .address = EZO_PH_ADDR }) == EZO_SUCCESS ? 0 : 1;
    }
    else if (strcasecmp(argv[idx], "low") == 0)
    {
      printf("Calibrating pH low-point (4.00)...\n");
      //return ph_calibrate_low() == EZO_SUCCESS ? 0 : 1;
      return ph_calibrate_low(&(ezo_sensor_t){ .address = EZO_PH_ADDR }) == EZO_SUCCESS ? 0 : 1;
    }
    else if (strcasecmp(argv[idx], "high") == 0)
    {
      printf("Calibrating pH high-point (10.00)...\n");
      //return ph_calibrate_high() == EZO_SUCCESS ? 0 : 1;
      return ph_calibrate_high(&(ezo_sensor_t){ .address = EZO_PH_ADDR }) == EZO_SUCCESS ? 0 : 1;
    }
    else if (strcasecmp(argv[2], "orp") == 0)
    {
      if (argc < 4)
      {
        fprintf(stderr, "Usage: %s calibrate orp <mv_value>\n", argv[0]);
        return 1;
      }
      float mv = atof(argv[3]);
      printf("Calibrating ORP at %.2f mV...\n", mv);
      //return orp_calibrate(mv) == EZO_SUCCESS ? 0 : 1;
      return orp_calibrate(&(ezo_sensor_t){ .address = EZO_ORP_ADDR }, mv) == EZO_SUCCESS ? 0 : 1;
    }
    else if ( (strcasecmp(argv[2], "rtd") == 0) ||
              (strcasecmp(argv[2], "temp") == 0))
    {
      if (argc < 4)
      {
        fprintf(stderr, "Usage: %s calibrate rtd <degC>\n", argv[0]);
        return 1;
      }
      float temp = atof(argv[3]);
      printf("Calibrating RTD / Temperature probe at %.2f°C...\n", temp);
      //return rtd_calibrate(temp) == EZO_SUCCESS ? 0 : 1;
      return rtd_calibrate(&(ezo_sensor_t){ .address = EZO_RTD_ADDR }, temp) == EZO_SUCCESS ? 0 : 1;
    }
    else if ( (strcasecmp(argv[2], "prs") == 0))
    {
      if (argc < 4)
      {
        fprintf(stderr, "Usage: %s calibrate prs <psi>\n", argv[0]);
        return 1;
      }
      float temp = atof(argv[3]);
      printf("Calibrating PRS sensor at %.2fpsi...\n", temp);
      //return prs_calibrate(temp) == EZO_SUCCESS ? 0 : 1;
      return prs_calibrate(&(ezo_sensor_t){ .address = EZO_PRS_ADDR }, temp) == EZO_SUCCESS ? 0 : 1;
    }
    else
    {
      fprintf(stderr, "Unknown calibration point '%s'\n", argv[2]);
      fprintf(stderr, "Valid options: low, mid, high, orp <mv>, rtd <degC>\n");
      return 1;
    }
  }

  // If we get here, we are in normal mode, so we continue with initialization and then loop reading sensors.
  // parse normal command line arguments
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "-h") == 0)
    {
      printHelp();
      return 0;
    }
    //if (strcmp(argv[i], "-d") == 0)
    //{
    //  _acdconfig_.deamonize = false;
    //}
    else if (strcmp(argv[i], "-c") == 0)
    {
      _acdconfig_.config_file = cleanalloc(argv[++i], STR_FULL_LENGTH);
    }
    else if (strcmp(argv[i], "-v") == 0)
    {
      _acdconfig_.log_level = LOG_DEBUG;
    }
  }
/*
  // ── Unknown argument ────────────────────────────────────────────────────────
  if (argc >= 2)
  {
    fprintf(stderr, "Unknown argument '%s'\n", argv[1]);
    fprintf(stderr, "Usage: %s [scan | calibrate <low|mid|high> | calibrate orp <mv>]\n", argv[0]);
    return 1;
  }
*/

  //pthread_mutex_init(&_thread_control.mutex, NULL);
  //pthread_cond_init(&_thread_control.cond, NULL);

  // Need to replace CLOCK_REALTIME with CLOCK_MONOTONIC for the main loop/sleep
  pthread_mutex_init(&_thread_control.mutex, NULL);
  pthread_condattr_t cattr;
  pthread_condattr_init(&cattr);
  pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
  pthread_cond_init(&_thread_control.cond, &cattr);
  pthread_condattr_destroy(&cattr);

  _thread_control.state = ACD_STARTING;
  _thread_control.id = pthread_self();

  // Make sure we catch termination
  setup_signal_handlers();

  // Test if to use systemd/journal or not.
  init_logging_backend();

  #ifdef USE_SYSTEMD
  sd_notify(0, "READY=1");
  #endif // USE_SYSTEMD

  LOG_STARTUP_EVENT();
  //FORCE_LOG(LOG_NOTICE, "Starting %s (%s) v%s\n", AQUACHEMD_NAME, AQUACHEMD_SHORT_NAME, AQUACHEMD_VERSION);

  // Setup master key/button
  acddata.keys = malloc(sizeof(acd_key_t));
  //acddata.keys->ID = "AquachemD";
  acddata.keys->ID = ACD_MASTER_ID;
  acddata.keys->type = ACD_TYPE_MASTER;
  acddata.keys->label = "AquachemD";
  acddata.keys->state = ACD_LED_ENABLED;


  // --- THE RESET ---
reload_configuration:

  LOG(LOG_NOTICE, "Initializing and launching daemon subsystems...");


  parse_config_file(&acddata);
  acddata.keys->label = _acdconfig_.main_label; // Update main label from config

  if (_acdconfig_.singleton && !acquire_singleton_lock()) {
    //LOG(LOG_ERROR, "%s: another instance is already running\n", AQUACHEMD_SHORT_NAME); // This would also show the message in the other instance
    fprintf(stderr, "%s: another instance is already running\n", AQUACHEMD_SHORT_NAME);
    return EXIT_FAILURE;
  }

  start_net_services(&acddata);

  // Setup any specifics for GPIO / D1W etc
  for (acd_key_t *curr = acddata.keys; curr != NULL; curr = curr->next) {
    if (curr->type == ACD_TYPE_GPIO_COND || curr->type == ACD_TYPE_GPIO_INPUT) {
      acddata.haveConditions = true;
      LOG(LOG_DEBUG,"Setting up GPIO %s: %s, pin %d\n", (curr->type == ACD_TYPE_GPIO_COND?"Condition":"Input"), curr->label, curr->data.gpio.pin);
      if (gpio_open(&curr->data.gpio, _acdconfig_.gpio_chip, curr->data.gpio.pin, GPIO_INPUT, curr->data.gpio.active) != 0) {
        LOG(LOG_ERR, "Failed to open GPIO for %s, pin %d\n", curr->label, curr->data.gpio.pin);
      }
      //curr->met = sensor_is_met(&curr->data.gpio);
      //curr->state = curr->met?ACD_LED_ON:ACD_LED_OFF;
    } else if (curr->type == ACD_TYPE_GPIO_PMP || curr->type == ACD_TYPE_GPIO_OUTPUT) {
      LOG(LOG_DEBUG,"Setting up GPIO %s: %s, pin %d\n", (curr->type == ACD_TYPE_GPIO_PMP?"Pump":"Output"), curr->label, curr->data.gpio.pin);
      if (gpio_open(&curr->data.gpio, _acdconfig_.gpio_chip, curr->data.gpio.pin, GPIO_OUTPUT, curr->data.gpio.active) != 0) {
        LOG(LOG_ERR, "Failed to open GPIO for %s, pin %d\n", curr->label, curr->data.gpio.pin);
      } else {
        gpio_write(&curr->data.gpio, 0); // Turn pump off
        //sync_pump_state(&acddata, curr);
      }
    } else if (curr->type == ACD_TYPE_EZO_EC) {
      ec_set_k(&curr->data.ezo, curr->data.ezo.ec_sensor_k);
      ec_set_tds_factor(&curr->data.ezo, curr->data.ezo.ec_sensor_tds_factor);
      ec_set_output(&curr->data.ezo, EC_OUTPUT_CONDUCTIVITY, true);
      ec_set_output(&curr->data.ezo, EC_OUTPUT_TDS, true);
      ec_set_output(&curr->data.ezo, EC_OUTPUT_SALINITY, true);
      ec_set_output(&curr->data.ezo, EC_OUTPUT_SPECIFIC_GRAVITY, true);
    } else if (curr->type == ACD_TYPE_EZO_PMP) {
      pump_stop(&curr->data.ezo);                           // never start with a pump running
      float max_ml_min = pump_get_max_flow_rate(&curr->data.ezo);
      if (max_ml_min > 0) {
        float dev = max_ml_min / 60.0f;
        if (fabsf(dev - curr->dose_stats.flow_rate) > 0.05f * dev)
        LOG(LOG_WARNING, "%s: configured %.2f ml/s, pump reports %.2f ml/s; using the pump's",curr->label, curr->dose_stats.flow_rate, dev);
        curr->dose_stats.flow_rate = dev;
      }
    } else if (curr->type == ACD_TYPE_D1W_TEMP) {
      w1_init_generic(&curr->data.w1, curr->data.w1.path, curr->data.w1.scale, curr->data.w1.offset);
    } else if (curr->type == ACD_TYPE_SYSFS_VALUE) {
      sysfs_init_sensor(&curr->data.sysfs);
    } else if (curr->type == ACD_TYPE_I2C_PRS) {
      //LOG(LOG_DEBUG,"Setting up I2C Pressure Sensor: %s, address 0x%02X\n", curr->label, curr->data.i2c.address);
      i2c_sensor_init(&curr->data.i2c, curr->data.i2c.type, curr->data.i2c.address, curr->data.i2c.min_value, curr->data.i2c.max_value);
    } else if (curr->type == ACD_TYPE_VIR_TANK) {
      calculate_tank_volumes(curr);
      curr->state = ACD_LED_ON;
    }

    if (curr->type == ACD_TYPE_MASTER) {
      curr->state = ACD_LED_ENABLED;
    } else if (IS_INPUT(curr->type)) {
      curr->state = ACD_LED_DISABLED; // DON'T use setKeyLed() here, startup need to force to enabled.
      if (isMASKSET(curr->flags,CALC_AVERAGE)) {
        pthread_mutex_init(&curr->stats.lock, NULL);
        reset_sensor_average(curr);
      }
    } else if (curr->type == ACD_TYPE_GPIO_OUTPUT /*|| curr->type == ACD_TYPE_GPIO_INPUT*/) {
      //if (curr->scope == ACD_ACTION_ALLOW)
        curr->state = ACD_LED_OFF;
      //else
      //  curr->state = ACD_LED_ENABLED; // ACD_ACTION_ LIMIT | BLOCK
    } else if (IS_OUTPUT(curr->type)) {
      // GPIO status will be set from sync_pump_state() above
      //if (curr->type != ACD_TYPE_GPIO_PMP) {
        curr->state = ACD_LED_ENABLED;
      //}
    } else if (IS_CONDITION(curr->type)) {
      curr->state = ACD_LED_OFF;
    }

    // reset error count
    curr->err_cnt = 0;
  }

  // Now we have tank volumes, check if pumps can be on.
  validate_pumps_against_tanks(&acddata);

  // If we don't have any conditions to test, set master to ON.
  if (!acddata.haveConditions){
    //set_key_state(&acddata, acddata.keys, ACD_LED_ON);
    state_change_request(&acddata, acddata.keys, ACD_LED_ON);
    //acddata.keys->state = ACD_LED_ON;
  }

  start_gpio_monitor(&acddata);

  // ── Normal mode — loop reading sensors ──────────────────────────────────────

  int reading_log_level = LOG_INFO;
  if (_acdconfig_.log_sensor_readings) {
    reading_log_level = LOG_NOTICE;
  } 

  update_display_message(&acddata, ACD_MSG_CLEAR, NULL);
  clock_gettime(CLOCK_MONOTONIC, &next_wake);

  _thread_control.state = ACD_KEEPRUNNING;

  //while ( _runstate == ACD_KEEPRUNNING )
  while (atomic_load_explicit(&_thread_control.state, memory_order_relaxed) == ACD_KEEPRUNNING)
  {
    //acd_scope_t sensors_read_scope = ACD_SCOPE_GLOBAL;
    float temp_reading_for_ph = UNKNOWN;
    char *master_temp_label = NULL;

    int gpio_state = GPIO_ERROR;

    LOG(reading_log_level,"---- taking reading(s) ----\n");
    update_display_message(&acddata, ACD_MSG_CLEAR, NULL);

    if (acddata.keys->state == ACD_LED_OFF) {
      LOG(LOG_DEBUG,"AquachemD is off, skipping reading of sensors!\n");
      goto next_wake; // Skip the rest of the loop and go straight to sleep if any condition is not met
    }

    for (acd_key_t *curr = acddata.keys; curr != NULL; curr = curr->next) {
      if (IS_CONDITION(curr->type)) {
        if (curr->type == ACD_TYPE_GPIO_COND) {
          // This should have been changed from the gpio_monitor, but 2nd check doesn't hurt
          gpio_state = sensor_is_met(&curr->data.gpio);
          if (gpio_state != GPIO_ERROR && gpio_state != curr->met) {
            ASSIGN_IF_CHANGED(curr->met, gpio_state, acddata.is_dirty, curr->is_dirty);
            set_key_state(&acddata, curr, curr->met?ACD_LED_ON:ACD_LED_OFF);
          } else if (gpio_state == GPIO_ERROR) {
            LOG(LOG_ERR, "Reading %s GPIO pin %d\n",curr->label,curr->data.gpio.pin);
          }
        }
        if (!curr->met) {
          if ( !isMASKSET(curr->flags, CONDITION_NOTIFIED)) {
            LOG(LOG_WARNING,"Condition not met: %s\n", curr->label);
            update_display_message(&acddata, ACD_MSG_CONDITION_FAILED, curr->label);
            setMASK(curr->flags, CONDITION_NOTIFIED);
          }
        } else if (curr->met && isMASKSET(curr->flags, CONDITION_NOTIFIED)) {
          removeMASK(curr->flags, CONDITION_NOTIFIED);
          LOG(LOG_NOTICE,"Condition satisfied: %s\n", curr->label);
        }
      }
      else if (curr->type == ACD_TYPE_EZO_PMP && curr->state == ACD_LED_ON) {
        // EZO pump is both input and putput, so we need to read the status of the pump 
      }
    }
    
    //  Master state = ON, scope = Allow         // All good.
    //  Master state = ENABLED, scope = Global   // Condition set to Global failed (but can read local sensors)
    //  Master state = ON, scope = Local         // Condition set to local failed (is can read all sensors, but not dose)
    //LOG(LOG_ERR, "Master state = %s, scope = %s\n",acd_state_to_str(acddata.keys->state), acd_scope_to_str(acddata.keys->scope) );

    for (acd_key_t *key = acddata.keys->next; key != NULL; key = key->next) {
 
      if (!should_sensor_read(&acddata, key)) {
        LOG(LOG_INFO,"Sensor %s set to not read, ignoring\n",key->label);
        LOG(reading_log_level,"Sensor %s set to not read, ignoring! (Master=%s Sensor=%s)\n",key->label,acd_action_to_str(acddata.keys->scope),acd_scope_to_str(key->scope));
        continue;
      } else if (isMASKSET(key->flags,  ACD_FLAG_FAULTED)) {
        LOG(LOG_DEBUG,"Sensor %s failed, skipping\n",key->label);
        continue;
      } else if (key->state == ACD_LED_DISABLED) {
        LOG(LOG_DEBUG,"Sensor %s is disabled, skipping\n",key->label);
        continue;
      } else if (isMASKSET(key->flags,  ACD_FLAG_VIRTUAL)) {
        LOG(LOG_DEBUG,"Sensor %s is a child/virtual sensor, skipping\n",key->label);
        continue;
      }

      switch (key->type) {
        case ACD_TYPE_MQTT_TEMP:          
          if (key->index == MASTER_ID) { // If this is the master temp sensor, also update the temp reading for pH compensation
            temp_reading_for_ph = key->value;
            master_temp_label = key->label;
          }
          LOG(LOG_DEBUG, "MQTT Sensor '%s' current value: %.2f\n", key->label, key->value);
          break;
        case ACD_TYPE_D1W_TEMP: {
          w1_reading_t temp_reading = w1_read(&key->data.w1);
          if (temp_reading.status == W1_SUCCESS) {
            LOG(reading_log_level,"Temp %s : %.2f°C\n", key->label, temp_reading.value);
            if (key->index == MASTER_ID) { // If this is the master temp sensor, also update the temp reading for pH compensation
              temp_reading_for_ph = temp_reading.value; 
              master_temp_label = key->label;
            }
            ASSIGN_IF_CHANGED(key->value, temp_reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "D1w Temp Sensor '%s' read failed (status %d)\n", key->label, temp_reading.status);            
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
        } break;
        case ACD_TYPE_EZO_TEMP: {
          rtd_reading_t temp_reading = rtd_get_reading(&key->data.ezo);
          if (temp_reading.status == EZO_SUCCESS) {
            LOG(reading_log_level,"Temp %s : %.2f°C\n", key->label, temp_reading.value);
            if (key->index == MASTER_ID) { // If this is the master temp sensor, also update the temp reading for pH compensation
              temp_reading_for_ph = temp_reading.value;
              master_temp_label = key->label;
            }
            ASSIGN_IF_CHANGED(key->value, temp_reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "EZO Temp Sensor '%s' read failed (status %d)\n", key->label, temp_reading.status);
            
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
        } break;
        case ACD_TYPE_EZO_PH: {
          ph_reading_t ph_reading;
          if (_acdconfig_.temp_compensated_ph == false) {
            ph_reading = ph_get_reading(&key->data.ezo);
          } else if ( temp_reading_for_ph != UNKNOWN) {
            if (temp_reading_for_ph > _acdconfig_.ph_reading_temp_max || temp_reading_for_ph < _acdconfig_.ph_reading_temp_min ) {
              char buf[128];
              sprintf(buf, "Water temperature %.2f°C too %s for Ph reading", temp_reading_for_ph, temp_reading_for_ph < _acdconfig_.ph_reading_temp_min?"cold":"hot");
              LOG(LOG_WARNING, "%s\n", buf);
              set_key_state(&acddata, key, ACD_LED_OFF);
              update_display_message(&acddata, ACD_MSG_CONDITION_FAILED, buf);
              break;
            }
            LOG(reading_log_level, "Using %s, %.2f for pH compensated reading", master_temp_label?master_temp_label:"", temp_reading_for_ph);
            ph_reading = ph_get_reading_compensated(&key->data.ezo, temp_reading_for_ph);
          } else {
            LOG(LOG_WARNING, "EZO pH Sensor '%s' skipped compensation because temp is unknown\n", key->label);
            set_key_state(&acddata, key, ACD_LED_OFF);
            break;
          }
          
          if (ph_reading.status == EZO_SUCCESS) {
            LOG(reading_log_level,"EZO pH Sensor %s : %.2f\n", key->label, ph_reading.value);
            ASSIGN_IF_CHANGED(key->value, ph_reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "EZO pH Sensor '%s' read failed (status %d)\n", key->label, ph_reading.status);
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
        } break;
        case ACD_TYPE_EZO_ORP: {
          orp_reading_t orp_reading = orp_get_reading(&key->data.ezo);
          if (orp_reading.status == EZO_SUCCESS) {
            LOG(reading_log_level,"EZO ORP Sensor %s : %.2f mV\n", key->label, orp_reading.value);
            ASSIGN_IF_CHANGED(key->value, orp_reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "EZO ORP Sensor '%s' read failed (status %d)\n", key->label, orp_reading.status);           
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
         } break;
        case ACD_TYPE_EZO_PRS: {
          prs_reading_t prs_reading = prs_get_reading(&key->data.ezo);
          if (prs_reading.status == EZO_SUCCESS) {
            LOG(reading_log_level,"EZO PRS Sensor %s : %.2f mV\n", key->label, prs_reading.value);
            ASSIGN_IF_CHANGED(key->value, prs_reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "EZO PRS Sensor '%s' read failed (status %d)\n", key->label, prs_reading.status);            
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }

        } break;
        case ACD_TYPE_EZO_EC: {
          ec_reading_t ec_reading = ec_get_reading(&key->data.ezo);
          if (ec_reading.status == EZO_SUCCESS) {
            // There are multiple values, so run through the children and update each one that is set to be read.
            for (acd_key_t *child = key; child != NULL; child = child->child) {
              if (isMASKSET(child->data.ezo.flags, EC_CONDUCTIVITY)) {
                LOG(reading_log_level,"EZO EC Sensor %s : %.2f %s\n", child->label, ec_reading.conductivity, uom_to_str(child->uom));
                ASSIGN_IF_CHANGED(child->value, ec_reading.conductivity, acddata.is_dirty, child->is_dirty);
              } else if (isMASKSET(child->data.ezo.flags, EC_TDS)) {
                LOG(reading_log_level,"EZO EC Sensor %s : %.2f %s\n", child->label, ec_reading.tds, uom_to_str(child->uom));
                ASSIGN_IF_CHANGED(child->value, ec_reading.tds, acddata.is_dirty, child->is_dirty);
               } else if (isMASKSET(child->data.ezo.flags, EC_SALINITY)) {
                LOG(reading_log_level,"EZO EC Sensor %s : %.2f %s\n", child->label, ec_reading.salinity, uom_to_str(child->uom));
                ASSIGN_IF_CHANGED(child->value, ec_reading.salinity, acddata.is_dirty, child->is_dirty);
              } else if (isMASKSET(child->data.ezo.flags, EC_SPECIFIC_GRAVITY)) {
                LOG(reading_log_level,"EZO EC Sensor %s : %.2f %s\n", child->label, ec_reading.specific_gravity, uom_to_str(child->uom));
                ASSIGN_IF_CHANGED(child->value, ec_reading.specific_gravity, acddata.is_dirty, child->is_dirty);    
              }
              set_key_state(&acddata, child, ACD_LED_ON);
              child->err_cnt=0;
              update_sensor_average(child);
            }
 
          } else {
            LOG(LOG_WARNING, "EZO EC Sensor '%s' read failed (status %d)\n", key->label, ec_reading.status);            
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
        } break;
        case ACD_TYPE_SYSFS_VALUE:{
          sysfs_reading_t reading = sysfs_read_sensor(&key->data.sysfs);
          if (reading.status == SYSFS_SUCCESS) {
            LOG(reading_log_level,"%s : %.2f\n", key->label, reading.value);
            ASSIGN_IF_CHANGED(key->value, reading.value, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
          } else {
            LOG(LOG_WARNING, "System FS Sensor '%s' read failed (status %d)\n", key->label, reading.status);
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
          }
        } break;
        case ACD_TYPE_I2C_PRS: {
          i2c_reading_t reading = i2c_sensor_get_reading(&key->data.i2c);
          if (reading.status == I2C_SUCCESS) {
            LOG(reading_log_level,"%s : %.2f psi (%.2f Bar), Temperature %.1f C\n", key->label, I2C_BAR_TO_PSI(reading.value), reading.value, reading.temp_c);
            ASSIGN_IF_CHANGED(key->value, I2C_BAR_TO_PSI(reading.value), acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, ACD_LED_ON);
            key->err_cnt=0;
            update_sensor_average(key);
            // If we have a child sensor (temp), update it as well
            if (key->child && key->child->type == ACD_TYPE_I2C_TEMP && key->data.i2c.type == I2C_SENSOR_PTE7300) {
              ASSIGN_IF_CHANGED(key->child->value, reading.temp_c, acddata.is_dirty, key->child->is_dirty);
              set_key_state(&acddata, key->child, ACD_LED_ON);
              key->child->err_cnt=0;
              update_sensor_average(key->child);
            }
          } else {
            LOG(LOG_WARNING, "I2C Sensor '%s' read failed (status %d)\n", key->label, reading.status);
            update_display_message(&acddata, ACD_MSG_SENSOR_READ_FAILED, key->label);
            sensor_read_error(&acddata, key);
            if (key->child && key->child->type == ACD_TYPE_I2C_TEMP && key->data.i2c.type == I2C_SENSOR_PTE7300) {
              LOG(LOG_WARNING, "I2C Sensor '%s' child temp read failed (status %d)\n", key->child->label, reading.status);
              sensor_read_error(&acddata, key->child);  
            }
          }
        } break;
    
        case ACD_TYPE_GPIO_INPUT:
          gpio_state = sensor_is_met(&key->data.gpio);
          if (gpio_state != GPIO_ERROR && ( gpio_state != key->ison || (key->ison?ACD_LED_ON:ACD_LED_OFF) != key->state )) {
            ASSIGN_IF_CHANGED(key->ison, gpio_state, acddata.is_dirty, key->is_dirty);
            set_key_state(&acddata, key, key->ison?ACD_LED_ON:ACD_LED_OFF);
          } else if (gpio_state == GPIO_ERROR) {
            LOG(LOG_ERR, "Reading %s GPIO pin %d\n",key->label,key->data.gpio.pin);
          }
          break;

        case ACD_TYPE_GPIO_OUTPUT:
          check_gpio_output_state(&acddata, key); 
        break;

        case ACD_TYPE_GPIO_PMP:
        case ACD_TYPE_EZO_PMP:
        case ACD_TYPE_MQTT_COND:
        case ACD_TYPE_GPIO_COND:
        case ACD_TYPE_MQTT_VALUE:
        break;

        default:
          LOG(LOG_WARNING, "Unknown sensor type for sensor '%s'\n", key->label);
      }
    }


next_wake:

    LOG(reading_log_level,"- reading(s) took: %.2fs -\n", elapsed_ms(&next_wake) / 1000);

    // Sleep until next_wake OR until forced by force_sensor_poll() / state change
    pthread_mutex_lock(&_thread_control.mutex);
    int wait_res = 0;
    if (atomic_load_explicit(&_thread_control.state, memory_order_relaxed) == ACD_KEEPRUNNING) {
        wait_res = pthread_cond_timedwait(&_thread_control.cond, &_thread_control.mutex, &next_wake);
    }
    pthread_mutex_unlock(&_thread_control.mutex);

    if (wait_res == 0) {LOG(LOG_INFO,"Woken early to read sensors\n");}

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // If woken early by force_sensor_poll() (wait_res == 0) or fell behind schedule:
    if (wait_res == 0 || now.tv_sec >= next_wake.tv_sec) {
        next_wake = now; // Reset poll timer base to right now
    }

    // Advance the target wake time for the next iteration
    next_wake.tv_sec += _acdconfig_.sensor_poll_time;
  }



  if (atomic_load_explicit(&_thread_control.state, memory_order_acquire) == ACD_RELOAD) {
        LOG(LOG_NOTICE, "--- CONFIGURATION RELOAD INITIATED ---");
        
        // Perform a safe, sequential shutdown of the active threads
        stop_all_timers();
        devices_emergency_stop();
        stop_net_services();
        stop_gpio_monitor();

        usleep(100000); // Wait 100ms for net_services to stop
        
        // Free the old configuration structures from memory
        free_config();
        acddata.keys->next = NULL;
        
        LOG(LOG_NOTICE, "--- TEARDOWN COMPLETE. REBOOTING SERVICES ---");
        
        // Jump right back to the initialization sequence
        goto reload_configuration; 
  }


  // Cleanup net services.
  LOG(LOG_INFO, "%s Stopping.....", AQUACHEMD_SHORT_NAME);
  
  // Stop any devices & timers
  stop_all_timers();
  devices_emergency_stop();
  
  stop_net_services();
  stop_gpio_monitor();
  usleep(100000); // Wait 100ms

  release_singleton_lock();
  /*
  pthread_t net_id = get_net_services_id();
  if (net_id != 0) {
    pthread_join(net_id, NULL);
  }*/

  // Free config memory
  free_config();
  free(acddata.keys);

  LOG(LOG_NOTICE, "%s Stoped!", AQUACHEMD_SHORT_NAME);

  _thread_control.state = ACD_FINISHED;
  return 0;
}







char *_upgrade_version = NULL;

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

// Update this macro when you officially transition to your new GitHub organization
#define AQUACHEMD_REPO "AqualinkD/AquachemD"
#define AQUACHEMD_REMOTE_INSTALL "remote-install.sh"

void set_upgrade_version(char *version)
{
  _upgrade_version = malloc( (sizeof(char*) * strlen(version)) + 2);
  snprintf(_upgrade_version, strlen(version)+1, version);
}



bool start_upgrade(const char *version)
{
    pid_t pid;
    char cmd[512];

    LOG(LOG_NOTICE, "Configuring upgrade command!\n");

    // 1. Build a self-contained shell command string
    // This downloads the file to /tmp, closes the network connection, 
    // runs it detached using nohup, redirects output, and sends it to the background.
    snprintf(cmd, sizeof(cmd),
             "curl -fsSl -H 'Accept: application/vnd.github.raw' "
             "\"https://api.github.com/repos/%s/contents/release/%s\" -o /tmp/%s && "
             "chmod +x /tmp/%s && "
             "nohup /tmp/%s %s > /dev/null 2>&1 &",
             AQUACHEMD_REPO, AQUACHEMD_REMOTE_INSTALL, AQUACHEMD_REMOTE_INSTALL,
             AQUACHEMD_REMOTE_INSTALL, AQUACHEMD_REMOTE_INSTALL,
             (_upgrade_version ? _upgrade_version : ""));

    LOG(LOG_NOTICE, "Initiating daemon upgrade script (Target: %s)...", 
        _upgrade_version ? _upgrade_version : "latest");

    // 2. Single fork to hand execution off to the system shell
    pid = fork();
    if (pid == -1) {
        LOG(LOG_ERR, "Upgrade error: fork failed");
        return false;
    }

    if (pid == 0) { // Child
        setsid(); // Escape the initial cgroup session completely
        
        // Execute via standard shell processor
        char *args[] = {"/bin/sh", "-c", cmd, NULL};
        execvp(args[0], args);
        _exit(127);
    }

    // Parent clean return
    LOG(LOG_NOTICE, "Upgrade pipeline completely detached. Handed over to system shell.");
    return true;
}



/**************************************************************
# Probably a better option would be to use the below in the systemd service file
[Service]
RuntimeDirectory=aquachemd
# Then use /run/aquachemd/aquachemd.pid as LOCK_FILE_PATH
#
**************************************************************/

#define LOCK_FILE_PATH "/run/aquachemd.pid"

static int _lock_fd = -1;   // kept open for the life of the process -- do not close early

// Returns true if this is the only running instance (lock acquired),
// false if another instance already holds the lock.
bool acquire_singleton_lock(void)
{
  _lock_fd = open(LOCK_FILE_PATH, O_CREAT | O_RDWR, 0644);
  if (_lock_fd < 0) {
    LOG(LOG_ERR, "acquire_singleton_lock: could not open %s: %s", LOCK_FILE_PATH, strerror(errno));
    return false;   // fail closed -- if we can't even open the lock file, don't proceed
  }

  if (flock(_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK)
      LOG(LOG_ERR, "acquire_singleton_lock: another instance is already running (lock held on %s)", LOCK_FILE_PATH);
    else
      LOG(LOG_ERR, "acquire_singleton_lock: flock() failed: %s", strerror(errno));

    close(_lock_fd);
    _lock_fd = -1;
    return false;
  }

  // We hold the lock -- record our PID purely for diagnostics (cat /run/aquachemd.pid)
  ftruncate(_lock_fd, 0);
  char pidstr[32];
  int len = snprintf(pidstr, sizeof(pidstr), "%d\n", getpid());
  write(_lock_fd, pidstr, len);   // best-effort; not critical if this fails

  return true;
}

// Optional at clean shutdown -- exiting the process (even via SIGKILL)
// releases the flock automatically anyway, so this is just tidiness.
void release_singleton_lock(void)
{
  if (_lock_fd >= 0) {
    flock(_lock_fd, LOCK_UN);
    close(_lock_fd);
    _lock_fd = -1;
  }
}










bool OLD_start_upgrade_OLD(const char *version)
{
    int pipe_curl_to_bash[2];
    pid_t pid_curl, pid_bash;
    int status_curl, status_bash;
    char url[256];

    LOG(LOG_NOTICE, "Configuring upgrade pipeline!\n");

    // Format target GitHub API URL using our repo path configuration
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/contents/release/%s", AQUACHEMD_REPO,AQUACHEMD_REMOTE_INSTALL);

    char *curl_args[] = {"curl", "-fsSl", "-H", "Accept: application/vnd.github.raw", url, NULL};
    char *bash_args[] = {"bash", "-s", "--", (char *)(_upgrade_version ? _upgrade_version : ""), NULL};

    // 1. Notice must be logged BEFORE execution, as the script will terminate this process
    LOG(LOG_NOTICE, "Initiating daemon upgrade script (Target: %s)...", _upgrade_version ? _upgrade_version : "latest");

    if (pipe(pipe_curl_to_bash) == -1)
    {
        LOG(LOG_ERR, "Upgrade error: unable to open pipeline");
        return false;
    }

    // --- FORK 1: CURL ---
    pid_curl = fork();
    if (pid_curl == -1)
    {
        LOG(LOG_ERR, "Upgrade error: fork failed (curl)");
        close(pipe_curl_to_bash[0]);
        close(pipe_curl_to_bash[1]);
        return false;
    }

    if (pid_curl == 0)
    { // Inside Child Process (curl)
        close(pipe_curl_to_bash[0]); // Close unused read end
        if (dup2(pipe_curl_to_bash[1], STDOUT_FILENO) == -1) {
            _exit(EXIT_FAILURE);
        }
        close(pipe_curl_to_bash[1]);
        
        execvp("curl", curl_args);
        // If execvp returns, it failed. Prevent child from escaping into daemon main loop!
        _exit(127); 
    }

    // --- FORK 2: BASH ---
    pid_bash = fork();
    if (pid_bash == -1)
    {
        LOG(LOG_ERR, "Upgrade error: fork failed (bash)");
        close(pipe_curl_to_bash[0]);
        close(pipe_curl_to_bash[1]);
        
        // Clean up the already running curl child to prevent resource leaks
        kill(pid_curl, SIGTERM);
        waitpid(pid_curl, NULL, 0);
        return false;
    }

    if (pid_bash == 0)
    { // Inside Child Process (bash)
        setsid(); // CRITICAL: Escape the systemd cgroup so systemctl stop doesn't kill this script

        close(pipe_curl_to_bash[1]); // Close unused write end
        if (dup2(pipe_curl_to_bash[0], STDIN_FILENO) == -1) {
            _exit(EXIT_FAILURE);
        }
        close(pipe_curl_to_bash[0]);
        
        execvp("bash", bash_args);
        // If execvp returns, it failed. Prevent child from escaping.
        _exit(127); 
    }

    // --- PARENT PROCESS CLEANUP ---
    // Close parent copy of descriptors immediately so EOF signals pass cleanly down the pipe
    close(pipe_curl_to_bash[0]);
    close(pipe_curl_to_bash[1]);

    /* * NOTE ON THE PROCESS LIFECYCLE:
     * If remote_install.sh executes successfully, your service manager (systemd)
     * will catch up to this parent process right here while it blocks on waitpid().
     * A SIGTERM will hit this daemon, stopping it cleanly so the installer can replace it.
     */

    /*. Because we use setsid(); in the above IF, don;t wait
    // Wait for curl download phase to finalize
    if (waitpid(pid_curl, &status_curl, 0) == -1)
    {
        LOG(LOG_ERR, "Upgrade error: waitpid failed (curl)");
        return false;
    }
    */
    LOG(LOG_NOTICE, "Upgrade script detached and running in background.");
    
    // Return immediately to the main loop. 
    // The bash script is now the captain.
    return true;

    // If curl explicitly failed (e.g., 404, network drop), stop bash from hanging on empty stdin
    if (WIFEXITED(status_curl) && WEXITSTATUS(status_curl) != 0) {
        LOG(LOG_ERR, "Upgrade error: curl failed with exit code: %d", WEXITSTATUS(status_curl));
        kill(pid_bash, SIGTERM);
        waitpid(pid_bash, NULL, 0);
        return false;
    }

    // Wait for bash installer phase to complete
    if (waitpid(pid_bash, &status_bash, 0) == -1)
    {
        LOG(LOG_ERR, "Upgrade error: waitpid failed (bash)");
        return false;
    }

    // Parse bash output metrics (Only reached if script doesn't force kill this process)
    if (WIFEXITED(status_bash))
    {
        if (WEXITSTATUS(status_bash) != 0) {
            LOG(LOG_ERR, "Upgrade error: bash script exited with error code: %d", WEXITSTATUS(status_bash));
            return false;
        }
    }
    else if (WIFSIGNALED(status_bash))
    {
        LOG(LOG_ERR, "Upgrade error: bash script terminated by signal: %d", WTERMSIG(status_bash));
        return false;
    }

    LOG(LOG_NOTICE, "Upgrade pipeline completed without daemon restart.");
    return true;
}


















